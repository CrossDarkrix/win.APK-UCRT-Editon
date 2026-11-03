// runtime_main.cpp -- winapk-runtime, the Windows host that runs an APK.
//
// The Windows counterpart of runtime/src/main.cpp, in the same order:
//
//   1. Find the runtime files: <root>/lib holds the jars that
//      scripts/build_java.sh builds, <root>/jre (or JAVA_HOME) the JDK.
//   2. Convert the APK's DEX to <app dir>/classes.jar with upstream's
//      converter (com.macapk.dex.Main), in a child java.exe, unless the cached
//      jar was made from this APK by this converter (.macapk-dex-stamp).
//   3. Create the JVM in-process with upstream's options, on a thread with a
//      large stack (-Xss does not apply to the thread that creates the VM).
//   4. Bind the host natives (host_natives.h).
//   5. Call com.macapk.loader.Boot.run.
//
//   winapk-runtime --apk <file.apk> [--data <dir>] [--jdk <dir>] [--lib <dir>] [--install-only]
//   winapk-runtime --apk <app> --package-to <file>   (the launcher's install helper: the package name)
//   winapk-runtime --java-main --jdk <dir> --cp <jar> <class> [args...]   (internal: run_java)
//
// One runtime runs an app at a time, as on the Mac, where the app's stub holds
// a lock and a second launch brings the running app forward: a second start
// of the same app shows the running one's window and exits 0, and a second
// --install-only (an update while the app runs) exits 3 and changes nothing.
//
// --install-only stops after step 2 plus the launcher's metadata: the app's
// icon and name in <app dir>/icon.png and label.txt (com.winapk.tools.AppMeta,
// in a JVM with the host natives but no app running), which is what the
// launcher (launcher/) runs when an APK is added to the library.
//
// App data lives under %LOCALAPPDATA%\Win.apk unless --data says otherwise.
// There is no window yet (milestone M2); what the app does shows in the log.
//
// Paths may hold any character, whatever "Language for non-Unicode programs"
// is set to (a user name in Hebrew on an English system, say). The program
// uses the wide Windows calls for its own files, and its manifest
// (runtime.manifest) sets the process's ANSI code page to UTF-8 on Windows 10
// 1903 and later, which is how the JVM, which reads paths in that code page,
// gets them whole. Older systems get the paths in a form the code page can
// hold (jvm_path). The steps that run in a child JVM run in a child of this
// program (--java-main), not in java.exe, which reads its own location and
// its arguments in the ANSI code page.

#include "guest_libs.h"
#include "host_natives.h"
#include "user_folders.h"
#include "win32_window.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <jni.h>
#include <jvmti.h>
#include <windows.h>

#include "winapk/arm64_aot.h"
#include "winapk/mini_bionic.h"

namespace macapk::sqlite {
void RegisterNativeSqliteMethods(JNIEnv* env);  // runtime/src/native_sqlite.cpp
}

namespace fs = std::filesystem;

namespace {

struct Options {
    fs::path apk, data, jdk, lib;
    bool install_only = false;
    bool default_data = false;  // no --data: the user's own library, not a test's scratch folder
    fs::path package_to;  // --package-to: write the app's package name there and stop
};

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring env_var(const wchar_t* name) {
    wchar_t buf[32768];
    const DWORD n = GetEnvironmentVariableW(name, buf, 32768);
    return (n > 0 && n < 32768) ? std::wstring(buf, n) : std::wstring();
}

fs::path exe_dir() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 4);
    return fs::path(std::wstring(buf, n)).parent_path();
}

// `w` in the process's ANSI code page, when every character of it survives
// the round trip. Under the manifest's UTF-8 code page that is always so.
bool to_ansi(const std::wstring& w, std::string* out) {
    if (GetACP() == CP_UTF8) {
        *out = utf8(w);
        return true;
    }
    out->clear();
    if (w.empty()) return true;
    BOOL lossy = FALSE;
    const int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.data(), static_cast<int>(w.size()), nullptr,
                                      0, nullptr, &lossy);
    if (n <= 0 || lossy) return false;
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr,
                        &lossy);
    if (lossy) return false;
    *out = std::move(s);
    return true;
}

// A form of the existing path `p` that the ANSI code page can hold: `p`
// itself when it can, else its 8.3 short form (plain ASCII; NTFS has it
// unless the volume turned short names off). `p` when neither works, with
// *ok false.
fs::path ansi_safe(const fs::path& p, bool* ok = nullptr) {
    std::string bytes;
    if (ok) *ok = true;
    if (to_ansi(p.wstring(), &bytes)) return p;
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetShortPathNameW(p.c_str(), buf, MAX_PATH * 4);
    if (n > 0 && n < MAX_PATH * 4 && to_ansi(std::wstring(buf, n), &bytes)) return fs::path(std::wstring(buf, n));
    if (ok) *ok = false;
    return p;
}

// A path for a JVM option: HotSpot reads option strings, and opens the files
// they name, in the ANSI code page.
std::string jvm_path(const fs::path& p) {
    bool ok = true;
    const fs::path safe = ansi_safe(p, &ok);
    std::string bytes;
    if (ok && to_ansi(safe.wstring(), &bytes)) return bytes;
    std::fprintf(stderr,
                 "[win.apk] warning: %s has characters the ANSI code page (%u) cannot hold and no short name; "
                 "the JVM may not find it. Windows 10 1903 or later, or a folder with plain letters, avoids this.\n",
                 utf8(p.wstring()).c_str(), GetACP());
    return utf8(p.wstring());
}

// One argument for a command line, quoted the way CommandLineToArgvW and the
// C runtime split it again.
std::wstring quoted(const std::wstring& a) {
    std::wstring q = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        q.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        q += c;
    }
    q.append(slashes * 2, L'\\');
    return q + L"\"";
}
std::wstring quoted(const fs::path& p) { return quoted(p.wstring()); }

// Loads the JDK's jvm.dll. HotSpot finds java.dll and the rest of the JDK from
// jvm.dll's own path, which it reads in the ANSI code page, so the DLL is
// loaded by a path that code page can hold.
HMODULE load_jvm(const fs::path& jdk) {
    const fs::path home = ansi_safe(jdk);
    // jvm.dll needs the DLLs next to java.exe (the VC runtime among them).
    SetDllDirectoryW((home / "bin").c_str());
    HMODULE jvm = LoadLibraryW((home / "bin" / "server" / "jvm.dll").c_str());
    if (!jvm) std::fprintf(stderr, "[win.apk] cannot load jvm.dll (error %lu)\n", GetLastError());
    return jvm;
}

using CreateVmFn = jint(JNICALL*)(JavaVM**, void**, void*);

bool create_vm(HMODULE jvm, std::vector<std::string>& opts, JavaVM** vm, JNIEnv** env) {
    auto create = reinterpret_cast<CreateVmFn>(GetProcAddress(jvm, "JNI_CreateJavaVM"));
    if (!create) return false;
    std::vector<JavaVMOption> jopts(opts.size());
    for (size_t i = 0; i < opts.size(); ++i) {
        jopts[i].optionString = &opts[i][0];
        jopts[i].extraInfo = nullptr;
    }
    JavaVMInitArgs args{};
    args.version = JNI_VERSION_21;
    args.nOptions = static_cast<jint>(jopts.size());
    args.options = jopts.data();
    args.ignoreUnrecognized = JNI_FALSE;
    if (create(vm, reinterpret_cast<void**>(env), &args) != JNI_OK) {
        std::fprintf(stderr, "[win.apk] JNI_CreateJavaVM failed\n");
        return false;
    }
    return true;
}

// A Java string straight from UTF-16, so any character survives.
jstring jstring_of(JNIEnv* env, const std::wstring& w) {
    return env->NewString(reinterpret_cast<const jchar*>(w.data()), static_cast<jsize>(w.size()));
}

