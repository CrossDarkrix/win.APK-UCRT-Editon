# Win.apk (UCRT Edition)

**Run Android applications on Windows 10 and Windows 11 x64 as ordinary Windows programs with a UCRT-focused build of Win.apk.**

> [!IMPORTANT]
> Win.apk (UCRT Edition) is a customized build designed for stable operation on Windows. Its native dependencies are unified around the MSYS2 UCRT64 environment, with additional work focused on startup performance, multimedia handling, Windows storage integration, and practical hardware compatibility. It preserves the core Win.apk architecture while adding UCRT-specific native bridges and Windows integrations.

## Overview

Win.apk installs `.apk`, `.apkm`, `.xapk`, and `.apks` packages and runs each Android application in its own Windows window. It does not keep a conventional Android emulator or virtual machine running in the background. Java code runs on a Java VM, x86_64 native libraries load directly into the process, and arm64-v8a libraries have an x64 translation path.

The UCRT Edition builds its native dependencies with the UCRT64 toolchain based on the Windows Universal C Runtime. Keeping the toolchain, DLLs, plugins, and native bridges on the same ABI avoids mixing incompatible C runtimes and build environments.

## UCRT Edition highlights

### UCRT64 stability work

- Uses **MSYS2 UCRT64** as the supported build environment
- Deploys UCRT64 GStreamer, plugins, plugin scanner, runtime DLLs, JNI code, and Windows host components as one ABI-compatible set
- Avoids mixing artifacts from MINGW64, CLANG64, or other MSYS2 environments
- Builds the native bridge, Windows host, and Java runtime staging through a consistent UCRT64 flow

### GStreamer graphics and media processing

- Asynchronous presentation through GStreamer `appsrc`, D3D11 upload, and `d3d11videosink`
- GStreamer-backed MediaExtractor PCM decoding path
- Pre-generates the GStreamer registry cache against the final deployment paths
- Delays GStreamer initialization until after the first frame, reducing the initial blank-window period
- Falls back to the Win32 presentation path when the GStreamer presenter is unavailable
- Stretches the complete rendered frame to the full client width and height for every screen preset

Android and OpenGL ES rendering is connected to Direct3D 11 through ANGLE. Final Windows presentation uses the GStreamer D3D11 presenter or the Win32 fallback path.

### Windows audio integration

- Enumerates the default Windows playback device and active audio endpoints
- Allows Android applications to select a Windows output device
- Implements output routing corresponding to `AudioTrack.setPreferredDevice()`
- Synchronizes application audio with the Windows session volume
- Provides Windows audio paths for AudioTrack, SoundPool, and MediaPlayer-related playback

### Audio CD and optical drive support

- UCRT CD bridge that exposes Windows optical drives to the Android side
- Drive enumeration and open operations
- TEST UNIT READY and INQUIRY commands
- READ TOC for track information
- READ CD for CD-DA sector access
- Chunked reads for larger requests
- Windows SCSI pass-through command transport

Available operations depend on the optical drive, inserted media, and device capabilities.

### Windows user folder integration

Android shared storage can map to the standard Windows user folders:

- Downloads
- Pictures
- Documents
- Music
- Videos, exposed as Movies on the Android side

The Windows document picker, URI grant layer, and shared-storage bridge allow compatible Android applications to open, read, and save Windows files. Network shares and cross-volume locations may fall back to ordinary directories when Windows filesystem restrictions prevent direct mapping.

### Additional Windows integration

- Installation of APK, APKM, XAPK, and APKS packages
- Per-application windows, icons, and Start menu shortcuts
- Windows clipboard integration
- Windows file picker integration
- Game controller support
- Host bridges for camera, microphone input, and the photo library
- WebView2-based WebView and browser integration
- Live runtime and installation logs
- English, Japanese, and Hebrew UI, including right-to-left layout support
- Light and dark themes

## Screen sizes and presentation

The launcher includes a 1270 x 720 Fit screen preset and several phone and tablet presets. In the UCRT Edition, every preset uses full-client stretch presentation without locking the aspect ratio.

- Uses the complete client width and height without horizontal or vertical black bars
- Follows live window resizing and maximization
- Uses the same full-client rectangle for mouse input, touch mapping, and child windows
- Keeps the selected preset as the internal resolution reported to the Android application

## Compatibility

### Main supported areas

