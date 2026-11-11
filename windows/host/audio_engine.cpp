// audio_engine.cpp -- see audio_engine.h.

#include "audio_engine.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

namespace winapk::audio {

int64_t nano_time() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<int64_t>(static_cast<double>(c.QuadPart) * 1e9 / static_cast<double>(freq));
}

// ---- the mixer -------------------------------------------------------------------------
class Source {
public:
    virtual ~Source() = default;
    // Adds `frames` stereo frames at `out_rate` into `out`. False: finished,
    // drop me.
    virtual bool mix(float* out, size_t frames, int out_rate) = 0;
};

namespace {

template <typename T>
struct Com {
    T* p = nullptr;
    ~Com() {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() { return p; }
    explicit operator bool() const { return p != nullptr; }
    void reset() { if (p) { p->Release(); p = nullptr; } }
};

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, spelled out: MinGW's import libraries do
// not all carry it.
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static std::atomic<int> g_preferred_source_rate{48000};

// Phase20-B2.3.6.4.19: the USB Direct preroll target is the single largest
// component of the pipeline latency that makes the app UI run ahead of
// the sound (~1000 ms of the ~1300 ms total). It is now tunable via
// WINAPK_USB_PREROLL_MS (50..3000, default 1000 = historical behaviour)
// so the lead can be shrunk safely -- by buffering less, not by
// distorting URB completion timing, which UCRT's stream supervision
// rejects (.17/.18). Shorter values trade skip margin for sync.
static int usb_preroll_target_ms() {
    static const int ms = [] {
        char value[16]{};
        const DWORD n = GetEnvironmentVariableA("WINAPK_USB_PREROLL_MS", value, static_cast<DWORD>(sizeof(value)));
        if (n != 0 && n < sizeof(value)) {
            const int parsed = std::atoi(value);
            if (parsed >= 50 && parsed <= 3000) return parsed;
        }
        return 1000;
    }();
    return ms;
}

// Phase20-B2.3.6.4.20: second real-latency knob. The exclusive endpoint
// buffer (250 ms by default) is the other large fixed component of the
// UI lead. WINAPK_USB_ENDPOINT_MS (25..250, default 250 = historical
// ladder) caps the ladder; the device period itself is always allowed
// so negotiation cannot fail purely because of the cap. Shorter values
// trade WASAPI stall margin for less lead, exactly like the preroll.
static int usb_endpoint_target_ms() {
    static const int ms = [] {
        char value[16]{};
        const DWORD n = GetEnvironmentVariableA("WINAPK_USB_ENDPOINT_MS", value, static_cast<DWORD>(sizeof(value)));
        if (n != 0 && n < sizeof(value)) {
            const int parsed = std::atoi(value);
            if (parsed >= 25 && parsed <= 250) return parsed;
        }
        return 250;
    }();
    return ms;
}

// Phase20-B2.3.6.4.23: the NORMAL mixer's exclusive ladder was the
// spectrum killer on the speaker route. With a 250 ms exclusive buffer
// the render loop can only consume the source rings in ~250 ms quanta,
// so AudioTrack positions and blocked app writers advance in 4 Hz
// steps -- and an app-side spectrum tap fed by that cadence steps with
// it (the USB-DAC route is immune: the app taps its own 4 ms URBs).
// The normal mixer therefore uses WASAPI SHARED mode by default
// (40 ms buffer, ~10 ms device-period consumption quanta, the cadence
// Android's own AudioTrack HAL works at). WINAPK_SPEAKER_EXCLUSIVE_MS
// (25..250) restores the exclusive ladder for this mixer, capped at
// the given size, for low-spec machines that need the reservoir.
static int speaker_exclusive_cap_ms() {
    static const int ms = [] {
        char value[16]{};
        const DWORD n = GetEnvironmentVariableA("WINAPK_SPEAKER_EXCLUSIVE_MS", value, static_cast<DWORD>(sizeof(value)));
        if (n != 0 && n < sizeof(value)) {
            const int parsed = std::atoi(value);
            if (parsed >= 25 && parsed <= 250) return parsed;
        }
        return 0;  // 0 = shared mode
    }();
    return ms;
}

static bool b2_verbose_log_enabled() {
    static const bool enabled = [] {
        char value[8]{};
        const DWORD n = GetEnvironmentVariableA("WINAPK_VERBOSE_AUDIO", value, static_cast<DWORD>(sizeof(value)));
        return n != 0 && n < sizeof(value) && value[0] != '0';
    }();
    return enabled;
}

static void b2_log(const char* format, ...) {
    // Phase 20-B2.3.5.8-A: routine render-path logging is opt-in.
    if (!b2_verbose_log_enabled()) return;
    std::fprintf(stderr, "[I] Win.apk.WASAPI: [Phase20-B2.3.5.8] ");
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
    // No fflush here. Fatal/rare errors use b2_error below.
}

static void b2_error(const char* operation, HRESULT hr) {
    std::fprintf(stderr, "[E] Win.apk.WASAPI: [Phase20-B2.3.5.8] %s failed hr=0x%08lX\n",
                 operation, static_cast<unsigned long>(hr));
    std::fflush(stderr);
}

// ---- Phase20-B2.3.6.4.21: Visualizer output tap -------------------------
// The upstream Visualizer.java was an honest stub: getWaveForm returned
// silence, getFft zeros, and the data-capture listener never fired,
// because "the mixer has no per-session tap". Apps that draw a spectrum
// for ordinary (non-USB) playback have no other Android API to get it
// from, so on speaker output their analyser starved while the same app
// in USB mode (its own tap) animated smoothly. This is that tap: a
// lock-free mono ring fed from Mixer::mix's final output. Session 0 is
// the output mix, and it is the only session here, so one ring is
// exactly the right source.
namespace viz {
constexpr size_t kCap = 8192;  // mono frames; >= 4x the largest capture
float ring[kCap]{};
std::atomic<uint64_t> written{0};
std::atomic<int> rate{0};

void feed(const float* stereo, size_t frames, int r) {
    if (!frames || r <= 0) return;
    rate.store(r, std::memory_order_relaxed);
    uint64_t w = written.load(std::memory_order_relaxed);
    for (size_t i = 0; i < frames; ++i) {
        ring[w % kCap] = 0.5f * (stereo[i * 2] + stereo[i * 2 + 1]);
        ++w;
    }
    written.store(w, std::memory_order_release);
}

// Copies the most recent `count` mono samples (oldest first) into dst.
// Returns the tap rate, or 0 when fewer than count samples exist so far
// (the head is silence-padded in that case, like a capture that starts
// before the stream does).
int snapshot(float* dst, int count) {
    if (count <= 0) return 0;
    uint64_t w = written.load(std::memory_order_acquire);
    const int r = rate.load(std::memory_order_relaxed);
    const uint64_t avail = w < static_cast<uint64_t>(count) ? w : static_cast<uint64_t>(count);
    const uint64_t start = w - avail;
    const int pad = count - static_cast<int>(avail);
    for (int i = 0; i < pad; ++i) dst[i] = 0.0f;
    for (uint64_t i = 0; i < avail; ++i)
        dst[pad + static_cast<int>(i)] = ring[(start + i) % kCap];
    return w >= static_cast<uint64_t>(count) || r > 0 ? r : 0;
}
}  // namespace viz


class Mixer {
public:
    static Mixer& get() {
        static Mixer* m = new Mixer(false);  // normal Android audio, Windows default endpoint
        return *m;
    }
    static Mixer& usb_direct() {
        static Mixer* m = new Mixer(true);   // UCRT USB bridge, configured physical endpoint only
        return *m;
    }
    void add(std::shared_ptr<Source> s) {
        {
            std::lock_guard<std::mutex> hold(lock_);
            sources_.push_back(std::move(s));
        }
        ensure_render_thread();
    }
    void remove(const Source* s) {
        std::lock_guard<std::mutex> hold(lock_);
        sources_.erase(std::remove_if(sources_.begin(), sources_.end(),
                                      [s](const std::shared_ptr<Source>& x) { return x.get() == s; }),
                       sources_.end());
    }
    int rate() const { return rate_.load(); }
    bool set_endpoint(const std::string& endpoint_id) {
        {
            std::lock_guard<std::mutex> hold(route_lock_);
            endpoint_id_ = endpoint_id;
            ++route_generation_;
        }
        return true;
    }
    void set_session_volume(float value) {
        const float clamped = std::max(0.0f, std::min(1.0f, value));
        requested_session_volume_.store(clamped, std::memory_order_relaxed);
        // Keep the Android/app-visible value stable while an exclusive stream has
        // no ISimpleAudioVolume object. Shared mode may later refresh it from Sndvol.
        observed_session_volume_.store(clamped, std::memory_order_release);
        session_volume_dirty_.store(true, std::memory_order_release);
    }
    float session_volume() const { return observed_session_volume_.load(std::memory_order_acquire); }
    // Phase20-B2.3.6.4.17: endpoint buffer depth for output-latency
    // reporting (written by the render thread at device setup; a plain
    // read is fine for a telemetry value).
    uint32_t endpoint_buffer_frames() const { return endpoint_buffer_frames_.load(std::memory_order_relaxed); }
    int endpoint_rate() const { return endpoint_rate_.load(std::memory_order_relaxed); }
    void set_session_mute(bool muted) {
        requested_session_mute_.store(muted, std::memory_order_relaxed);
        observed_session_mute_.store(muted, std::memory_order_release);
        session_mute_dirty_.store(true, std::memory_order_release);
    }
    bool session_mute() const { return observed_session_mute_.load(std::memory_order_acquire); }
    std::string endpoint() const {
        std::lock_guard<std::mutex> hold(route_lock_);
        return endpoint_id_;
    }

private:
    void ensure_render_thread() {
        bool expected = false;
        if (!render_thread_started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
        std::thread([this] { run(); }).detach();
    }

    explicit Mixer(bool usb_direct) : usb_direct_(usb_direct) {
        HKEY key = nullptr;
        if (!usb_direct_) { b2_log("Phase20-B2.3.5.8 normal mixer configured endpoint=default-console lazyThread=1"); return; }
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Win.apk", 0, KEY_READ, &key) == ERROR_SUCCESS) {
            DWORD type = 0, bytes = 0;
            if (RegQueryValueExW(key, L"UsbDirectOutputEndpointId", nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS && type == REG_SZ && bytes >= sizeof(wchar_t)) {
                std::wstring value(bytes / sizeof(wchar_t), L'\0');
                if (RegQueryValueExW(key, L"UsbDirectOutputEndpointId", nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &bytes) == ERROR_SUCCESS) {
                    while (!value.empty() && value.back() == L'\0') value.pop_back();
                    if (!value.empty()) { int n = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr); endpoint_id_.resize(size_t(n > 0 ? n - 1 : 0)); if (n > 1) WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, endpoint_id_.data(), n, nullptr, nullptr); }
                }
            }
            RegCloseKey(key);
        }
        DWORD gain_step=0,gain_bytes=sizeof(gain_step);if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\Win.apk",L"UsbDirectGainStep",RRF_RT_REG_DWORD,nullptr,&gain_step,&gain_bytes)!=ERROR_SUCCESS)gain_step=0;gain_step=(std::min)(gain_step,6ul);usb_gain_db_=float(gain_step*3);usb_gain_target_=std::pow(10.0f,usb_gain_db_/20.0f);usb_gain_current_=1.0f;DWORD punch=3,punch_bytes=sizeof(punch);if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\Win.apk",L"UsbDirectPunchLevel",RRF_RT_REG_DWORD,nullptr,&punch,&punch_bytes)!=ERROR_SUCCESS)punch=3;usb_punch_level_=(std::min)(punch,3ul);
        b2_log("Phase20-B2.3.5.8 USB Direct configured endpoint=%s gainDb=%.1f linearTarget=%.6f lazyThread=1", endpoint_id_.empty() ? "MISSING" : endpoint_id_.c_str(),double(usb_gain_db_),double(usb_gain_target_)); b2_log("Audio Punch configured level=%lu", static_cast<unsigned long>(usb_punch_level_));
    }

    void mix(float* out, size_t frames) {
        std::fill(out, out + frames * 2, 0.0f);
        std::vector<std::shared_ptr<Source>> now;
        {
            std::lock_guard<std::mutex> hold(lock_);
            now = sources_;
        }
        for (const auto& s : now) {
            if (!s->mix(out, frames, rate_.load())) remove(s.get());
        }
        for (size_t i = 0; i < frames * 2; ++i) out[i] = std::clamp(out[i], -1.0f, 1.0f);
        // Phase20-B2.3.6.4.21: feed the Visualizer output tap with the
        // final mix exactly as rendered (post-clamp, pre-device).
        viz::feed(out, frames, rate_.load());
    }

    void run() {
        DWORD thread_throttle_error = ERROR_SUCCESS;
        bool thread_throttle_off = false;
        using SetThreadInformationFn = BOOL (WINAPI*)(HANDLE, THREAD_INFORMATION_CLASS, LPVOID, DWORD);
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        auto set_thread_info = kernel32 ? reinterpret_cast<SetThreadInformationFn>(
            GetProcAddress(kernel32, "SetThreadInformation")) : nullptr;
        if (set_thread_info) {
            THREAD_POWER_THROTTLING_STATE state{};
            state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
            state.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
            state.StateMask = 0;
            SetLastError(ERROR_SUCCESS);
            thread_throttle_off = set_thread_info(GetCurrentThread(), ThreadPowerThrottling,
                                                   &state, sizeof(state)) != FALSE;
            thread_throttle_error = thread_throttle_off ? ERROR_SUCCESS : GetLastError();
        } else {
            thread_throttle_error = ERROR_CALL_NOT_IMPLEMENTED;
        }
        const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // Keep the render service loop from being delayed by normal UI/background work.
        // Resolve MMCSS dynamically so existing build/link settings remain unchanged.
        HMODULE avrt = LoadLibraryW(L"avrt.dll");
        using AvSetFn = HANDLE (WINAPI*)(LPCWSTR, LPDWORD);
        using AvPriorityFn = BOOL (WINAPI*)(HANDLE, int);
        auto av_set = avrt ? reinterpret_cast<AvSetFn>(GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW")) : nullptr;
        auto av_priority = avrt ? reinterpret_cast<AvPriorityFn>(GetProcAddress(avrt, "AvSetMmThreadPriority")) : nullptr;
        DWORD mmcss_task_index = 0;
        HANDLE mmcss = av_set ? av_set(L"Pro Audio", &mmcss_task_index) : nullptr;
        // AVRT_PRIORITY_CRITICAL is 2. Prefer MMCSS so Windows can prioritize
        // this time-sensitive render thread without starving unrelated work.
        const BOOL mmcss_priority_ok = mmcss && av_priority ? av_priority(mmcss, 2) : FALSE;
        // If MMCSS is unavailable, use the requested second scheduling boost for
        // this render thread. The process stays HIGH rather than REALTIME so the
        // operating system retains priority over the application as a whole.
        const BOOL fallback_priority_ok = mmcss ? TRUE :
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        b2_log("Audio mixer thread started CoInitializeEx=0x%08lX mmcss=%s taskIndex=%lu mmcssCritical=%d fallbackTimeCritical=%d efficiencyModeOff=%d powerThrottleError=%lu",
               static_cast<unsigned long>(co_hr), mmcss ? "Pro Audio" : "unavailable",
               static_cast<unsigned long>(mmcss_task_index), mmcss_priority_ok ? 1 : 0,
               (!mmcss && fallback_priority_ok) ? 1 : 0,
               thread_throttle_off ? 1 : 0, static_cast<unsigned long>(thread_throttle_error));
        for (;;) {
            if (!run_device()) {
                // Keep streams alive while an endpoint is temporarily unavailable.
                for (int i = 0; i < 10; ++i) {
                    run_silent_once();
                    Sleep(20);
                }
            }
        }
    }

    // The selected render endpoint. Phase 19-B3.4 negotiates an integer PCM
    // exclusive format before falling back to the endpoint shared mix format.
    bool run_device() {
        uint64_t generation;
        std::string endpoint_id;
        {
            std::lock_guard<std::mutex> hold(route_lock_);
            generation = route_generation_;
            endpoint_id = endpoint_id_;
        }
        if (usb_direct_ && endpoint_id.empty()) { b2_log("Phase20-B2.3.2 USB Direct endpoint missing; refusing default speaker"); return false; }
        Com<IMMDeviceEnumerator> enumerator;
        Com<IMMDevice> device;
        Com<IAudioClient> client;
        Com<IAudioRenderClient> render;
        Com<ISimpleAudioVolume> session_volume;
        WAVEFORMATEX* mix_format = nullptr;
        HRESULT device_hr = E_FAIL;
        const HRESULT enum_hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        b2_log("Phase20-B2.3.1 endpoint resolve configured=%s CoCreateInstance=0x%08lX", endpoint_id.empty()?"default-console":endpoint_id.c_str(), static_cast<unsigned long>(enum_hr));
        if (SUCCEEDED(enum_hr)) {
            if (endpoint_id.empty()) {
                device_hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
            } else {
                int wn = MultiByteToWideChar(CP_UTF8, 0, endpoint_id.c_str(), -1, nullptr, 0);
                std::wstring wid(wn > 0 ? static_cast<size_t>(wn) : 0, L'\0');
                if (wn > 1) {
                    MultiByteToWideChar(CP_UTF8, 0, endpoint_id.c_str(), -1, wid.data(), wn);
                    wid.resize(static_cast<size_t>(wn - 1));
                }
                if (!wid.empty()) device_hr = enumerator->GetDevice(wid.c_str(), &device);
            }
        }
        b2_log("Phase20-B2.3.1 GetDevice/default result=0x%08lX device=%s", static_cast<unsigned long>(device_hr), device?"yes":"no");
        if (FAILED(device_hr)) { b2_error(usb_direct_ ? "USB Direct endpoint open" : "Endpoint open", device_hr); return false; }
        if (usb_direct_) { LPWSTR actual=nullptr;HRESULT idhr=device->GetId(&actual);std::string actual8;if(SUCCEEDED(idhr)&&actual){int n=WideCharToMultiByte(CP_UTF8,0,actual,-1,nullptr,0,nullptr,nullptr);actual8.resize(size_t(n>0?n-1:0));if(n>1)WideCharToMultiByte(CP_UTF8,0,actual,-1,actual8.data(),n,nullptr,nullptr);CoTaskMemFree(actual);}if(FAILED(idhr)||actual8!=endpoint_id){b2_log("Phase20-B2.3.2 endpoint identity mismatch requested=%s actual=%s hr=0x%08lX",endpoint_id.c_str(),actual8.c_str(),static_cast<unsigned long>(idhr));return false;}b2_log("Phase20-B2.3.2 dedicated endpoint verified id=%s",actual8.c_str());}
        b2_log("Endpoint opened route=%s", endpoint_id.empty() ? "default-console" : endpoint_id.c_str());
        HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
        b2_log("Phase20-B2.3.1 IAudioClient Activate=0x%08lX client=%s", static_cast<unsigned long>(hr), client?"yes":"no");
        if (FAILED(hr)) { b2_error("IAudioClient Activate", hr); return false; }
        hr = client->GetMixFormat(&mix_format);
        b2_log("Phase20-B2.3.1 GetMixFormat=0x%08lX format=%s", static_cast<unsigned long>(hr), mix_format?"yes":"no");
        if (FAILED(hr)) { b2_error("GetMixFormat", hr); return false; }

        const bool mix_is_float = mix_format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
            (mix_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix_format)->SubFormat == kSubtypeFloat);
        b2_log("Endpoint format rate=%lu channels=%d bits=%d blockAlign=%d avgBytesPerSec=%lu cbSize=%u subtype=%s",
               static_cast<unsigned long>(mix_format->nSamplesPerSec), mix_format->nChannels,
               mix_format->wBitsPerSample, mix_format->nBlockAlign,
               static_cast<unsigned long>(mix_format->nAvgBytesPerSec),
               static_cast<unsigned>(mix_format->cbSize), mix_is_float ? "float" : "pcm");

        struct Candidate { int rate; int container_bits; int valid_bits; const char* name; };
        const int preferred = g_preferred_source_rate.load(std::memory_order_acquire);
        const int mix_rate = static_cast<int>(mix_format->nSamplesPerSec);
        int rates[] = {preferred, mix_rate, 192000, 176400, 96000, 88200, 48000, 44100};
        const Candidate layouts[] = {
            {0, 32, 24, "pcm24in32"}, {0, 32, 32, "pcm32"},
            {0, 24, 24, "pcm24packed"}, {0, 16, 16, "pcm16"}
        };
        WAVEFORMATEXTENSIBLE selected{};
        bool have_selected = false;
        b2_log("Exclusive negotiation begin preferredRate=%d mixRate=%d", preferred, mix_rate);
        for (int rate_index = 0; rate_index < static_cast<int>(sizeof(rates) / sizeof(rates[0])) && !have_selected;
             ++rate_index) {
            const int candidate_rate = rates[rate_index];
            if (candidate_rate <= 0) continue;
            bool duplicate = false;
            for (int previous = 0; previous < rate_index; ++previous)
                if (rates[previous] == candidate_rate) duplicate = true;
            if (duplicate) continue;
            for (const Candidate& layout : layouts) {
                WAVEFORMATEXTENSIBLE candidate{};
                candidate.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
                candidate.Format.nChannels = 2;
                candidate.Format.nSamplesPerSec = static_cast<DWORD>(candidate_rate);
                candidate.Format.wBitsPerSample = static_cast<WORD>(layout.container_bits);
                candidate.Format.nBlockAlign = static_cast<WORD>(2 * (layout.container_bits / 8));
                candidate.Format.nAvgBytesPerSec = candidate.Format.nSamplesPerSec * candidate.Format.nBlockAlign;
                candidate.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
                candidate.Samples.wValidBitsPerSample = static_cast<WORD>(layout.valid_bits);
                candidate.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
                candidate.SubFormat = kSubtypePcm;
                const HRESULT probe = client->IsFormatSupported(
                    AUDCLNT_SHAREMODE_EXCLUSIVE, &candidate.Format, nullptr);
                b2_log("Exclusive candidate rate=%d container=%d valid=%d layout=%s probe=0x%08lX",
                       candidate_rate, layout.container_bits, layout.valid_bits, layout.name,
                       static_cast<unsigned long>(probe));
                if (probe == S_OK) { selected = candidate; have_selected = true; break; }
            }
        }

        bool exclusive = false;
        WAVEFORMATEX* active = mix_format;
        // Phase20-B2.3.6.4.23: only the USB Direct mixer always tries
        // exclusive; the normal (speaker) mixer tries it only when the
        // user opted in via WINAPK_SPEAKER_EXCLUSIVE_MS, and otherwise
        // goes straight to the shared fallback below.
        const bool try_exclusive = have_selected &&
            (usb_direct_ || speaker_exclusive_cap_ms() > 0);
        const int effective_cap_ms = usb_direct_ ? usb_endpoint_target_ms()
                                                 : speaker_exclusive_cap_ms();
        if (try_exclusive) {
            REFERENCE_TIME default_period = 0, minimum_period = 0;
            hr = client->GetDevicePeriod(&default_period, &minimum_period);
            if (SUCCEEDED(hr)) {
                const REFERENCE_TIME period = default_period > 0 ? default_period : minimum_period;
                // Phase 20-B2.3.6.4.14: low-spec PCs can experience longer DPC,
                // storage, graphics, or scheduler stalls. Prefer a 250 ms endpoint
                // reservoir, then descend through the proven 4.13 ladder. This is
                // independent from the upper source-ring preroll and preserves the
                // existing shared-mode fallback when the driver rejects every size.
                const REFERENCE_TIME requested_periods[] = {
                    2500000,  // 250 ms
                    2000000,  // 200 ms
                    1600000,  // 160 ms
                    1200000,  // 120 ms
                    800000,   // 80 ms
                    400000,   // 40 ms
                    period > 0 ? period * 2 : 0,
                    period
                };
                REFERENCE_TIME accepted_period = 0;
                REFERENCE_TIME attempted_periods[8]{};
                size_t attempted_count = 0;
                HRESULT last_initialize_hr = E_INVALIDARG;
                const REFERENCE_TIME endpoint_cap =
                    static_cast<REFERENCE_TIME>(effective_cap_ms) * 10000;
                for (REFERENCE_TIME requested : requested_periods) {
                    if (requested <= 0) continue;
                    // Phase20-B2.3.6.4.20: honour WINAPK_USB_ENDPOINT_MS
                    // (the bare device period is always attempted).
                    if (requested > endpoint_cap && requested != period) continue;
                    bool duplicate = false;
                    for (size_t i = 0; i < attempted_count; ++i) {
                        if (attempted_periods[i] == requested) { duplicate = true; break; }
                    }
                    if (duplicate) continue;
                    attempted_periods[attempted_count++] = requested;
                    last_initialize_hr = client->Initialize(
                        AUDCLNT_SHAREMODE_EXCLUSIVE,
                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                        requested, requested, &selected.Format, nullptr);
                    if (SUCCEEDED(last_initialize_hr)) {
                        accepted_period = requested;
                        break;
                    }
                }
                hr = last_initialize_hr;
                if (SUCCEEDED(hr)) {
                    exclusive = true;
                    active = &selected.Format;
                    std::fprintf(stderr,
                        "[I] Phase20-B2.3.6.4.13: exclusive endpoint buffer accepted rate=%lu requestedMs=%.3f container=%d valid=%d capMs=%d\n",
                        static_cast<unsigned long>(selected.Format.nSamplesPerSec),
                        static_cast<double>(accepted_period) / 10000.0,
                        selected.Format.wBitsPerSample,
                        selected.Samples.wValidBitsPerSample,
                        effective_cap_ms);
                    std::fflush(stderr);
                } else {
                    std::fprintf(stderr,
                        "[W] Phase20-B2.3.6.4.13: exclusive endpoint buffer ladder failed hr=0x%08lX; shared fallback follows\n",
                        static_cast<unsigned long>(hr));
                    std::fflush(stderr);
                }
            } else {
                b2_error("GetDevicePeriod", hr);
            }
        } else {
            b2_log("Exclusive negotiation found no supported integer PCM format");
        }

        if (!exclusive) {
            client.reset(); render.reset(); session_volume.reset();
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(&client));
            if (FAILED(hr)) { CoTaskMemFree(mix_format); b2_error("Shared Activate", hr); return false; }
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                    400000, 0, mix_format, nullptr);
            b2_log("Shared Initialize hr=0x%08lX reason=%s", static_cast<unsigned long>(hr),
                   usb_direct_ ? "exclusive-failed"
                               : (speaker_exclusive_cap_ms() > 0 ? "exclusive-failed" : "speaker-default-phase23"));
            if (FAILED(hr)) { CoTaskMemFree(mix_format); return false; }
            active = mix_format;
        }

        const bool output_float = active->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
            (active->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             reinterpret_cast<WAVEFORMATEXTENSIBLE*>(active)->SubFormat == kSubtypeFloat);
        const int channels = active->nChannels;
        const int bits = active->wBitsPerSample;
        const int valid_bits = active->wFormatTag == WAVE_FORMAT_EXTENSIBLE ?
            reinterpret_cast<WAVEFORMATEXTENSIBLE*>(active)->Samples.wValidBitsPerSample : bits;
        const int output_rate = static_cast<int>(active->nSamplesPerSec);
        const int block_align = active->nBlockAlign;
        HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!ready) { CoTaskMemFree(mix_format); return false; }
        UINT32 buffer_frames = 0;
        const HRESULT event_hr = ready ? client->SetEventHandle(ready) : E_HANDLE;
        const HRESULT size_hr = SUCCEEDED(event_hr) ? client->GetBufferSize(&buffer_frames) : event_hr;
        const HRESULT render_hr = SUCCEEDED(size_hr) ? client->GetService(IID_PPV_ARGS(&render)) : size_hr;
        bool ok = ready && SUCCEEDED(event_hr) && SUCCEEDED(size_hr) && SUCCEEDED(render_hr);
        // Phase20-B2.3.6.4.17: remember the live endpoint geometry for
        // output-latency reporting (cleared again if this device fails).
        if (ok) {
            endpoint_buffer_frames_.store(buffer_frames, std::memory_order_relaxed);
            endpoint_rate_.store(output_rate, std::memory_order_relaxed);
        }
        HRESULT volume_hr = S_FALSE;
        if (!exclusive && ok) { volume_hr = client->GetService(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&session_volume)); ok = SUCCEEDED(volume_hr); if(ok&&usb_direct_){const HRESULT vh=session_volume->SetMasterVolume(1.0f,nullptr);const HRESULT mh=session_volume->SetMute(FALSE,nullptr);ok=SUCCEEDED(vh)&&SUCCEEDED(mh);b2_log("Phase20-B2.3.2 unity session volume=1.0 mute=0 volumeHr=0x%08lX muteHr=0x%08lX",static_cast<unsigned long>(vh),static_cast<unsigned long>(mh));} }
        b2_log("Audio Space2 Stability2 actualBuffer frames=%lu durationMs=%.3f exclusive=%d", static_cast<unsigned long>(buffer_frames), output_rate > 0 ? 1000.0 * double(buffer_frames) / double(output_rate) : 0.0, exclusive ? 1 : 0);
        b2_log("Phase20-B2.3.1 services event=0x%08lX bufferSize=0x%08lX frames=%lu render=0x%08lX renderClient=%s volume=0x%08lX",static_cast<unsigned long>(event_hr),static_cast<unsigned long>(size_hr),static_cast<unsigned long>(buffer_frames),static_cast<unsigned long>(render_hr),render?"yes":"no",static_cast<unsigned long>(volume_hr));
        // Prime the exclusive endpoint before Start. Starting an empty hardware buffer
        // makes the first event arrive only after the endpoint has already underrun.
        if (exclusive && ok) {
            BYTE* startup = nullptr;
            const HRESULT get_startup_hr = render->GetBuffer(buffer_frames, &startup);
            HRESULT release_startup_hr = get_startup_hr;
            if (SUCCEEDED(get_startup_hr)) {
                std::memset(startup, 0, static_cast<size_t>(buffer_frames) * block_align);
                release_startup_hr = render->ReleaseBuffer(buffer_frames, AUDCLNT_BUFFERFLAGS_SILENT);
            }
            ok = SUCCEEDED(get_startup_hr) && SUCCEEDED(release_startup_hr);
            b2_log("Exclusive startup prefill frames=%lu bytes=%llu GetBuffer=0x%08lX ReleaseBuffer=0x%08lX",
                   static_cast<unsigned long>(buffer_frames),
                   static_cast<unsigned long long>(buffer_frames) * static_cast<unsigned long long>(block_align),
                   static_cast<unsigned long>(get_startup_hr), static_cast<unsigned long>(release_startup_hr));
        }
        HRESULT start_hr = ok ? client->Start() : E_FAIL;
        if (ok) ok = SUCCEEDED(start_hr);
        b2_log("Phase20-B2.3.1 Start=0x%08lX ok=%d mode=%s rate=%d bufferFrames=%lu",static_cast<unsigned long>(start_hr),ok?1:0,exclusive?"exclusive":"shared",output_rate,static_cast<unsigned long>(buffer_frames));
        rate_ = output_rate;
        fmt_block_align_ = static_cast<unsigned long long>(block_align);
        CoTaskMemFree(mix_format);
        if (!ok) { CloseHandle(ready); return false; }
        b2_log("Render client started mode=%s bufferFrames=%lu rate=%d channels=%d container=%d valid=%d blockAlign=%d subtype=%s",
               exclusive ? "exclusive-event" : "shared-event", static_cast<unsigned long>(buffer_frames),
               output_rate, channels, bits, valid_bits, block_align, output_float ? "float" : "pcm");

        float initial_volume = 1.0f; BOOL initial_mute = FALSE;
        if (session_volume) {
            if (SUCCEEDED(session_volume->GetMasterVolume(&initial_volume))) {
                observed_session_volume_.store(initial_volume, std::memory_order_release);
                requested_session_volume_.store(initial_volume, std::memory_order_relaxed);
            }
            if (SUCCEEDED(session_volume->GetMute(&initial_mute))) {
                observed_session_mute_.store(initial_mute != FALSE, std::memory_order_release);
                requested_session_mute_.store(initial_mute != FALSE, std::memory_order_relaxed);
            }
        }
        std::vector<float> mixbuf;
        int session_poll_countdown = 0;
        unsigned wait_timeouts = 0;
        unsigned exclusive_submit_logs = 0;
        bool first_submit_logged = false;
        uint64_t total_submitted_frames = 0;
        uint64_t total_submitted_bytes = 0;
        int64_t first_submit_nanos = 0;
        int64_t previous_submit_nanos = 0;
        unsigned cadence_samples = 0;
        double cadence_sum_ms = 0.0;
        double cadence_min_ms = 1.0e30;
        double cadence_max_ms = 0.0;
        bool cadence_summary_logged = false;
        if (exclusive) {
            const float persisted_gain = requested_session_volume_.load(std::memory_order_acquire);
            const bool persisted_mute = requested_session_mute_.load(std::memory_order_acquire);
            observed_session_volume_.store(persisted_gain, std::memory_order_release);
            observed_session_mute_.store(persisted_mute, std::memory_order_release);
            session_volume_dirty_.store(false, std::memory_order_release);
            session_mute_dirty_.store(false, std::memory_order_release);
            b2_log("Exclusive volume restore gain=%.6f mute=%d softwareGain=enabled",
                   static_cast<double>(persisted_gain), persisted_mute ? 1 : 0);
            b2_log("Exclusive stream geometry rate=%d channels=%d containerBits=%d validBits=%d blockAlign=%llu expectedBytesPerSec=%llu bufferFrames=%lu bufferBytes=%llu",
                   output_rate, channels, bits, valid_bits, static_cast<unsigned long long>(block_align),
                   static_cast<unsigned long long>(output_rate) * static_cast<unsigned long long>(block_align),
                   static_cast<unsigned long>(buffer_frames),
                   static_cast<unsigned long long>(buffer_frames) * block_align);
        }
        for (;;) {
            const DWORD wait_result = WaitForSingleObject(ready, 200);
            if (wait_result != WAIT_OBJECT_0) {
                if (wait_result == WAIT_TIMEOUT) {
                    if (exclusive && wait_timeouts < 3) {
                        b2_log("Exclusive event wait timeout count=%u", wait_timeouts + 1);
                        ++wait_timeouts;
                    }
                    continue;
                }
                b2_log("Event wait failed result=0x%08lX error=%lu",
                       static_cast<unsigned long>(wait_result), GetLastError());
                break;
            }
            if (session_volume) {
                if (session_volume_dirty_.exchange(false, std::memory_order_acq_rel))
                    session_volume->SetMasterVolume(requested_session_volume_.load(std::memory_order_relaxed), nullptr);
                if (session_mute_dirty_.exchange(false, std::memory_order_acq_rel))
                    session_volume->SetMute(requested_session_mute_.load(std::memory_order_relaxed) ? TRUE : FALSE, nullptr);
                if (--session_poll_countdown <= 0) {
                    float value = 1.0f; BOOL muted = FALSE;
                    if (SUCCEEDED(session_volume->GetMasterVolume(&value))) observed_session_volume_.store(value);
                    if (SUCCEEDED(session_volume->GetMute(&muted))) observed_session_mute_.store(muted != FALSE);
                    session_poll_countdown = 5;
                }
            }
            {
                std::lock_guard<std::mutex> hold(route_lock_);
                if (generation != route_generation_) { client->Stop(); CloseHandle(ready); return true; }
            }
            UINT32 frames = buffer_frames;
            UINT32 padding = 0;
            if (!exclusive) {
                const HRESULT padding_hr = client->GetCurrentPadding(&padding);
                if (FAILED(padding_hr)) { b2_error("GetCurrentPadding", padding_hr); break; }
                frames = buffer_frames - padding;
                if (!frames) continue;
            }
            if (exclusive && exclusive_submit_logs < 3) {
                b2_log("Exclusive event signaled bufferFrames=%lu requestFrames=%lu bytes=%llu submission=%u",
                       static_cast<unsigned long>(buffer_frames), static_cast<unsigned long>(frames),
                       static_cast<unsigned long long>(frames) * block_align, exclusive_submit_logs + 1);
            }
            BYTE* data = nullptr;
            if (FAILED(render->GetBuffer(frames, &data))) break;
            mixbuf.resize(static_cast<size_t>(frames) * 2);
            mix(mixbuf.data(), frames);
            const float software_gain = usb_direct_ ? usb_gain_target_ : (exclusive
                ? (requested_session_mute_.load(std::memory_order_acquire) ? 0.0f
                   : requested_session_volume_.load(std::memory_order_acquire))
                : 1.0f);
            if (usb_direct_) {
                // Phase-coherent two-crossover M/S shaping. Below 110 Hz stays
                // centered; low-mid and mid/high side energy are widened without
                // Haas delay or polarity tricks. Pre-trim preserves limiter headroom.
                const float rate_f = float((std::max)(1, output_rate));
                const float a110 = 1.0f - std::exp(-2.0f * 3.14159265358979323846f * 110.0f / rate_f);
                const float a450 = 1.0f - std::exp(-2.0f * 3.14159265358979323846f * 450.0f / rate_f);
                constexpr float low_mid_gain = 1.12f;
                constexpr float body_mid_gain = 1.08f;
                constexpr float body_side_gain = 1.08f;
                constexpr float upper_side_gain = 1.12f;
                constexpr float headroom = 0.92f;
                for (size_t i = 0; i + 1 < mixbuf.size(); i += 2) {
                    const float l = mixbuf[i], r = mixbuf[i + 1];
                    const float mid = 0.5f * (l + r);
                    const float side = 0.5f * (l - r);
                    usb_space_mid110_ += a110 * (mid - usb_space_mid110_);
                    usb_space_mid450_ += a450 * (mid - usb_space_mid450_);
                    usb_space_side110_ += a110 * (side - usb_space_side110_);
                    usb_space_side450_ += a450 * (side - usb_space_side450_);
                    const float mid_low = usb_space_mid110_;
                    const float mid_body = usb_space_mid450_ - usb_space_mid110_;
                    const float mid_upper = mid - usb_space_mid450_;
                    const float side_low = usb_space_side110_;
                    const float side_body = usb_space_side450_ - usb_space_side110_;
                    const float side_upper = side - usb_space_side450_;
                    const float shaped_mid = mid_low * low_mid_gain + mid_body * body_mid_gain + mid_upper;
                    const float shaped_side = side_low + side_body * body_side_gain + side_upper * upper_side_gain;
                    mixbuf[i] = (shaped_mid + shaped_side) * headroom;
                    mixbuf[i + 1] = (shaped_mid - shaped_side) * headroom;
                }
            }
            if (usb_direct_ && usb_punch_level_ > 0) {
                const float cutoff_hz = 145.0f;
                const float alpha = 1.0f - std::exp(-2.0f * 3.14159265358979323846f * cutoff_hz / float((std::max)(1, output_rate)));
                const float bass_gain = 0.18f * float(usb_punch_level_);
                const float body_gain = 1.0f + 0.025f * float(usb_punch_level_);
                for (size_t i = 0; i + 1 < mixbuf.size(); i += 2) {
                    usb_bass_l_ += alpha * (mixbuf[i] - usb_bass_l_);
                    usb_bass_r_ += alpha * (mixbuf[i + 1] - usb_bass_r_);
                    mixbuf[i] = mixbuf[i] * body_gain + usb_bass_l_ * bass_gain;
                    mixbuf[i + 1] = mixbuf[i + 1] * body_gain + usb_bass_r_ * bass_gain;
                }
            }
            if (usb_direct_) {
                // Loudness1: linked-stereo RMS bus compressor. One gain envelope
                // is applied to both channels, preserving stereo image and punch.
                const float fs = float((std::max)(1, output_rate));
                const float rms_coeff = 1.0f - std::exp(-1.0f / (0.050f * fs));
                const float attack_coeff = 1.0f - std::exp(-1.0f / (0.020f * fs));
                const float release_coeff = 1.0f - std::exp(-1.0f / (0.180f * fs));
                constexpr float threshold_db = -12.0f;
                constexpr float ratio = 1.5f;
                constexpr float max_reduction_db = 2.5f;
                constexpr float makeup_db = 2.0f;
                const float makeup = std::pow(10.0f, makeup_db / 20.0f);
                float block_max_reduction_db = 0.0f;
                double block_reduction_sum_db = 0.0;
                uint64_t block_reduction_samples = 0;
                for (size_t i = 0; i + 1 < mixbuf.size(); i += 2) {
                    const float l = mixbuf[i], r = mixbuf[i + 1];
                    const float energy = 0.5f * (l * l + r * r);
                    usb_comp_rms_sq_ += rms_coeff * (energy - usb_comp_rms_sq_);
                    const float rms = std::sqrt((std::max)(usb_comp_rms_sq_, 1.0e-12f));
                    const float level_db = 20.0f * std::log10(rms);
                    float reduction_db = 0.0f;
                    if (level_db > threshold_db) {
                        reduction_db = (level_db - threshold_db) * (1.0f - 1.0f / ratio);
                        reduction_db = (std::min)(reduction_db, max_reduction_db);
                    }
                    const float target_gain = std::pow(10.0f, -reduction_db / 20.0f);
                    const float coeff = target_gain < usb_comp_gain_ ? attack_coeff : release_coeff;
                    usb_comp_gain_ += coeff * (target_gain - usb_comp_gain_);
                    const float linked_gain = usb_comp_gain_ * makeup;
                    mixbuf[i] = l * linked_gain;
                    mixbuf[i + 1] = r * linked_gain;
                    const float actual_reduction_db = -20.0f * std::log10((std::max)(usb_comp_gain_, 1.0e-6f));
                    block_max_reduction_db = (std::max)(block_max_reduction_db, actual_reduction_db);
                    block_reduction_sum_db += actual_reduction_db;
                    ++block_reduction_samples;
                }
                usb_comp_max_reduction_db_ = (std::max)(usb_comp_max_reduction_db_, block_max_reduction_db);
                usb_comp_reduction_sum_db_ += block_reduction_sum_db;
                usb_comp_reduction_samples_ += block_reduction_samples;
            }
            if(usb_direct_){constexpr float threshold=0.92f,ceiling=0.97f;const bool bypass=usb_gain_db_==0.0f && usb_punch_level_==0;const size_t ramp_samples=static_cast<size_t>((std::max)(1,output_rate/10))*2;double input_peak=0,pre_peak=0,out_peak=0;uint64_t limited=0;for(float& sample:mixbuf){input_peak=(std::max)(input_peak,std::fabs(double(sample)));if(bypass){usb_gain_current_=1.0f;pre_peak=(std::max)(pre_peak,std::fabs(double(sample)));out_peak=(std::max)(out_peak,std::fabs(double(sample)));continue;}const float delta=usb_gain_target_-usb_gain_current_;if(std::fabs(delta)>0.000001f)usb_gain_current_+=delta/float((std::max)(size_t(1),ramp_samples));else usb_gain_current_=usb_gain_target_;const float pre=sample*usb_gain_current_;const float magnitude=std::fabs(pre);pre_peak=(std::max)(pre_peak,double(magnitude));if(magnitude<=threshold)sample=pre;else{++limited;const float range=ceiling-threshold;const float compressed=threshold+range*(1.0f-std::exp(-(magnitude-threshold)/range));sample=std::copysign((std::min)(compressed,ceiling),pre);}out_peak=(std::max)(out_peak,std::fabs(double(sample)));}usb_limited_samples_+=limited;usb_diag_frames_+=frames;if(usb_diag_frames_>=uint64_t((std::max)(1,output_rate)*10)){const double reduction_db=pre_peak>0.0&&out_peak>0.0?20.0*std::log10(out_peak/pre_peak):0.0;b2_log("Phase20-B2.3.3.1 USB Direct gain configuredDb=%.1f target=%.6f current=%.6f inputPeak=%.9f postGainPeak=%.9f outputPeak=%.9f gainReductionDb=%.6f limitedSamples=%llu totalLimited=%llu bypass=%d compressorMaxReductionDb=%.3f compressorAverageReductionDb=%.3f",double(usb_gain_db_),double(usb_gain_target_),double(usb_gain_current_),input_peak,pre_peak,out_peak,reduction_db,(unsigned long long)limited,(unsigned long long)usb_limited_samples_,bypass?1:0,double(usb_comp_max_reduction_db_),usb_comp_reduction_samples_?usb_comp_reduction_sum_db_/double(usb_comp_reduction_samples_):0.0);usb_diag_frames_=0;usb_comp_max_reduction_db_=0.0f;usb_comp_reduction_sum_db_=0.0;usb_comp_reduction_samples_=0;}}
            if (exclusive) {
                observed_session_volume_.store(requested_session_volume_.load(std::memory_order_acquire),
                                               std::memory_order_release);
                observed_session_mute_.store(requested_session_mute_.load(std::memory_order_acquire),
                                             std::memory_order_release);
                session_volume_dirty_.store(false, std::memory_order_release);
                session_mute_dirty_.store(false, std::memory_order_release);
            }
            std::memset(data, 0, static_cast<size_t>(frames) * block_align);
            for (UINT32 f = 0; f < frames; ++f) {
                const float lr[2] = {mixbuf[f * 2], mixbuf[f * 2 + 1]};
                for (int c = 0; c < channels; ++c) {
                    const float unscaled = channels == 1 ? (lr[0] + lr[1]) * 0.5f : (c < 2 ? lr[c] : 0.0f);
                    const float v = std::clamp(unscaled * (usb_direct_ ? 1.0f : software_gain), -1.0f, 1.0f);
                    BYTE* sample = data + static_cast<size_t>(f) * block_align + c * (bits / 8);
                    if (output_float && bits == 32) {
                        float value = v; std::memcpy(sample, &value, sizeof(value));
                    } else if (bits == 16) {
                        const int16_t value = static_cast<int16_t>(std::lrint(v * 32767.0f));
                        std::memcpy(sample, &value, sizeof(value));
                    } else if (bits == 24) {
                        const int32_t value = static_cast<int32_t>(std::lrint(v * 8388607.0f));
                        sample[0] = static_cast<BYTE>(value); sample[1] = static_cast<BYTE>(value >> 8);
                        sample[2] = static_cast<BYTE>(value >> 16);
                    } else if (bits == 32) {
                        int32_t value = valid_bits == 24 ?
                            static_cast<int32_t>(std::lrint(v * 8388607.0f)) << 8 :
                            static_cast<int32_t>(std::lrint(static_cast<double>(v) * 2147483647.0));
                        std::memcpy(sample, &value, sizeof(value));
                    }
                }
            }
            if (exclusive && exclusive_submit_logs < 3) {
                float peak = 0.0f;
                double sum_abs = 0.0;
                uint64_t nonzero = 0;
                for (size_t i = 0; i < mixbuf.size(); ++i) {
                    const float scaled = std::clamp(mixbuf[i] * (usb_direct_ ? 1.0f : software_gain), -1.0f, 1.0f);
                    peak = std::max(peak, std::fabs(scaled));
                    sum_abs += std::fabs(static_cast<double>(scaled));
                    if (scaled != 0.0f) ++nonzero;
                }
                const double mean_abs = mixbuf.empty() ? 0.0 : sum_abs / static_cast<double>(mixbuf.size());
                const size_t payload_bytes = static_cast<size_t>(frames) * block_align;
                uint32_t hash = 2166136261u;
                for (size_t i = 0; i < payload_bytes; ++i) hash = (hash ^ data[i]) * 16777619u;
                b2_log("Exclusive payload diag submission=%u gain=%.6f mute=%d peak=%.9f meanAbs=%.9f nonzeroSamples=%llu/%llu hash=0x%08lX first16=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
                       exclusive_submit_logs + 1, static_cast<double>(software_gain),
                       requested_session_mute_.load(std::memory_order_acquire) ? 1 : 0,
                       static_cast<double>(peak), mean_abs,
                       static_cast<unsigned long long>(nonzero),
                       static_cast<unsigned long long>(mixbuf.size()),
                       static_cast<unsigned long>(hash),
                       payload_bytes > 0 ? data[0] : 0, payload_bytes > 1 ? data[1] : 0,
                       payload_bytes > 2 ? data[2] : 0, payload_bytes > 3 ? data[3] : 0,
                       payload_bytes > 4 ? data[4] : 0, payload_bytes > 5 ? data[5] : 0,
                       payload_bytes > 6 ? data[6] : 0, payload_bytes > 7 ? data[7] : 0,
                       payload_bytes > 8 ? data[8] : 0, payload_bytes > 9 ? data[9] : 0,
                       payload_bytes > 10 ? data[10] : 0, payload_bytes > 11 ? data[11] : 0,
                       payload_bytes > 12 ? data[12] : 0, payload_bytes > 13 ? data[13] : 0,
                       payload_bytes > 14 ? data[14] : 0, payload_bytes > 15 ? data[15] : 0);
            }
            const HRESULT release_hr = render->ReleaseBuffer(frames, 0);
            if (FAILED(release_hr)) { b2_error("ReleaseBuffer", release_hr); break; }
            const int64_t submit_nanos = nano_time();
            if (first_submit_nanos == 0) first_submit_nanos = submit_nanos;
            total_submitted_frames += frames;
            total_submitted_bytes += static_cast<uint64_t>(frames) * block_align;
            if (exclusive && previous_submit_nanos != 0 && !cadence_summary_logged) {
                const double delta_ms = static_cast<double>(submit_nanos - previous_submit_nanos) / 1000000.0;
                cadence_sum_ms += delta_ms;
                cadence_min_ms = std::min(cadence_min_ms, delta_ms);
                cadence_max_ms = std::max(cadence_max_ms, delta_ms);
                ++cadence_samples;
                if (cadence_samples >= 64) {
                    const double elapsed_sec = static_cast<double>(submit_nanos - first_submit_nanos) / 1000000000.0;
                    const double effective_rate = elapsed_sec > 0.0
                        ? static_cast<double>(total_submitted_frames - buffer_frames) / elapsed_sec : 0.0;
                    b2_log("Exclusive cadence summary samples=%u meanMs=%.3f minMs=%.3f maxMs=%.3f effectiveRate=%.3f targetRate=%d totalFrames=%llu totalBytes=%llu",
                           cadence_samples, cadence_sum_ms / cadence_samples, cadence_min_ms, cadence_max_ms,
                           effective_rate, output_rate,
                           static_cast<unsigned long long>(total_submitted_frames),
                           static_cast<unsigned long long>(total_submitted_bytes));
                    cadence_summary_logged = true;
                }
            }
            previous_submit_nanos = submit_nanos;
            if (exclusive && exclusive_submit_logs < 3) {
                b2_log("Exclusive full buffer submitted frames=%lu bytes=%llu bufferFrames=%lu submission=%u",
                       static_cast<unsigned long>(frames),
                       static_cast<unsigned long long>(frames) * block_align,
                       static_cast<unsigned long>(buffer_frames), exclusive_submit_logs + 1);
                ++exclusive_submit_logs;
            }
            if (!first_submit_logged) {
                b2_log("First WASAPI buffer submitted frames=%lu bytes=%llu mode=%s rate=%d container=%d valid=%d",
                       static_cast<unsigned long>(frames), static_cast<unsigned long long>(frames) * block_align,
                       exclusive ? "exclusive-event" : "shared-event", output_rate, bits, valid_bits);
                first_submit_logged = true;
            }
        }
        client->Stop(); CloseHandle(ready); return false;
    }

    // No device: consume a short slice, then retry opening the requested endpoint.
    void run_silent_once() {
        if (rate_.load() <= 0) rate_ = 48000;
        std::vector<float> mixbuf(static_cast<size_t>(rate_.load() / 50) * 2);
        mix(mixbuf.data(), mixbuf.size() / 2);
    }

    const bool usb_direct_;
    std::atomic<bool> render_thread_started_{false};
    float usb_gain_db_=0.0f,usb_gain_target_=1.0f,usb_gain_current_=1.0f;
    DWORD usb_punch_level_=3;
    float usb_bass_l_=0.0f,usb_bass_r_=0.0f;
    float usb_space_mid110_=0.0f,usb_space_mid450_=0.0f;
    float usb_space_side110_=0.0f,usb_space_side450_=0.0f;
    float usb_comp_rms_sq_=0.0f,usb_comp_gain_=1.0f,usb_comp_max_reduction_db_=0.0f;
    double usb_comp_reduction_sum_db_=0.0;
    uint64_t usb_comp_reduction_samples_=0;
    uint64_t usb_limited_samples_=0,usb_diag_frames_=0;
    std::mutex lock_;
    std::vector<std::shared_ptr<Source>> sources_;
    std::atomic<int> rate_{48000};
    unsigned long long fmt_block_align_ = 0;
    mutable std::mutex route_lock_;
    std::string endpoint_id_;
    uint64_t route_generation_ = 0;
    std::atomic<float> requested_session_volume_{1.0f};
    std::atomic<float> observed_session_volume_{1.0f};
    std::atomic<bool> requested_session_mute_{false};
    std::atomic<bool> observed_session_mute_{false};
    std::atomic<bool> session_volume_dirty_{false};
    std::atomic<bool> session_mute_dirty_{false};
    // Phase20-B2.3.6.4.17: live endpoint geometry for latency reporting.
    std::atomic<uint32_t> endpoint_buffer_frames_{0};
    std::atomic<int> endpoint_rate_{0};
};

}  // namespace