bool parse_args(int argc, wchar_t** argv, Options* o) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto value = [&](fs::path* out) {
            if (i + 1 >= argc) return false;
            *out = fs::path(argv[++i]);
            return true;
        };
        bool ok = true;
        if (a == L"--apk") ok = value(&o->apk);
        else if (a == L"--data") ok = value(&o->data);
        else if (a == L"--jdk") ok = value(&o->jdk);
        else if (a == L"--lib") ok = value(&o->lib);
        else if (a == L"--install-only") o->install_only = true;
        else if (a == L"--package-to") ok = value(&o->package_to);
        else ok = false;
        if (!ok) return false;
    }
    return !o->apk.empty();
}

// Fills in whatever the command line left out, and checks it exists.
bool resolve_paths(Options* o) {
    std::error_code ec;
    o->apk = fs::absolute(o->apk, ec);
    if (!fs::is_regular_file(o->apk, ec)) {
        std::fprintf(stderr, "[win.apk] no such APK: %s\n", utf8(o->apk.wstring()).c_str());
        return false;
    }
    const fs::path root = exe_dir().parent_path();
    if (o->lib.empty()) o->lib = root / "lib";
    if (!fs::is_regular_file(o->lib / "android-framework.jar", ec)) {
        std::fprintf(stderr, "[win.apk] android-framework.jar not found in %s (use --lib)\n",
                     utf8(o->lib.wstring()).c_str());
        return false;
    }
    if (o->jdk.empty()) {
        if (!env_var(L"WINAPK_JAVA_HOME").empty()) o->jdk = env_var(L"WINAPK_JAVA_HOME");
        else if (fs::exists(root / "jre" / "bin" / "server" / "jvm.dll", ec)) o->jdk = root / "jre";
        else o->jdk = env_var(L"JAVA_HOME");
    }
    if (!fs::exists(o->jdk / "bin" / "server" / "jvm.dll", ec)) {
        std::fprintf(stderr, "[win.apk] no JDK with bin\\server\\jvm.dll (use --jdk or JAVA_HOME)\n");
        return false;
    }
    if (o->data.empty()) {
        const std::wstring local = env_var(L"LOCALAPPDATA");
        if (local.empty()) {
            std::fprintf(stderr, "[win.apk] LOCALAPPDATA is not set (use --data)\n");
            return false;
        }
        o->data = fs::path(local) / L"Win.apk";
        o->default_data = true;
    }
    return true;
}

// Runs `main_class` from `jar` with `args` in a child JVM, as
// `java -cp <jar> <main_class> <args>` would, and returns its exit code, or -1
// if it did not start. The child is this program (--java-main), not the JDK's
// java.exe: java.exe finds java.dll from its own path and parses its
// arguments in the ANSI code page, so under a folder with characters outside
// it it stopped at "could not find java.dll", and an argument such as the
// data folder reached Java as "?". The child gets the arguments as UTF-16
// Java strings, and its own default heap, as java.exe did: the DEX converter
// needs more than the app's 1 GiB for large apps.
int run_java(const Options& o, const fs::path& jar, const wchar_t* main_class, const std::vector<std::wstring>& args) {
    wchar_t self[32768];
    const DWORD n = GetModuleFileNameW(nullptr, self, 32768);
    if (n == 0 || n >= 32768) return -1;
    std::wstring cmdline = quoted(std::wstring(self, n)) + L" --java-main --jdk " + quoted(o.jdk) + L" --cp " +
                           quoted(jar) + L" " + main_class;
    for (const std::wstring& a : args) cmdline += L" " + quoted(a);
    std::fflush(stderr);  // our lines first, then the child's
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(self, &cmdline[0], nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

// Runs a command line (a tool that is not Java: winapk-aot, clang) and returns
// its exit code, or -1 if it did not start.
int run_process(std::wstring cmdline) {
    std::fflush(stderr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmdline[0], nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

// ---- which converter made the cached code -----------------------------------
//
// <app dir>\.macapk-dex-stamp records what classes.jar was made from:
//
//   converter=<converter_id>   the converter (com/macapk/dex/ in macapk-loader.jar)
//   apk=<file_id>              the installed base.apk
//
// The jar is converted again when either line differs, and when there is no
// stamp (a jar from before the stamp, made by a converter nobody recorded,
// perhaps the very one an update fixed), so a converter fix reaches every
// app that was already converted. As upstream's CONVERTER_ID
// (runtime/scripts/build_ids.py), the id hashes the converter's entries'
// names, lengths and CRC-32s from the jar's central directory: a rebuild of
// the same sources, or a change to the framework or the loader only, keeps
// the jars. (After 5566brs/win.apk 67f7f16.) The name starts with ".macapk",
// so Boot's code-dir scan leaves the file here.

// "<size>:<last write time>" of a file, or "" when it cannot be read.
std::string file_id(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return {};
    const uint64_t size = static_cast<uint64_t>(a.nFileSizeHigh) << 32 | a.nFileSizeLow;
    const uint64_t time =
        static_cast<uint64_t>(a.ftLastWriteTime.dwHighDateTime) << 32 | a.ftLastWriteTime.dwLowDateTime;
    return std::to_string(size) + ":" + std::to_string(time);
}

uint32_t le16(const unsigned char* p) { return p[0] | p[1] << 8; }
uint32_t le32(const unsigned char* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }

// "<name> <length> <CRC-32>" of each file entry of `jar` under `prefix`, from
// the central directory (nothing is decompressed). False when the jar cannot
// be read that way (not a zip, or ZIP64).
bool jar_entries(const fs::path& jar, const std::string& prefix, std::vector<std::string>* rows) {
    FILE* f = _wfopen(jar.c_str(), L"rb");
    if (!f) return false;
    auto read_at = [f](long long at, size_t n, std::vector<unsigned char>* out) {
        out->resize(n);
        return _fseeki64(f, at, SEEK_SET) == 0 && std::fread(out->data(), 1, n, f) == n;
    };
    bool ok = false;
    const long long size = _fseeki64(f, 0, SEEK_END) == 0 ? _ftelli64(f) : -1;
    // The end of central directory record: 22 bytes, then up to 64 KB of comment.
    const long long tail = std::min<long long>(size, 22 + 0xFFFF);
    std::vector<unsigned char> end, dir;
    if (tail >= 22 && read_at(size - tail, static_cast<size_t>(tail), &end)) {
        for (size_t i = end.size() - 22 + 1; i-- > 0;) {
            if (le32(&end[i]) != 0x06054b50) continue;
            const uint32_t dir_size = le32(&end[i + 12]), dir_at = le32(&end[i + 16]);
            if (static_cast<long long>(dir_at) + dir_size > size - tail + static_cast<long long>(i) ||
                !read_at(dir_at, dir_size, &dir)) {
                break;
            }
            size_t p = 0;
            while (p + 46 <= dir.size() && le32(&dir[p]) == 0x02014b50) {
                const size_t name_len = le16(&dir[p + 28]);
                const size_t next = p + 46 + name_len + le16(&dir[p + 30]) + le16(&dir[p + 32]);
                if (next > dir.size()) break;
                const std::string name(reinterpret_cast<const char*>(&dir[p + 46]), name_len);
                if (name.rfind(prefix, 0) == 0 && name.back() != '/') {
                    rows->push_back(name + ' ' + std::to_string(le32(&dir[p + 24])) + ' ' +
                                    std::to_string(le32(&dir[p + 16])));
                }
                p = next;
            }
            ok = p == dir.size();
            break;
        }
    }
    std::fclose(f);
    return ok;
}

// "dex:" and an FNV-1a hash of the converter's entries, or "jar:<file_id>"
// when the jar cannot be read so.
std::string converter_id(const fs::path& loader_jar) {
    std::vector<std::string> rows;
    if (!jar_entries(loader_jar, "com/macapk/dex/", &rows) || rows.empty()) return "jar:" + file_id(loader_jar);
    std::sort(rows.begin(), rows.end());
    uint64_t h = 1469598103934665603ull;
    for (const std::string& r : rows) {
        for (const char c : r + '\n') h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    }
    char id[24];
    std::snprintf(id, sizeof id, "dex:%016llx", static_cast<unsigned long long>(h));
    return id;
}

struct DexStamp {
    std::string converter, apk;
};

bool read_dex_stamp(const fs::path& path, DexStamp* s) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    std::string text;
    char buf[512];
    for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
    std::fclose(f);
    for (size_t i = 0; i < text.size();) {
        const size_t j = std::min(text.find('\n', i), text.size());
        const std::string line = text.substr(i, j - i);
        if (line.rfind("converter=", 0) == 0) s->converter = line.substr(10);
        else if (line.rfind("apk=", 0) == 0) s->apk = line.substr(4);
        i = j + 1;
    }
    return !s->converter.empty() && !s->apk.empty();
}

// To a temporary name, then renamed: the stamp is whole or absent.
bool write_dex_stamp(const fs::path& path, const DexStamp& s) {
    std::error_code ec;
    const fs::path tmp = path.wstring() + L".tmp";
    const std::string text = "converter=" + s.converter + "\napk=" + s.apk + "\n";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    const bool written = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    if (std::fclose(f) != 0 || !written) {
        fs::remove(tmp, ec);
        return false;
    }
    fs::rename(tmp, path, ec);
    return !ec;
}

// Step 2. Exit codes are com.macapk.dex.Main's: 0 converted, 3 no DEX in the
// APK (a valid case: the app then has no classes.jar). A failed conversion
// keeps the cached jar when only the converter changed: the app then runs as
// before the update, and the next start converts again.
bool convert_dex(const Options& o, const fs::path& app_dir, fs::path* jar) {
    std::error_code ec;
    *jar = app_dir / "classes.jar";
    const fs::path stamp = app_dir / ".macapk-dex-stamp";
    const DexStamp want{converter_id(o.lib / "macapk-loader.jar"), file_id(o.apk)};
    DexStamp have;
    const bool stamped = read_dex_stamp(stamp, &have);
    const bool cached = fs::exists(*jar, ec);
    const char* why = !cached                            ? "once per install"
                      : !stamped                         ? "the cached copy has no converter stamp"
                      : have.converter != want.converter ? "the converter changed"
                      : have.apk != want.apk             ? "the app changed"
                                                         : nullptr;
    if (!why) return true;
    std::fprintf(stderr, "[win.apk] converting the app's code (%s)...\n", why);
    const fs::path tmp = app_dir / "classes.jar.tmp";
    const int rc = run_java(o, o.lib / "macapk-loader.jar", L"com.macapk.dex.Main", {o.apk.wstring(), tmp.wstring()});
    if (rc == 3) {
        jar->clear();
        return true;
    }
    if (rc == 0) {
        fs::rename(tmp, *jar, ec);
        if (!ec) {
            if (!write_dex_stamp(stamp, want)) {
                std::fprintf(stderr, "[win.apk] warning: cannot write %s; the next start converts the code again\n",
                             utf8(stamp.wstring()).c_str());
            }
            return true;
        }
        std::fprintf(stderr, "[win.apk] DEX conversion failed: cannot replace %s (%s)\n", utf8(jar->wstring()).c_str(),
                     ec.message().c_str());
        fs::remove(tmp, ec);
    } else {
        std::fprintf(stderr, "[win.apk] DEX conversion failed (exit code %d)\n", rc);
    }
    const bool same_apk = cached && (stamped ? have.apk == want.apk
                                             : fs::last_write_time(*jar, ec) >= fs::last_write_time(o.apk, ec));
    if (!same_apk) return false;
    std::fprintf(stderr, "[win.apk] WARNING: the app's code was NOT converted again; it runs on the previous "
                         "converter's copy, and the next start converts again\n");
    return true;
}

// Step 1: install the app the way a device's installer does, into
// <android root>\data\app\~~<nonce>\<package>-<nonce>: base.apk (a hard link
// to a plain APK, or unpacked from a bundle) plus split_*.apk, and the native
// libraries of this host's ABI under lib\<isa>. Upstream's runtime and its DEX
// converter recognize an installed app only in that layout
// (com.winapk.tools.AppInstall). Done once per input file; a small record next
// to the installs names the result. Afterwards the app is the installed
// base.apk.
//
// WINAPK_ABIS overrides the ABI list (WINAPK_ABIS=arm64-v8a runs an app's
// arm64 build although it has an x86_64 one, for comparing the two); an
// override gets its own install.
bool install_app(Options* o, const fs::path& android_root) {
    std::error_code ec;
    const fs::path apps = android_root / "data" / "app";
    fs::create_directories(apps, ec);
    std::wstring abis = env_var(L"WINAPK_ABIS");
    const std::wstring tag = abis.empty() ? L"" : L"." + abis;
    if (abis.empty()) abis = L"x86_64,arm64-v8a";
    const fs::path record = apps / (L"." + o->apk.stem().wstring() + tag + L".installed");
    auto installed = [&]() -> fs::path {
        if (!fs::exists(record, ec) || fs::last_write_time(record, ec) < fs::last_write_time(o->apk, ec)) return {};
        FILE* f = _wfopen(record.c_str(), L"rb");
        if (!f) return {};
        std::string path;
        char buf[1024];
        for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) path.append(buf, n);
        std::fclose(f);
        const int len = MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(len > 0 ? len : 0), L'\0');
        if (len > 0) MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), &w[0], len);
        const fs::path base(w);
        return fs::exists(base, ec) ? base : fs::path();
    };
    fs::path base = installed();
    if (base.empty()) {
        std::fprintf(stderr, "[win.apk] installing the app (once per install)...\n");
        const int rc = run_java(*o, o->lib / "winapk-tools.jar", L"com.winapk.tools.AppInstall",
                                {o->apk.wstring(), android_root.wstring(), abis, record.wstring()});
        base = rc == 0 ? installed() : fs::path();
        if (base.empty()) {
            std::fprintf(stderr, "[win.apk] installing the app failed (exit code %d)\n", rc);
            return false;
        }
    }
    o->apk = base;
    return true;
}

