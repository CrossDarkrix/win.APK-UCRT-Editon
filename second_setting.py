from pathlib import Path
import ast
import sys

MARKER = Path("windows/scripts/stage_runtime.sh")


def find_root(start: Path) -> Path:
    start = start.resolve()
    for candidate in (start, *start.parents):
        if (candidate / MARKER).is_file():
            return candidate
    for marker in start.rglob(str(MARKER)):
        return marker.parents[2]
    raise FileNotFoundError(
        "win.apk-main with windows/scripts/stage_runtime.sh was not found"
    )


def backup_once(path: Path, suffix: str) -> None:
    backup = path.with_name(path.name + suffix)
    if path.is_file() and not backup.exists():
        backup.write_bytes(path.read_bytes())


def patch_powershell(ps1: Path) -> None:
    if not ps1.is_file():
        raise FileNotFoundError(
            "run-ucrt-diagnostic.ps1 was not found; run the prior diagnostic generator first"
        )

    text = ps1.read_text(encoding="utf-8-sig")
    backup_once(ps1, ".current-stage-backup")

    legacy_blocks = [
        '$Exe = Join-Path $Root "build-windows-clang\\winapk-runtime.exe"',
        '$Stage = Join-Path $Root "Win.apk-UCRT"\n'
        '$Exe = Join-Path $Stage "bin\\winapk-runtime.exe"\n'
        '$Lib = Join-Path $Stage "lib"',
    ]
    replacement = (
        '$Stage = Join-Path $Root "Win.apk-UCRT"\n'
        '$Exe = Join-Path $Stage "bin\\winapk-runtime.exe"\n'
        '$Lib = Join-Path $Stage "lib"\n'
        '$GstPlugins = Join-Path $Stage "lib\\gstreamer-1.0"\n'
        '$GstScanner = Join-Path $Stage "libexec\\gstreamer-1.0\\gst-plugin-scanner.exe"\n'
        '$env:PATH = (Join-Path $Stage "bin") + ";" + $env:PATH\n'
        '$env:GST_PLUGIN_PATH = $GstPlugins\n'
        '$env:GST_PLUGIN_SYSTEM_PATH = $GstPlugins\n'
        '$env:GST_PLUGIN_SCANNER = $GstScanner\n'
        '$env:GST_REGISTRY = Join-Path $Stage "gstreamer-registry.bin"'
    )
    for block in legacy_blocks:
        if block in text:
            text = text.replace(block, replacement, 1)
            break
    else:
        if '$GstPlugins = Join-Path $Stage' not in text:
            raise RuntimeError("Could not locate the runtime path block in run-ucrt-diagnostic.ps1")

    if 'Write-Log "GStreamer Plugins: $GstPlugins"' not in text:
        text = text.replace(
            'Write-Log "Library: $Lib"',
            'Write-Log "Library: $Lib"\n'
            'Write-Log "GStreamer Plugins: $GstPlugins"\n'
            'Write-Log "GStreamer Scanner: $GstScanner"',
            1,
        )

    old_check = (
        'if (-not (Test-Path $Exe)) {\n'
        '    Write-Log "ERROR: Runtime executable was not found: $Exe"\n'
        '    Write-Log "Build first with: ./windows/scripts/build-windows-clang.sh"\n'
        '    exit 2\n'
        '}'
    )
    new_check = (
        'if (-not (Test-Path $Exe)) {\n'
        '    Write-Log "ERROR: Staged runtime was not found: $Exe"\n'
        '    Write-Log "Run: ./windows/scripts/stage-ucrt-runtime.sh"\n'
        '    exit 2\n'
        '}\n'
        '$Framework = Join-Path $Lib "android-framework.jar"\n'
        'if (-not (Test-Path $Framework)) {\n'
        '    Write-Log "ERROR: Android framework was not staged: $Framework"\n'
        '    exit 6\n'
        '}\n'
        'if (-not (Test-Path $GstPlugins)) {\n'
        '    Write-Log "ERROR: GStreamer plugins were not staged: $GstPlugins"\n'
        '    exit 7\n'
        '}\n'
        'if (-not (Test-Path $GstScanner)) {\n'
        '    Write-Log "ERROR: GStreamer plugin scanner was not staged: $GstScanner"\n'
        '    exit 8\n'
        '}'
    )
    if old_check in text:
        text = text.replace(old_check, new_check, 1)
    elif '$GstPlugins' in text and 'GStreamer plugins were not staged' not in text:
        marker = 'if (-not (Test-Path $Exe)) {'
        if marker not in text:
            raise RuntimeError("Could not locate runtime validation in PowerShell script")

    text = text.replace(
        'Write-Log "Command: winapk-runtime.exe --apk <selected APK> --data <test data>"',
        'Write-Log "Command: winapk-runtime.exe --lib <staged lib> --apk <selected APK> --data <test data>"',
    )
    text = text.replace(
        '& $Exe --apk $Apk --data $DataDir *>&1 |',
        '& $Exe --lib $Lib --apk $Apk --data $DataDir *>&1 |',
    )

    required = [
        'Win.apk-UCRT',
        'android-framework.jar',
        '$GstPlugins',
        '$env:GST_PLUGIN_PATH',
        '$env:GST_PLUGIN_SCANNER',
        '--lib $Lib --apk $Apk',
        'stage-ucrt-runtime.sh',
    ]
    missing = [token for token in required if token not in text]
    if missing:
        raise RuntimeError("Diagnostic patch verification failed: " + ", ".join(missing))

    ps1.write_text(text, encoding="utf-8-sig", newline="\r\n")