bool set_output_endpoint(const std::string& endpoint_id) {
    return Mixer::get().set_endpoint(endpoint_id);
}

std::string output_endpoint() {
    return Mixer::get().endpoint();
}
void set_session_volume(float volume) { Mixer::get().set_session_volume(volume); }
float session_volume() { return Mixer::get().session_volume(); }
void set_session_mute(bool muted) { Mixer::get().set_session_mute(muted); }
bool session_mute() { return Mixer::get().session_mute(); }

// Phase20-B2.3.6.4.21 (link fix in .22): the Visualizer readers must
// live OUTSIDE the anonymous namespace above -- inside it they get
// internal linkage and the exported winapk::audio symbols vanish.
int visualizer_rate() { return viz::rate.load(std::memory_order_relaxed); }

int visualizer_waveform(uint8_t* out, int count) {
    if (!out || count <= 0) return 0;
    std::vector<float> buf(static_cast<size_t>(count));
    const int r = viz::snapshot(buf.data(), count);
    for (int i = 0; i < count; ++i) {
        const float v = std::clamp(buf[static_cast<size_t>(i)], -1.0f, 1.0f);
        out[i] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(128.0f + 127.0f * v)), 0, 255));
    }
    return r;
}

int visualizer_fft(int8_t* out, int count) {
    if (!out || count < 128 || count > 2048 || (count & (count - 1)) != 0) return 0;
    std::vector<float> buf(static_cast<size_t>(count));
    const int r = viz::snapshot(buf.data(), count);
    // Iterative radix-2 DFT (rectangular window, like AOSP's HAL tap).
    const int n = count;
    std::vector<float> re(static_cast<size_t>(n)), im(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) re[static_cast<size_t>(i)] = buf[static_cast<size_t>(i)];
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[static_cast<size_t>(i)], re[static_cast<size_t>(j)]);
            std::swap(im[static_cast<size_t>(i)], im[static_cast<size_t>(j)]);
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const float ang = -2.0f * 3.14159265358979323846f / static_cast<float>(len);
        const float wr = std::cos(ang), wi = std::sin(ang);
        for (int i = 0; i < n; i += len) {
            float cwr = 1.0f, cwi = 0.0f;
            for (int k = 0; k < len / 2; ++k) {
                const size_t a = static_cast<size_t>(i + k);
                const size_t b = a + static_cast<size_t>(len / 2);
                const float vr = re[b] * cwr - im[b] * cwi;
                const float vi = re[b] * cwi + im[b] * cwr;
                re[b] = re[a] - vr; im[b] = im[a] - vi;
                re[a] += vr;        im[a] += vi;
                const float nwr = cwr * wr - cwi * wi;
                cwi = cwr * wi + cwi * wr;
                cwr = nwr;
            }
        }
    }
    // AOSP layout: [0]=Re(DC), [1]=Re(Nyquist), then Re/Im pairs for
    // bins 1..N/2-1. Scale: a full-scale sine peaks at |X|=N/2, and
    // 256/N maps that to ~128, matching the magnitude range apps expect.
    const float scale = 256.0f / static_cast<float>(n);
    auto byte_of = [scale](float v) {
        return static_cast<int8_t>(std::clamp(static_cast<int>(std::lround(v * scale)), -128, 127));
    };
    out[0] = byte_of(re[0]);
    out[1] = byte_of(re[static_cast<size_t>(n / 2)]);
    for (int k = 1; k < n / 2; ++k) {
        out[2 * k] = byte_of(re[static_cast<size_t>(k)]);
        out[2 * k + 1] = byte_of(im[static_cast<size_t>(k)]);
    }
    return r;
}