// ---- arm64 translation, in the background ------------------------------------
//
// An app's arm64 libraries are translated ahead of time
// (include/winapk/arm64_aot.h): winapk-aot writes C, clang compiles it for
// this CPU into <lib>.so.aot.dll, which the loader picks up. Compiling a large
// library whole took most of an hour (libwaze.so, 1.5M instructions), and the
// app waited for it. Now nothing waits: the app starts on the JIT while a
// background process (this program, --aot-translate) does the work, at below
// normal priority, the C split in parts that clang compiles on all but one
// core at -O1 (a quarter faster than -O2 for code about as good).
// A library of up to kAotFullBelow instructions is translated whole; a larger
// one only where its profile says code ran (arm64_aot.h, "Profiles"), and
// again when the profile has grown by a tenth. A translation finished while
// the app runs is loaded at once (aot_load_pending); one that replaces a DLL
// in use waits as <lib>.so.aot.dll.new for the next start.
//
// <lib>.so.aot.stamp says what the last translation used: "full",
// "profile <lines>" (the profile's line count then), "wait <lines>" (too
// large without a profile, or failed: tried again when the profile grows),
// "skip" (not an arm64 ELF file: packed or encrypted, loaded by the app's
// own code), or "bad" (its translation crashed: the loader marks it, and the
// JIT runs the library; see arm64_aot.cpp). A DLL without a stamp is a whole translation from before stamps.
//
// Many apps ship the same libraries (libc++_shared.so, libcrashlytics.so,
// SQLite, ...). As Windows on ARM's translation cache and Box64's DynaCache
// do, a whole translation is kept in <data>\aot-cache under a key of the
// library's code (the text hash the loader checks), winapk-aot and clang
// (each by size and date) and the flags, and an app with the same library
// copies it instead of compiling. Translations made from a profile differ by
// app and are not kept. The cache keeps to kAotCacheBytes, oldest use first.

constexpr size_t kAotFullBelow = 400000;
constexpr uintmax_t kAotCacheBytes = uintmax_t{2} << 30;
const wchar_t* const kAotCompileFlags = L"-O1 -march=native -fno-strict-aliasing -w";

