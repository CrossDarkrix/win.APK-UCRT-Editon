// stdio.cpp -- file descriptors, FILE streams, directories and stat, for guest
// code.
//
// Descriptors are the UCRT's own (_wopen and friends), so 0, 1 and 2 are the
// process's standard streams as on Android. What differs is translated here:
// Linux's open flags and errno numbers, UTF-8 paths (Windows' native APIs take
// UTF-16), and the layouts of Bionic's struct stat and struct dirent on
// x86_64. A guest FILE* is the CRT's FILE*; guest code never looks inside one.
// The standard streams are also exported the two ways Bionic does: the
// variables stdin/stdout/stderr (API 23+) and the older __sF array, whose
// elements stand for the same three streams.

#include "bionic_internal.h"
#include "../../host/audio_engine.h"
#include "../../host/audio_ipc_shared.h"
#include "winapk/linker.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#include <cstdint>
#include <deque>
#include <condition_variable>
#include <chrono>
#include <cmath>
#include <memory>
#include <unordered_set>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <bcrypt.h>

namespace winapk::bionic {

namespace {

std::string narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

int fail_crt() {
    set_errno_from_crt(errno);
    return -1;
}

// ---- the standard streams ----------------------------------------------------
// Bionic's __sF: three FILE structs, 152 bytes each on LP64. Only their
// addresses matter; they are mapped back to the CRT's streams.
alignas(16) char g_sF[3 * 152];
FILE* g_stdin_var = stdin;
FILE* g_stdout_var = stdout;
FILE* g_stderr_var = stderr;

FILE* host_file(void* f) {
    const auto* p = static_cast<const char*>(f);
    if (p >= g_sF && p < g_sF + sizeof(g_sF)) {
        const ptrdiff_t i = (p - g_sF) / 152;
        return i == 0 ? stdin : i == 1 ? stdout : stderr;
    }
    return static_cast<FILE*>(f);
}

// ---- file descriptors -----------------------------------------------------------
// Linux's open(2) flags (x86_64 values).
constexpr int kOWrOnly = 01, kORdWr = 02, kOCreat = 0100, kOExcl = 0200, kOTrunc = 01000, kOAppend = 02000,
              kODirectory = 0200000, kOCloexec = 02000000;

int crt_open_flags(int f) {
    int c = _O_BINARY;
    c |= (f & kORdWr) ? _O_RDWR : (f & kOWrOnly) ? _O_WRONLY : _O_RDONLY;
    if (f & kOCreat) c |= _O_CREAT;
    if (f & kOExcl) c |= _O_EXCL;
    if (f & kOTrunc) c |= _O_TRUNC;
    if (f & kOAppend) c |= _O_APPEND;
    if (f & kOCloexec) c |= _O_NOINHERIT;
    return c;
}

// /dev/urandom and /dev/random: a character device that reads as an endless
// stream from the system RNG. Windows has no such file, so open() failed and
// native code that CHECKs the descriptor (folly's Random.cpp, in WhatsApp
// right after registration) aborted the process.
class RandomDevice : public FdObject {
public:
    int64_t read(void* buf, size_t n) override {
        if (n == 0) return 0;
        if (n > 0x7fffffff) n = 0x7fffffff;
        if (BCryptGenRandom(nullptr, static_cast<PUCHAR>(buf), static_cast<ULONG>(n),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            *guest_errno() = 5;  // EIO
            return -1;
        }
        return static_cast<int64_t>(n);
    }
    int64_t write(const void*, size_t n) override { return static_cast<int64_t>(n); }  // entropy is discarded
    short ready(short events) override { return static_cast<short>(events & (1 | 4)); }  // POLLIN | POLLOUT
    unsigned mode() const override { return 0020000; }  // S_IFCHR
};

// /proc/self/maps (and /proc/<our pid>/maps): synthesized from the guest
// linker on every open, as the kernel does, into a delete-on-close temporary
// file, so read, lseek, fstat and stdio all behave as on a regular file.
bool is_proc_maps(const char* path) {
    if (!path) return false;
    if (std::strcmp(path, "/proc/self/maps") == 0 || std::strcmp(path, "/proc/thread-self/maps") == 0) return true;
    char own[48];
    std::snprintf(own, sizeof own, "/proc/%lu/maps", static_cast<unsigned long>(GetCurrentProcessId()));
    return std::strcmp(path, own) == 0;
}

// A delete-on-close temporary file holding `content`, as a CRT descriptor
// positioned at 0; -1 with errno set.
int open_temp_with(const std::string& content) {
    wchar_t dir[MAX_PATH + 1], name[MAX_PATH + 1];
    if (!GetTempPathW(MAX_PATH, dir) || !GetTempFileNameW(dir, L"prc", 0, name)) {
        *guest_errno() = 5;  // EIO
        return -1;
    }
    const int fd = _wopen(name, _O_RDWR | _O_BINARY | _O_TEMPORARY);
    if (fd < 0) {
        set_errno_from_crt(errno);
        DeleteFileW(name);
        return -1;
    }
    if (!content.empty() && _write(fd, content.data(), static_cast<unsigned>(content.size())) < 0) {
        set_errno_from_crt(errno);
        _close(fd);
        return -1;
    }
    _lseek(fd, 0, SEEK_SET);
    return fd;
}

std::string proc_maps_text() {
    GuestLinker* linker = GuestLinker::process();
    return linker ? linker->proc_maps() : std::string();
}

bool is_random_device(const char* path) {
    return path && (std::strcmp(path, "/dev/urandom") == 0 || std::strcmp(path, "/dev/random") == 0);
}

// open() is variadic (its mode is the third argument only with O_CREAT), but a
// variadic int travels in the same register as a fixed one.
WINAPK_GUEST_ABI int g_open(const char* path, int flags, unsigned mode) {
    if (is_random_device(path)) return fd_install(std::make_shared<RandomDevice>());
    if (is_proc_maps(path)) return open_temp_with(proc_maps_text());
    const std::wstring w = host_path(path);
    if (flags & kODirectory) {
        const DWORD a = GetFileAttributesW(w.c_str());
        if (a == INVALID_FILE_ATTRIBUTES) {
            set_errno_from_win32(GetLastError());
            return -1;
        }
        if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
            *guest_errno() = kENOTDIR;
            return -1;
        }
        // A directory descriptor (for fsync or *at() calls): a handle wrapped
        // in a CRT descriptor.
        HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            set_errno_from_win32(GetLastError());
            return -1;
        }
        return _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_RDONLY);
    }
    const int fd = _wopen(w.c_str(), crt_open_flags(flags), (mode & 0200) ? (_S_IREAD | _S_IWRITE) : _S_IREAD);
    return fd < 0 ? fail_crt() : fd;
}
WINAPK_GUEST_ABI int g_open_2(const char* path, int flags) { return g_open(path, flags, 0); }
WINAPK_GUEST_ABI int g_creat(const char* path, unsigned mode) { return g_open(path, kOWrOnly | kOCreat | kOTrunc, mode); }
// Pipes, eventfds, epoll and sockets are objects (fd.cpp); each descriptor
// call checks for one first.
WINAPK_GUEST_ABI int g_close(int fd) {
    if (fd_close(fd)) return 0;
    return _close(fd) < 0 ? fail_crt() : 0;
}
WINAPK_GUEST_ABI int64_t g_read(int fd, void* buf, size_t n) {
    if (auto o = fd_object(fd)) return o->read(buf, n);
    const int r = _read(fd, buf, static_cast<unsigned>(n > 0x7fffffff ? 0x7fffffff : n));
    return r < 0 ? fail_crt() : r;
}
WINAPK_GUEST_ABI int64_t g_write(int fd, const void* buf, size_t n) {
    if (auto o = fd_object(fd)) return o->write(buf, n);
    const int r = _write(fd, buf, static_cast<unsigned>(n > 0x7fffffff ? 0x7fffffff : n));
    return r < 0 ? fail_crt() : r;
}
WINAPK_GUEST_ABI int64_t g_lseek(int fd, int64_t off, int whence) {
    if (fd_object(fd)) {
        *guest_errno() = kESPIPE;
        return -1;
    }
    const int64_t r = _lseeki64(fd, off, whence);
    return r < 0 ? fail_crt() : r;
}
// pread/pwrite at an offset without moving the file position.
WINAPK_GUEST_ABI int64_t g_pread(int fd, void* buf, size_t n, int64_t off) {
    const int64_t at = _lseeki64(fd, 0, SEEK_CUR);
    if (at < 0 || _lseeki64(fd, off, SEEK_SET) < 0) return fail_crt();
    const int r = _read(fd, buf, static_cast<unsigned>(n));
    const int e = errno;
    _lseeki64(fd, at, SEEK_SET);
    if (r < 0) {
        set_errno_from_crt(e);
        return -1;
    }
    return r;
}
WINAPK_GUEST_ABI int64_t g_pwrite(int fd, const void* buf, size_t n, int64_t off) {
    const int64_t at = _lseeki64(fd, 0, SEEK_CUR);
    if (at < 0 || _lseeki64(fd, off, SEEK_SET) < 0) return fail_crt();
    const int r = _write(fd, buf, static_cast<unsigned>(n));
    const int e = errno;
    _lseeki64(fd, at, SEEK_SET);
    if (r < 0) {
        set_errno_from_crt(e);
        return -1;
    }
    return r;
}
WINAPK_GUEST_ABI int64_t g_read_chk(int fd, void* buf, size_t n, size_t bn) {
    if (n > bn) {
        std::fprintf(stderr, "[win.apk] guest FORTIFY: read: prevented buffer overflow\n");
        std::abort();
    }
    return g_read(fd, buf, n);
}
WINAPK_GUEST_ABI int64_t g_write_chk(int fd, const void* buf, size_t n, size_t bn) {
    if (n > bn) {
        std::fprintf(stderr, "[win.apk] guest FORTIFY: write: prevented buffer over-read\n");
        std::abort();
    }
    return g_write(fd, buf, n);
}
WINAPK_GUEST_ABI int g_dup(int fd) {
    const int o = fd_dup(fd, -1);
    if (o != -2) return o;
    const int r = _dup(fd);
    return r < 0 ? fail_crt() : r;
}
WINAPK_GUEST_ABI int g_dup2(int fd, int to) {
    const int o = fd_dup(fd, to);
    if (o != -2) return o;
    fd_close(to);  // a file replacing an object
    return _dup2(fd, to) < 0 ? fail_crt() : to;
}
WINAPK_GUEST_ABI int g_dup3(int fd, int to, int) { return g_dup2(fd, to); }
WINAPK_GUEST_ABI int g_fsync(int fd) { return _commit(fd) < 0 ? fail_crt() : 0; }
WINAPK_GUEST_ABI int g_ftruncate(int fd, int64_t len) {
    const errno_t e = _chsize_s(fd, len);
    if (e) {
        set_errno_from_crt(e);
        return -1;
    }
    return 0;
}
WINAPK_GUEST_ABI int g_isatty(int fd) { return !fd_object(fd) && _isatty(fd) ? 1 : 0; }
// fcntl: descriptor flags are accepted and ignored (no fork, so close-on-exec
// has nothing to protect); F_GETFL answers read-write.
WINAPK_GUEST_ABI int g_fcntl(int fd, int cmd, int64_t arg) {
    if (auto o = fd_object(fd)) {
        // O_NONBLOCK is the status flag that matters for objects.
        if (cmd == 3) return kORdWr | (o->nonblocking ? 04000 : 0);  // F_GETFL
        if (cmd == 4) {                                              // F_SETFL
            o->set_nonblocking((arg & 04000) != 0);
            return 0;
        }
    }
    switch (cmd) {
        case 0: return g_dup(fd);                      // F_DUPFD
        case 1030: return g_dup(fd);                   // F_DUPFD_CLOEXEC
        case 1: case 2: case 4: return 0;              // F_GETFD, F_SETFD, F_SETFL
        case 3: return kORdWr;                         // F_GETFL
        // Locks are always granted, so F_GETLK answers that nothing holds
        // one: l_type, the struct flock's first field on both ABIs, is
        // F_UNLCK. Left as the caller set it, SQLite's own VFS (an app's
        // bundled libsqlite3) saw a lock it never got and failed with
        // SQLITE_PROTOCOL ("locking protocol" in X).
        case 5:                                        // F_GETLK
            if (arg) *reinterpret_cast<int16_t*>(arg) = 2;
            return 0;
        case 6: case 7: return 0;                      // F_SETLK, F_SETLKW
        default: (void)arg; *guest_errno() = kEINVAL; return -1;
    }
}

