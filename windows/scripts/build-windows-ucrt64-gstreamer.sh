#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SOURCE="$ROOT/windows"
BUILD="$ROOT/build-windows-ucrt64"
PREFIX=/ucrt64

# Entire build and deployment use one ABI: UCRT64.
export MSYSTEM=UCRT64
export PATH="$PREFIX/bin:/usr/bin"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"
unset CMAKE_PREFIX_PATH CMAKE_LIBRARY_PATH CMAKE_INCLUDE_PATH LIBRARY_PATH CPATH CPLUS_INCLUDE_PATH

CC="$PREFIX/bin/clang.exe"
CXX="$PREFIX/bin/clang++.exe"
CMAKE="$PREFIX/bin/cmake.exe"
NINJA="$PREFIX/bin/ninja.exe"
PKG_CONFIG="$PREFIX/bin/pkg-config.exe"
GST_INSPECT="$PREFIX/bin/gst-inspect-1.0.exe"

for tool in "$CC" "$CXX" "$CMAKE" "$NINJA" "$PKG_CONFIG" "$GST_INSPECT"; do
  [[ -x "$tool" ]] || { echo "[ERROR] Missing UCRT64 tool: $tool" >&2; exit 1; }
done
[[ -f "$PREFIX/lib/libz.a" && -f "$PREFIX/include/zlib.h" ]] || {
  echo "[ERROR] UCRT64 zlib is missing. Install mingw-w64-ucrt-x86_64-zlib" >&2; exit 2;
}

D3D11_PLUGIN="$PREFIX/lib/gstreamer-1.0/libgstd3d11.dll"
[[ -f "$D3D11_PLUGIN" ]] || {
  echo "[ERROR] UCRT64 GStreamer D3D11 plugin is missing: $D3D11_PLUGIN" >&2
  echo "Install: pacman -S --needed mingw-w64-ucrt-x86_64-gst-plugins-bad" >&2
  exit 3
}

JAVA_HOME_WIN="${JAVA_HOME:-}"
[[ -n "$JAVA_HOME_WIN" ]] || JAVA_HOME_WIN="$(cmd.exe /d /c "echo %JAVA_HOME%" 2>/dev/null | tr -d '\r')"
if [[ -z "$JAVA_HOME_WIN" || "$JAVA_HOME_WIN" == "%JAVA_HOME%" ]]; then
  echo "[ERROR] JAVA_HOME is not set to a Windows JDK." >&2; exit 4
fi
JAVA_HOME_MSYS="$(cygpath -u "$JAVA_HOME_WIN")"
JNI_INCLUDE="$JAVA_HOME_MSYS/include"
[[ -f "$JNI_INCLUDE/jni.h" && -f "$JNI_INCLUDE/win32/jni_md.h" ]] || {
  echo "[ERROR] JNI headers were not found under: $JNI_INCLUDE" >&2; exit 5;
}

# Never reuse the former MINGW64 cache or objects.
rm -rf "$BUILD"
mkdir -p "$BUILD"

"$CMAKE" -S "$SOURCE" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$CC" \
  -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_MAKE_PROGRAM="$NINJA" \
  -DCMAKE_PREFIX_PATH="$PREFIX" \
  -DCMAKE_FIND_ROOT_PATH="$PREFIX" \
  -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
  -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
  -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
  -DPKG_CONFIG_EXECUTABLE="$PKG_CONFIG" \
  -DZLIB_ROOT="$PREFIX" \
  -DZLIB_LIBRARY="$PREFIX/lib/libz.a" \
  -DZLIB_INCLUDE_DIR="$PREFIX/include" \
  -DWINAPK_JNI_INCLUDE="$JNI_INCLUDE"

CACHE="$BUILD/CMakeCache.txt"
if grep -Eiq '(^|[=;:/\\])mingw64([/\\]|$)|(^|[=;:/\\])clang64([/\\]|$)' "$CACHE"; then
  echo "[ERROR] MINGW64/CLANG64 path entered the UCRT64 CMake cache." >&2
  grep -Ein 'mingw64|clang64' "$CACHE" >&2 || true
  exit 6