fs::path aot_clang() {
    std::error_code ec;
    const fs::path set = env_var(L"WINAPK_AOT_CLANG");
    if (!set.empty() && fs::is_regular_file(set, ec)) return set;
    const fs::path bundled = exe_dir().parent_path() / "clang" / "bin" / "clang.exe";
    if (fs::is_regular_file(bundled, ec)) return bundled;
    wchar_t found[MAX_PATH * 4];
    if (SearchPathW(nullptr, L"clang.exe", nullptr, MAX_PATH * 4, found, nullptr) > 0) return found;
    return {};
}

fs::path arm64_libs(const fs::path& base) { return base.parent_path() / "lib" / "arm64"; }

fs::path with_suffix(const fs::path& so, const wchar_t* suffix) { return so.wstring() + suffix; }

size_t profile_lines(const fs::path& so) {
    FILE* f = _wfopen(with_suffix(so, L".aot.profile").c_str(), L"rb");
    if (!f) return 0;
    size_t lines = 0;
    char buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) lines += std::count(buf, buf + n, '\n');
    std::fclose(f);
    return lines > 0 ? lines - 1 : 0;  // the header
}

struct AotStamp {
    std::string kind;  // "full", "profile", "wait", or "" (none)
    size_t lines = 0;
};

AotStamp read_aot_stamp(const fs::path& so) {
    AotStamp s;
    FILE* f = _wfopen(with_suffix(so, L".aot.stamp").c_str(), L"rb");
    if (!f) return s;
    char kind[16] = {};
    unsigned long long lines = 0;
    if (std::fscanf(f, "%15s %llu", kind, &lines) >= 1) {
        s.kind = kind;
        s.lines = static_cast<size_t>(lines);
    }
    std::fclose(f);
    return s;
}

void write_aot_stamp(const fs::path& so, const char* kind, size_t lines) {
    if (FILE* f = _wfopen(with_suffix(so, L".aot.stamp").c_str(), L"wb")) {
        std::fprintf(f, "%s %zu\n", kind, lines);
        std::fclose(f);
    }
}

bool newer_than(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    return fs::exists(a, ec) && fs::last_write_time(a, ec) >= fs::last_write_time(b, ec);
}

// Whether `so` needs a (new) translation now.
bool aot_needs_work(const fs::path& so) {
    const bool have = newer_than(with_suffix(so, L".aot.dll"), so) || newer_than(with_suffix(so, L".aot.dll.new"), so);
    const AotStamp stamp = read_aot_stamp(so);
    if (stamp.kind == "skip" || stamp.kind == "bad") return !newer_than(with_suffix(so, L".aot.stamp"), so);
    auto grown = [&] {
        const size_t p = profile_lines(so);
        return p >= stamp.lines + std::max<size_t>(200, stamp.lines / 10);
    };
    if (!have) return stamp.kind != "wait" || grown();
    if (stamp.kind.empty() || stamp.kind == "full") return false;
    return grown();
}

std::vector<fs::path> arm64_sos(const fs::path& libs) {
    std::vector<fs::path> sos;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(libs, ec)) {
        if (e.path().extension() == L".so") sos.push_back(e.path());
    }
    // Small ones first: they finish soonest.
    std::sort(sos.begin(), sos.end(), [](const fs::path& a, const fs::path& b) {
        std::error_code e1, e2;
        return fs::file_size(a, e1) < fs::file_size(b, e2);
    });
    return sos;
}

// Runs `cmds` (command lines), at most `jobs` at a time, below normal
// priority; true when every one exited 0.
bool run_parallel(const std::vector<std::wstring>& cmds, size_t jobs) {
    std::fflush(stderr);
    std::vector<HANDLE> running;
    bool ok = true;
    auto reap = [&](DWORD timeout) {
        const DWORD w = WaitForMultipleObjects(static_cast<DWORD>(running.size()), running.data(), FALSE, timeout);
        if (w >= WAIT_OBJECT_0 + running.size()) return;
        HANDLE h = running[w - WAIT_OBJECT_0];
        DWORD code = 1;
        GetExitCodeProcess(h, &code);
        ok = ok && code == 0;
        CloseHandle(h);
        running.erase(running.begin() + (w - WAIT_OBJECT_0));
    };
    for (std::wstring cmd : cmds) {
        while (running.size() >= std::min<size_t>(jobs, MAXIMUM_WAIT_OBJECTS)) reap(INFINITE);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, BELOW_NORMAL_PRIORITY_CLASS | CREATE_NO_WINDOW,
                            nullptr, nullptr, &si, &pi)) {
            ok = false;
            continue;
        }
        CloseHandle(pi.hThread);
        running.push_back(pi.hProcess);
    }
    while (!running.empty()) reap(INFINITE);
    return ok;
}