// Phase 20-B2.2: x86_64 usbdevfs URB ABI, failure-stop, cancellation and real-time pacing.
namespace {
constexpr int kP20FdBase=0x5A20,kP20FdLimit=0x7A20; int g_p20_active_fd=-1; uint64_t g_p20_session_id=0;
constexpr uint32_t kP20Submit=0x8038550au,kP20Discard=0x0000550bu,kP20Reap=0x4008550cu,kP20ReapNd=0x4008550du;
constexpr uint32_t kP20GetSpeed=0x0000551fu,kP20SetInterface=0x80085504u,kP20Claim=0x8004550fu,kP20Release=0x80045510u;
struct P20Iso { uint32_t length,actual_length; int32_t status; };
struct P20Urb { uint8_t type,endpoint; int32_t status; uint32_t flags; void* buffer; int32_t buffer_length,actual_length,start_frame,number_of_packets,error_count; uint32_t signr; void* usercontext; };
static_assert(sizeof(P20Iso)==12,"usbdevfs_iso_packet_desc ABI");
static_assert(sizeof(P20Urb)==56,"x86_64 usbdevfs_urb ABI");
static_assert(offsetof(P20Urb,status)==4&&offsetof(P20Urb,flags)==8&&offsetof(P20Urb,buffer)==16&&offsetof(P20Urb,buffer_length)==24&&offsetof(P20Urb,number_of_packets)==36&&offsetof(P20Urb,usercontext)==48,"x86_64 usbdevfs_urb offsets");
struct P20Pending { P20Urb* urb; void* submit_urb; void* submit_buffer; std::chrono::steady_clock::time_point ready; uint64_t generation,submit_id,pool_epoch; };

// Phase 20-B2.3.5: the virtual USB DAC usbfs caller is a separate real-time
// producer from the WASAPI mixer. Configure only threads that enter p20_ioctl;
// ordinary UCRT/JVM/UI work keeps its existing scheduling policy.
struct P20UsbRealtimeThreadState {
    bool configured = false;
    bool power_throttling_off = false;
    DWORD power_error = ERROR_SUCCESS;
    HANDLE mmcss = nullptr;
    DWORD mmcss_index = 0;
    bool mmcss_critical = false;
    bool fallback_time_critical = false;
    bool priority_boost_disabled = false;
};

P20UsbRealtimeThreadState& p20_usb_realtime_thread() {
    thread_local P20UsbRealtimeThreadState state;
    if (state.configured) return state;
    state.configured = true;

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    using SetThreadInformationFn = BOOL (WINAPI*)(HANDLE, THREAD_INFORMATION_CLASS, LPVOID, DWORD);
    auto set_thread_info = kernel32 ? reinterpret_cast<SetThreadInformationFn>(
        GetProcAddress(kernel32, "SetThreadInformation")) : nullptr;
    if (set_thread_info) {
        THREAD_POWER_THROTTLING_STATE power{};
        power.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
        power.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
        power.StateMask = 0;
        SetLastError(ERROR_SUCCESS);
        state.power_throttling_off = set_thread_info(GetCurrentThread(), ThreadPowerThrottling,
                                                      &power, sizeof(power)) != FALSE;
        state.power_error = state.power_throttling_off ? ERROR_SUCCESS : GetLastError();
    } else {
        state.power_error = ERROR_CALL_NOT_IMPLEMENTED;
    }

    HMODULE avrt = LoadLibraryW(L"avrt.dll");
    using AvSetFn = HANDLE (WINAPI*)(LPCWSTR, LPDWORD);
    using AvPriorityFn = BOOL (WINAPI*)(HANDLE, int);
    auto av_set = avrt ? reinterpret_cast<AvSetFn>(
        GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW")) : nullptr;
    auto av_priority = avrt ? reinterpret_cast<AvPriorityFn>(
        GetProcAddress(avrt, "AvSetMmThreadPriority")) : nullptr;
    state.mmcss = av_set ? av_set(L"Pro Audio", &state.mmcss_index) : nullptr;
    state.mmcss_critical = state.mmcss && av_priority && av_priority(state.mmcss, 2) != FALSE;
    if (!state.mmcss) {
        state.fallback_time_critical =
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) != FALSE;
    }
    // Do not let transient waits permanently lower the virtual-DAC submit/reap
    // thread. MMCSS remains the primary scheduler; this only disables dynamic
    // priority boosting/deboosting for this dedicated path.
    state.priority_boost_disabled =
        SetThreadPriorityBoost(GetCurrentThread(), TRUE) != FALSE;

    std::fprintf(stderr,
        "[I] Phase20-B2.3.5: virtual DAC realtime thread tid=%lu efficiencyModeOff=%d powerError=%lu mmcss=%d taskIndex=%lu mmcssCritical=%d fallbackTimeCritical=%d priorityBoostDisabled=%d threadPriority=%d processPriority=0x%08lX\n",
        static_cast<unsigned long>(GetCurrentThreadId()),
        state.power_throttling_off ? 1 : 0,
        static_cast<unsigned long>(state.power_error),
        state.mmcss ? 1 : 0,
        static_cast<unsigned long>(state.mmcss_index),
        state.mmcss_critical ? 1 : 0,
        state.fallback_time_critical ? 1 : 0,
        state.priority_boost_disabled ? 1 : 0,
        GetThreadPriority(GetCurrentThread()),
        static_cast<unsigned long>(GetPriorityClass(GetCurrentProcess())));
    std::fflush(stderr);
    return state;
}
std::mutex g_p20_lock; std::condition_variable g_p20_cv; std::deque<P20Pending> g_p20_pending; std::shared_ptr<winapk::audio::Stream> g_p20_stream;
std::unordered_set<P20Urb*> g_p20_current_pool,g_p20_retired_pool;
bool g_p20_pool_building=true,g_p20_alt_zero_boundary=false,g_p20_reap_gate=false;
uint64_t g_p20_pool_epoch=1; size_t g_p20_gate_submits=0; std::chrono::steady_clock::time_point g_p20_gate_until{};
uint64_t g_p20_bytes=0,g_p20_urbs=0,g_p20_generation=1,g_p20_submit_id=0,g_p20_stale_dropped=0,g_p20_duplicate_submit=0,g_p20_silent_urbs=0,g_p20_nonzero_urbs=0,g_p20_nonzero_frames=0,g_p20_pool_recoveries=0;
uint64_t g_p20_silence_fill_urbs=0,g_p20_silence_fill_frames=0,g_p20_silence_phase=0;
// Phase 20-B2.3.7 lock acquisition telemetry. Updated only while g_p20_lock is
// held, so no extra atomic or secondary mutex is introduced on the audio path.
uint64_t g_p20_lock_wait_samples=0,g_p20_lock_wait_sum_us=0,g_p20_lock_wait_max_us=0;
uint64_t g_p20_lock_wait_over_500us=0,g_p20_lock_wait_over_1ms=0,g_p20_lock_wait_over_2ms=0,g_p20_lock_wait_over_5ms=0;
uint64_t g_p20_lock_wait_slow_logs=0;
std::atomic<uint64_t> g_p20_pcm_generation{1};
std::atomic<uint64_t> g_p20_lock_held_samples{0},g_p20_lock_held_sum_us{0},g_p20_lock_held_max_us{0};
std::atomic<uint64_t> g_p20_async_jobs{0},g_p20_async_written_samples{0},g_p20_async_write_sum_us{0},g_p20_async_write_max_us{0};
std::atomic<uint64_t> g_p20_async_stale_jobs{0},g_p20_async_queue_drops{0};
std::atomic<uint64_t> g_p20_async_queue_depth_max{0},g_p20_producer_gap_max_us{0},g_p20_producer_gap_over10{0},g_p20_producer_gap_over25{0},g_p20_producer_gap_over50{0};
std::atomic<int64_t> g_p20_producer_last_us{0};
bool g_p20_failed=false,g_p20_active=true,g_p20_first_nonzero_logged=false; uint32_t g_p20_alt=0; int g_p20_rate=96000; std::chrono::steady_clock::time_point g_p20_timeline{};

bool p20_diag_enabled(){
 static const bool enabled=[](){char v[8]{};const DWORD n=GetEnvironmentVariableA("WINAPK_PHASE20_VERBOSE",v,static_cast<DWORD>(sizeof(v)));return n!=0&&n<sizeof(v)&&v[0]!='0';}();
 return enabled;
}

struct P20HeldTimer {
 std::chrono::steady_clock::time_point acquired;
 explicit P20HeldTimer(std::chrono::steady_clock::time_point t):acquired(t){}
 ~P20HeldTimer(){
  const auto raw=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-acquired).count();
  const uint64_t us=static_cast<uint64_t>((std::max)(int64_t(0),raw));
  g_p20_lock_held_samples.fetch_add(1,std::memory_order_relaxed);
  g_p20_lock_held_sum_us.fetch_add(us,std::memory_order_relaxed);
  uint64_t old=g_p20_lock_held_max_us.load(std::memory_order_relaxed);
  while(old<us&&!g_p20_lock_held_max_us.compare_exchange_weak(old,us,std::memory_order_relaxed)){}
 }
};

