// audio_engine.cpp -- see audio_engine.h.

#include "audio_engine.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
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
};

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, spelled out: MinGW's import libraries do
// not all carry it.
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

class Mixer {
public:
    static Mixer& get() {
        static Mixer* m = new Mixer();  // lives for the process; its thread never stops
        return *m;
    }
    void add(std::shared_ptr<Source> s) {
        std::lock_guard<std::mutex> hold(lock_);
        sources_.push_back(std::move(s));
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
        requested_session_volume_.store(std::max(0.0f, std::min(1.0f, value)), std::memory_order_relaxed);
        session_volume_dirty_.store(true, std::memory_order_release);
    }
    float session_volume() const { return observed_session_volume_.load(std::memory_order_acquire); }
    void set_session_mute(bool muted) {
        requested_session_mute_.store(muted, std::memory_order_relaxed);
        session_mute_dirty_.store(true, std::memory_order_release);
    }
    bool session_mute() const { return observed_session_mute_.load(std::memory_order_acquire); }
    std::string endpoint() const {
        std::lock_guard<std::mutex> hold(route_lock_);
        return endpoint_id_;
    }

private:
    Mixer() {
        std::thread([this] { run(); }).detach();
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
    }

    void run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
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

    // The selected render endpoint (or Windows default), shared mode, event driven.
    bool run_device() {
        uint64_t generation;
        std::string endpoint_id;
        {
            std::lock_guard<std::mutex> hold(route_lock_);
            generation = route_generation_;
            endpoint_id = endpoint_id_;
        }
        Com<IMMDeviceEnumerator> enumerator;
        Com<IMMDevice> device;
        Com<IAudioClient> client;
        Com<IAudioRenderClient> render;
        Com<ISimpleAudioVolume> session_volume;
        WAVEFORMATEX* fmt = nullptr;
        HRESULT device_hr = E_FAIL;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
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
        if (FAILED(device_hr) ||
            FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) ||
            FAILED(client->GetMixFormat(&fmt))) {
            return false;
        }
        const bool is_float =
            fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
            (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             reinterpret_cast<WAVEFORMATEXTENSIBLE*>(fmt)->SubFormat == kSubtypeFloat);
        const int channels = fmt->nChannels;
        const int bits = fmt->wBitsPerSample;
        HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        UINT32 buffer_frames = 0;
        const bool ok = (is_float ? bits == 32 : bits == 16) &&
                        SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                                     400000 /* 40 ms */, 0, fmt, nullptr)) &&
                        SUCCEEDED(client->SetEventHandle(ready)) && SUCCEEDED(client->GetBufferSize(&buffer_frames)) &&
                        SUCCEEDED(client->GetService(IID_PPV_ARGS(&render))) &&
                        SUCCEEDED(client->GetService(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&session_volume))) &&
                        SUCCEEDED(client->Start());
        rate_ = static_cast<int>(fmt->nSamplesPerSec);
        CoTaskMemFree(fmt);
        if (!ok) {
            CloseHandle(ready);
            return false;
        }
        float initial_volume = 1.0f;
        BOOL initial_mute = FALSE;
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
        for (;;) {
            WaitForSingleObject(ready, 200);
            if (session_volume) {
                if (session_volume_dirty_.exchange(false, std::memory_order_acq_rel)) {
                    session_volume->SetMasterVolume(requested_session_volume_.load(std::memory_order_relaxed), nullptr);
                }
                if (session_mute_dirty_.exchange(false, std::memory_order_acq_rel)) {
                    session_volume->SetMute(requested_session_mute_.load(std::memory_order_relaxed) ? TRUE : FALSE, nullptr);
                }
                if (--session_poll_countdown <= 0) {
                    float value = 1.0f;
                    BOOL muted = FALSE;
                    if (SUCCEEDED(session_volume->GetMasterVolume(&value)))
                        observed_session_volume_.store(value, std::memory_order_release);
                    if (SUCCEEDED(session_volume->GetMute(&muted)))
                        observed_session_mute_.store(muted != FALSE, std::memory_order_release);
                    session_poll_countdown = 5;
                }
            }
            {
                std::lock_guard<std::mutex> hold(route_lock_);
                if (generation != route_generation_) {
                    client->Stop();
                    CloseHandle(ready);
                    return true;
                }
            }
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) break;  // device gone
            const UINT32 frames = buffer_frames - padding;
            if (frames == 0) continue;
            BYTE* data = nullptr;
            if (FAILED(render->GetBuffer(frames, &data))) break;
            mixbuf.resize(frames * 2);
            mix(mixbuf.data(), frames);
            for (UINT32 f = 0; f < frames; ++f) {
                const float l = mixbuf[f * 2], r = mixbuf[f * 2 + 1];
                for (int c = 0; c < channels; ++c) {
                    const float v = channels == 1 ? (l + r) * 0.5f : c == 0 ? l : c == 1 ? r : 0.0f;
                    if (is_float) {
                        reinterpret_cast<float*>(data)[f * channels + c] = v;
                    } else {
                        reinterpret_cast<int16_t*>(data)[f * channels + c] = static_cast<int16_t>(v * 32767.0f);
                    }
                }
            }
            render->ReleaseBuffer(frames, 0);
        }
        client->Stop();
        CloseHandle(ready);
        return false;  // the device went away: keep time silently from here on
    }

    // No device: consume a short slice, then retry opening the requested endpoint.
    void run_silent_once() {
        if (rate_.load() <= 0) rate_ = 48000;
        std::vector<float> mixbuf(static_cast<size_t>(rate_.load() / 50) * 2);
        mix(mixbuf.data(), mixbuf.size() / 2);
    }

    std::mutex lock_;
    std::vector<std::shared_ptr<Source>> sources_;
    std::atomic<int> rate_{48000};
    mutable std::mutex route_lock_;
    std::string endpoint_id_;
    uint64_t route_generation_ = 0;
    std::atomic<float> requested_session_volume_{1.0f};
    std::atomic<float> observed_session_volume_{1.0f};
    std::atomic<bool> requested_session_mute_{false};
    std::atomic<bool> observed_session_mute_{false};
    std::atomic<bool> session_volume_dirty_{false};
    std::atomic<bool> session_mute_dirty_{false};
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