// Runs `cmdline` and returns its exit code (-1 if it did not start), with
// its standard output in *out.
int run_capture(std::wstring cmdline, std::string* out) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 0)) return -1;
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    const BOOL started =
        CreateProcessW(nullptr, &cmdline[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(write);
    if (!started) {
        CloseHandle(read);
        return -1;
    }
    char buf[4096];
    for (DWORD n; ReadFile(read, buf, sizeof buf, &n, nullptr) && n > 0;) out->append(buf, n);
    CloseHandle(read);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

// The shared cache's name for a whole translation of `so`, or "" when `so`
// is not an arm64 ELF file winapk-aot can read.
std::string aot_cache_key(const fs::path& so, const fs::path& clang) {
    std::string hash;
    if (run_capture(quoted(exe_dir() / "winapk-aot.exe") + L" --hash " + quoted(so), &hash) != 0) return {};
    while (!hash.empty() && std::isspace(static_cast<unsigned char>(hash.back()))) hash.pop_back();
    const std::string id = hash + "|" + file_id(exe_dir() / "winapk-aot.exe") + "|" + file_id(clang) + "|" +
                           utf8(kAotCompileFlags) + "|" + utf8(env_var(L"WINAPK_AOT_OPTNONE_INSNS"));
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char ch : id) h = (h ^ ch) * 1099511628211ull;
    char key[48];
    std::snprintf(key, sizeof key, "%016llx", static_cast<unsigned long long>(h));
    return key;
}

// Keeps the cache within kAotCacheBytes, dropping the least recently used.
void aot_cache_trim(const fs::path& cache) {
    std::error_code ec;
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    uintmax_t total = 0;
    for (const auto& e : fs::directory_iterator(cache, ec)) {
        if (!e.is_regular_file(ec)) continue;
        total += e.file_size(ec);
        files.emplace_back(e.last_write_time(ec), e.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
        if (total <= kAotCacheBytes) break;
        const uintmax_t size = fs::file_size(f.second, ec);
        if (fs::remove(f.second, ec)) total -= size;
    }
}

// Puts a finished translation in place, unless the running app has the old
// one loaded: then for the next start.
bool install_aot_dll(const fs::path& dll, const fs::path& so) {
    const fs::path target = with_suffix(so, L".aot.dll"), later = with_suffix(so, L".aot.dll.new");
    return MoveFileExW(dll.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING) ||
           MoveFileExW(dll.c_str(), later.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// Translates one library: from the shared cache if it is there, else C from
// winapk-aot in parts, the parts compiled in parallel, linked into a DLL,
// put in place (and, when whole, into the cache).
void aot_translate_one(const fs::path& so, const fs::path& clang, size_t jobs, const fs::path& cache) {
    std::error_code ec;
    const fs::path tmp = so.parent_path() / ".aot-tmp";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    const fs::path c = tmp / (so.filename().wstring() + L".c");
    const size_t lines = profile_lines(so);
    const std::string name = utf8(so.filename().wstring());
    const ULONGLONG t0 = GetTickCount64();
    const std::string key = aot_cache_key(so, clang);
    if (key.empty()) {
        std::fprintf(stderr, "[win.apk] %s is not an arm64 ELF file (packed or encrypted; the app loads it "
                             "itself): the JIT runs its code\n", name.c_str());
        write_aot_stamp(so, "skip", 0);
        fs::remove_all(tmp, ec);
        return;
    }
    const fs::path cached = cache.empty() ? fs::path() : cache / (key + ".aot.dll");
    const fs::path dll = tmp / (so.filename().wstring() + L".aot.dll");
    if (!cached.empty() && fs::exists(cached, ec) && CopyFileW(cached.c_str(), dll.c_str(), FALSE) &&
        install_aot_dll(dll, so)) {
        fs::last_write_time(cached, fs::file_time_type::clock::now(), ec);  // recently used
        write_aot_stamp(so, "full", lines);
        fs::remove_all(tmp, ec);
        std::fprintf(stderr, "[win.apk] %s: translation taken from the shared cache\n", name.c_str());
        return;
    }
    std::fprintf(stderr, "[win.apk] translating %s for this CPU, in the background...\n", name.c_str());
    const int rc = run_process(quoted(exe_dir() / "winapk-aot.exe") + L" " + quoted(so) + L" " + quoted(c) + L" " +
                               std::to_wstring(jobs) + L" --profile " + quoted(with_suffix(so, L".aot.profile")) +
                               L" --full-below " + std::to_wstring(kAotFullBelow));
    if (rc == 3) {
        std::fprintf(stderr, "[win.apk] %s: too large to translate whole; waiting for its profile\n", name.c_str());
        write_aot_stamp(so, "wait", lines);
        fs::remove_all(tmp, ec);
        return;
    }
    bool ok = rc == 0 || rc == 4;
    std::vector<fs::path> objs;
    if (ok) {
        std::vector<std::wstring> compiles;
        for (int i = 0;; ++i) {
            const fs::path src = i == 0 ? c : fs::path(c.wstring() + L"." + std::to_wstring(i) + L".c");
            if (!fs::exists(src, ec)) break;
            objs.push_back(src.wstring() + L".o");
            compiles.push_back(quoted(clang) + L" " + kAotCompileFlags + L" -c -o " + quoted(objs.back()) + L" " +
                               quoted(src));
        }
        ok = run_parallel(compiles, jobs);
    }
    if (ok) {
        std::wstring link = quoted(clang) + L" -shared -o " + quoted(dll);
        for (const fs::path& o : objs) link += L" " + quoted(o);
        ok = run_process(link) == 0;
    }
    if (ok && rc == 0 && !cached.empty()) {  // whole: for every app with this library
        fs::create_directories(cache, ec);
        const fs::path part = cached.wstring() + L".tmp";
        if (CopyFileW(dll.c_str(), part.c_str(), FALSE) &&
            MoveFileExW(part.c_str(), cached.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            aot_cache_trim(cache);
        }
    }
    ok = ok && install_aot_dll(dll, so);
    fs::remove_all(tmp, ec);
    if (!ok) {
        std::fprintf(stderr, "[win.apk] %s: translation failed (winapk-aot exit code %d); the JIT runs it\n",
                     name.c_str(), rc);
        write_aot_stamp(so, "wait", lines);
        return;
    }
    write_aot_stamp(so, rc == 4 ? "profile" : "full", lines);
    std::fprintf(stderr, "[win.apk] %s: translated%s in %.1f s\n", name.c_str(), rc == 4 ? " (what ran)" : "",
                 static_cast<double>(GetTickCount64() - t0) / 1000);
}

std::wstring aot_mutex_name(const fs::path& libs) {
    std::wstring key = libs.lexically_normal().wstring();
    for (wchar_t& ch : key) ch = static_cast<wchar_t>(towlower(ch == L'/' ? L'\\' : ch));
    uint64_t h = 1469598103934665603ull;
    for (const wchar_t ch : key) h = (h ^ static_cast<uint16_t>(ch)) * 1099511628211ull;
    wchar_t name[48];
    swprintf(name, 48, L"Local\\WinApk-aot-%016llx", static_cast<unsigned long long>(h));
    return name;
}

// winapk-runtime --aot-translate <lib\arm64 dir> [--cache <dir>]: the
// background worker. One per app; passes until nothing is left to do (the
// profile can grow while it works).
int aot_translate_main(const fs::path& libs, const fs::path& cache) {
    HANDLE m = CreateMutexW(nullptr, FALSE, aot_mutex_name(libs).c_str());
    if (m && WaitForSingleObject(m, 0) == WAIT_TIMEOUT) return 0;  // another one is at it
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    const fs::path clang = aot_clang();
    if (clang.empty()) {
        std::fprintf(stderr, "[win.apk] no clang found: arm64 code runs on the JIT\n");
        return 1;
    }
    const size_t jobs = std::max<size_t>(1, std::thread::hardware_concurrency() - 1);
    for (int pass = 0; pass < 8; ++pass) {
        bool worked = false;
        for (const fs::path& so : arm64_sos(libs)) {
            if (!aot_needs_work(so)) continue;
            aot_translate_one(so, clang, jobs, cache);
            worked = true;
        }
        if (!worked) break;
    }
    return 0;
}

// Starts the worker for `libs` unless one is running; its output goes to
// `log` (appended).
void start_aot_worker(const fs::path& libs, const fs::path& log, const fs::path& cache) {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, aot_mutex_name(libs).c_str());
    if (m) {  // a worker holds it, or has just let go: either way it rescans
        CloseHandle(m);
        return;
    }
    std::error_code ec;
    fs::create_directories(log.parent_path(), ec);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out = CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return;
    // The log is the only handle it gets: the app's own (the launcher's pipe
    // for last-run.log) must not outlive the app in it.
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<uint8_t> attrs(size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrs.data());
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = si.StartupInfo.hStdError = out;
    if (InitializeProcThreadAttributeList(list, 1, 0, &size) &&
        UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &out, sizeof out, nullptr, nullptr)) {
        si.lpAttributeList = list;
    }
    std::wstring cmd = quoted(exe_dir() / "winapk-runtime.exe") + L" --aot-translate " + quoted(libs) + L" --cache " +
                       quoted(cache);
    const DWORD flags = BELOW_NORMAL_PRIORITY_CLASS | CREATE_NO_WINDOW |
                        (si.lpAttributeList ? EXTENDED_STARTUPINFO_PRESENT : 0);
    PROCESS_INFORMATION pi{};
    // Out of the launcher's job, if it has one, so the work goes on after the app.
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, flags | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr,
                       &si.StartupInfo, &pi) ||
        CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, flags, nullptr, nullptr, &si.StartupInfo, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    if (si.lpAttributeList) DeleteProcThreadAttributeList(list);
    CloseHandle(out);
}

bool any_aot_work(const fs::path& libs) {
    for (const fs::path& so : arm64_sos(libs)) {
        if (aot_needs_work(so)) return true;
    }
    return false;
}

struct AotWatch {
    fs::path libs, log, cache;
};

// While the app runs: writes the JIT's profile, starts the worker when it
// has grown, and loads translations as they are finished.
DWORD WINAPI aot_watch_thread(void* param) {
    const AotWatch w = *static_cast<AotWatch*>(param);
    delete static_cast<AotWatch*>(param);
    // Every 2 s for the first half minute (a flush with nothing new writes
    // nothing): an app that stops early (Waze, ten seconds in) still leaves
    // the profile of what ran. Then every 30 s.
    for (int tick = 0;; ++tick) {
        Sleep(tick < 15 ? 2000 : 30000);
        if (winapk::arm64::aot_profile_flush() > 0 && any_aot_work(w.libs)) start_aot_worker(w.libs, w.log, w.cache);
        winapk::arm64::aot_load_pending();
    }
}

// Step 1b, before the app's libraries load: puts in place what the worker
// finished while the app was not running, starts it for what is left, and
// watches the profile while the app runs. Never waits for a translation.
void prepare_arm64(const Options& o, const fs::path& app_dir) {
    std::error_code ec;
    const fs::path libs = arm64_libs(o.apk);
    if (!fs::is_directory(libs, ec) || !fs::exists(exe_dir() / "winapk-aot.exe", ec)) return;
    if (aot_clang().empty()) {
        std::fprintf(stderr, "[win.apk] no clang found (WINAPK_AOT_CLANG, clang\\bin\\clang.exe in Win.apk's folder, "
                             "or the PATH): the app's arm64 code runs on the JIT, which is slower\n");
        return;
    }
    for (const fs::path& so : arm64_sos(libs)) {
        const fs::path later = with_suffix(so, L".aot.dll.new");
        if (fs::exists(later, ec) &&
            !MoveFileExW(later.c_str(), with_suffix(so, L".aot.dll").c_str(), MOVEFILE_REPLACE_EXISTING)) {
            std::fprintf(stderr, "[win.apk] cannot put %s in place (error %lu)\n", utf8(later.wstring()).c_str(),
                         GetLastError());
        }
    }
    const fs::path log = o.data / "logs" / (L"aot-" + app_dir.filename().wstring() + L".log");
    const fs::path cache = o.data / "aot-cache";
    if (any_aot_work(libs)) {
        std::fprintf(stderr, "[win.apk] arm64 code is being translated in the background (log: %s); "
                             "until then the JIT runs it\n", utf8(log.wstring()).c_str());
        start_aot_worker(libs, log, cache);
    }
    if (!o.install_only) {
        if (HANDLE t = CreateThread(nullptr, 0, aot_watch_thread, new AotWatch{libs, log, cache}, 0, nullptr)) CloseHandle(t);
    }
}

struct BootArgs {
    Options options;
    fs::path app_dir, app_jar;
    int result = 1;
};

// The display density, as upstream derives it from the Mac screen's point
// scale (Retina 2x = 320 dpi): the Windows scale of the monitor the app opens
// on x 160, so 100% is 160 (mdpi), 150% is 240 (hdpi), 200% is 320 (xhdpi).
// When the whole window would not fit that monitor's work area, the scale is
// lowered until it does (win32_window.h, window_plan): the app keeps its dp
// size and draws at exactly the window's pixel size, so nothing is stretched
// or shrunk. Headless runs (tests, CI) keep upstream's 320 so their frames
// stay 720x1280; WINAPK_DENSITY (a dpi) overrides both.
int display_density_dpi() {
    const std::wstring forced = env_var(L"WINAPK_DENSITY");
    if (!forced.empty()) return _wtoi(forced.c_str());
    if (!env_var(L"WINAPK_HEADLESS").empty()) return 320;
    int w = 0, h = 0;
    winapk::host::launch_phone_size(&w, &h);
    return winapk::host::window_density_dpi(winapk::host::window_plan(w, h));
}

std::vector<std::string> jvm_options(const BootArgs& b) {
    const Options& o = b.options;
    std::error_code ec;
    std::vector<std::string> v;
    v.push_back("-Djava.class.path=" + jvm_path(o.lib / "android-framework.jar") + ";" +
                jvm_path(o.lib / "macapk-loader.jar") + ";" + jvm_path(o.lib / "bcprov-jdk18on-1.79.jar") +
                (o.install_only ? ";" + jvm_path(o.lib / "winapk-tools.jar") : std::string()));
    // Upstream's set (runtime/src/jvm_loader.cpp), minus the Mac-only parts.
    v.push_back("-Xrs");
    v.push_back("-Djava.library.path=");
    v.push_back("-XX:+UseSerialGC");
    v.push_back("-Xms128m");
    v.push_back("-Xmx1g");
    v.push_back("-Xss16m");
    v.push_back("-XX:+UnlockDiagnosticVMOptions");
    v.push_back("-XX:-BytecodeVerificationRemote");
    v.push_back("-XX:-BytecodeVerificationLocal");
    v.push_back("--enable-native-access=ALL-UNNAMED");
    v.push_back("--add-opens=java.base/java.io=ALL-UNNAMED");
    v.push_back("--add-opens=java.base/java.util=ALL-UNNAMED");
    v.push_back("--add-opens=java.base/java.lang=ALL-UNNAMED");
    v.push_back("--add-opens=java.base/jdk.internal.loader=ALL-UNNAMED");
    v.push_back("--add-exports=java.base/sun.net.www.protocol.http=ALL-UNNAMED");
    v.push_back("--add-exports=java.base/sun.net.www.protocol.https=ALL-UNNAMED");
    if (fs::is_directory(o.lib / "java_base_patch", ec))
        v.push_back("--patch-module=java.base=" + jvm_path(o.lib / "java_base_patch"));
    if (fs::is_directory(o.lib / "jdk_unsupported_patch", ec))
        v.push_back("--patch-module=jdk.unsupported=" + jvm_path(o.lib / "jdk_unsupported_patch"));
    // Android's file system root (data/data/<app>, /proc mirror) and logs.
    const fs::path android_root = o.data / "android";
    const fs::path logs = o.data / "logs";
    fs::create_directories(android_root, ec);
    fs::create_directories(logs, ec);
    v.push_back("-Dmacapk.android.root=" + jvm_path(android_root));
    // Guest code's "/data/...", "/sdcard/..." resolve under the same folder.
    winapk::set_android_root(utf8(android_root.wstring()).c_str());
    v.push_back("-Dmacapk.bundle.apk=" + jvm_path(o.apk));
    v.push_back("-Dmacapk.display.density=" + std::to_string(display_density_dpi()));
    v.push_back("-XX:ErrorFile=" + jvm_path(logs) + "\\hs_err_pid%p.log");
    // Extra options, space-separated, as upstream reads them.
    const std::string extra = utf8(env_var(L"MACAPK_JVM_OPTS"));
    for (size_t i = 0; i < extra.size();) {
        const size_t j = std::min(extra.find(' ', i), extra.size());
        if (j > i) v.push_back(extra.substr(i, j - i));
        i = j + 1;
    }
    return v;
}

void check_exception(JNIEnv* env, const char* what) {
    if (!env->ExceptionCheck()) return;
    std::fprintf(stderr, "[win.apk] exception in %s:\n", what);
    env->ExceptionDescribe();
    env->ExceptionClear();
}

// One line on how paths reach the JVM: the ANSI code page (65001 is UTF-8,
// which the manifest asks for) and the encoding the JVM takes file names in
// (sun.jnu.encoding).
void report_path_encoding(JNIEnv* env) {
    std::string jnu = "?";
    jclass sys = env->FindClass("java/lang/System");
    jmethodID get = sys ? env->GetStaticMethodID(sys, "getProperty", "(Ljava/lang/String;)Ljava/lang/String;")
                        : nullptr;
    jstring v = get ? static_cast<jstring>(
                          env->CallStaticObjectMethod(sys, get, env->NewStringUTF("sun.jnu.encoding")))
                    : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (v) {
        const char* c = env->GetStringUTFChars(v, nullptr);
        if (c) {
            jnu = c;
            env->ReleaseStringUTFChars(v, c);
        }
    }
    std::fprintf(stderr, "[win.apk] ANSI code page %u; the JVM takes file names in %s\n", GetACP(), jnu.c_str());
}

// winapk-runtime --java-main --jdk <dir> --cp <jar> <class> [args...]
//
// `java -cp <jar> <class> args...` for run_java: creates a JVM with the
// default options (heap and all) and calls <class>.main with the arguments
// as UTF-16 Java strings. A System.exit in main ends the process with its
// code; otherwise the exit code is 0, or 1 when main throws, as with java.exe.
struct JavaMainArgs {
    fs::path jdk, cp;
    std::string main_class;  // binary name with '/', "com/macapk/dex/Main"
    std::vector<std::wstring> args;
    int result = 1;
};

DWORD WINAPI java_main_thread(void* param) {
    auto* a = static_cast<JavaMainArgs*>(param);
    HMODULE jvm = load_jvm(a->jdk);
    if (!jvm) return 1;
    std::vector<std::string> opts{"-Djava.class.path=" + jvm_path(a->cp)};
    JavaVM* vm = nullptr;
    JNIEnv* env = nullptr;
    if (!create_vm(jvm, opts, &vm, &env)) return 1;
    jclass cls = env->FindClass(a->main_class.c_str());
    jmethodID main = cls ? env->GetStaticMethodID(cls, "main", "([Ljava/lang/String;)V") : nullptr;
    if (!main) {
        check_exception(env, "the main class lookup");
        std::fprintf(stderr, "[win.apk] no main method in %s\n", a->main_class.c_str());
        return 1;
    }
    jobjectArray args =
        env->NewObjectArray(static_cast<jsize>(a->args.size()), env->FindClass("java/lang/String"), nullptr);
    for (size_t i = 0; i < a->args.size(); ++i) {
        env->SetObjectArrayElement(args, static_cast<jsize>(i), jstring_of(env, a->args[i]));
    }
    env->CallStaticVoidMethod(cls, main, args);
    const bool threw = env->ExceptionCheck();
    if (threw) {
        // As java.exe reports it: the thread's uncaught exception handler
        // prints "Exception in thread "main" ...".
        jthrowable t = env->ExceptionOccurred();
        env->ExceptionClear();
        jclass thread_cls = env->FindClass("java/lang/Thread");
        jmethodID current = env->GetStaticMethodID(thread_cls, "currentThread", "()Ljava/lang/Thread;");
        jmethodID dispatch = env->GetMethodID(thread_cls, "dispatchUncaughtException", "(Ljava/lang/Throwable;)V");
        jobject self = current ? env->CallStaticObjectMethod(thread_cls, current) : nullptr;
        if (self && dispatch) env->CallVoidMethod(self, dispatch, t);
        if (env->ExceptionCheck()) env->ExceptionDescribe();
        env->ExceptionClear();
    }
    vm->DestroyJavaVM();  // waits for the program's other threads, as java.exe does
    a->result = threw ? 1 : 0;
    return 0;
}

int java_main(int argc, wchar_t** argv) {
    JavaMainArgs a;
    int i = 2;
    for (; i + 1 < argc; i += 2) {
        const std::wstring k = argv[i];
        if (k == L"--jdk") a.jdk = argv[i + 1];
        else if (k == L"--cp") a.cp = argv[i + 1];
        else break;
    }
    if (a.jdk.empty() || a.cp.empty() || i >= argc) {
        std::fprintf(stderr, "usage: winapk-runtime --java-main --jdk <dir> --cp <jar> <class> [args...]\n");
        return 2;
    }
    a.main_class = utf8(argv[i++]);
    std::replace(a.main_class.begin(), a.main_class.end(), '.', '/');
    for (; i < argc; ++i) a.args.emplace_back(argv[i]);
    // HotSpot's main thread is the one that creates the VM: give it at least
    // java.exe's room.
    HANDLE t = CreateThread(nullptr, 64 * 1024 * 1024, java_main_thread, &a, STACK_SIZE_PARAM_IS_A_RESERVATION,
                            nullptr);
    if (!t) return 1;
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    std::fflush(stderr);
    return a.result;
}

// Steps 3 to 5, on the large-stack thread.
DWORD WINAPI boot_thread(void* param) {
    auto* b = static_cast<BootArgs*>(param);
    const Options& o = b->options;

    HMODULE jvm = load_jvm(o.jdk);
    if (!jvm) return 1;
    std::vector<std::string> opts = jvm_options(*b);
    JavaVM* vm = nullptr;
    JNIEnv* env = nullptr;
    if (!create_vm(jvm, opts, &vm, &env)) return 1;
    report_path_encoding(env);
    jvmtiEnv* jvmti = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&jvmti), JVMTI_VERSION_1_2) != JNI_OK) {
        std::fprintf(stderr, "[win.apk] no JVMTI\n");
        return 1;
    }

    winapk::host::NativeStats stats;
    if (!winapk::host::register_host_natives(env, jvmti, &stats)) {
        std::fprintf(stderr, "[win.apk] binding host natives failed\n");
        return 1;
    }
    std::fprintf(stderr, "[win.apk] host natives: %d classes, %d implemented, %d stubbed\n", stats.classes,
                 stats.implemented, stats.stubbed);
    // The app's own native libraries load through the guest linker.
    if (!winapk::host::guest_libs_init(env, jvmti)) {
        std::fprintf(stderr, "[win.apk] native library support is unavailable for this run\n");
    }
    // Upstream's own NativeSqlite bridge replaces those stubs (see CMakeLists).
    macapk::sqlite::RegisterNativeSqliteMethods(env);
    check_exception(env, "RegisterNativeSqliteMethods");

    auto jstr = [&](const fs::path& p) { return jstring_of(env, p.wstring()); };

    if (o.install_only) {
        jclass meta = env->FindClass("com/winapk/tools/AppMeta");
        jmethodID write = meta ? env->GetStaticMethodID(meta, "write", "(Ljava/lang/String;Ljava/lang/String;)V")
                               : nullptr;
        if (write) env->CallStaticVoidMethod(meta, write, jstr(o.apk), jstr(b->app_dir));
        check_exception(env, "AppMeta.write");
        // Framework threads the icon render started must not keep the
        // process alive.
        std::fflush(stderr);
        TerminateProcess(GetCurrentProcess(), write ? 0 : 1);
    }

    jclass boot = env->FindClass("com/macapk/loader/Boot");
    check_exception(env, "FindClass com.macapk.loader.Boot");
    jmethodID run = boot ? env->GetStaticMethodID(
                               boot, "run",
                               "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V")
                         : nullptr;
    if (!run) {
        check_exception(env, "Boot.run lookup");
        return 1;
    }
    // The manifest argument names a decoded text manifest, which Windows does
    // not produce; Manifest.parse then reads the binary one from the APK.
    env->CallStaticVoidMethod(boot, run, jstr(o.apk), jstr(b->app_jar), jstr(b->app_dir),
                              jstr(b->app_dir / "AndroidManifest.txt"));
    check_exception(env, "Boot.run");

    // Returns once the app's non-daemon threads are done.
    vm->DestroyJavaVM();
    b->result = 0;
    return 0;
}