// ---- AudioTrack streams ------------------------------------------------------------------
class Stream final : public Source {
public:
    Stream(int rate, int channels, int buffer_frames, bool usb_direct_stream=false)
        : base_rate_(rate), rate_(rate), channels_(channels), usb_direct_stream_(usb_direct_stream),
          capacity_(static_cast<size_t>(std::max(buffer_frames, (rate * 2) / 5))) {
        if (rate > 0) g_preferred_source_rate.store(rate, std::memory_order_release);
    }

    bool mix(float* out, size_t frames, int out_rate) override {
        std::lock_guard<std::mutex> hold(lock_);
        last_mix_nanos_ = nano_time();
        if (state_ != kPlaying && !draining_) return true;
        // Do not expose a just-started stream to WASAPI until the producer has
        // accumulated a stable source-side lead. This is a frame-count gate,
        // not an amplitude gate, so intentionally quiet program material is kept.
        if (!draining_ && !preroll_ready_) {
            const size_t queued_frames = buf_.size() / 2;
            const size_t target_frames = std::min(capacity_,
                static_cast<size_t>(std::max(1, rate_ * preroll_target_ms() / 1000)));
            const int64_t waited_nanos = nano_time() - play_nanos_;
            const bool enough_audio = queued_frames >= target_frames;
            const bool bounded_fallback = queued_frames > 0 && waited_nanos >= (usb_direct_stream_ ? 3000000000LL : 150000000LL);
            if (!enough_audio && !bounded_fallback) return true;
            preroll_ready_ = true;
            // Phase20-B2.3.6.4.15: ramp in from silence instead of stepping
            // straight to full amplitude at the preroll boundary.
            fade_in_remaining_ = conceal_fade_frames(out_rate);
            if (!preroll_logged_) {
                b2_log("Source preroll gate opened rate=%d queuedFrames=%llu targetFrames=%llu waitedMs=%.3f reason=%s",
                       rate_, static_cast<unsigned long long>(queued_frames),
                       static_cast<unsigned long long>(target_frames),
                       static_cast<double>(waited_nanos) / 1000000.0,
                       enough_audio ? "target" : "timeout");
                preroll_logged_ = true;
            }
        }
        const double step = rate_ * speed_ / out_rate;
        // Phase20-B2.3.6.4.15: adaptive drift compensation (USB Direct only).
        //
        // The producer clock (the app's nominal sample rate, paced in
        // p20_submit by steady_clock) and the consumer clock (the WASAPI
        // endpoint's hardware clock) are independent crystals. Both are
        // nominally `rate_`, but they differ by tens to hundreds of ppm.
        // With a fixed resampling step the source ring therefore walks
        // monotonically: a faster producer eventually fills every ring in
        // the chain until a whole PCM job is dropped, and a slower one
        // eventually drains the ring into a zero-filled WASAPI buffer.
        // Both events are a waveform discontinuity -- the occasional
        // click/noise heard minutes into otherwise clean playback. Larger
        // buffers and longer prerolls only postpone the walk; nothing in
        // the chain corrected it. Close the loop here, at the final
        // consumer: nudge the effective step by a smoothed, bounded PI
        // correction driven by the ring's deviation from its preroll
        // target (1 s for USB Direct). The proportional term reacts to
        // level error now; the integral term learns the actual crystal
        // error, so the ring settles at its target for any constant ppm
        // offset instead of at an offset proportional to that error.
        // +/-2500 ppm of authority is far above any real crystal error,
        // the equilibrium pitch error equals the crystal error itself
        // (inaudible, and exactly what a real USB DAC's feedback endpoint
        // does), and the correction is smoothed over seconds so it cannot
        // modulate the program.
        double step_eff = step;
        if (usb_direct_stream_ && preroll_ready_ && !draining_ && out_rate > 0) {
            const double target_frames = static_cast<double>(
                std::max(1, rate_ * preroll_target_ms() / 1000));  // the preroll target above
            const double available_frames =
                std::max(0.0, static_cast<double>(buf_.size() / 2) - cursor_);
            const double deviation =
                (available_frames - target_frames) / target_frames;
            const double dt_seconds =
                static_cast<double>(frames) / static_cast<double>(out_rate);
            drift_int_ppm_ = std::clamp(
                drift_int_ppm_ + 50.0 * deviation * dt_seconds, -2500.0, 2500.0);
            const double desired_ppm =
                std::clamp(deviation * 1000.0 + drift_int_ppm_, -2500.0, 2500.0);
            const double alpha = 1.0 - std::exp(-dt_seconds / 2.0);
            drift_ppm_ += alpha * (desired_ppm - drift_ppm_);
            step_eff = step * (1.0 + drift_ppm_ * 1e-6);
            const int64_t drift_now = nano_time();
            if (std::fabs(drift_ppm_) >= 1.0 &&
                drift_now - last_drift_log_nanos_ >= 30000000000LL) {
                last_drift_log_nanos_ = drift_now;
                b2_log("Phase20-B2.3.6.4.15 drift compensation ppm=%.1f desiredPpm=%.1f queuedFrames=%.0f targetFrames=%.0f",
                       drift_ppm_, desired_ppm, available_frames, target_frames);
            }
        }
        // Phase20-B2.3.6.4.15: clickless concealment. When this call will
        // exhaust the input (underrun or end of drain), fade the rendered
        // tail to zero over ~5 ms, and fade back in on the next call that
        // has data again (armed below / at preroll). A hard step to or
        // from zero-filled silence is a broadband click; a 5 ms ramp is a
        // barely audible dip. The fade-out is latched per call from the
        // frames actually renderable, so a producer that tops the ring up
        // just in time never triggers gain modulation.
        const size_t fade_len = conceal_fade_frames(out_rate);
        const double renderable_frames =
            step_eff > 0.0
                ? std::max(0.0, static_cast<double>(buf_.size() / 2) - cursor_) / step_eff
                : 0.0;
        const bool will_exhaust =
            renderable_frames < static_cast<double>(frames) - 0.5;
        size_t rendered = 0;
        for (; rendered < frames; ++rendered) {
            const size_t have = buf_.size() / 2;
            const size_t i = static_cast<size_t>(cursor_);
            if (i >= have) break;  // underrun: no complete input frame remains
            double envelope = 1.0;
            if (fade_in_remaining_ > 0) {
                envelope = std::min(envelope,
                    1.0 - static_cast<double>(fade_in_remaining_) /
                              static_cast<double>(std::max<size_t>(fade_len, 1)));
                --fade_in_remaining_;
            }
            if (will_exhaust && fade_len > 0) {
                const double tail = renderable_frames - static_cast<double>(rendered);
                if (tail < static_cast<double>(fade_len))
                    envelope = std::min(envelope,
                        std::max(0.0, tail / static_cast<double>(fade_len)));
            }
            const float env = static_cast<float>(envelope);
            if (i + 1 < have && step_eff >= 2.0 && step_eff <= 64.0) {
                // Phase20-B2.3.6.4.16: anti-aliased downsampling. The log of
                // 2026-11-12 shows the real configuration is a 192 kHz
                // virtual DAC feeding a 48 kHz exclusive endpoint: step is
                // exactly 4, so the linear path below degenerates into
                // taking every 4th sample (t is always 0) -- decimation
                // with no anti-alias filter, folding everything above
                // 24 kHz back into the audible band. Average the input
                // span one output frame covers instead: a boxcar of length
                // `step` has nulls at multiples of the output rate, which
                // is the correct cheap filter for integer ratios. The
                // sub-frame phase error versus true band-limited SRC is
                // under one input frame (5 us at 192 kHz), inaudible here.
                const size_t span = std::min(static_cast<size_t>(step_eff), have - i);
                const float inv = 1.0f / static_cast<float>(std::max<size_t>(span, 1));
                for (int c = 0; c < 2; ++c) {
                    float sum = 0.0f;
                    for (size_t k = 0; k < span; ++k) sum += buf_[(i + k) * 2 + c];
                    out[rendered * 2 + c] += sum * inv *
                                                   (c == 0 ? left_ : right_) * env;
                }
                cursor_ += step_eff;
            } else if (i + 1 < have) {
                const double t = cursor_ - static_cast<double>(i);
                for (int c = 0; c < 2; ++c) {
                    const float a = buf_[i * 2 + c], b = buf_[(i + 1) * 2 + c];
                    out[rendered * 2 + c] += static_cast<float>(a + (b - a) * t) *
                                                   (c == 0 ? left_ : right_) * env;
                }
                cursor_ += step_eff;
            } else {
                // Linear interpolation normally needs a look-ahead frame. At
                // end-of-stream there is no next frame, and the previous code
                // left this final complete frame queued forever. AudioTrack's
                // playback head then remained one frame behind framesWritten,
                // so clients waiting for exact drain completion never reached
                // their repeat/next-track transition. Render the final frame
                // directly and consume it. The same rule is safe on a genuine
                // streaming underrun: the frame is already due for playback.
                out[rendered * 2] += buf_[i * 2] * left_ * env;
                out[rendered * 2 + 1] += buf_[i * 2 + 1] * right_ * env;
                cursor_ = static_cast<double>(have);
            }
        }
        if (rendered < frames && state_ == kPlaying && !draining_ && preroll_ready_) {
            // Phase20-B2.3.6.4.15: the tail of this call was faded out;
            // fade the next data back in instead of stepping to full level.
            fade_in_remaining_ = fade_len;
            ++underrun_events_; underrun_frames_ += frames-rendered;
            const int64_t now=nano_time(); const int64_t gap=last_producer_nanos_?now-last_producer_nanos_:0;
            max_producer_gap_nanos_=(std::max)(max_producer_gap_nanos_,gap);
            if(now-last_underrun_log_nanos_>=5000000000LL){std::fprintf(stderr,"[I] Win.apk.WASAPI: Phase20-B2.3.6.4.16 underrun events=%llu frames=%llu requestFrames=%llu renderedFrames=%llu queuedFrames=%llu producerGapMs=%.3f producerGapMaxMs=%.3f action=fade+repreroll\n",(unsigned long long)underrun_events_,(unsigned long long)underrun_frames_,(unsigned long long)frames,(unsigned long long)rendered,(unsigned long long)(buf_.size()/2),double(gap)/1000000.0,double(max_producer_gap_nanos_)/1000000.0);std::fflush(stderr);last_underrun_log_nanos_=now;}
            // Phase20-B2.3.6.4.16: re-arm the preroll gate after an underrun.
            // Until now preroll_ready_ stayed true forever, so once the lead
            // was spent the stream kept playing at ~zero lead and every
            // subsequent producer jitter (10-50 ms gaps are routine when the
            // app is backgrounded under load) became another audible skip --
            // the scratched-CD pattern in the 2026-11-12 report. Re-arming
            // with a shorter 250 ms target turns a cluster of micro-skips
            // into one brief, faded pause while the producer's burst
            // catch-up refills the ring. The gate timer restarts here,
            // otherwise the bounded fallback (measured from play()) would
            // reopen the gate instantly with an empty ring.
            preroll_ready_ = false;
            repreroll_ = true;
            play_nanos_ = nano_time();
        }
        const size_t used = std::min(static_cast<size_t>(cursor_), buf_.size() / 2);
        if (used) {
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<ptrdiff_t>(used * 2));
            cursor_ -= static_cast<double>(used);
            played_ += static_cast<int64_t>(used);
            room_.notify_all();
        }
        if (rendered) last_nanos_ = nano_time();
        if (draining_ && buf_.size() / 2 <= 1) {  // stop() lets what was written play out
            draining_ = false;
            buf_.clear();
            cursor_ = 0;
            room_.notify_all();
        }
        return true;
    }

    size_t write(const void* p, size_t count, int encoding, bool blocking) {
        {std::lock_guard<std::mutex> hold(lock_);last_producer_nanos_=nano_time();}
        if (!first_pcm_logged_.exchange(true, std::memory_order_acq_rel)) {
            b2_log("First PCM frames received rate=%d channels=%d encoding=%d samples=%llu blocking=%d",
                   rate_, channels_, encoding, static_cast<unsigned long long>(count), blocking ? 1 : 0);
        }
        const size_t whole = count - count % static_cast<size_t>(channels_);
        size_t done = 0;
        std::unique_lock<std::mutex> hold(lock_);
        while (done < whole) {
            size_t free_frames = capacity_ > buf_.size() / 2 ? capacity_ - buf_.size() / 2 : 0;
            if (free_frames == 0 && state_ == kPlaying) {
                // A healthy host mixer visits mix() continuously. If the WASAPI
                // render path stops visiting us while this MODE_STREAM ring is
                // full, non-blocking Android writers otherwise receive zero
                // forever and can never reach EOS. Advance the already queued
                // audio in real time, exactly like the no-device silent clock,
                // until the host mixer resumes. A 500 ms guard prevents normal
                // scheduling jitter from discarding queued audio.
                const int64_t now = nano_time();
                if (last_mix_nanos_ > 0 && now - last_mix_nanos_ >= 500000000LL) {
                    const int64_t elapsed = now - recovery_nanos_;
                    const size_t queued = buf_.size() / 2;
                    const size_t due = elapsed > 0
                        ? static_cast<size_t>(elapsed * rate_ / 1000000000LL)
                        : 0;
                    const size_t drop = std::min(queued, std::max<size_t>(due, 1));
                    if (drop) {
                        buf_.erase(buf_.begin(), buf_.begin() + static_cast<ptrdiff_t>(drop * 2));
                        cursor_ = 0;
                        played_ += static_cast<int64_t>(drop);
                        last_nanos_ = now;
                        recovery_nanos_ = now;
                        room_.notify_all();
                        free_frames = capacity_ - buf_.size() / 2;
                    }
                }
            }
            if (free_frames == 0) {
                // Only a playing track drains; a blocking write to one that is
                // not (pre-filling before play) takes what fits, as AOSP's does.
                if (!blocking || state_ != kPlaying) break;
                room_.wait_for(hold, std::chrono::milliseconds(100));
                continue;
            }
            const size_t take = std::min(free_frames, (whole - done) / channels_);
            for (size_t f = 0; f < take; ++f) {
                float l = 0, r = 0;
                for (int c = 0; c < std::min(channels_, 2); ++c) {
                    const size_t i = done + f * channels_ + c;
                    float v;
                    if (encoding == 4) v = static_cast<const float*>(p)[i];
                    else if (encoding == 3) v = (static_cast<const uint8_t*>(p)[i] - 128) / 128.0f;
                    else v = static_cast<const int16_t*>(p)[i] / 32768.0f;
                    (c == 0 ? l : r) = v;
                }
                if (channels_ == 1) r = l;
                buf_.push_back(l);
                buf_.push_back(r);
            }
            done += take * channels_;
        }
        return done;
    }

    void play() {
        std::lock_guard<std::mutex> hold(lock_);
        const bool entering_playback = state_ != kPlaying;
        state_ = kPlaying;
        draining_ = false;
        recovery_nanos_ = nano_time();
        if (entering_playback) {
            play_nanos_ = recovery_nanos_;
            preroll_ready_ = false;
            preroll_logged_ = false;
            drift_ppm_ = 0;  // Phase20-B2.3.6.4.15: re-converge per playback
            drift_int_ppm_ = 0;
            repreroll_ = false;  // Phase20-B2.3.6.4.16: first gate uses the full target
            b2_log("Source preroll gate armed rate=%d queuedFrames=%llu targetFrames=%llu timeoutMs=%d",
                   rate_, static_cast<unsigned long long>(buf_.size() / 2),
                   static_cast<unsigned long long>(std::min(capacity_,
                       static_cast<size_t>(std::max(1, rate_ * preroll_target_ms() / 1000)))), usb_direct_stream_ ? 3000 : 150);
        }
        if (last_mix_nanos_ == 0) last_mix_nanos_ = recovery_nanos_;
        room_.notify_all();
    }
    void pause() {
        std::lock_guard<std::mutex> hold(lock_);
        if (state_ == kPlaying) state_ = kPaused;
        room_.notify_all();
    }
    void stop() {
        std::lock_guard<std::mutex> hold(lock_);
        draining_ = state_ == kPlaying && !buf_.empty();
        state_ = kStopped;
        preroll_ready_ = false;
        preroll_logged_ = false;
        played_ = 0;
        recovery_nanos_ = nano_time();
        has_timestamp_reset();
        room_.notify_all();
    }
    void flush() {
        std::lock_guard<std::mutex> hold(lock_);
        if (state_ == kPlaying) return;  // AOSP ignores flush while playing
        buf_.clear();
        cursor_ = 0;
        draining_ = false;
        preroll_ready_ = false;
        preroll_logged_ = false;
        played_ = 0;
        recovery_nanos_ = nano_time();
        has_timestamp_reset();
        room_.notify_all();
    }
    void volume(float l, float r) {
        std::lock_guard<std::mutex> hold(lock_);
        left_ = l;
        right_ = r;
    }
    int state() {
        std::lock_guard<std::mutex> hold(lock_);
        return state_;
    }
    int64_t position() {
        std::lock_guard<std::mutex> hold(lock_);
        return played_;
    }
    bool timestamp(int64_t* frames, int64_t* nanos) {
        std::lock_guard<std::mutex> hold(lock_);
        if (last_nanos_ <= reset_nanos_) return false;
        *frames = played_;
        *nanos = last_nanos_;
        return true;
    }
    bool set_rate(int hz) {
        if (hz <= 0 || hz > base_rate_ * 2) return false;  // AOSP's limit: twice the construction rate
        std::lock_guard<std::mutex> hold(lock_);
        rate_ = hz;
        return true;
    }
    int rate() {
        std::lock_guard<std::mutex> hold(lock_);
        return rate_;
    }
    void set_params(float speed, float pitch) {
        std::lock_guard<std::mutex> hold(lock_);
        speed_ = speed;
        pitch_ = pitch;
    }
    void params(float* speed, float* pitch) {
        std::lock_guard<std::mutex> hold(lock_);
        *speed = speed_;
        *pitch = pitch_;
    }
    void release() {
        std::lock_guard<std::mutex> hold(lock_);
        state_ = kStopped;
        draining_ = false;
        room_.notify_all();
    }
    // Phase20-B2.3.6.4.19: effective preroll target for this stream
    // (USB Direct: WINAPK_USB_PREROLL_MS, re-preroll capped at 250 ms).
    int preroll_target_ms() const {
        if (!usb_direct_stream_) return 30;
        const int full = usb_preroll_target_ms();
        return repreroll_ ? std::min(250, full) : full;
    }
    // Phase20-B2.3.6.4.17: source-ring part of the output latency, in
    // source-rate frames: the ring occupancy, floored at the preroll
    // target while playing (which is where the drift controller holds
    // it). The endpoint-buffer part is added by
    // stream_output_latency_frames(), which knows the owning mixer.
    long long ring_latency_frames() {
        std::lock_guard<std::mutex> hold(lock_);
        long long queued = static_cast<long long>(buf_.size() / 2);
        if (state_ == kPlaying && usb_direct_stream_) {
            const long long target = static_cast<long long>(
                std::max(1, rate_ * preroll_target_ms() / 1000));
            queued = std::max(queued, target);
        }
        return queued;
    }
    bool is_usb_direct() const { return usb_direct_stream_; }
    int stream_rate() const { return rate_; }