- Java and Kotlin Android applications
- x86_64 native libraries
- arm64-v8a native libraries through the x64 translation path
- OpenGL ES from Java and native code
- AudioTrack, SoundPool, and MediaPlayer-related audio paths
- Files, SharedPreferences, SQLite, and clipboard access
- File pickers and game controllers
- Windows user folders
- Audio CD reading

### Current limitations

- 32-bit ARM-only applications using `armeabi-v7a` are not supported
- Features that require Google Play services may not work, depending on the application
- Device-specific sensors, DRM, authentication, and hardware functions are not fully compatible with a physical Android device
- The project remains under development, and compatibility differs by application

## Requirements

- Windows 10 or Windows 11, 64-bit x64
- A Direct3D 11-capable GPU is recommended
- Enough storage for Win.apk, its Java runtime, GStreamer, ANGLE, converted code, and installed applications

## Installation and basic use

1. Download the installer or portable ZIP from the project release files.
2. Run the installer, or extract the portable package to a writable directory.
3. Start `Win.apk.exe`.
4. Drop an APK or supported APK bundle onto the launcher, or select the file through the Add App command.
5. Open the installed application from its library tile.

The first installation may take additional time while Win.apk prepares and translates application code. If an application fails to start or behaves incorrectly, inspect `last-run.log` and the installation log through the launcher log viewer.

## Data locations

| Item | Location |
|---|---|
| Win.apk program files | `%LOCALAPPDATA%\Programs\Win.apk`, or the portable extraction directory |
| Installed APK copies | `%LOCALAPPDATA%\Win.apk\library` |
| Converted code and application metadata | `%LOCALAPPDATA%\Win.apk\apps\<app>` |
| Android application data | `%LOCALAPPDATA%\Win.apk\android` |
| Logs | `%LOCALAPPDATA%\Win.apk\logs` |
| Store metadata | `%LOCALAPPDATA%\Win.apk\store` |
| Settings | `HKEY_CURRENT_USER\Software\Win.apk` |

Some removal operations preserve application data. To remove everything, review the relevant data directory before deleting it.

## Building from source

### Prerequisites

- MSYS2
- The **MSYS2 UCRT64** shell
- Git, Make, CMake, and Ninja
- UCRT64 GCC and G++ toolchain
- UCRT64 Clang
- A JDK
- UCRT64 GStreamer development packages

### Standard build

```bash
./build.sh
```

If the executable bit was lost while copying or extracting the source tree, run:

```bash
bash build.sh
```

Confirm that the active environment is UCRT64:

```bash
printf '%s\n' "$MSYSTEM"
```

The output must be `UCRT64`. Do not reuse build directories, DLLs, or native artifacts created under CLANG64, MINGW64, or another toolchain. Mixing ABIs can cause startup failures, plugin loading failures, and audio or graphics bridge incompatibilities.

## Important environment variables

| Variable | Purpose |
|---|---|
| `WINAPK_PHONE_SIZE=1270x720` | Internal screen size reported to the Android application |
| `MACAPK_LETTERBOX=0` | Disables aspect-ratio locking and stretches to the full window client area |
| `MACAPK_GSTREAMER_PRESENT=0` | Disables the GStreamer presenter and uses the Win32 presentation path |
| `GST_PLUGIN_PATH` | GStreamer plugin path |
| `GST_PLUGIN_SYSTEM_PATH` | GStreamer system plugin path |
| `GST_PLUGIN_SCANNER` | GStreamer plugin scanner executable |
| `GST_REGISTRY` | Pre-generated GStreamer registry cache |
| `WINAPK_HEADLESS=1` | Runs without a visible window for testing |
| `WINAPK_HEADLESS_FRAME=frame.bmp` | Saves the last rendered frame |

## Repository layout

| Path | Description |
|---|---|
| `windows/` | Windows host, JNI bridge, launcher, packaging, and tests |
| `windows/host/` | Windows bridges for GStreamer, audio, CD access, user folders, input, and other host services |
| `runtime/` | Android Java framework and shared native runtime sources |
| `docs/` | Implementation notes, validation records, and images |
| `.github/workflows/` | Windows builds, compatibility checks, and release workflows |

## Credits

Win.apk began as a port of Mac.apk by Kaleb Simpson / Voltare and inherits part of its Android runtime from that project. Win.apk (UCRT Edition) is not affiliated with or endorsed by the Mac.apk project.

Major external components include ANGLE, Eclipse Temurin, and GStreamer. See `THIRD_PARTY_NOTICES.txt` for the complete third-party notices.

## License

[AGPL-3.0](LICENSE)