// One runtime per app at a time. Two runtimes on the same app share its
// databases, preferences and, for a messenger, its identity keys: SQLite's
// locking keeps the files intact only as far as each process expects to be
// alone, and an app's own state (a message counter, a ratchet) breaks at once.
// Upstream's app stub holds run/<app>.pid for the runtime's life and a second
// launch only activates the running app; a shortcut, the library and a script
// here could each start a second copy.
//
// The lock is a named mutex keyed on the app's data folder (absolute,
// lower-cased), held until the process exits; a mutex left by a process that
// died counts as free (WAIT_ABANDONED). Next to it, a named mapping holds the
// holder's process id, so a second start can find the holder's window.
// WINAPK_ALLOW_SECOND_INSTANCE=1 turns the check off.
std::wstring instance_key(const fs::path& app_dir) {
    std::error_code ec;
    std::wstring key = fs::absolute(app_dir, ec).lexically_normal().wstring();
    if (ec) key = app_dir.wstring();
    while (!key.empty() && (key.back() == L'\\' || key.back() == L'/')) key.pop_back();
    for (wchar_t& c : key) c = static_cast<wchar_t>(towlower(c == L'/' ? L'\\' : c));
    uint64_t h = 1469598103934665603ull;  // FNV-1a over the UTF-16 units
    for (const wchar_t c : key) h = (h ^ static_cast<uint16_t>(c)) * 1099511628211ull;
    wchar_t name[48];
    swprintf(name, 48, L"Local\\WinApk-app-%016llx", static_cast<unsigned long long>(h));
    return name;
}