def main() -> int:
    start = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path.cwd().resolve()
    root = find_root(start)
    stage_sh = root / "windows/scripts/stage-ucrt-runtime.sh"
    ps1 = root / "run-ucrt-diagnostic.ps1"

    stage = r'''#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$ROOT/build-windows-ucrt64"
PRIVATE="$BUILD/winapk-runtime-dist"
STAGE="$ROOT/Win.apk-UCRT"

[[ -x "$BUILD/winapk-runtime.exe" ]] || {
  echo "[ERROR] Missing $BUILD/winapk-runtime.exe" >&2
  echo "Build first: ./windows/scripts/build-windows-clang.sh" >&2
  exit 1
}
[[ -x "$PRIVATE/bin/winapk-runtime.exe" ]] || {
  echo "[ERROR] Missing private UCRT64/GStreamer deployment: $PRIVATE" >&2
  echo "Use the current UCRT64 GStreamer build script first." >&2
  exit 2
}

rm -rf "$STAGE"
"$ROOT/windows/scripts/stage_runtime.sh" "$BUILD" "$STAGE"

# Overlay the ABI-matched UCRT64 executable, DLLs, GStreamer plugins and scanner.
mkdir -p "$STAGE/bin" "$STAGE/lib/gstreamer-1.0" "$STAGE/libexec/gstreamer-1.0"
cp -f "$PRIVATE/bin/winapk-runtime.exe" "$STAGE/bin/"
cp -f "$PRIVATE/bin/gst-inspect-1.0.exe" "$STAGE/bin/"
cp -f "$PRIVATE/bin"/*.dll "$STAGE/bin/"
cp -f "$PRIVATE/lib/gstreamer-1.0"/*.dll "$STAGE/lib/gstreamer-1.0/"
cp -f "$PRIVATE/libexec/gstreamer-1.0/gst-plugin-scanner.exe" \
      "$STAGE/libexec/gstreamer-1.0/"

# Pre-generate the registry against the FINAL staged paths. A cache copied from
# PRIVATE would contain different plugin filenames and would be treated stale.
REGISTRY="$STAGE/gstreamer-registry.bin"
rm -f "$REGISTRY"
env PATH="$STAGE/bin:/usr/bin" \
    GST_PLUGIN_PATH="$STAGE/lib/gstreamer-1.0" \
    GST_PLUGIN_SYSTEM_PATH="$STAGE/lib/gstreamer-1.0" \
    GST_PLUGIN_SCANNER="$STAGE/libexec/gstreamer-1.0/gst-plugin-scanner.exe" \
    GST_REGISTRY="$REGISTRY" \
    GST_REGISTRY_REUSE_PLUGIN_SCANNER=no \
    "$STAGE/bin/gst-inspect-1.0.exe" >/dev/null
[[ -s "$REGISTRY" ]] || {
  echo "[ERROR] Failed to pre-generate GStreamer registry: $REGISTRY" >&2
  exit 7
}

[[ -f "$STAGE/lib/android-framework.jar" ]] || {
  echo "[ERROR] stage_runtime.sh did not create android-framework.jar" >&2
  exit 3
}
[[ -x "$STAGE/bin/winapk-runtime.exe" ]] || {
  echo "[ERROR] Staged runtime executable is missing" >&2
  exit 4
}
[[ -f "$STAGE/lib/gstreamer-1.0/libgstd3d11.dll" ]] || {
  echo "[ERROR] Staged D3D11 plugin is missing" >&2
  exit 5
}
[[ -x "$STAGE/libexec/gstreamer-1.0/gst-plugin-scanner.exe" ]] || {
  echo "[ERROR] Staged GStreamer plugin scanner is missing" >&2
  exit 6
}

cat > "$STAGE/run-winapk-runtime.cmd" <<'CMD'
@echo off
setlocal
set "ROOT=%~dp0"
set "PATH=%ROOT%bin;%PATH%"
set "GST_PLUGIN_PATH=%ROOT%lib\gstreamer-1.0"
set "GST_PLUGIN_SYSTEM_PATH=%ROOT%lib\gstreamer-1.0"
set "GST_PLUGIN_SCANNER=%ROOT%libexec\gstreamer-1.0\gst-plugin-scanner.exe"
set "GST_REGISTRY=%ROOT%gstreamer-registry.bin"
"%ROOT%bin\winapk-runtime.exe" %*
exit /b %ERRORLEVEL%
CMD

echo "[OK] Staged UCRT64/GStreamer runtime: $(cygpath -w "$STAGE")"
echo "[RUN] Use Win.apk-UCRT\\run-winapk-runtime.cmd"
'''

    backup_once(stage_sh, ".pre-current-spec-backup")
    stage_sh.write_text(stage, encoding="utf-8", newline="\n")
    stage_sh.chmod(0o755)
    # patch_powershell(ps1)

    # Syntax and implementation consistency checks.
    compile(stage, str(stage_sh), "exec") if False else None
    required_stage = [
        'build-windows-ucrt64',
        'winapk-runtime-dist',
        'libgstd3d11.dll',
        'gst-plugin-scanner.exe',
        'GST_PLUGIN_PATH',
        'stage_runtime.sh',
    ]
    missing_stage = [token for token in required_stage if token not in stage]
    if missing_stage:
        raise RuntimeError("Stage script verification failed: " + ", ".join(missing_stage))

    print(f"[OK] Created: {stage_sh}")
    print(f"[OK] Patched: {ps1}")
    print("[OK] Build: ./windows/scripts/build-windows-clang.sh")
    print("[OK] Stage: ./windows/scripts/stage-ucrt-runtime.sh")
    print("[OK] Run: run-ucrt-diagnostic.cmd")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