struct P20AudioHostClient {
 HANDLE map=nullptr,data=nullptr,space=nullptr,process=nullptr; winapk::audio_ipc::Header* h=nullptr; std::wstring name; std::mutex start_lock;
 bool start(){std::lock_guard<std::mutex> guard(start_lock);if(h&&h->host_ready)return true;DWORD pid=GetCurrentProcessId();name=L"Local\\WinApkUcrtAudio-"+std::to_wstring(pid);map=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,DWORD(winapk::audio_ipc::mapping_bytes()),name.c_str());if(!map)return false;h=static_cast<winapk::audio_ipc::Header*>(MapViewOfFile(map,FILE_MAP_ALL_ACCESS,0,0,winapk::audio_ipc::mapping_bytes()));if(!h)return false;std::memset(h,0,sizeof(*h));h->magic=winapk::audio_ipc::kMagic;h->abi=winapk::audio_ipc::kAbi;h->header_bytes=sizeof(*h);h->frame_bytes=8;h->capacity_frames=winapk::audio_ipc::kCapacityFrames;h->parent_pid=pid;h->session_id=1;
  swprintf(h->data_event,96,L"Local\\WinApkUcrtAudioData-%lu",pid);swprintf(h->space_event,96,L"Local\\WinApkUcrtAudioSpace-%lu",pid);data=CreateEventW(nullptr,FALSE,FALSE,h->data_event);space=CreateEventW(nullptr,FALSE,FALSE,h->space_event);
  wchar_t exe[MAX_PATH]{};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring p(exe);
  const auto pos = p.find_last_of(L"\\/");
  p = (pos == std::wstring::npos ? L"" : p.substr(0, pos + 1)) + L"winapk-audio-host.exe";
  std::wstring cmd;
  cmd.reserve(p.size() + name.size() + 48);
  cmd.push_back(L'"');
  cmd += p;
  cmd += L"\" \"";
  cmd += name;
  cmd.push_back(L'"');
  cmd.push_back(L' ');
  cmd += std::to_wstring(pid);
  // Phase20-B2.3.6.4.16: give the audio host a log of its own. Until now it
  // was created with bInheritHandles=FALSE and no standard handles, so its
  // stderr -- the WASAPI underrun lines, the bridge summary (readable /
  // underruns / overruns / bridgeShortWrites), CONFIGURE_COMMIT and the
  // host-bridge QoS line -- went nowhere. last-run.log therefore contained
  // only the runtime process's view, and the one process that can say why
  // a buffer was late was invisible (log of 2026-11-12: 8.5 minutes of
  // playback, zero audio-host lines). Append its stdout/stderr to
  // winapk-audio-host.log next to the executable instead.
  std::wstring log_path = p;
  { const auto slash = log_path.find_last_of(L"\\/");
    log_path = (slash == std::wstring::npos ? std::wstring() : log_path.substr(0, slash + 1)) + L"winapk-audio-host.log"; }
  SECURITY_ATTRIBUTES log_sa{sizeof(log_sa), nullptr, TRUE};
  HANDLE log_file = CreateFileW(log_path.c_str(), FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, &log_sa,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  HANDLE nul_input = log_file != INVALID_HANDLE_VALUE
      ? CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    &log_sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)
      : INVALID_HANDLE_VALUE;
  STARTUPINFOW si{sizeof(si)};
  if (log_file != INVALID_HANDLE_VALUE) {
   si.dwFlags = STARTF_USESTDHANDLES;
   si.hStdOutput = log_file;
   si.hStdError = log_file;
   si.hStdInput = nul_input != INVALID_HANDLE_VALUE ? nul_input : log_file;
  }
  PROCESS_INFORMATION pi{};
  BOOL ok = CreateProcessW(p.c_str(), cmd.data(), nullptr, nullptr, log_file != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW | HIGH_PRIORITY_CLASS, nullptr, nullptr, &si, &pi);
  if (log_file != INVALID_HANDLE_VALUE) {
   std::fprintf(stderr, "[I] Phase20-B2.3.6.4.16: audio host log file=%ls\n", log_path.c_str());
   CloseHandle(log_file);
  }
  if (nul_input != INVALID_HANDLE_VALUE) CloseHandle(nul_input);
  if (!ok) { h->last_error = GetLastError(); return false; }
  process = pi.hProcess;
  CloseHandle(pi.hThread);
  for (int wait = 0; wait < 200 && !h->host_ready && !h->host_failed; ++wait) Sleep(5);
  std::fprintf(stderr, "[I] Phase20-B2.3.6.4: audio host launch pid=%lu ready=%ld failed=%ld error=%lu mapping=%ls\n", pi.dwProcessId, h->host_ready, h->host_failed, h->last_error, name.c_str());
  return h->host_ready != 0;
 }
 bool configure(int rate,int alt,uint64_t gen,uint64_t session){
  if(!start())return false;
  if(h->host_failed)return false;
  const LONG state=InterlockedCompareExchange(&h->state,0,0);
  const LONG current_rate=InterlockedCompareExchange(&h->rate,0,0);
  const LONG current_alt=InterlockedCompareExchange(&h->alt,0,0);
  const LONG64 current_gen=InterlockedCompareExchange64(&h->generation,0,0);
  const LONG64 current_session=InterlockedCompareExchange64(&h->session_id,0,0);
  if(state==winapk::audio_ipc::kRunning&&current_rate==rate&&current_alt==alt&&current_gen==LONG64(gen)&&current_session==LONG64(session))return true;
  InterlockedExchange(&h->state,winapk::audio_ipc::kStopped);
  InterlockedExchange64(&h->generation,LONG64(gen));
  InterlockedExchange64(&h->session_id,LONG64(session));
  InterlockedExchange(&h->rate,rate);
  InterlockedExchange(&h->alt,alt);
  InterlockedExchange64(&h->read_pos,InterlockedCompareExchange64(&h->write_pos,0,0));
  InterlockedExchange(&h->state,winapk::audio_ipc::kRunning);
  SetEvent(data);
  std::fprintf(stderr,"[I] Phase20-B2.3.6.4.7: audio host configure changed session=%llu generation=%llu rate=%d alt=%d\n",(unsigned long long)session,(unsigned long long)gen,rate,alt);
  return true;
 }
 bool route_ready(uint64_t gen) const{return h&&h->host_ready&&!h->host_failed&&h->state==winapk::audio_ipc::kRunning&&uint64_t(h->generation)==gen;}
 bool write(const float* p,size_t frames,uint64_t gen){if(!route_ready(gen))return false;if(uint64_t(h->generation)!=gen){InterlockedIncrement64(&h->stale_drops);return true;}LONG64 w=InterlockedCompareExchange64(&h->write_pos,0,0),r=InterlockedCompareExchange64(&h->read_pos,0,0);uint64_t free=h->capacity_frames-(w>=r?uint64_t(w-r):0);if(frames>free){InterlockedIncrement64(&h->overruns);return false;}float* q=winapk::audio_ipc::pcm(h);for(size_t i=0;i<frames;i++){size_t d=size_t((uint64_t(w)+i)%h->capacity_frames)*2;q[d]=p[i*2];q[d+1]=p[i*2+1];}MemoryBarrier();InterlockedExchange64(&h->write_pos,w+LONG64(frames));InterlockedIncrement64(&h->producer_seq);InterlockedIncrement64(&h->parent_heartbeat);SetEvent(data);return true;}
 // Phase20-B2.3.6.4.17: latency the audio host currently reports.
 long long output_latency_frames(){return h?static_cast<long long>(h->output_latency_frames):0;}
};
P20AudioHostClient& p20_audio_host(){static P20AudioHostClient* c=new P20AudioHostClient();return *c;}
// Phase20-B2.3.6.4.17: how long a normally-completing URB's completion is
// held back so the app observes consumption at the moment the samples are
// actually audible. The app drives its progress bar, remaining time and
// spectrum analyser from its USB submit/completion pipeline; without this
// offset all of them run ahead of the sound by the whole downstream
// buffering (source ring ~1 s preroll + WASAPI endpoint 250 ms + queues).
// The offset is measured, not fixed: the audio host publishes its current
// output latency into the IPC header (fallback: ask the local stream).
// Cancelled URBs (status -2) are never delayed.
//
// Phase20-B2.3.6.4.18: .17 applied the full measured offset (~1.3 s) from
// the very first completion. last-run-3.log shows the result: 15 submits,
// 3 reaps, then the app's stream thread never comes back -- a ~1.3 s
// completion gap at startup trips UCRT's USB supervision and playback
// never starts. The offset is therefore ramped in per pool epoch: it
// starts at the 48 ms the 12-deep URB pool already implies and grows at
// 0.2x realtime, so completions keep arriving every ~5 ms (never a
// suspicious gap) while the app-visible clock slides onto the audible
// output over a few seconds. It is also capped (default 800 ms, safely
// below the gap that killed .17's startup). WINAPK_USB_LATENCY_SYNC_MS
// now sets the cap in ms: 0 disables the sync; a larger value (e.g.
// 1300) allows fuller sync, to test how much completion latency the app
// actually tolerates.
//
// Phase20-B2.3.6.4.19: .18 still broke playback, differently: it now
// played ~1 s, stopped, and repeated. The ramp keeps completions
// arriving, but at 0.8x realtime -- and UCRT's stream supervision
// evidently also validates the consumption *rate* (a DAC consuming 20%
// slow looks broken), resetting the stream about once a second. Any
// completion-timing distortion is therefore unusable with this app:
// the sync is now OFF unless explicitly opted in with
// WINAPK_USB_LATENCY_SYNC_MS=<cap ms>. The supported way to shrink the
// UI lead is WINAPK_USB_PREROLL_MS (audio_engine.cpp), which reduces
// the real buffering instead of lying about consumption timing.
static std::chrono::nanoseconds p20_latency_offset(const P20Pending* front){
 static std::once_flag once;static long cap_ms=-1;
 std::call_once(once,[]{const char* e=std::getenv("WINAPK_USB_LATENCY_SYNC_MS");if(e){cap_ms=std::atol(e);std::fprintf(stderr,"[I] Phase20-B2.3.6.4.19: URB completion latency sync capMs=%ld (opt-in)\n",cap_ms);}});
 if(cap_ms<=0)return std::chrono::nanoseconds(0);
 long long frames=p20_audio_host().output_latency_frames();
 if(frames<=0&&g_p20_stream)frames=winapk::audio::stream_output_latency_frames(g_p20_stream.get());
 long long target_ns=0;
 if(frames>0&&g_p20_rate>0){target_ns=frames*1000000000LL/g_p20_rate;if(target_ns>2500000000LL)target_ns=2500000000LL;}
 target_ns=(std::min)(target_ns,(long long)cap_ms*1000000LL);
 if(!front)return std::chrono::nanoseconds(target_ns);
 static uint64_t ramp_epoch=0;static std::chrono::steady_clock::time_point ramp_start{};
 static std::mutex ramp_lock;std::lock_guard<std::mutex> guard(ramp_lock);
 const auto now=std::chrono::steady_clock::now();
 if(ramp_epoch!=g_p20_pool_epoch){ramp_epoch=g_p20_pool_epoch;ramp_start=now;}
 const long long elapsed_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(now-ramp_start).count();
 const long long allowed_ns=48000000LL+elapsed_ns/5;
 return std::chrono::nanoseconds((std::min)(target_ns,allowed_ns));
}
struct P20AudioHostWarmup {
 P20AudioHostWarmup(){
  std::thread([]{
   const bool ready=p20_audio_host().start();
   std::fprintf(stderr,"[I] Phase20-B2.3.6.4.6: audio host UI warmup ready=%d\n",ready?1:0);
  }).detach();
 }
};
P20AudioHostWarmup g_p20_audio_host_warmup;
struct P20PcmJob { std::shared_ptr<winapk::audio::Stream> stream; std::vector<float> pcm; uint64_t generation; };
struct P20PcmQueue { std::mutex lock; std::condition_variable cv; std::deque<P20PcmJob> jobs; };
P20PcmQueue& p20_pcm_queue(){static P20PcmQueue* q=new P20PcmQueue();return *q;}
struct P20ProducerQosState {
 bool process_throttling_off=false;
 bool thread_throttling_off=false;
 bool mmcss_high=false;
 bool priority_boost_disabled=false;
 bool fallback_highest=false;
 DWORD process_error=ERROR_SUCCESS;
 DWORD thread_error=ERROR_SUCCESS;
 DWORD mmcss_index=0;
 HANDLE mmcss=nullptr;
};
P20ProducerQosState& p20_configure_producer_qos(){
 thread_local P20ProducerQosState state;
 thread_local bool configured=false;
 if(configured)return state;
 configured=true;
 HMODULE kernel32=GetModuleHandleW(L"kernel32.dll");
 using SetProcessInformationFn=BOOL (WINAPI*)(HANDLE,PROCESS_INFORMATION_CLASS,LPVOID,DWORD);
 using SetThreadInformationFn=BOOL (WINAPI*)(HANDLE,THREAD_INFORMATION_CLASS,LPVOID,DWORD);
 auto set_process_info=kernel32?reinterpret_cast<SetProcessInformationFn>(GetProcAddress(kernel32,"SetProcessInformation")):nullptr;
 auto set_thread_info=kernel32?reinterpret_cast<SetThreadInformationFn>(GetProcAddress(kernel32,"SetThreadInformation")):nullptr;
 if(set_process_info){
  PROCESS_POWER_THROTTLING_STATE power{};
  power.Version=PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  power.ControlMask=PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
  power.StateMask=0;
  SetLastError(ERROR_SUCCESS);
  state.process_throttling_off=set_process_info(GetCurrentProcess(),ProcessPowerThrottling,&power,sizeof(power))!=FALSE;
  state.process_error=state.process_throttling_off?ERROR_SUCCESS:GetLastError();
 }else state.process_error=ERROR_CALL_NOT_IMPLEMENTED;
 if(set_thread_info){
  THREAD_POWER_THROTTLING_STATE power{};
  power.Version=THREAD_POWER_THROTTLING_CURRENT_VERSION;
  power.ControlMask=THREAD_POWER_THROTTLING_EXECUTION_SPEED;
  power.StateMask=0;
  SetLastError(ERROR_SUCCESS);
  state.thread_throttling_off=set_thread_info(GetCurrentThread(),ThreadPowerThrottling,&power,sizeof(power))!=FALSE;
  state.thread_error=state.thread_throttling_off?ERROR_SUCCESS:GetLastError();
 }else state.thread_error=ERROR_CALL_NOT_IMPLEMENTED;
 HMODULE avrt=LoadLibraryW(L"avrt.dll");
 using AvSetFn=HANDLE (WINAPI*)(LPCWSTR,LPDWORD);
 using AvPriorityFn=BOOL (WINAPI*)(HANDLE,int);
 auto av_set=avrt?reinterpret_cast<AvSetFn>(GetProcAddress(avrt,"AvSetMmThreadCharacteristicsW")):nullptr;
 auto av_priority=avrt?reinterpret_cast<AvPriorityFn>(GetProcAddress(avrt,"AvSetMmThreadPriority")):nullptr;
 state.mmcss=av_set?av_set(L"Pro Audio",&state.mmcss_index):nullptr;
 state.mmcss_high=state.mmcss&&av_priority&&av_priority(state.mmcss,1)!=FALSE;
 if(!state.mmcss)state.fallback_highest=SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_HIGHEST)!=FALSE;
 state.priority_boost_disabled=SetThreadPriorityBoost(GetCurrentThread(),TRUE)!=FALSE;
 std::fprintf(stderr,"[I] Phase20-B2.3.6.4.10: producer QoS processThrottleOff=%d processError=%lu threadThrottleOff=%d threadError=%lu mmcss=%d mmcssHigh=%d taskIndex=%lu fallbackHighest=%d boostDisabled=%d threadPriority=%d processPriority=0x%08lX\n",state.process_throttling_off?1:0,state.process_error,state.thread_throttling_off?1:0,state.thread_error,state.mmcss?1:0,state.mmcss_high?1:0,state.mmcss_index,state.fallback_highest?1:0,state.priority_boost_disabled?1:0,GetThreadPriority(GetCurrentThread()),GetPriorityClass(GetCurrentProcess()));
 return state;
}
bool p20_runtime_is_foreground(){
 HWND foreground=GetForegroundWindow();
 if(!foreground)return false;
 DWORD pid=0;GetWindowThreadProcessId(foreground,&pid);
 return pid==GetCurrentProcessId();
}
void p20_pcm_worker(){
 auto& qos=p20_configure_producer_qos();
 (void)qos;
 auto& q=p20_pcm_queue();
 for(;;){
  P20PcmJob job;
  {std::unique_lock<std::mutex> hold(q.lock);q.cv.wait(hold,[&]{return !q.jobs.empty();});job=std::move(q.jobs.front());q.jobs.pop_front();}
  if(job.generation!=g_p20_pcm_generation.load(std::memory_order_acquire)){g_p20_async_stale_jobs.fetch_add(1,std::memory_order_relaxed);continue;}
  const auto begin=std::chrono::steady_clock::now();
  const bool isolated=!job.pcm.empty()&&p20_audio_host().write(job.pcm.data(),job.pcm.size()/2,job.generation);
  const size_t wrote=isolated?job.pcm.size():(job.stream&&!job.pcm.empty()?winapk::audio::stream_write(job.stream.get(),job.pcm.data(),job.pcm.size(),4,true):0);
  if(wrote==0&&!job.pcm.empty())std::fprintf(stderr,"[E] Phase20-B2.3.6.4.8: PCM worker lost owner generation=%llu samples=%zu hostReady=%d\n",(unsigned long long)job.generation,job.pcm.size(),p20_audio_host().route_ready(job.generation)?1:0);
  const uint64_t us=static_cast<uint64_t>((std::max)(int64_t(0),std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-begin).count()));
  g_p20_async_jobs.fetch_add(1,std::memory_order_relaxed);g_p20_async_written_samples.fetch_add(wrote,std::memory_order_relaxed);g_p20_async_write_sum_us.fetch_add(us,std::memory_order_relaxed);
  uint64_t old=g_p20_async_write_max_us.load(std::memory_order_relaxed);while(old<us&&!g_p20_async_write_max_us.compare_exchange_weak(old,us,std::memory_order_relaxed)){}
  static auto last_summary=std::chrono::steady_clock::now();const auto summary_now=std::chrono::steady_clock::now();if(summary_now-last_summary>=std::chrono::seconds(30)){last_summary=summary_now;const uint64_t writes=g_p20_async_jobs.load();std::fprintf(stderr,"[I] Phase20-B2.3.6.0: USB stall summary producerGapMaxUs=%llu gapOver10=%llu gapOver25=%llu gapOver50=%llu queueDepthMax=%llu queueDrops=%llu asyncWrites=%llu asyncWriteAvgUs=%.3f asyncWriteMaxUs=%llu writtenSamples=%llu foreground=%d\n",(unsigned long long)g_p20_producer_gap_max_us.load(),(unsigned long long)g_p20_producer_gap_over10.load(),(unsigned long long)g_p20_producer_gap_over25.load(),(unsigned long long)g_p20_producer_gap_over50.load(),(unsigned long long)g_p20_async_queue_depth_max.load(),(unsigned long long)g_p20_async_queue_drops.load(),(unsigned long long)writes,writes?double(g_p20_async_write_sum_us.load())/double(writes):0.0,(unsigned long long)g_p20_async_write_max_us.load(),(unsigned long long)g_p20_async_written_samples.load(),p20_runtime_is_foreground()?1:0);std::fprintf(stderr,"[I] Phase20-B2.3.6.4.19: latency sync targetMs=%lld\n",(long long)std::chrono::duration_cast<std::chrono::milliseconds>(p20_latency_offset(nullptr)).count());}
 }
}
bool p20_enqueue_pcm(const std::shared_ptr<winapk::audio::Stream>& stream,std::vector<float>& pcm,uint64_t generation){
 if(pcm.empty())return false;
 if(!stream&&!p20_audio_host().route_ready(generation)){std::fprintf(stderr,"[E] Phase20-B2.3.6.4.8: PCM queue has no active owner generation=%llu\n",(unsigned long long)generation);return false;}
 static std::once_flag once;std::call_once(once,[]{std::thread(p20_pcm_worker).detach();});
 auto& q=p20_pcm_queue();
 const int64_t now_us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();const int64_t prev_us=g_p20_producer_last_us.exchange(now_us,std::memory_order_relaxed);if(prev_us>0&&now_us>prev_us){const uint64_t gap=uint64_t(now_us-prev_us);uint64_t old_max=g_p20_producer_gap_max_us.load();while(old_max<gap&&!g_p20_producer_gap_max_us.compare_exchange_weak(old_max,gap)){}if(gap>=10000)++g_p20_producer_gap_over10;if(gap>=25000)++g_p20_producer_gap_over25;if(gap>=50000)++g_p20_producer_gap_over50;}
 {std::lock_guard<std::mutex> hold(q.lock);if(q.jobs.size()>=256){q.jobs.pop_front();++g_p20_async_queue_drops;}q.jobs.push_back(P20PcmJob{stream,std::move(pcm),generation});const uint64_t depth=q.jobs.size();uint64_t old_max=g_p20_async_queue_depth_max.load();while(old_max<depth&&!g_p20_async_queue_depth_max.compare_exchange_weak(old_max,depth)){} }
 q.cv.notify_one();return true;
}
int p20_rate_for_alt(uint32_t alt){static constexpr int rates[]={0,44100,48000,88200,96000,176400,192000};return alt<sizeof(rates)/sizeof(rates[0])?rates[alt]:0;}
int p20_alt_for_rate(int rate){static constexpr int rates[]={0,44100,48000,88200,96000,176400,192000};for(int alt=1;alt<=6;alt++)if(rates[alt]==rate)return alt;return 0;}
int p20_rate_from_urb(size_t bytes,int packets){
 if(bytes==0||packets<=0)return 0;
 const uint64_t estimate=(uint64_t(bytes)*8000ULL+uint64_t(packets)*3ULL)/(uint64_t(packets)*6ULL);
 static constexpr int rates[]={44100,48000,88200,96000,176400,192000};
 int best=0;uint64_t error=~uint64_t(0);
 for(int rate:rates){uint64_t e=estimate>uint64_t(rate)?estimate-uint64_t(rate):uint64_t(rate)-estimate;if(e<error){error=e;best=rate;}}
 return error<=1500?best:0;
}
void p20_dump(const void* p,size_t n){auto* b=static_cast<const uint8_t*>(p);if(p20_diag_enabled())std::fprintf(stderr,"[I] Phase20-B2.2: URB raw");for(size_t i=0;i<n;i++)std::fprintf(stderr,"%s%02x",i%16?" ":"\n  ",unsigned(b[i]));std::fputc('\n',stderr);}
void p20_start(){const bool isolated=p20_audio_host().configure(g_p20_rate,int(g_p20_alt),g_p20_generation,g_p20_session_id);if(isolated){if(g_p20_stream){winapk::audio::stream_stop(g_p20_stream.get());winapk::audio::stream_release(g_p20_stream);g_p20_stream.reset();}if(p20_diag_enabled())std::fprintf(stderr,"[I] Phase20-B2.3.6.4.5: exclusive owner=audio-host rate=%d alt=%u generation=%llu\n",g_p20_rate,g_p20_alt,(unsigned long long)g_p20_generation);return;}if(!g_p20_stream){const int buffer_frames=(std::max)(g_p20_rate/10,1024);g_p20_stream=winapk::audio::usb_direct_stream_create(g_p20_rate,2,4,buffer_frames);if(g_p20_stream)winapk::audio::stream_play(g_p20_stream.get());if(p20_diag_enabled())std::fprintf(stderr,"[I] Phase20-B2.3.6.4.5: exclusive owner=runtime fallback rate=%d alt=%u channels=2 float=%s bufferFrames=%d generation=%llu\n",g_p20_rate,g_p20_alt,g_p20_stream?"ok":"failed",buffer_frames,(unsigned long long)g_p20_generation);}}
void p20_retire_pool_locked(){
 g_p20_retired_pool.insert(g_p20_current_pool.begin(),g_p20_current_pool.end());
 g_p20_current_pool.clear();
 g_p20_pool_building=true;
 ++g_p20_pool_epoch;
}
void p20_soft_boundary_locked(const char* reason){
 const size_t old_pool=g_p20_current_pool.size(),pending=g_p20_pending.size();
 // Do not clear pending completions and do not change generation. The blocking
 // REAP remains attached to the same queue and can consume the old pool's
 // already-scheduled completions before naturally moving to the replacement pool.
 g_p20_retired_pool.insert(g_p20_current_pool.begin(),g_p20_current_pool.end());
 g_p20_current_pool.clear();g_p20_pool_building=true;++g_p20_pool_epoch;g_p20_reap_gate=true;g_p20_gate_submits=0;g_p20_gate_until={};
 if(g_p20_stream){winapk::audio::stream_stop(g_p20_stream.get());winapk::audio::stream_release(g_p20_stream);g_p20_stream.reset();}
 g_p20_timeline={};g_p20_silence_phase=0;++g_p20_pool_recoveries;g_p20_failed=false;g_p20_active=true;
 if(p20_diag_enabled())std::fprintf(stderr,"[I] Phase20-B2.3.4.6: USB DAC soft boundary reason=%s oldPool=%zu pendingPreserved=%zu generation=%llu poolEpoch=%llu boundaries=%llu reapPreserved=1\\n",reason,old_pool,pending,(unsigned long long)g_p20_generation,(unsigned long long)g_p20_pool_epoch,(unsigned long long)g_p20_pool_recoveries);
}
void p20_stop_stream_locked(const char* reason){p20_soft_boundary_locked(reason);}
void p20_observe_urb_pool_locked(P20Urb* u){
 if(!u)return;
 if(g_p20_pool_building){
  const bool repeated=g_p20_current_pool.count(u)!=0;
  g_p20_current_pool.insert(u);g_p20_retired_pool.erase(u);
  if(repeated&&g_p20_current_pool.size()>=4){g_p20_pool_building=false;if(p20_diag_enabled())std::fprintf(stderr,"[I] Phase20-B2.3.4.6: URB pool established size=%zu generation=%llu poolEpoch=%llu\n",g_p20_current_pool.size(),(unsigned long long)g_p20_generation,(unsigned long long)g_p20_pool_epoch);}
  return;
 }
 if(g_p20_current_pool.count(u)){g_p20_retired_pool.erase(u);return;}
 // Once a ring has repeated, one unseen pointer is enough to identify a new
 // player/decoder pool. This deliberately favors recovery over preservation.
 p20_soft_boundary_locked("first-unseen-urb-after-established-pool");
 g_p20_current_pool.insert(u);g_p20_retired_pool.erase(u);
}
void p20_cancel_locked(const char* reason,bool reset){const size_t cancelled=g_p20_pending.size();for(auto& p:g_p20_pending)if(p.urb)p.urb->status=-2;g_p20_pending.clear();if(reset)p20_retire_pool_locked();if(g_p20_stream){winapk::audio::stream_stop(g_p20_stream.get());winapk::audio::stream_release(g_p20_stream);g_p20_stream.reset();}g_p20_timeline={};if(reset){g_p20_generation++;g_p20_pcm_generation.store(g_p20_generation,std::memory_order_release);g_p20_failed=false;g_p20_active=true;}g_p20_cv.notify_all();fd_notify();std::fprintf(stderr,"[I] Phase20-B2.3.5.8: session summary reason=%s generation=%llu lockHeldSamples=%llu lockHeldAvgUs=%.3f lockHeldMaxUs=%llu asyncJobs=%llu asyncWriteAvgUs=%.3f asyncWriteMaxUs=%llu asyncWrittenSamples=%llu asyncStale=%llu asyncQueueDrops=%llu\n",reason,(unsigned long long)g_p20_generation,(unsigned long long)g_p20_lock_held_samples.load(),g_p20_lock_held_samples.load()?double(g_p20_lock_held_sum_us.load())/double(g_p20_lock_held_samples.load()):0.0,(unsigned long long)g_p20_lock_held_max_us.load(),(unsigned long long)g_p20_async_jobs.load(),g_p20_async_jobs.load()?double(g_p20_async_write_sum_us.load())/double(g_p20_async_jobs.load()):0.0,(unsigned long long)g_p20_async_write_max_us.load(),(unsigned long long)g_p20_async_written_samples.load(),(unsigned long long)g_p20_async_stale_jobs.load(),(unsigned long long)g_p20_async_queue_drops.load());if(p20_diag_enabled())std::fprintf(stderr,"[I] Phase20-B2.3.4.5: stop reason=%s cancelled=%zu pending=0 generation=%llu reset=%d poolEpoch=%llu staleDropped=%llu duplicateSubmit=%llu\n",reason,cancelled,(unsigned long long)g_p20_generation,reset?1:0,(unsigned long long)g_p20_pool_epoch,(unsigned long long)g_p20_stale_dropped,(unsigned long long)g_p20_duplicate_submit);}
int p20_reject(P20Urb* u,const char* why){if(u)u->status=-22;g_p20_failed=true;g_p20_active=false;std::fprintf(stderr,"[E] Phase20-B2.2: SUBMIT rejected reason=%s session=FAILED_STOPPED generation=%llu\n",why,(unsigned long long)g_p20_generation);p20_cancel_locked("submit-invalid",false);*guest_errno()=22;return -1;}
int p20_submit(void* arg){
 if(!arg){*guest_errno()=14;g_p20_failed=true;g_p20_active=false;return -1;}auto* u=static_cast<P20Urb*>(arg);p20_observe_urb_pool_locked(u);
 if(g_p20_urbs==0){if(p20_diag_enabled())std::fprintf(stderr,"[I] Phase20-B2.3: ABI=x86_64 size=56 type=%u ep=0x%02x status=%d flags=0x%x buffer=%p bufferLength=%d actual=%d start=%d packets=%d errors=%d signr=%u user=%p\n",unsigned(u->type),unsigned(u->endpoint),u->status,u->flags,u->buffer,u->buffer_length,u->actual_length,u->start_frame,u->number_of_packets,u->error_count,u->signr,u->usercontext);p20_dump(arg,56);}
 if(g_p20_failed||!g_p20_active){*guest_errno()=32;return -1;}if(u->type!=0)return p20_reject(u,"type");if(u->endpoint!=1)return p20_reject(u,"endpoint");if(u->number_of_packets<=0||u->number_of_packets>1024)return p20_reject(u,"packet-count");if(!u->buffer||u->buffer_length<=0)return p20_reject(u,"buffer");
 auto* src=static_cast<const uint8_t*>(u->buffer);auto* iso=reinterpret_cast<P20Iso*>(reinterpret_cast<uint8_t*>(u)+56);size_t off=0,total=0;std::vector<float> pcm;u->error_count=0;
 for(int i=0;i<u->number_of_packets;i++){size_t n=iso[i].length;if(n>576||off+n>size_t(u->buffer_length))return p20_reject(u,"iso-range");size_t usable=n-n%6;pcm.reserve(pcm.size()+usable/3);for(size_t k=0;k<usable;k+=3){int32_t v=int32_t(src[off+k])|(int32_t(src[off+k+1])<<8)|(int32_t(src[off+k+2])<<16);if(v&0x800000)v|=~0xffffff;pcm.push_back(float(v)/8388608.0f);}iso[i].status=0;iso[i].actual_length=uint32_t(usable);off+=n;total+=usable;}
 // Phase 20-B2.3.6: a producer underrun may arrive as an isochronous URB
 // whose packet descriptors contain no complete stereo 24-bit frame. Do not
 // stop the USB audio clock. Synthesize exactly one URB interval of zero PCM,
 // using a fixed-point accumulator so 44.1/88.2/176.4 kHz keep their correct
 // alternating packet sizes without drift. This path is deliberately limited
 // to an entirely empty URB; valid program samples are never overwritten.
 if(total==0&&g_p20_rate>0){
  const uint64_t numerator=uint64_t(g_p20_rate)*uint64_t(u->number_of_packets)+g_p20_silence_phase;
  const size_t silence_frames=static_cast<size_t>(numerator/8000ULL);
  g_p20_silence_phase=numerator%8000ULL;
  if(silence_frames>0){
   pcm.assign(silence_frames*2,0.0f);total=silence_frames*6;
   size_t remaining=total;
   for(int i=0;i<u->number_of_packets;i++){
    const size_t packets_left=static_cast<size_t>(u->number_of_packets-i);
    size_t packet_bytes=((remaining/6+packets_left-1)/packets_left)*6;
    if(packet_bytes>remaining)packet_bytes=remaining;
    iso[i].status=0;iso[i].actual_length=static_cast<uint32_t>(packet_bytes);remaining-=packet_bytes;
   }
   ++g_p20_silence_fill_urbs;g_p20_silence_fill_frames+=silence_frames;
   if(g_p20_silence_fill_urbs<=16||(g_p20_silence_fill_urbs%250)==0)std::fprintf(stderr,"[W] Phase20-B2.3.6: virtual DAC PCM silence fill urb=%llu frames=%zu bytes=%zu packets=%d rate=%d phase=%llu totalFillUrbs=%llu totalFillFrames=%llu\n",(unsigned long long)(g_p20_urbs+1),silence_frames,total,u->number_of_packets,g_p20_rate,(unsigned long long)g_p20_silence_phase,(unsigned long long)g_p20_silence_fill_urbs,(unsigned long long)g_p20_silence_fill_frames);
  }
 }
 const int inferred_rate=p20_rate_from_urb(total,u->number_of_packets);
 if(inferred_rate==0){std::fprintf(stderr,"[E] Phase20-B2.3.3.3: URB rate inference failed bytes=%zu packets=%d currentRate=%d currentAlt=%u\n",total,u->number_of_packets,g_p20_rate,g_p20_alt);return p20_reject(u,"rate-inference");}
 const uint32_t inferred_alt=static_cast<uint32_t>(p20_alt_for_rate(inferred_rate));
 if(g_p20_stream&&(g_p20_rate!=inferred_rate||g_p20_alt!=inferred_alt))p20_stop_stream_locked("urb-rate-change");
 if(g_p20_rate!=inferred_rate||g_p20_alt!=inferred_alt||g_p20_urbs==0)if(p20_diag_enabled())std::fprintf(stderr,"[I] Phase20-B2.3.3.3: URB rate synchronized bytes=%zu packets=%d oldRate=%d oldAlt=%u newRate=%d newAlt=%u\n",total,u->number_of_packets,g_p20_rate,g_p20_alt,inferred_rate,inferred_alt);
 g_p20_rate=inferred_rate;g_p20_alt=inferred_alt;g_p20_active=true;g_p20_failed=false;
 double p20_sum_l=0,p20_sum_r=0;float p20_peak_l=0,p20_peak_r=0;size_t p20_nonzero=0,p20_frames=pcm.size()/2;for(size_t i=0;i+1<pcm.size();i+=2){float l=pcm[i],r=pcm[i+1];p20_peak_l=(std::max)(p20_peak_l,std::fabs(l));p20_peak_r=(std::max)(p20_peak_r,std::fabs(r));p20_sum_l+=double(l)*l;p20_sum_r+=double(r)*r;if(l!=0||r!=0)p20_nonzero++;}if(p20_nonzero){g_p20_nonzero_urbs++;g_p20_nonzero_frames+=p20_nonzero;}else g_p20_silent_urbs++;uint64_t p20_urb_no=g_p20_urbs+1;if(p20_urb_no==1||(!g_p20_first_nonzero_logged&&p20_nonzero)||(p20_urb_no%2500)==0){const char* kind=p20_nonzero?(!g_p20_first_nonzero_logged?"first-nonzero":"periodic-nonzero"):"silent";if(p20_diag_enabled())std::fprintf(stderr,"[I] Phase20-B2.3.1: PCM monitor kind=%s urb=%llu frames=%zu peakL=%.6f peakR=%.6f rmsL=%.6f rmsR=%.6f nonZero=%zu silentUrbs=%llu nonzeroUrbs=%llu nonzeroFrames=%llu\n",kind,(unsigned long long)p20_urb_no,p20_frames,p20_peak_l,p20_peak_r,p20_frames?std::sqrt(p20_sum_l/p20_frames):0.0,p20_frames?std::sqrt(p20_sum_r/p20_frames):0.0,p20_nonzero,(unsigned long long)g_p20_silent_urbs,(unsigned long long)g_p20_nonzero_urbs,(unsigned long long)g_p20_nonzero_frames);if(p20_nonzero)g_p20_first_nonzero_logged=true;}
 p20_start();const size_t pcm_samples=pcm.size();const bool queued=p20_enqueue_pcm(g_p20_stream,pcm,g_p20_generation);size_t wrote=queued?pcm_samples:0;if(wrote<pcm.size()){u->status=-5;g_p20_failed=true;g_p20_active=false;p20_cancel_locked("wasapi-write",false);*guest_errno()=5;return -1;}
 u->status=0;u->actual_length=int32_t(total);auto now=std::chrono::steady_clock::now();if(g_p20_timeline<now)g_p20_timeline=now;const uint64_t bytes_per_second=uint64_t((std::max)(1,g_p20_rate))*6ULL;auto duration=std::chrono::nanoseconds((total*1000000000ULL+bytes_per_second-1)/bytes_per_second);g_p20_timeline+=duration;for(auto it=g_p20_pending.begin();it!=g_p20_pending.end();){if(it->urb==u){if(it->urb)it->urb->status=-2;it=g_p20_pending.erase(it);g_p20_duplicate_submit++;}else ++it;}g_p20_current_pool.insert(u);g_p20_retired_pool.erase(u);const uint64_t submit_id=++g_p20_submit_id;g_p20_pending.push_back({u,u,u->buffer,g_p20_timeline,g_p20_generation,submit_id,g_p20_pool_epoch});if(g_p20_reap_gate){g_p20_gate_submits++;if(g_p20_gate_submits>=12&&g_p20_gate_until.time_since_epoch().count()==0){g_p20_gate_until=std::chrono::steady_clock::now()+std::chrono::milliseconds(20);g_p20_cv.notify_all();if(p20_diag_enabled())std::fprintf(stderr,"[I] Phase20-B2.3.4.8: REAP identity gate armed submitted=%zu delayMs=20 poolEpoch=%llu generation=%llu\n",g_p20_gate_submits,(unsigned long long)g_p20_pool_epoch,(unsigned long long)g_p20_generation);}}if(submit_id<=24||(submit_id%2500)==0)std::fprintf(stderr,"[I] Phase20-B2.3.4.8: SUBMIT identity urb=%p submitUrb=%p buffer=%p submit=%llu rate=%d poolEpoch=%llu pending=%zu\n",static_cast<void*>(u),static_cast<void*>(u),u->buffer,(unsigned long long)submit_id,g_p20_rate,(unsigned long long)g_p20_pool_epoch,g_p20_pending.size());g_p20_bytes+=total;g_p20_urbs++;g_p20_cv.notify_all();fd_notify();const int submit_errno_before=*guest_errno();*guest_errno()=0;if(submit_id<=24||(submit_id%2500)==0)std::fprintf(stderr,"[I] Phase20-B2.3.4.9: SUBMIT result urb=%p submit=%llu return=0 guestErrnoBefore=%d guestErrnoAfter=%d\n",static_cast<void*>(u),(unsigned long long)submit_id,submit_errno_before,*guest_errno());if(g_p20_urbs<=4||(g_p20_urbs%2500)==0)std::fprintf(stderr,"[I] Phase20-B2.3.1: SUBMIT accepted urb=%llu packets=%d pcmBytes=%zu rate=%d alt=%u queued=%zu paceUs=%lld generation=%llu submit=%llu duplicateSubmit=%llu\n",(unsigned long long)g_p20_urbs,u->number_of_packets,total,g_p20_rate,g_p20_alt,g_p20_pending.size(),(long long)std::chrono::duration_cast<std::chrono::microseconds>(duration).count(),(unsigned long long)g_p20_generation,(unsigned long long)submit_id,(unsigned long long)g_p20_duplicate_submit);return 0;
}
bool p20_is_fd(int fd){return fd>=kP20FdBase&&fd<kP20FdLimit;}
void p20_activate_fd_locked(int fd,const char* reason){
 if(fd==g_p20_active_fd)return;
 const int old_fd=g_p20_active_fd;const size_t dropped=g_p20_pending.size();
 for(auto& p:g_p20_pending)if(p.urb)p.urb->status=-2;
 g_p20_pending.clear();g_p20_current_pool.clear();g_p20_retired_pool.clear();g_p20_pool_building=true;++g_p20_pool_epoch;g_p20_reap_gate=true;g_p20_gate_submits=0;g_p20_gate_until={};
 if(g_p20_stream){winapk::audio::stream_stop(g_p20_stream.get());winapk::audio::stream_release(g_p20_stream);g_p20_stream.reset();}
 g_p20_timeline={};g_p20_silence_phase=0;g_p20_failed=false;g_p20_active=true;g_p20_alt=0;g_p20_active_fd=fd;++g_p20_session_id;
 g_p20_cv.notify_all();fd_notify();
 std::fprintf(stderr,"[W] Phase20-B2.3.4.7: USBFS session activated oldFd=0x%x newFd=0x%x session=%llu dropped=%zu reason=%s\\n",old_fd,fd,(unsigned long long)g_p20_session_id,dropped,reason);
}
int p20_success(const char* op){const int before=*guest_errno();*guest_errno()=0;if(p20_diag_enabled())std::fprintf(stderr,"[I] Phase20-B2.3.4.9: ioctl success op=%s return=0 guestErrnoBefore=%d guestErrnoAfter=%d\n",op,before,*guest_errno());return 0;}
int p20_ioctl(int fd,int request,void* arg){
 (void)p20_usb_realtime_thread();
 const auto p20_lock_wait_begin=std::chrono::steady_clock::now();
 std::unique_lock<std::mutex> hold(g_p20_lock);
 const auto p20_lock_wait_end=std::chrono::steady_clock::now();
 P20HeldTimer p20_held_timer(p20_lock_wait_end);
 const uint64_t p20_lock_wait_us=static_cast<uint64_t>((std::max)(int64_t(0),std::chrono::duration_cast<std::chrono::microseconds>(p20_lock_wait_end-p20_lock_wait_begin).count()));
 ++g_p20_lock_wait_samples;g_p20_lock_wait_sum_us+=p20_lock_wait_us;g_p20_lock_wait_max_us=(std::max)(g_p20_lock_wait_max_us,p20_lock_wait_us);
 if(p20_lock_wait_us>500)++g_p20_lock_wait_over_500us;if(p20_lock_wait_us>1000)++g_p20_lock_wait_over_1ms;if(p20_lock_wait_us>2000)++g_p20_lock_wait_over_2ms;if(p20_lock_wait_us>5000)++g_p20_lock_wait_over_5ms;
 const bool p20_lock_wait_slow=p20_lock_wait_us>500;
 if(p20_lock_wait_slow)++g_p20_lock_wait_slow_logs;
 if(p20_diag_enabled()&&((p20_lock_wait_slow&&(g_p20_lock_wait_slow_logs<=16||(g_p20_lock_wait_slow_logs%250)==0))||(g_p20_lock_wait_samples%2500)==0))std::fprintf(stderr,"[I] Phase20-B2.3.7: USBFS lock wait op=0x%x waitUs=%llu samples=%llu avgUs=%.3f maxUs=%llu over500us=%llu over1ms=%llu over2ms=%llu over5ms=%llu pending=%zu rate=%d generation=%llu session=%llu tid=%lu\n",uint32_t(request),(unsigned long long)p20_lock_wait_us,(unsigned long long)g_p20_lock_wait_samples,g_p20_lock_wait_samples?double(g_p20_lock_wait_sum_us)/double(g_p20_lock_wait_samples):0.0,(unsigned long long)g_p20_lock_wait_max_us,(unsigned long long)g_p20_lock_wait_over_500us,(unsigned long long)g_p20_lock_wait_over_1ms,(unsigned long long)g_p20_lock_wait_over_2ms,(unsigned long long)g_p20_lock_wait_over_5ms,g_p20_pending.size(),g_p20_rate,(unsigned long long)g_p20_generation,(unsigned long long)g_p20_session_id,(unsigned long)GetCurrentThreadId());
 if(g_p20_active_fd<0||fd>g_p20_active_fd)p20_activate_fd_locked(fd,"newer-virtual-connection");
 if(fd!=g_p20_active_fd){*guest_errno()=19;if(uint32_t(request)!=kP20ReapNd)if(p20_diag_enabled())std::fprintf(stderr,"[I] Phase20-B2.3.4.7: stale USBFS session rejected fd=0x%x activeFd=0x%x op=0x%x\n",fd,g_p20_active_fd,request);return -1;}
 if(uint32_t(request)!=kP20ReapNd&&uint32_t(request)!=kP20Submit)if(p20_diag_enabled())std::fprintf(stderr,"[I] Phase20-B2.3.4.7: ioctl fd=0x%x session=%llu op=0x%x arg=%p\n",fd,(unsigned long long)g_p20_session_id,request,arg);switch(uint32_t(request)){
 case kP20GetSpeed:return 3;case kP20Claim:return 0;case kP20Release:p20_cancel_locked("release-interface",true);return 0;
 case kP20SetInterface:{if(!arg){*guest_errno()=14;return -1;}auto* v=static_cast<uint32_t*>(arg);const int rate=v[0]==1?p20_rate_for_alt(v[1]):0;if(p20_diag_enabled())std::fprintf(stderr,"[I] Phase20-B2.3.4.6: SETINTERFACE if=%u alt=%u rate=%d\n",v[0],v[1],rate);if(v[0]!=1||v[1]>6){*guest_errno()=22;return -1;}if(v[1]==0){g_p20_alt=0;g_p20_alt_zero_boundary=true;p20_soft_boundary_locked("alt-zero-track-boundary");}else{if(!g_p20_alt_zero_boundary&&g_p20_stream&&(g_p20_alt!=v[1]||g_p20_rate!=rate))p20_soft_boundary_locked("rate-alt-change-without-zero");g_p20_alt_zero_boundary=false;g_p20_alt=v[1];g_p20_rate=rate;g_p20_active=true;g_p20_failed=false;}return p20_success("SET_INTERFACE");}
 case 0x5514:p20_cancel_locked("reset",true);return p20_success("RESET");case 0x5516:p20_cancel_locked("disconnect",true);return p20_success("DISCONNECT");case kP20Submit:return p20_submit(arg);
 case kP20Reap:case kP20ReapNd:{if(!arg){*guest_errno()=14;return -1;}*static_cast<void**>(arg)=nullptr;const bool nd=uint32_t(request)==kP20ReapNd;const uint64_t reap_generation=g_p20_generation;for(;;){if(g_p20_reap_gate){if(g_p20_gate_submits<12||g_p20_gate_until.time_since_epoch().count()==0){if(nd){*guest_errno()=11;return -1;}g_p20_cv.wait(hold);continue;}const auto gate_now=std::chrono::steady_clock::now();if(gate_now<g_p20_gate_until){if(nd){*guest_errno()=11;return -1;}g_p20_cv.wait_until(hold,g_p20_gate_until);continue;}g_p20_reap_gate=false;if(p20_diag_enabled())std::fprintf(stderr,"[I] Phase20-B2.3.4.8: REAP identity gate opened submitted=%zu poolEpoch=%llu generation=%llu pending=%zu\n",g_p20_gate_submits,(unsigned long long)g_p20_pool_epoch,(unsigned long long)g_p20_generation,g_p20_pending.size());}if(reap_generation!=g_p20_generation){*guest_errno()=11;std::fprintf(stderr,"[I] Phase20-B2.3.4.0: REAP epoch retired waitGeneration=%llu currentGeneration=%llu pending=%zu\n",(unsigned long long)reap_generation,(unsigned long long)g_p20_generation,g_p20_pending.size());return -1;}while(!g_p20_pending.empty()&&g_p20_pending.front().generation!=g_p20_generation){auto stale=g_p20_pending.front();g_p20_pending.pop_front();if(stale.urb)stale.urb->status=-2;g_p20_stale_dropped++;if(g_p20_stale_dropped<=8||(g_p20_stale_dropped%100)==0)std::fprintf(stderr,"[W] Phase20-B2.3.3.5: stale completion quarantined urb=%p submit=%llu oldGeneration=%llu currentGeneration=%llu total=%llu\n",static_cast<void*>(stale.urb),(unsigned long long)stale.submit_id,(unsigned long long)stale.generation,(unsigned long long)g_p20_generation,(unsigned long long)g_p20_stale_dropped);}if(g_p20_pending.empty()){if(nd||g_p20_failed||!g_p20_active){*guest_errno()=11;return -1;}g_p20_cv.wait(hold);if(reap_generation!=g_p20_generation){*guest_errno()=11;std::fprintf(stderr,"[I] Phase20-B2.3.4.6: blocking REAP released by explicit reset/disconnect waitGeneration=%llu currentGeneration=%llu\n",(unsigned long long)reap_generation,(unsigned long long)g_p20_generation);return -1;}continue;}const auto ready=g_p20_pending.front().ready+((g_p20_pending.front().urb&&g_p20_pending.front().urb->status==-2)?std::chrono::nanoseconds(0):p20_latency_offset(&g_p20_pending.front()));const auto now=std::chrono::steady_clock::now();if(now<ready){if(nd){*guest_errno()=11;return -1;}g_p20_cv.wait_until(hold,ready);if(reap_generation!=g_p20_generation){*guest_errno()=11;std::fprintf(stderr,"[I] Phase20-B2.3.4.6: timed REAP released by explicit reset/disconnect waitGeneration=%llu currentGeneration=%llu\n",(unsigned long long)reap_generation,(unsigned long long)g_p20_generation);return -1;}continue;}auto completed=g_p20_pending.front();
 const auto completion_now=std::chrono::steady_clock::now();
 const auto completion_late_us=std::chrono::duration_cast<std::chrono::microseconds>(completion_now-completed.ready).count();
 const size_t pending_before_reap=g_p20_pending.size();
 g_p20_pending.pop_front();
 if((completed.submit_id<=24||(completed.submit_id%2500)==0||completion_late_us>2000))if(p20_diag_enabled())std::fprintf(stderr,"[I] Phase20-B2.3.7: virtual DAC completion submit=%llu pendingBefore=%zu lateUs=%lld lockWaitUs=%llu lockWaitMaxUs=%llu lockWaitAvgUs=%.3f rate=%d generation=%llu session=%llu\n",(unsigned long long)completed.submit_id,pending_before_reap,(long long)completion_late_us,(unsigned long long)p20_lock_wait_us,(unsigned long long)g_p20_lock_wait_max_us,g_p20_lock_wait_samples?double(g_p20_lock_wait_sum_us)/double(g_p20_lock_wait_samples):0.0,g_p20_rate,(unsigned long long)g_p20_generation,(unsigned long long)g_p20_session_id);
 if(completed.generation!=g_p20_generation){if(completed.urb)completed.urb->status=-2;g_p20_stale_dropped++;continue;}const bool exact=completed.urb==completed.submit_urb;const bool current=g_p20_current_pool.count(completed.urb)!=0;const bool retired=g_p20_retired_pool.count(completed.urb)!=0;if(completed.submit_id<=24||(completed.submit_id%2500)==0||!exact)if(p20_diag_enabled())std::fprintf(stderr,"[I] Phase20-B2.3.4.8: REAP identity urb=%p submitUrb=%p buffer=%p submit=%llu exactMatch=%d current=%d retired=%d submitPoolEpoch=%llu currentPoolEpoch=%llu pending=%zu status=%d cancelled=%d\n",static_cast<void*>(completed.urb),completed.submit_urb,completed.submit_buffer,(unsigned long long)completed.submit_id,exact?1:0,current?1:0,retired?1:0,(unsigned long long)completed.pool_epoch,(unsigned long long)g_p20_pool_epoch,g_p20_pending.size(),completed.urb?completed.urb->status:0,(completed.urb&&completed.urb->status==-2)?1:0);if(!exact){if(completed.urb)completed.urb->status=-2;g_p20_stale_dropped++;continue;}const int reap_errno_before=*guest_errno();*guest_errno()=0;if(completed.submit_id<=24||(completed.submit_id%2500)==0)std::fprintf(stderr,"[I] Phase20-B2.3.4.9: REAP result urb=%p submit=%llu return=0 guestErrnoBefore=%d guestErrnoAfter=%d exactMatch=%d\n",completed.submit_urb,(unsigned long long)completed.submit_id,reap_errno_before,*guest_errno(),exact?1:0);*static_cast<void**>(arg)=completed.submit_urb;return 0;}}
 case kP20Discard:{
 auto* u=static_cast<P20Urb*>(arg);if(!u){*guest_errno()=14;return -1;}
 bool preserved=false,already_cancelled=false;uint64_t discarded_submit=0;
 for(auto& pending:g_p20_pending){
  if(pending.urb!=u)continue;
  discarded_submit=pending.submit_id;
  if(u->status==-2){already_cancelled=true;break;}
  u->status=-2;u->actual_length=0;u->error_count=0;
  if(u->number_of_packets>0&&u->number_of_packets<=1024){auto* iso=reinterpret_cast<P20Iso*>(reinterpret_cast<uint8_t*>(u)+56);for(int i=0;i<u->number_of_packets;i++){iso[i].actual_length=0;iso[i].status=-2;}}
  pending.ready=std::chrono::steady_clock::now();preserved=true;break;
 }
 const bool retired=g_p20_retired_pool.erase(u)!=0;const bool current=g_p20_current_pool.erase(u)!=0;
 if(!preserved&&!already_cancelled)u->status=-2;
 g_p20_cv.notify_all();fd_notify();
 if(p20_diag_enabled())std::fprintf(stderr,"[I] Phase20-B2.3.4.10: DISCARD completion preservation urb=%p submit=%llu preserved=%d alreadyCancelled=%d current=%d retired=%d pending=%zu status=%d\n",static_cast<void*>(u),(unsigned long long)discarded_submit,preserved?1:0,already_cancelled?1:0,current?1:0,retired?1:0,g_p20_pending.size(),u->status);
 const int discard_errno_before=*guest_errno();*guest_errno()=0;
 if(p20_diag_enabled())std::fprintf(stderr,"[I] Phase20-B2.3.4.10: DISCARD result urb=%p return=0 guestErrnoBefore=%d guestErrnoAfter=%d completionRetained=%d\n",static_cast<void*>(u),discard_errno_before,*guest_errno(),(preserved||already_cancelled)?1:0);
 return 0;}
 default:std::fprintf(stderr,"[E] Phase20-B2.2: unsupported usbfs op=0x%x\n",request);*guest_errno()=25;return -1;}}
}