bool claim_single_instance(const std::wstring& key) {
    if (!env_var(L"WINAPK_ALLOW_SECOND_INSTANCE").empty()) return true;
    HANDLE m = CreateMutexW(nullptr, FALSE, key.c_str());
    if (!m) return true;  // no lock to be had: do not block the start
    const DWORD w = WaitForSingleObject(m, 0);
    if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) {
        CloseHandle(m);
        return false;
    }
    // Held until exit, like the mapping that names us.
    if (HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(DWORD),
                                        (key + L"-pid").c_str())) {
        if (auto* pid = static_cast<DWORD*>(MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, sizeof(DWORD))))
            *pid = GetCurrentProcessId();
    }
    return true;
}

struct WindowSearch {
    DWORD pid;
    HWND found;
};

BOOL CALLBACK find_app_window(HWND w, LPARAM lp) {
    auto* f = reinterpret_cast<WindowSearch*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    wchar_t cls[32] = {};
    GetClassNameW(w, cls, 32);
    if (pid != f->pid || !IsWindowVisible(w) || wcscmp(cls, L"WinApkAppWindow") != 0) return TRUE;
    f->found = w;
    return FALSE;
}

// Brings the running copy's window forward. It may still be starting, with
// no window yet, so this waits a little for one.
void activate_running(const std::wstring& key) {
    HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, (key + L"-pid").c_str());
    if (!map) return;
    DWORD pid = 0;
    if (const auto* p = static_cast<const DWORD*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(DWORD)))) {
        pid = *p;
        UnmapViewOfFile(p);
    }
    CloseHandle(map);
    if (!pid) return;
    AllowSetForegroundWindow(pid);
    for (int i = 0; i < 60; ++i) {
        WindowSearch f{pid, nullptr};
        EnumWindows(find_app_window, reinterpret_cast<LPARAM>(&f));
        if (f.found) {
            if (IsIconic(f.found)) ShowWindow(f.found, SW_RESTORE);
            SetForegroundWindow(f.found);
            return;
        }
        Sleep(250);
    }
}

}  // namespace