private:
    void has_timestamp_reset() { reset_nanos_ = last_nanos_; }

    std::mutex lock_;
    std::condition_variable room_;
    std::deque<float> buf_;  // stereo frames
    double cursor_ = 0;
    std::atomic<bool> first_pcm_logged_{false};
    const int base_rate_;
    int rate_;
    const int channels_;
    const bool usb_direct_stream_;
    const size_t capacity_;  // frames
    int state_ = kStopped;
    bool draining_ = false;
    float left_ = 1, right_ = 1, speed_ = 1, pitch_ = 1;
    int64_t played_ = 0, last_nanos_ = 0, reset_nanos_ = 0;
    int64_t last_mix_nanos_ = 0, recovery_nanos_ = 0, play_nanos_ = 0;
    bool preroll_ready_ = false;
    bool preroll_logged_ = false;
    uint64_t underrun_events_=0,underrun_frames_=0;
    int64_t last_producer_nanos_=0,max_producer_gap_nanos_=0,last_underrun_log_nanos_=0;
    // Phase20-B2.3.6.4.15: drift compensation and clickless concealment.
    static size_t conceal_fade_frames(int out_rate) {
        const size_t rate = static_cast<size_t>(std::max(1, out_rate));
        return std::clamp(rate / 200, size_t(64), size_t(1024));  // ~5 ms
    }
    double drift_ppm_ = 0;
    double drift_int_ppm_ = 0;
    int64_t last_drift_log_nanos_ = 0;
    size_t fade_in_remaining_ = 0;
    bool repreroll_ = false;  // Phase20-B2.3.6.4.16: an underrun has occurred
};

