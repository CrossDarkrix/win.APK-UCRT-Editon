# Win.apk: notes for Claude

Runs Android apps on Windows x64 as normal Windows programs: Java on HotSpot,
x86_64 `.so` files loaded by our own ELF loader over a Bionic shim, graphics on
ANGLE (D3D11). Started as a port of Mac.apk; its `runtime/` comes from there.
Read this file instead of re-mapping the repository; open
`windows/README.md` (build, tests, the list of `runtime/` changes) or
`windows/PORTING.md` (design and plan, ~950 lines: grep it, don't read it
whole) only when a task needs them.

## Layout

| Path | What |
|---|---|
| `windows/src/`, `windows/include/winapk/` | ELF loader, linker, ELF TLS, `%fs` patch, ABI thunks, guest JNI. `src/bionic/` + `mini_bionic.cpp`: the Bionic libc shim (~520 functions, `sysv_abi`) |
| `windows/host/` | The runtime host: `runtime_main.cpp` (winapk-runtime.exe: args `--apk --data --jdk --lib --install-only`), `win32_window.cpp` (window, input), `native_*.cpp` (the Java natives: `native_cocoa.cpp` = window/phone size/icon, audio, clipboard, gamepad, ICU, images, OpenSL ES...), `gl/` |
| `windows/launcher/launcher_main.cpp` | `Win.apk.exe`, the library app (one file, GDI+ custom-drawn). See below |
| `windows/java/` | Windows-only Java (`com/winapk/tools/AppMeta.java`: writes `icon.png` + `label.txt` at install) |
| `windows/scripts/` | `build_java.sh` (builds `runtime/java` with a stock JDK), `stage_runtime.sh` (runnable `bin/`+`lib/`), `fetch_angle.sh`, `build_framework_res.sh` |
| `windows/test/` | Unit tests (`*_test.cpp`), guest libs (`guest/`), Wine runners (`run_*_wine.sh`), `compat/` (app sweep: `apps.txt`, `sweep.py`) |
| `windows/package/` | Installer (`winapk.iss`, Inno Setup), `README.txt` and `RELEASE_NOTES.md` shipped in the package |
| `windows/CMakeLists.txt` | All Windows targets. Pulls `runtime/src/native_methods.cpp` (GL bridges) and `native_sqlite.cpp` from upstream |
| `runtime/java/` | The Android framework in Java (~3000 files) + `com/macapk/loader/` (Boot, ActivityHost, package manager...). Names say macapk: that is upstream's, keep them |
| `runtime/src/`, `runtime/translator/` | Upstream native sources; Windows uses only the two files above |
| `scripts/build_framework_res.sh` | The platform resource APK (called by `windows/scripts/`) |
| `.github/workflows/` | `windows-poc.yml` (build + tests + package, on PRs and by hand), `compat-sweep.yml` (app sweep, PRs and by hand), `windows-release.yml` (release, by hand only) |

Data at run time: `%LOCALAPPDATA%\Win.apk\` (`library\` APKs + `.label`, `apps\<name>\`, `android\`, `logs\last-run.log`, `logs\install-<name>.log`, `store\` the Store tab's F-Droid catalog.tsv + icons). Settings: `HKCU\Software\Win.apk` (`Theme`, `Language`, `PhoneSize`, `Landscape`, `DesktopShortcut`). Runtime env: `WINAPK_PHONE_SIZE=WxH` (dp), `WINAPK_HEADLESS=1`, `WINAPK_HEADLESS_FRAME`, `WINAPK_HEADLESS_EXIT_AFTER_FRAMES`.

## The launcher (`windows/launcher/launcher_main.cpp`)

Sections in order: paths, settings (registry), Windows integration (file types,
shortcuts), library, running/installing (threads post `kInstalled`,
`kInstallStarted`, `kRunExited`), the store (WinHTTP, F-Droid's index-v1.json
-> `store\catalog.tsv`, icon and download threads posting `kStore*`), theme + drawing helpers, state, layout,
painting, dialogs, actions, sheets (Settings, Get Info: `Row`/`Section` model,
`run_sheet` modal loop, the Store's app sheet), install window, log window, drag and drop
(`IDropTarget`), main `wnd_proc`, `wWinMain`.

- Every user-visible string goes through `tr(L"English")` or
  `fmt(L"... {0} ...", {args})`; add the Hebrew to the table in `tr()`.
- Hebrew = RTL: windows get `WS_EX_LAYOUTRTL`, `buffered_paint` mirrors the
  layout, and `draw_text` / `draw_image` un-mirror text and pictures. Always
  draw text with `draw_text` and images with `draw_image`, never directly.
  Layout and hit-testing code stays left-to-right.
- Icons are glyphs from Segoe Fluent Icons / MDL2 Assets (`kGlyph*`).

## Build and check

- Toolchain: **clang only** (MSVC cannot emit System V calls). CI builds on
  `windows-latest` with MSYS2 CLANG64.
- Quick launcher-only compile check on Linux (no full CMake needed):
  `apt-get install g++-mingw-w64-x86-64-posix`, then
  `x86_64-w64-mingw32-windres windows/launcher/launcher.rc -O coff -o l.res` and
  `x86_64-w64-mingw32-g++-posix -std=c++17 -Wall -Wextra -Wno-missing-field-initializers -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -municode -mwindows -static windows/launcher/launcher_main.cpp l.res -o Win.apk.exe -lcomctl32 -lshell32 -lole32 -luuid -lgdiplus -ldwmapi -luxtheme -lgdi32 -lwinhttp -lbcrypt`.
- Looking at the UI: `apt-get install wine64 xvfb xdotool`; Wine is in
  `/usr/lib/wine` (`wine64`). Run under `Xvfb :9`, screenshot with
  `import -window root`. Wine lacks Segoe fonts, so icon glyphs show as boxes.
  For the README screenshots (`docs/images/`), stand-in fonts were dropped into
  the prefix's `C:\windows\Fonts`: Open Sans instanced and renamed "Segoe UI" /
  "Segoe UI Semibold" (fontTools), and a "Segoe Fluent Icons" built from
  Fluent UI System Icons SVGs on the same code points (it needs name IDs 3, 4
  and 6 or Wine's GDI+ will not find it). Wine's GDI+ does no bidi, so Hebrew
  screenshots come out wrong: take those on Windows.
- Full build and tests: `windows/README.md`, "Build and run".

## Conventions

- Win.apk code goes in `windows/`. Touch `runtime/` only for a needed Windows
  hook, and add it to the list in `windows/README.md`.
- Match the surrounding style: C++17, 120 columns, comments say why.
- Don't add app-specific hacks; fix the framework, shim or host.
- PRs go to `main`; CI must be green. Releases only via the manual
  `windows-release.yml` run.