WINAPK_GUEST_ABI int g_ioctl(int fd, int request, void* arg) {
    if (p20_is_fd(fd)) return p20_ioctl(fd,request,arg);
    if (auto o = fd_object(fd)) {
        if (request == 0x5421) {  // FIONBIO
            o->set_nonblocking(*static_cast<int*>(arg) != 0);
            return 0;
        }
        return o->ioctl(request, arg);
    }
    *guest_errno() = 25;  // ENOTTY
    return -1;
}

// ---- stat ----------------------------------------------------------------------------
// Bionic's struct stat on x86_64.
struct GuestStat {
    uint64_t st_dev, st_ino, st_nlink;
    uint32_t st_mode, st_uid, st_gid;
    int32_t pad0;
    uint64_t st_rdev;
    int64_t st_size, st_blksize, st_blocks;
    int64_t st_atime_sec, st_atime_nsec, st_mtime_sec, st_mtime_nsec, st_ctime_sec, st_ctime_nsec;
    int64_t unused[3];
};
static_assert(sizeof(GuestStat) == 144, "Bionic's struct stat on x86_64");

void fill_stat(const struct _stat64& s, GuestStat* g) {
    std::memset(g, 0, sizeof(*g));
    const bool dir = (s.st_mode & _S_IFMT) == _S_IFDIR;
    g->st_dev = s.st_dev;
    g->st_ino = s.st_ino;
    g->st_nlink = s.st_nlink ? s.st_nlink : 1;
    g->st_mode = dir ? (0040000 | 0771) : (0100000 | ((s.st_mode & _S_IWRITE) ? 0660 : 0440));
    g->st_size = s.st_size;
    g->st_blksize = 4096;
    g->st_blocks = (s.st_size + 511) / 512;
    g->st_atime_sec = s.st_atime;
    g->st_mtime_sec = s.st_mtime;
    g->st_ctime_sec = s.st_ctime;
}
WINAPK_GUEST_ABI int g_stat(const char* path, GuestStat* g) {
    struct _stat64 s;
    if (_wstat64(host_path(path).c_str(), &s) < 0) return fail_crt();
    fill_stat(s, g);
    return 0;
}
WINAPK_GUEST_ABI int g_fstat(int fd, GuestStat* g) {
    if (auto o = fd_object(fd)) {
        std::memset(g, 0, sizeof(*g));
        g->st_mode = o->mode() | 0600;
        g->st_nlink = 1;
        g->st_blksize = 4096;
        return 0;
    }
    struct _stat64 s;
    if (_fstat64(fd, &s) < 0) return fail_crt();
    fill_stat(s, g);
    return 0;
}
// memfd_create: an anonymous file. A temporary file deleted on close behaves
// the same for its uses (ftruncate, read/write, mmap of a private copy).
WINAPK_GUEST_ABI int g_memfd_create(const char*, unsigned) {
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir) || !GetTempFileNameW(dir, L"mfd", 0, path)) {
        set_errno_from_win32(GetLastError());
        return -1;
    }
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        set_errno_from_win32(GetLastError());
        return -1;
    }
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_RDWR | _O_BINARY);
    if (fd < 0) {
        CloseHandle(h);
        return fail_crt();
    }
    return fd;
}