std::shared_ptr<Stream> stream_create(int rate, int channels, int, int buffer_frames) {
    if (rate <= 0 || channels <= 0) return nullptr;
    auto s = std::make_shared<Stream>(rate, channels, buffer_frames);
    Mixer::get().add(s);
    return s;
}
std::shared_ptr<Stream> usb_direct_stream_create(int rate, int channels, int, int buffer_frames) {
    if (rate <= 0 || channels <= 0) return nullptr;
    auto s = std::make_shared<Stream>(rate, channels, buffer_frames, true);
    Mixer::usb_direct().add(s);
    b2_log("Phase20-B2.3.2 dedicated USB Direct stream created rate=%d channels=%d bufferFrames=%d",rate,channels,buffer_frames);
    return s;
}
void stream_release(const std::shared_ptr<Stream>& s) {
    if (!s) return;
    s->release();
    Mixer::get().remove(s.get());
    Mixer::usb_direct().remove(s.get());
}
size_t stream_write(Stream* s, const void* p, size_t count, int encoding, bool blocking) {
    return s->write(p, count, encoding, blocking);
}
void stream_play(Stream* s) { s->play(); }
void stream_pause(Stream* s) { s->pause(); }
void stream_stop(Stream* s) { s->stop(); }
void stream_flush(Stream* s) { s->flush(); }
void stream_set_volume(Stream* s, float l, float r) { s->volume(l, r); }
int stream_state(Stream* s) { return s->state(); }
int64_t stream_position(Stream* s) { return s->position(); }
bool stream_timestamp(Stream* s, int64_t* frames, int64_t* nanos) { return s->timestamp(frames, nanos); }
bool stream_set_rate(Stream* s, int hz) { return s->set_rate(hz); }
int stream_rate(Stream* s) { return s->rate(); }
// Phase20-B2.3.6.4.17: total output latency of a stream in source-rate
// frames (source ring + WASAPI endpoint buffer rescaled). The audio host
// publishes this to the runtime, which delays URB completions by it.
long long stream_output_latency_frames(Stream* s) {
    if (!s) return 0;
    long long total = s->ring_latency_frames();
    Mixer& mixer = s->is_usb_direct() ? Mixer::usb_direct() : Mixer::get();
    const int erate = mixer.endpoint_rate();
    const uint32_t eframes = mixer.endpoint_buffer_frames();
    const int rate = s->stream_rate();
    if (erate > 0 && eframes > 0 && rate > 0)
        total += static_cast<long long>(eframes) * rate / erate;
    return total;
}
void stream_set_params(Stream* s, float speed, float pitch) { s->set_params(speed, pitch); }
void stream_params(Stream* s, float* speed, float* pitch) { s->params(speed, pitch); }

