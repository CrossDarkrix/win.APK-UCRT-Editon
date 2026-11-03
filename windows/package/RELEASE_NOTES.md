A pre-release for testing: Win.apk runs Android apps on Windows 10/11 x64.

## Getting started
- **Install:** run `Win.apk-Setup.exe`. It installs per user, with no administrator rights, and puts Win.apk in the Start menu. If you'd rather not install, unzip `Win.apk-windows-x64.zip` and start `Win.apk.exe`.
- **Add apps:** get them from the **Store** tab (F-Droid's free and open-source apps: search, Install, Update), drag `.apk` files onto the Win.apk window, or double-click them in Explorer. Each app gets its own icon in the library, the Start menu and the desktop.
- **Included:** Java and ANGLE. `README.txt` has the details.

## What works
- **Java/Kotlin apps:** each opens in its own window, with mouse, wheel and keyboard input. Back is Escape, Alt+Left or the mouse's back button.
- **OpenGL ES:** works from Java (`GLSurfaceView`, libGDX) and from an app's native code through a SurfaceView (SDL games). It runs on ANGLE (Direct3D 11), falling back to WARP or SwiftShader without a GPU.
- **Native libraries:** apps that ship `lib/x86_64` libraries load them natively. Apps that only ship `lib/arm64-v8a` libraries are supported: their ARM code is translated to x64 in the background (`winapk-aot`, compiled with clang for your CPU, in parallel) and runs at close to native speed. Apps open at once on an ARM JIT and switch to the translated code as it becomes ready; large libraries are translated only where the app's code ran. Installs use the ABIs `x86_64,arm64-v8a`.
- **Sound from Java:** AudioTrack and SoundPool, for WAV, Ogg and MP3.
- **Storage:** files, SharedPreferences and SQLite.
- **Desktop services:** clipboard, file pickers and game controllers.

In the automated sweep of open-source apps, about 33 of 38 draw their first screen.

## Not yet
- Native code for 32-bit ARM only (`armeabi-v7a`).
- NativeActivity games.
- MediaPlayer and native audio (OpenSL ES and AAudio).
- Video, camera, WebView and location.
- Google Play services.

## Reporting
Please include the app's name and version, and attach `%LOCALAPPDATA%\Win.apk\logs\last-run.log`.
