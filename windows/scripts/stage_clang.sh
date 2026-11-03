#!/usr/bin/env bash
# stage_clang.sh <runtime dir> -- copies the clang Win.apk translates arm64
# libraries with (include/winapk/arm64_aot.h) into <runtime dir>/clang, so a
# user never has to install one. winapk-runtime looks for
# clang\bin\clang.exe next to bin\ (aot_clang in host/runtime_main.cpp).
#
# Only what that compile needs: clang and lld with the DLLs they load, the
# MinGW start files and import libraries the link names, compiler-rt's
# builtins, and the two headers the generated C includes (stddef.h and
# stdint.h; without MinGW's headers beside it, clang's stdint.h defines
# everything itself). Taken from the clang running this script: MSYS2
# CLANG64, as CI builds with.
set -euo pipefail

OUT="${1:?usage: stage_clang.sh <runtime dir>}/clang"
CLANG="$(command -v clang)"
PREFIX="$(cd "$(dirname "$CLANG")/.." && pwd)"
RES="$(clang -print-resource-dir)"
RES="$(cygpath -u "$RES" 2>/dev/null || echo "$RES")"
VER="$(basename "$RES")"

rm -rf "$OUT"
mkdir -p "$OUT/bin" "$OUT/lib/clang/$VER/include" "$OUT/lib/clang/$VER/lib/windows"

# The compiler and the linker, and every DLL they load from the toolchain.
cp -f "$PREFIX/bin/clang.exe" "$OUT/bin/clang.exe"
cp -f "$PREFIX/bin/ld.lld.exe" "$OUT/bin/ld.lld.exe"
ldd "$PREFIX/bin/clang.exe" "$PREFIX/bin/ld.lld.exe" | awk '{print $3}' | grep -i "^$PREFIX/bin/" | sort -u |
    while read -r dll; do cp -f "$dll" "$OUT/bin/"; done

# What `clang -shared` links a DLL with (clang -### lists it).
for f in dllcrt2.o crtbegin.o crtend.o libmingw32.a libmingwex.a libmoldname.a libmsvcrt.a libunwind.a \
         libkernel32.a libadvapi32.a libshell32.a libuser32.a; do
    cp -f "$PREFIX/lib/$f" "$OUT/lib/"
done
cp -f "$RES/lib/windows/libclang_rt.builtins-x86_64.a" "$OUT/lib/clang/$VER/lib/windows/"

# stddef.h and stdint.h, with the pieces stddef.h includes.
cp -f "$RES/include/stddef.h" "$RES/include/stdint.h" "$RES/include"/__stddef_*.h "$OUT/lib/clang/$VER/include/"

# Licenses: LLVM (Apache-2.0 with LLVM exceptions) and MinGW-w64.
mkdir -p "$OUT/licenses"
for d in "$PREFIX/share/licenses"/{clang,llvm,lld,compiler-rt,mingw-w64-crt,libc++,zlib,zstd,libffi,libiconv,libxml2}; do
    [[ -d "$d" ]] && cp -rf "$d" "$OUT/licenses/" || true
done

# It must compile on its own: no MSYS2 on the PATH, as on a user's machine.
probe="$(mktemp -d)"
printf '#include <stddef.h>\n#include <stdint.h>\n__declspec(dllexport) int64_t f(int64_t a) { return a * 3; }\n' \
    > "$probe/t.c"
env -i SYSTEMROOT="${SYSTEMROOT:-C:\\Windows}" PATH="/usr/bin:/bin" \
    "$OUT/bin/clang.exe" -O1 -march=x86-64-v2 -shared -fno-strict-aliasing -w -o "$probe/t.dll" "$probe/t.c"
rm -rf "$probe"
echo "[stage_clang] clang $VER -> $OUT ($(du -sh "$OUT" | cut -f1))"