// ---- clip voices -------------------------------------------------------------------------
class Voice final : public Source {
public:
    Voice(std::shared_ptr<const Clip> clip, float l, float r, int loop, float rate, bool paused)
        : clip_(std::move(clip)), left_(l), right_(r), loop_(loop), rate_(rate), paused_(paused) {}

    bool mix(float* out, size_t frames, int out_rate) override {
        std::lock_guard<std::mutex> hold(lock_);
        if (stopped_) return false;
        if (paused_) return true;
        const size_t n = clip_->frames();
        if (n == 0) return false;
        const double step = clip_->rate * static_cast<double>(rate_) / out_rate;
        const float* s = clip_->samples.data();
        for (size_t f = 0; f < frames; ++f) {
            if (cursor_ >= static_cast<double>(n)) {
                if (loop_ == 0) {
                    finished_ = true;
                    return false;
                }
                if (loop_ > 0) --loop_;
                cursor_ -= static_cast<double>(n);
            }
            const size_t i = static_cast<size_t>(cursor_);
            const size_t j = i + 1 < n ? i + 1 : (loop_ != 0 ? 0 : i);
            const float t = static_cast<float>(cursor_ - static_cast<double>(i));
            out[f * 2] += (s[i * 2] + (s[j * 2] - s[i * 2]) * t) * left_;
            out[f * 2 + 1] += (s[i * 2 + 1] + (s[j * 2 + 1] - s[i * 2 + 1]) * t) * right_;
            cursor_ += step;
        }
        return true;
    }