// ---- AudioTrack streams ------------------------------------------------------------------
class Stream final : public Source {
public:
    Stream(int rate, int channels, int buffer_frames)
        : base_rate_(rate), rate_(rate), channels_(channels),
          capacity_(static_cast<size_t>(std::max(buffer_frames, rate / 5))) {}

    bool mix(float* out, size_t frames, int out_rate) override {
        std::lock_guard<std::mutex> hold(lock_);
        last_mix_nanos_ = nano_time();
        if (state_ != kPlaying && !draining_) return true;
        const double step = rate_ * speed_ / out_rate;
        size_t rendered = 0;
        for (; rendered < frames; ++rendered) {
            const size_t have = buf_.size() / 2;
            const size_t i = static_cast<size_t>(cursor_);
            if (i >= have) break;  // underrun: no complete input frame remains
            if (i + 1 < have) {
                const double t = cursor_ - static_cast<double>(i);
                for (int c = 0; c < 2; ++c) {
                    const float a = buf_[i * 2 + c], b = buf_[(i + 1) * 2 + c];
                    out[rendered * 2 + c] += static_cast<float>(a + (b - a) * t) *
                                                   (c == 0 ? left_ : right_);
                }
                cursor_ += step;
            } else {
                // Linear interpolation normally needs a look-ahead frame. At
                // end-of-stream there is no next frame, and the previous code
                // left this final complete frame queued forever. AudioTrack's
                // playback head then remained one frame behind framesWritten,
                // so clients waiting for exact drain completion never reached
                // their repeat/next-track transition. Render the final frame
                // directly and consume it. The same rule is safe on a genuine
                // streaming underrun: the frame is already due for playback.
                out[rendered * 2] += buf_[i * 2] * left_;
                out[rendered * 2 + 1] += buf_[i * 2 + 1] * right_;
                cursor_ = static_cast<double>(have);
            }
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
        state_ = kPlaying;
        draining_ = false;
        recovery_nanos_ = nano_time();
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

private:
    void has_timestamp_reset() { reset_nanos_ = last_nanos_; }

    std::mutex lock_;
    std::condition_variable room_;
    std::deque<float> buf_;  // stereo frames
    double cursor_ = 0;
    const int base_rate_;
    int rate_;
    const int channels_;
    const size_t capacity_;  // frames
    int state_ = kStopped;
    bool draining_ = false;
    float left_ = 1, right_ = 1, speed_ = 1, pitch_ = 1;
    int64_t played_ = 0, last_nanos_ = 0, reset_nanos_ = 0;
    int64_t last_mix_nanos_ = 0, recovery_nanos_ = 0;
};

std::shared_ptr<Stream> stream_create(int rate, int channels, int, int buffer_frames) {
    if (rate <= 0 || channels <= 0) return nullptr;
    auto s = std::make_shared<Stream>(rate, channels, buffer_frames);
    Mixer::get().add(s);
    return s;
}
void stream_release(const std::shared_ptr<Stream>& s) {
    s->release();
    Mixer::get().remove(s.get());
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
