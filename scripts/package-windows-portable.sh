#!/usr/bin/env bash
# Build shared zlib/libzip for MinGW and package GUI + CLI + DLLs into a zip.
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
for f in common ir hwpx_read hwpx_write hwp; do
  "$WIN_CC" $CFLAGS -c "src/${f}.c" -o "build/win-dll/${f}.o"
done
"$WIN_CC" $CFLAGS -c src/cli.c -o build/win-dll/cli.o
"$WIN_CC" $CFLAGS -c src/gui_win.c -o build/win-dll/gui_win.o

LIBS=(-L"$WIN_PREFIX/lib" -lzip -lz -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic -lcomdlg32 -luser32 -lgdi32 -lshell32)

# Console CLI
"$WIN_CC" -o "$OUT_DIR/hp.exe" \
  build/win-dll/common.o build/win-dll/ir.o build/win-dll/hwpx_read.o \
  build/win-dll/hwpx_write.o build/win-dll/hwp.o build/win-dll/cli.o \
  "${LIBS[@]}"
x86_64-w64-mingw32-strip "$OUT_DIR/hp.exe"

# GUI app (no console window). -municode selects wWinMain entry.
"$WIN_CC" -mwindows -municode -o "$OUT_DIR/HangulPad.exe" \
  build/win-dll/common.o build/win-dll/ir.o build/win-dll/hwpx_read.o \
  build/win-dll/hwpx_write.o build/win-dll/hwp.o build/win-dll/gui_win.o \
  "${LIBS[@]}"
x86_64-w64-mingw32-strip "$OUT_DIR/HangulPad.exe"

cp -f "$WIN_PREFIX/bin/zlib1.dll" "$OUT_DIR/"
cp -f "$WIN_PREFIX/bin/libzip.dll" "$OUT_DIR/"
[[ -f /usr/x86_64-w64-mingw32/lib/libwinpthread-1.dll ]] && \
  cp -f /usr/x86_64-w64-mingw32/lib/libwinpthread-1.dll "$OUT_DIR/"
[[ -f /usr/lib/gcc/x86_64-w64-mingw32/13-win32/libgcc_s_seh-1.dll ]] && \
  cp -f /usr/lib/gcc/x86_64-w64-mingw32/13-win32/libgcc_s_seh-1.dll "$OUT_DIR/"

cat > "$OUT_DIR/사용법.txt" << 'EOF'
한글패드 Windows 포터블 패키지

★ 창 프로그램 (권장)
  HangulPad.exe  더블클릭 → 윈도우 창이 열리고 내용을 보고 편집할 수 있습니다.
  메뉴: 파일 → 열기 / 저장 (HWPX)

★ 콘솔 CLI
  hp.exe new demo.hwpx --text "한글패드"
  hp.exe open demo.hwpx

중요: HangulPad.exe / hp.exe 와 모든 DLL 을 같은 폴더에 두세요.
EOF

cat > "$OUT_DIR/README.txt" << 'EOF'
HangulPad Windows x64 portable

Run HangulPad.exe for the GUI window (open/save HWPX, edit text).
Keep all DLLs next to the EXE files.
CLI: hp.exe
EOF

# Sample document for first launch demo (optional)
if [[ -x "$ROOT/build/hp" ]]; then
  "$ROOT/build/hp" new "$OUT_DIR/샘플.hwpx" --text "한글패드 샘플입니다. 파일→열기로 이 문서를 불러보세요." --title "샘플" || true
fi

rm -f "$ZIP_OUT"
(cd "$ROOT/dist" && zip -r "$(basename "$ZIP_OUT")" windows-portable)
echo "Wrote $ZIP_OUT"
ls -la "$OUT_DIR" "$ZIP_OUT"
file "$OUT_DIR/HangulPad.exe" "$OUT_DIR/hp.exe"