    std::mutex lock_;
    std::shared_ptr<const Clip> clip_;
    double cursor_ = 0;  // in clip frames
    float left_, right_;
    int loop_;
    float rate_;
    bool paused_, stopped_ = false, finished_ = false;
};

std::shared_ptr<Voice> voice_start(std::shared_ptr<const Clip> clip, float l, float r, int loop, float rate,
                                   bool paused) {
    auto v = std::make_shared<Voice>(std::move(clip), l, r, loop, rate, paused);
    Mixer::get().add(v);
    return v;
}
void voice_stop(Voice* v) {
    std::lock_guard<std::mutex> hold(v->lock_);
    v->stopped_ = true;
}
void voice_set_paused(Voice* v, bool paused) {
    std::lock_guard<std::mutex> hold(v->lock_);
    v->paused_ = paused;
}
bool voice_finished(Voice* v) {
    std::lock_guard<std::mutex> hold(v->lock_);
    return v->finished_ || v->stopped_;
}
void voice_set_volume(Voice* v, float l, float r) {
    std::lock_guard<std::mutex> hold(v->lock_);
    v->left_ = l;
    v->right_ = r;
}
void voice_set_loop(Voice* v, int loop) {
    std::lock_guard<std::mutex> hold(v->lock_);
    v->loop_ = loop;
}
void voice_set_rate(Voice* v, float rate) {
    std::lock_guard<std::mutex> hold(v->lock_);
    v->rate_ = std::clamp(rate, 0.125f, 8.0f);
}
void voice_seek(Voice* v, double seconds) {
    std::lock_guard<std::mutex> hold(v->lock_);
    v->cursor_ = std::clamp(seconds * v->clip_->rate, 0.0, static_cast<double>(v->clip_->frames()));
}
double voice_position(Voice* v) {
    std::lock_guard<std::mutex> hold(v->lock_);
    return v->clip_->rate ? std::min(v->cursor_, static_cast<double>(v->clip_->frames())) / v->clip_->rate : 0;
}
bool voice_playing(Voice* v) {
    std::lock_guard<std::mutex> hold(v->lock_);
    return !v->paused_ && !v->stopped_ && !v->finished_;
}

}  // namespace winapk::audio