// The volume's Download, Pictures, Documents, Music and Movies are the user's
// Windows folders (user_folders.h). Only for the user's own library: a run
// with --data (tests, the compat sweep, CI) must never touch real folders.
// MACAPK_USER_FOLDERS decides when set (=0 changes nothing, as upstream);
// otherwise the launcher's Settings switch (UserFolders, default on), whose
// off undoes the junctions so it visibly does what it says.
void map_shared_folders(const Options& o) {
    if (!o.default_data) return;
    using winapk::host::UserFolderMode;
    UserFolderMode mode = UserFolderMode::kMap;
    const std::wstring forced = env_var(L"MACAPK_USER_FOLDERS");
    if (!forced.empty()) {
        if (forced[0] == L'0') mode = UserFolderMode::kKeep;
    } else {
        DWORD on = 1, size = sizeof on;
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Win.apk", L"UserFolders", RRF_RT_REG_DWORD, nullptr, &on,
                         &size) == ERROR_SUCCESS &&
            on == 0) {
            mode = UserFolderMode::kUnmap;
        }
    }
    const fs::path android_root = o.data / "android";
    const auto mapped =
        winapk::host::map_user_folders(android_root / "sdcard", winapk::host::user_folders(),
                                       android_root / "data" / "system" / "storage" / "usermap-journal.tsv", mode);
    // Set even when empty: its presence tells the Java side the host owns the
    // mapping, so SharedStorage.mapUserFolders never runs here.
    SetEnvironmentVariableW(L"WINAPK_USER_FOLDER_TARGETS", winapk::host::user_folder_env(mapped).c_str());
}

// stderr is unbuffered in the UCRT, and an unbuffered fprintf hands the
// stream ONE CHARACTER per WriteFile: measured 564 us per 150-byte log line
// against 9 us for the same line through a buffer, all of it under the stream
// lock. Every log line of every thread queued behind that lock, and lines from
// different threads interleaved character by character. A JFR of WhatsApp's
// boot had its main thread in NativeLog.print for most of the samples it was
// not idle in: the first frame took 48 to 76 s, and about 5 s with this.
//
// So stderr gets a 64 KB buffer, flushed every 200 ms by a background thread,
// at once for warnings and errors (NativeLog), and on the exit paths that
// already flush. A hard crash can lose at most the last 200 ms of INFO lines.
DWORD WINAPI stderr_flusher(void*) {
    for (;;) {
        Sleep(200);
        std::fflush(stderr);
    }
}

void buffer_stderr() {
    static char buf[1 << 16];
    std::setvbuf(stderr, buf, _IOFBF, sizeof buf);
    HANDLE t = CreateThread(nullptr, 64 * 1024, stderr_flusher, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (t) CloseHandle(t);
}

int wmain(int argc, wchar_t** argv) {
    buffer_stderr();
    if (argc > 1 && std::wstring(argv[1]) == L"--java-main") return java_main(argc, argv);
    if ((argc == 3 || (argc == 5 && std::wstring(argv[3]) == L"--cache")) && std::wstring(argv[1]) == L"--aot-translate")
        return aot_translate_main(argv[2], argc == 5 ? fs::path(argv[4]) : fs::path());
    BootArgs b;
    if (!parse_args(argc, argv, &b.options)) {
        std::fprintf(stderr, "usage: winapk-runtime --apk <file.apk|.apkm|.xapk|.apks> [--data <dir>] [--jdk <dir>] [--lib <dir>] "
                     "[--install-only] [--package-to <file>]\n");
        return 2;
    }
    if (!resolve_paths(&b.options)) return 2;
    if (!b.options.package_to.empty()) {
        return run_java(b.options, b.options.lib / "winapk-tools.jar", L"com.winapk.tools.AppInstall",
                        {L"--package", b.options.apk.wstring(), b.options.package_to.wstring()});
    }

    // F-Droid and other stores install through PackageInstaller, which hands
    // the APK to the install helper (PackageInstallerBackend), as on the Mac:
    // here the helper is the launcher, Win.apk.exe install --apk ... It keeps
    // the installed-apps registry PackageManager answers from, and stages
    // under the data folder.
    {
        std::error_code ec0;
        const fs::path launcher = exe_dir().parent_path() / L"Win.apk.exe";
        if (env_var(L"MACAPK_INSTALLHELPER").empty() && fs::is_regular_file(launcher, ec0))
            SetEnvironmentVariableW(L"MACAPK_INSTALLHELPER", launcher.c_str());
        if (env_var(L"MACAPK_PM_REGISTRY_FILE").empty())
            SetEnvironmentVariableW(L"MACAPK_PM_REGISTRY_FILE",
                                    (b.options.data / L"installed_apps.json").c_str());
        if (env_var(L"MACAPK_STAGING_DIR").empty())
            SetEnvironmentVariableW(L"MACAPK_STAGING_DIR", (b.options.data / L"staging").c_str());
    }

    // "All files access" (MANAGE_EXTERNAL_STORAGE) is granted on the Mac by
    // macapk-installhelper's consent dialog, which signs the answer into a
    // store SpecialAccessStore reads. Windows has no such helper yet, so a
    // grant could never be given and apps that need it (AnkiDroid) stop at
    // their permission screen. Until it exists, every app holds the access,
    // which is upstream's own answer before that helper (the switch below).
    // With the user's folders mapped (map_shared_folders) that means every app
    // can read and write Downloads, Documents, Pictures, Music and Videos, as
    // any Windows program can.
    if (env_var(L"MACAPK_ALLFILES_HONEST").empty()) SetEnvironmentVariableW(L"MACAPK_ALLFILES_HONEST", L"0");

    if (!b.options.install_only) map_shared_folders(b.options);

    std::error_code ec;
    b.app_dir = b.options.data / "apps" / b.options.apk.stem();
    winapk::host::set_app_dir(b.app_dir.wstring());
    fs::create_directories(b.app_dir, ec);
    if (ec) {
        std::fprintf(stderr, "[win.apk] cannot create %s\n", utf8(b.app_dir.wstring()).c_str());
        return 1;
    }
    const std::wstring instance = instance_key(b.app_dir);
    if (!claim_single_instance(instance)) {
        std::fprintf(stderr, "[win.apk] %s is already running; not starting a second copy\n",
                     utf8(b.app_dir.filename().wstring()).c_str());
        if (b.options.install_only) return 3;
        if (env_var(L"WINAPK_HEADLESS").empty()) activate_running(instance);
        return 0;
    }
    if (!install_app(&b.options, b.options.data / "android")) return 1;
    prepare_arm64(b.options, b.app_dir);
    if (!convert_dex(b.options, b.app_dir, &b.app_jar)) return 1;

    // Upstream runs the JVM with -Xss16m; give the creating thread as much.
    HANDLE t = CreateThread(nullptr, 64 * 1024 * 1024, boot_thread, &b, STACK_SIZE_PARAM_IS_A_RESERVATION,
                            nullptr);
    if (!t) return 1;
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return b.result;
}