fi
cache_cxx="$(sed -n 's/^CMAKE_CXX_COMPILER:[^=]*=//p' "$CACHE" | head -n 1 | tr -d '\r')"
case "$(printf '%s' "$cache_cxx" | tr '[:upper:]' '[:lower:]')" in
  *ucrt64*clang++*) ;;
  *) echo "[ERROR] CMake did not select UCRT64 Clang: $cache_cxx" >&2; exit 7 ;;
esac

"$CMAKE" --build "$BUILD" --parallel
[[ -f "$BUILD/winapk-runtime.exe" ]] || { echo "[ERROR] Runtime was not produced." >&2; exit 8; }

DIST="$BUILD/winapk-runtime-dist"
BIN="$DIST/bin"
PLUGINS="$DIST/lib/gstreamer-1.0"
SCANNER_DIR="$DIST/libexec/gstreamer-1.0"
rm -rf "$DIST"
mkdir -p "$BIN" "$PLUGINS" "$SCANNER_DIR"
cp -f "$BUILD/winapk-runtime.exe" "$BIN/"
cp -f "$PREFIX/bin"/*.dll "$BIN/"
cp -f "$PREFIX/lib/gstreamer-1.0"/*.dll "$PLUGINS/"
SCANNER="$PREFIX/libexec/gstreamer-1.0/gst-plugin-scanner.exe"
[[ -x "$SCANNER" ]] || { echo "[ERROR] Plugin scanner missing: $SCANNER" >&2; exit 9; }
cp -f "$SCANNER" "$SCANNER_DIR/"

cat > "$DIST/run-winapk-runtime.cmd" <<'CMD'
@echo off
setlocal
set "ROOT=%~dp0"
set "PATH=%ROOT%bin;%PATH%"
set "GST_PLUGIN_PATH=%ROOT%lib\gstreamer-1.0"
set "GST_PLUGIN_SYSTEM_PATH=%ROOT%lib\gstreamer-1.0"
set "GST_PLUGIN_SCANNER=%ROOT%libexec\gstreamer-1.0\gst-plugin-scanner.exe"
set "GST_REGISTRY=%ROOT%registry.bin"
"%ROOT%bin\winapk-runtime.exe" %*
exit /b %ERRORLEVEL%
CMD

REGISTRY="$DIST/registry.bin"
rm -f "$REGISTRY"
GST_ENV=(PATH="$BIN:/usr/bin" GST_PLUGIN_PATH="$PLUGINS" GST_PLUGIN_SYSTEM_PATH="$PLUGINS"
  GST_PLUGIN_SCANNER="$SCANNER_DIR/gst-plugin-scanner.exe" GST_REGISTRY="$REGISTRY"
  GST_REGISTRY_REUSE_PLUGIN_SCANNER=no)
for element in d3d11 d3d11upload d3d11videosink; do
  env "${GST_ENV[@]}" "$GST_INSPECT" "$element" >/dev/null || {
    echo "[ERROR] UCRT64 private deployment cannot load: $element" >&2
    env "${GST_ENV[@]}" GST_DEBUG=GST_PLUGIN_LOADING:7 "$GST_INSPECT" "$D3D11_PLUGIN" \
      >"$DIST/gstreamer-d3d11-error.log" 2>&1 || true
    echo "[LOG] $(cygpath -w "$DIST/gstreamer-d3d11-error.log")" >&2
    exit 10
  }
done

echo "[OK] UCRT64 Clang build: $(cygpath -w "$BUILD/winapk-runtime.exe")"
echo "[OK] UCRT64 GStreamer deployment: $(cygpath -w "$DIST")"
echo "[RUN] Use winapk-runtime-dist\\run-winapk-runtime.cmd"