// flock: LOCK_SH 1, LOCK_EX 2, LOCK_NB 4, LOCK_UN 8, on the whole file.
WINAPK_GUEST_ABI int g_flock(int fd, int op) {
    const HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (h == INVALID_HANDLE_VALUE) {
        *guest_errno() = kEBADF;
        return -1;
    }
    OVERLAPPED at{};
    if (op & 8) {
        UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &at);
        return 0;
    }
    DWORD flags = (op & 2) ? LOCKFILE_EXCLUSIVE_LOCK : 0;
    if (op & 4) flags |= LOCKFILE_FAIL_IMMEDIATELY;
    if (LockFileEx(h, flags, 0, MAXDWORD, MAXDWORD, &at)) return 0;
    const DWORD e = GetLastError();
    if (e == ERROR_LOCK_VIOLATION || e == ERROR_IO_PENDING) {
        *guest_errno() = kEAGAIN;  // EWOULDBLOCK
        return -1;
    }
    set_errno_from_win32(e);
    return -1;
}

// Bionic's struct statvfs (x86_64): eleven unsigned longs, then spare ints.
struct GuestStatvfs {
    uint64_t f_bsize, f_frsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree, f_favail, f_fsid, f_flag, f_namemax;
    uint32_t spare[6];
};
static_assert(sizeof(GuestStatvfs) == 112, "Bionic's struct statvfs on x86_64");
int statvfs_of(const std::wstring& path, GuestStatvfs* out) {
    ULARGE_INTEGER avail, total, free_bytes;
    BOOL ok = GetDiskFreeSpaceExW(path.c_str(), &avail, &total, &free_bytes);
    DWORD error = ok ? 0 : GetLastError();
    // GetDiskFreeSpaceExW takes directories only; Linux answers for the
    // filesystem of any path, a file's included.
    wchar_t root[MAX_PATH];
    if (!ok && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES &&
        GetVolumePathNameW(path.c_str(), root, MAX_PATH)) {
        ok = GetDiskFreeSpaceExW(root, &avail, &total, &free_bytes);
        if (!ok) error = GetLastError();
    }
    if (!ok) {
        set_errno_from_win32(error);
        return -1;
    }
    std::memset(out, 0, sizeof(*out));
    out->f_bsize = out->f_frsize = 4096;
    out->f_blocks = total.QuadPart / 4096;
    out->f_bfree = free_bytes.QuadPart / 4096;
    out->f_bavail = avail.QuadPart / 4096;
    out->f_files = out->f_ffree = out->f_favail = 1u << 20;  // NTFS has no inode limit to report
    out->f_namemax = 255;
    return 0;
}
WINAPK_GUEST_ABI int g_statvfs(const char* path, GuestStatvfs* out) { return statvfs_of(host_path(path), out); }
WINAPK_GUEST_ABI int g_fstatvfs(int fd, GuestStatvfs* out) {
    const HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    wchar_t path[MAX_PATH + 8];
    if (h == INVALID_HANDLE_VALUE || !GetFinalPathNameByHandleW(h, path, MAX_PATH, FILE_NAME_NORMALIZED)) {
        *guest_errno() = kEBADF;
        return -1;
    }
    // \\?\C:\dir\file -> its volume root.
    wchar_t root[MAX_PATH];
    if (!GetVolumePathNameW(path, root, MAX_PATH)) {
        set_errno_from_win32(GetLastError());
        return -1;
    }
    return statvfs_of(root, out);
}

