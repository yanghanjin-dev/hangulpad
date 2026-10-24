#!/usr/bin/env bash
# Build shared zlib/libzip for MinGW and package hp.exe + DLLs into a zip.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WIN_PREFIX="${WIN_PREFIX:-/tmp/win-prefix-dll}"
WORKDIR="${WORKDIR:-/tmp/win-deps-dll}"
OUT_DIR="$ROOT/dist/windows-portable"
ZIP_OUT="$ROOT/dist/hp-windows-x86_64-portable.zip"
WIN_CC="${WIN_CC:-x86_64-w64-mingw32-gcc}"

command -v "$WIN_CC" >/dev/null || { echo "missing $WIN_CC"; exit 1; }

if [[ ! -f "$WIN_PREFIX/bin/libzip.dll" || ! -f "$WIN_PREFIX/bin/zlib1.dll" ]]; then
  echo "Building shared Windows deps into $WIN_PREFIX ..."
  mkdir -p "$WIN_PREFIX"/{bin,include,lib} "$WORKDIR"
  cd "$WORKDIR"
  if [[ ! -f "$WIN_PREFIX/bin/zlib1.dll" ]]; then
    curl -fsSL -o zlib-1.3.1.tar.gz https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
    tar xf zlib-1.3.1.tar.gz
    make -C zlib-1.3.1 -f win32/Makefile.gcc \
      PREFIX=x86_64-w64-mingw32- \
      BINARY_PATH="$WIN_PREFIX/bin" INCLUDE_PATH="$WIN_PREFIX/include" \
      LIBRARY_PATH="$WIN_PREFIX/lib" SHARED_MODE=1 install
  fi
  if [[ ! -f "$WIN_PREFIX/bin/libzip.dll" ]]; then
    curl -fsSL -o libzip-1.10.1.tar.xz https://libzip.org/download/libzip-1.10.1.tar.xz
    tar xf libzip-1.10.1.tar.xz
    cmake -S libzip-1.10.1 -B libzip-1.10.1/build \
      -DCMAKE_SYSTEM_NAME=Windows \
      -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
      -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres \
      -DZLIB_INCLUDE_DIR="$WIN_PREFIX/include" \
      -DZLIB_LIBRARY="$WIN_PREFIX/lib/libz.dll.a" \
      -DBUILD_SHARED_LIBS=ON -DBUILD_TOOLS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_DOC=OFF \
      -DENABLE_COMMONCRYPTO=OFF -DENABLE_GNUTLS=OFF -DENABLE_MBEDTLS=OFF \
      -DENABLE_OPENSSL=OFF -DENABLE_WINDOWS_CRYPTO=OFF \
      -DENABLE_BZIP2=OFF -DENABLE_LZMA=OFF -DENABLE_ZSTD=OFF \
      -DCMAKE_INSTALL_PREFIX="$WIN_PREFIX"
    cmake --build libzip-1.10.1/build -j"$(nproc)"
    cmake --install libzip-1.10.1/build
  fi
fi

cd "$ROOT"
mkdir -p build/win-dll "$OUT_DIR"
CFLAGS="-std=c11 -Wall -Wextra -O2 -Iinclude -I$WIN_PREFIX/include"
for f in common ir hwpx_read hwpx_write hwp cli; do
  "$WIN_CC" $CFLAGS -c "src/${f}.c" -o "build/win-dll/${f}.o"
done
"$WIN_CC" -o "$OUT_DIR/hp.exe" build/win-dll/*.o \
  -L"$WIN_PREFIX/lib" -lzip -lz \
  -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic
x86_64-w64-mingw32-strip "$OUT_DIR/hp.exe"

cp -f "$WIN_PREFIX/bin/zlib1.dll" "$OUT_DIR/"
cp -f "$WIN_PREFIX/bin/libzip.dll" "$OUT_DIR/"
[[ -f /usr/x86_64-w64-mingw32/lib/libwinpthread-1.dll ]] && \
  cp -f /usr/x86_64-w64-mingw32/lib/libwinpthread-1.dll "$OUT_DIR/"
[[ -f /usr/lib/gcc/x86_64-w64-mingw32/13-win32/libgcc_s_seh-1.dll ]] && \
  cp -f /usr/lib/gcc/x86_64-w64-mingw32/13-win32/libgcc_s_seh-1.dll "$OUT_DIR/"

cat > "$OUT_DIR/사용법.txt" << 'EOF'
한글패드 엔진 CLI (Windows x64 portable)
hp.exe 와 DLL 을 같은 폴더에 두고 실행하세요.

  hp.exe new demo.hwpx --text "한글패드"
  hp.exe open demo.hwpx
EOF

cat > "$OUT_DIR/README.txt" << 'EOF'
HangulPad engine CLI — Windows x64 portable
Keep hp.exe and DLLs in the same folder, then run hp.exe.
EOF

rm -f "$ZIP_OUT"
(cd "$ROOT/dist" && zip -r "$(basename "$ZIP_OUT")" windows-portable)
echo "Wrote $ZIP_OUT"
ls -la "$OUT_DIR" "$ZIP_OUT"