// Bionic's struct statfs on x86_64, which is also its struct statfs64: the
// kernel's, where every field is a 64-bit __kernel_long_t except f_fsid (two
// ints). Windows has no statfs, so it is filled from statvfs_of's answer:
//
//   field       offset  size   from
//   f_type         0     8     ext4's magic (below)
//   f_bsize        8     8     f_bsize
//   f_blocks      16     8     f_blocks
//   f_bfree       24     8     f_bfree
//   f_bavail      32     8     f_bavail
//   f_files       40     8     f_files
//   f_ffree       48     8     f_ffree
//   f_fsid        56     8     0 (int[2])
//   f_namelen     64     8     f_namemax
//   f_frsize      72     8     f_frsize
//   f_flags       80     8     /data's mount flags (below)
//   f_spare       88    32     0 (long[4])
//   sizeof       120
//
// f_type and f_flags are what an Android /data reports, as upstream answers:
// EXT4_SUPER_MAGIC 0xef53, and ST_VALID | ST_NOSUID | ST_NODEV | ST_NOATIME.
struct GuestStatfs {
    uint64_t f_type, f_bsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    int32_t f_fsid[2];
    uint64_t f_namelen, f_frsize, f_flags, f_spare[4];
};
static_assert(sizeof(GuestStatfs) == 120 && offsetof(GuestStatfs, f_fsid) == 56 &&
                  offsetof(GuestStatfs, f_namelen) == 64 && offsetof(GuestStatfs, f_spare) == 88,
              "Bionic's struct statfs on x86_64");
int to_statfs(const GuestStatvfs& v, GuestStatfs* out) {
    std::memset(out, 0, sizeof(*out));
    out->f_type = 0xef53;
    out->f_bsize = v.f_bsize;
    out->f_blocks = v.f_blocks;
    out->f_bfree = v.f_bfree;
    out->f_bavail = v.f_bavail;
    out->f_files = v.f_files;
    out->f_ffree = v.f_ffree;
    out->f_namelen = v.f_namemax;
    out->f_frsize = v.f_frsize;
    out->f_flags = 0x0426;
    return 0;
}
WINAPK_GUEST_ABI int g_statfs(const char* path, GuestStatfs* out) {
    GuestStatvfs v;
    return statvfs_of(host_path(path), &v) < 0 ? -1 : to_statfs(v, out);
}
WINAPK_GUEST_ABI int g_fstatfs(int fd, GuestStatfs* out) {
    GuestStatvfs v;
    return g_fstatvfs(fd, &v) < 0 ? -1 : to_statfs(v, out);
}

// AT_FDCWD (-100) with a path, the common case of the *at() calls.
WINAPK_GUEST_ABI int g_fstatat(int dirfd, const char* path, GuestStat* g, int) {
    if (dirfd != -100 && path && path[0] != '/' && !(path[0] && path[1] == ':')) {
        *guest_errno() = kENOSYS;
        return -1;
    }
    return g_stat(path, g);
}
WINAPK_GUEST_ABI int g_access(const char* path, int mode) {
    // _waccess knows no X_OK (1); anything readable counts.
    return _waccess(host_path(path).c_str(), mode & 6) < 0 ? fail_crt() : 0;
}
WINAPK_GUEST_ABI int g_faccessat(int, const char* path, int mode, int) { return g_access(path, mode); }

// ---- the file system -------------------------------------------------------------------
WINAPK_GUEST_ABI int g_mkdir(const char* path, unsigned) { return _wmkdir(host_path(path).c_str()) < 0 ? fail_crt() : 0; }
WINAPK_GUEST_ABI int g_rmdir(const char* path) { return _wrmdir(host_path(path).c_str()) < 0 ? fail_crt() : 0; }
WINAPK_GUEST_ABI int g_unlink(const char* path) {
    // POSIX unlinks an open file; Windows needs it shared for delete, which
    // the CRT does not ask for, so fall back to delete-on-close semantics.
    const std::wstring w = host_path(path);
    if (DeleteFileW(w.c_str())) return 0;
    set_errno_from_win32(GetLastError());
    return -1;
}
WINAPK_GUEST_ABI int g_rename(const char* from, const char* to) {
    if (MoveFileExW(host_path(from).c_str(), host_path(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) return 0;
    set_errno_from_win32(GetLastError());
    return -1;
}
WINAPK_GUEST_ABI int g_remove(const char* path) {
    const std::wstring w = host_path(path);
    const DWORD a = GetFileAttributesW(w.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return g_rmdir(path);
    return g_unlink(path);
}
WINAPK_GUEST_ABI int g_chdir(const char* path) { return _wchdir(host_path(path).c_str()) < 0 ? fail_crt() : 0; }
WINAPK_GUEST_ABI char* g_getcwd(char* buf, size_t n) {
    wchar_t w[MAX_PATH * 4];
    if (!_wgetcwd(w, MAX_PATH * 4)) {
        fail_crt();
        return nullptr;
    }
    // POSIX code takes a directory that does not start with '/' for a relative
    // one and resolves it against getcwd() again: JUCE's File did that until
    // the stack overflowed (Spotify's libcomScore). So the directory is rooted,
    // "/C:/dir", which host_path() reads back as "C:/dir".
    std::string s = narrow(w);
    for (char& c : s) {
        if (c == '\\') c = '/';
    }
    if (s[0] != '/') s.insert(0, 1, '/');
    if (!buf) return _strdup(s.c_str());
    if (s.size() + 1 > n) {
        *guest_errno() = kERANGE;
        return nullptr;
    }
    std::memcpy(buf, s.c_str(), s.size() + 1);
    return buf;
}
WINAPK_GUEST_ABI char* g_realpath(const char* path, char* out) {
    wchar_t full[MAX_PATH * 4];
    if (!GetFullPathNameW(host_path(path).c_str(), MAX_PATH * 4, full, nullptr)) {
        set_errno_from_win32(GetLastError());
        return nullptr;
    }
    if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) {
        set_errno_from_win32(GetLastError());
        return nullptr;
    }
    const std::string s = narrow(full);
    if (!out) return _strdup(s.c_str());
    std::strncpy(out, s.c_str(), 4095);
    out[4095] = '\0';
    return out;
}
WINAPK_GUEST_ABI int g_chmod(const char*, unsigned) { return 0; }
WINAPK_GUEST_ABI int g_fchmod(int, unsigned) { return 0; }
WINAPK_GUEST_ABI unsigned g_umask(unsigned) { return 022; }
WINAPK_GUEST_ABI unsigned g_umask_chk(unsigned mode) { return g_umask(mode); }

// mkstemp/mkostemp: the template's trailing XXXXXX replaced until a new file
// can be created (O_EXCL), as bionic does; the path goes through g_open, so
// guest paths map as for any other open.
WINAPK_GUEST_ABI int g_mkostemp(char* tmpl, int flags) {
    const size_t n = tmpl ? std::strlen(tmpl) : 0;
    if (n < 6 || std::strcmp(tmpl + n - 6, "XXXXXX") != 0) return *guest_errno() = kEINVAL, -1;
    static const char kChars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    uint64_t seed = GetTickCount64() ^ (static_cast<uint64_t>(GetCurrentThreadId()) << 32);
    for (int attempt = 0; attempt < 100; ++attempt) {
        for (size_t i = n - 6; i < n; ++i) {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            tmpl[i] = kChars[(seed >> 33) % (sizeof kChars - 1)];
        }
        const int fd = g_open(tmpl, kORdWr | kOCreat | kOExcl | (flags & ~(kOCreat | kOExcl | 03)), 0600);
        if (fd >= 0 || *guest_errno() != 17) return fd;  // EEXIST: try another name
    }
    return -1;
}
WINAPK_GUEST_ABI int g_mkstemp(char* tmpl) { return g_mkostemp(tmpl, 0); }

// ---- directories -------------------------------------------------------------------------
// Bionic's struct dirent on x86_64.
struct GuestDirent {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[256];
};

struct GuestDir {
    HANDLE find = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAW data;
    bool pending = false;  // `data` holds an entry not handed out yet
    int64_t offset = 0;
    GuestDirent entry;
};

WINAPK_GUEST_ABI GuestDir* g_opendir(const char* path) {
    std::wstring pattern = host_path(path);
    if (!pattern.empty() && pattern.back() != L'/' && pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';
    auto* d = new GuestDir();
    d->find = FindFirstFileW(pattern.c_str(), &d->data);
    if (d->find == INVALID_HANDLE_VALUE) {
        set_errno_from_win32(GetLastError());
        delete d;
        return nullptr;
    }
    d->pending = true;
    return d;
}
WINAPK_GUEST_ABI GuestDirent* g_readdir(GuestDir* d) {
    if (!d) return nullptr;
    if (!d->pending) {
        if (!FindNextFileW(d->find, &d->data)) return nullptr;  // end: errno untouched
    }
    d->pending = false;
    std::memset(&d->entry, 0, sizeof(d->entry));
    const std::string name = narrow(d->data.cFileName);
    std::strncpy(d->entry.d_name, name.c_str(), sizeof(d->entry.d_name) - 1);
    d->entry.d_ino = static_cast<uint64_t>(++d->offset);
    d->entry.d_off = d->offset;
    d->entry.d_reclen = sizeof(GuestDirent);
    d->entry.d_type = (d->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 4 : 8;  // DT_DIR, DT_REG
    return &d->entry;
}
WINAPK_GUEST_ABI int g_closedir(GuestDir* d) {
    if (!d) return -1;
    if (d->find != INVALID_HANDLE_VALUE) FindClose(d->find);
    delete d;
    return 0;
}
WINAPK_GUEST_ABI void g_rewinddir(GuestDir*) {}

// ---- FILE streams ----------------------------------------------------------------------------
// Bionic's mode letters, less "e" (O_CLOEXEC); the CRT stream is always binary,
// as Linux's are.
std::wstring crt_mode(const char* mode) {
    std::wstring m;
    for (const char* p = mode; p && *p; ++p) {
        if (*p == 'e' || *p == 'b') continue;
        m.push_back(static_cast<wchar_t>(*p));
    }
    m.push_back(L'b');
    return m;
}

// fopen("/dev/urandom"): a guest FILE* is the CRT's, which cannot wrap an
// FdObject, so hand out a delete-on-close temporary file pre-filled from the
// system RNG. Entropy readers (mbedtls' platform poll, in WhatsApp's msys
// networking) open, read a few dozen bytes and close; without this their DRBG
// seed failed (-52) and the process aborted.
FILE* open_random_stream() {
    wchar_t dir[MAX_PATH + 1], name[MAX_PATH + 1];
    if (!GetTempPathW(MAX_PATH, dir) || !GetTempFileNameW(dir, L"rnd", 0, name)) {
        *guest_errno() = 5;  // EIO
        return nullptr;
    }
    FILE* f = _wfopen(name, L"w+bD");  // D: deleted when closed
    if (!f) {
        set_errno_from_crt(errno);
        DeleteFileW(name);
        return nullptr;
    }
    static constexpr size_t kBytes = 64 * 1024;
    std::unique_ptr<unsigned char[]> buf(new unsigned char[kBytes]);
    BCryptGenRandom(nullptr, buf.get(), static_cast<ULONG>(kBytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    std::fwrite(buf.get(), 1, kBytes, f);
    std::rewind(f);
    return f;
}
WINAPK_GUEST_ABI FILE* g_fopen(const char* path, const char* mode) {
    if (is_random_device(path)) return open_random_stream();
    if (is_proc_maps(path)) {
        const int fd = open_temp_with(proc_maps_text());
        if (fd < 0) return nullptr;
        FILE* f = _fdopen(fd, "rb");
        if (!f) {
            set_errno_from_crt(errno);
            _close(fd);
        }
        return f;
    }
    FILE* f = _wfopen(host_path(path).c_str(), crt_mode(mode).c_str());
    if (!f) set_errno_from_crt(errno);
    return f;
}
WINAPK_GUEST_ABI FILE* g_fdopen(int fd, const char* mode) {
    std::string m;
    for (const wchar_t c : crt_mode(mode)) m.push_back(static_cast<char>(c));
    FILE* f = _fdopen(fd, m.c_str());
    if (!f) set_errno_from_crt(errno);
    return f;
}
WINAPK_GUEST_ABI FILE* g_freopen(const char* path, const char* mode, void* f) {
    FILE* r = _wfreopen(host_path(path).c_str(), crt_mode(mode).c_str(), host_file(f));
    if (!r) set_errno_from_crt(errno);
    return r;
}
WINAPK_GUEST_ABI int g_fclose(void* f) { return std::fclose(host_file(f)) ? fail_crt() : 0; }
WINAPK_GUEST_ABI size_t g_fread(void* buf, size_t sz, size_t n, void* f) { return std::fread(buf, sz, n, host_file(f)); }
WINAPK_GUEST_ABI size_t g_fwrite(const void* buf, size_t sz, size_t n, void* f) {
    return std::fwrite(buf, sz, n, host_file(f));
}
WINAPK_GUEST_ABI int g_fputs(const char* s, void* f) { return std::fputs(s, host_file(f)); }
WINAPK_GUEST_ABI int g_fputc(int c, void* f) { return std::fputc(c, host_file(f)); }
WINAPK_GUEST_ABI int g_putchar(int c) { return std::fputc(c, stdout); }
WINAPK_GUEST_ABI char* g_fgets(char* s, int n, void* f) { return std::fgets(s, n, host_file(f)); }
WINAPK_GUEST_ABI char* g_fgets_chk(char* s, int n, void* f, size_t bn) {
    if (n < 0 || static_cast<size_t>(n) > bn) {
        std::fprintf(stderr, "[win.apk] guest FORTIFY: fgets: prevented buffer overflow\n");
        std::abort();
    }
    return g_fgets(s, n, f);
}
WINAPK_GUEST_ABI int g_fgetc(void* f) { return std::fgetc(host_file(f)); }
WINAPK_GUEST_ABI int g_ungetc(int c, void* f) { return std::ungetc(c, host_file(f)); }
WINAPK_GUEST_ABI int g_fflush(void* f) { return std::fflush(f ? host_file(f) : nullptr); }
WINAPK_GUEST_ABI int g_fseek(void* f, glong off, int whence) { return _fseeki64(host_file(f), off, whence) ? fail_crt() : 0; }
WINAPK_GUEST_ABI glong g_ftell(void* f) { return _ftelli64(host_file(f)); }
WINAPK_GUEST_ABI void g_rewind(void* f) { std::rewind(host_file(f)); }
WINAPK_GUEST_ABI int g_feof(void* f) { return std::feof(host_file(f)); }
WINAPK_GUEST_ABI int g_ferror(void* f) { return std::ferror(host_file(f)); }
WINAPK_GUEST_ABI void g_clearerr(void* f) { std::clearerr(host_file(f)); }
WINAPK_GUEST_ABI int g_fileno(void* f) { return _fileno(host_file(f)); }
WINAPK_GUEST_ABI int g_setvbuf(void* f, char* buf, int mode, size_t n) {
    // Linux: _IOFBF 0, _IOLBF 1, _IONBF 2.
    const int crt = mode == 2 ? _IONBF : mode == 1 ? _IOLBF : _IOFBF;
    return std::setvbuf(host_file(f), buf, crt, (crt == _IONBF || n >= 2) ? n : 2);
}
WINAPK_GUEST_ABI void g_setbuf(void* f, char* buf) { std::setbuf(host_file(f), buf); }
WINAPK_GUEST_ABI FILE* g_tmpfile() { return std::tmpfile(); }
WINAPK_GUEST_ABI void g_perror(const char* s) {
    std::fprintf(stderr, "%s%s%s\n", s ? s : "", s && *s ? ": " : "", errno_text(*guest_errno()));
}
// getline / getdelim: grow *line with the guest's allocator (the CRT's).
WINAPK_GUEST_ABI int64_t g_getdelim(char** line, size_t* cap, int delim, void* f) {
    FILE* h = host_file(f);
    size_t n = 0;
    for (;;) {
        const int c = std::fgetc(h);
        if (c == EOF) break;
        if (!*line || n + 2 > *cap) {
            const size_t grow = *cap ? *cap * 2 : 128;
            char* p = static_cast<char*>(std::realloc(*line, grow));
            if (!p) {
                *guest_errno() = kENOMEM;
                return -1;
            }
            *line = p;
            *cap = grow;
        }
        (*line)[n++] = static_cast<char>(c);
        if (c == delim) break;
    }
    if (n == 0) return -1;
    (*line)[n] = '\0';
    return static_cast<int64_t>(n);
}
WINAPK_GUEST_ABI int64_t g_getline(char** line, size_t* cap, void* f) { return g_getdelim(line, cap, '\n', f); }

const ShimSymbol kSymbols[] = {
    {"__sF", g_sF},
    {"stdin", &g_stdin_var},
    {"stdout", &g_stdout_var},
    {"stderr", &g_stderr_var},
    WINAPK_SHIM("open", g_open),
    WINAPK_SHIM("open64", g_open),
    WINAPK_SHIM("__open_2", g_open_2),
    WINAPK_SHIM("creat", g_creat),
    WINAPK_SHIM("close", g_close),
    WINAPK_SHIM("read", g_read),
    WINAPK_SHIM("__read_chk", g_read_chk),
    WINAPK_SHIM("__write_chk", g_write_chk),
    WINAPK_SHIM("write", g_write),
    WINAPK_SHIM("lseek", g_lseek),
    WINAPK_SHIM("lseek64", g_lseek),
    WINAPK_SHIM("pread", g_pread),
    WINAPK_SHIM("pread64", g_pread),
    WINAPK_SHIM("pwrite", g_pwrite),
    WINAPK_SHIM("pwrite64", g_pwrite),
    WINAPK_SHIM("dup", g_dup),
    WINAPK_SHIM("dup2", g_dup2),
    WINAPK_SHIM("dup3", g_dup3),
    WINAPK_SHIM("fsync", g_fsync),
    WINAPK_SHIM("fdatasync", g_fsync),
    WINAPK_SHIM("ftruncate", g_ftruncate),
    WINAPK_SHIM("ftruncate64", g_ftruncate),
    WINAPK_SHIM("isatty", g_isatty),
    WINAPK_SHIM("fcntl", g_fcntl),
    WINAPK_SHIM("ioctl", g_ioctl),
    WINAPK_SHIM("stat", g_stat),
    WINAPK_SHIM("stat64", g_stat),
    WINAPK_SHIM("lstat", g_stat),
    WINAPK_SHIM("lstat64", g_stat),
    WINAPK_SHIM("fstat", g_fstat),
    WINAPK_SHIM("fstat64", g_fstat),
    WINAPK_SHIM("fstatat", g_fstatat),
    WINAPK_SHIM("memfd_create", g_memfd_create),
    WINAPK_SHIM("flock", g_flock),
    WINAPK_SHIM("statvfs", g_statvfs),
    WINAPK_SHIM("fstatvfs", g_fstatvfs),
    WINAPK_SHIM("statfs", g_statfs),
    WINAPK_SHIM("statfs64", g_statfs),
    WINAPK_SHIM("fstatfs", g_fstatfs),
    WINAPK_SHIM("fstatfs64", g_fstatfs),
    WINAPK_SHIM("fstatat64", g_fstatat),
    WINAPK_SHIM("access", g_access),
    WINAPK_SHIM("faccessat", g_faccessat),
    WINAPK_SHIM("mkdir", g_mkdir),
    WINAPK_SHIM("rmdir", g_rmdir),
    WINAPK_SHIM("unlink", g_unlink),
    WINAPK_SHIM("rename", g_rename),
    WINAPK_SHIM("remove", g_remove),
    WINAPK_SHIM("chdir", g_chdir),
    WINAPK_SHIM("getcwd", g_getcwd),
    WINAPK_SHIM("realpath", g_realpath),
    WINAPK_SHIM("chmod", g_chmod),
    WINAPK_SHIM("fchmod", g_fchmod),
    WINAPK_SHIM("umask", g_umask),
    WINAPK_SHIM("__umask_chk", g_umask_chk),
    WINAPK_SHIM("mkstemp", g_mkstemp),
    WINAPK_SHIM("mkstemp64", g_mkstemp),
    WINAPK_SHIM("mkostemp", g_mkostemp),
    WINAPK_SHIM("mkostemp64", g_mkostemp),
    WINAPK_SHIM("opendir", g_opendir),
    WINAPK_SHIM("readdir", g_readdir),
    WINAPK_SHIM("readdir64", g_readdir),
    WINAPK_SHIM("closedir", g_closedir),
    WINAPK_SHIM("rewinddir", g_rewinddir),
    WINAPK_SHIM("fopen", g_fopen),
    WINAPK_SHIM("fopen64", g_fopen),
    WINAPK_SHIM("fdopen", g_fdopen),
    WINAPK_SHIM("freopen", g_freopen),
    WINAPK_SHIM("fclose", g_fclose),
    WINAPK_SHIM("fread", g_fread),
    WINAPK_SHIM("fwrite", g_fwrite),
    WINAPK_SHIM("fputs", g_fputs),
    WINAPK_SHIM("fputc", g_fputc),
    WINAPK_SHIM("putc", g_fputc),
    WINAPK_SHIM("putchar", g_putchar),
    WINAPK_SHIM("fgets", g_fgets),
    WINAPK_SHIM("__fgets_chk", g_fgets_chk),
    WINAPK_SHIM("fgetc", g_fgetc),
    WINAPK_SHIM("getc", g_fgetc),
    WINAPK_SHIM("ungetc", g_ungetc),
    WINAPK_SHIM("fflush", g_fflush),
    WINAPK_SHIM("fseek", g_fseek),
    WINAPK_SHIM("fseeko", g_fseek),
    WINAPK_SHIM("fseeko64", g_fseek),
    WINAPK_SHIM("ftell", g_ftell),
    WINAPK_SHIM("ftello", g_ftell),
    WINAPK_SHIM("ftello64", g_ftell),
    WINAPK_SHIM("rewind", g_rewind),
    WINAPK_SHIM("feof", g_feof),
    WINAPK_SHIM("ferror", g_ferror),
    WINAPK_SHIM("clearerr", g_clearerr),
    WINAPK_SHIM("fileno", g_fileno),
    WINAPK_SHIM("setvbuf", g_setvbuf),
    WINAPK_SHIM("setbuf", g_setbuf),
    WINAPK_SHIM("tmpfile", g_tmpfile),
    WINAPK_SHIM("perror", g_perror),
    WINAPK_SHIM("getdelim", g_getdelim),
    WINAPK_SHIM("getline", g_getline),
};

}  // namespace

extern "C" WINAPK_GUEST_ABI int winapk_phase20_usbfs_ioctl(int fd, int request, void* arg) {
    return g_ioctl(fd, request, arg);
}

size_t file_write(void* f, const char* data, size_t n) { return std::fwrite(data, 1, n, host_file(f)); }
int64_t fd_write(int fd, const void* data, size_t n) { return g_write(fd, data, n); }
void* guest_stdout() { return stdout; }
void* guest_stderr() { return stderr; }

ShimTable stdio_symbols() { return WINAPK_SHIM_TABLE(kSymbols); }

}  // namespace winapk::bionic
