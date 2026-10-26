#!/usr/bin/env bash
# Build shared zlib/libzip for MinGW and package GUI + CLI + DLLs + sample pages.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(sed -n 's/^#define HP_VERSION_STRING "\(.*\)"/\1/p' "$ROOT/include/hp/version.h")"
WIN_PREFIX="${WIN_PREFIX:-/tmp/win-prefix-dll}"
WORKDIR="${WORKDIR:-/tmp/win-deps-dll}"
OUT_DIR="$ROOT/dist/windows-portable"
ZIP_OUT="$ROOT/dist/hangulpad-${VERSION}-windows-x86_64-portable.zip"
WIN_CC="${WIN_CC:-x86_64-w64-mingw32-gcc}"
SAMPLE_HWPX_SRC="${SAMPLE_HWPX_SRC:-}"
SAMPLE_PAGES_SRC="${SAMPLE_PAGES_SRC:-$ROOT/sample/pages}"

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
mkdir -p build/win-dll
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"
CFLAGS="-std=c11 -Wall -Wextra -O2 -Iinclude -I$WIN_PREFIX/include"
for f in common ir xml hwpx_package hwpx_read hwpx_write hwp; do
  "$WIN_CC" $CFLAGS -c "src/${f}.c" -o "build/win-dll/${f}.o"
done
"$WIN_CC" $CFLAGS -c src/cli.c -o build/win-dll/cli.o
"$WIN_CC" $CFLAGS -c src/gui_win.c -o build/win-dll/gui_win.o
# Version info shown in Explorer > Properties > Details
x86_64-w64-mingw32-windres -Iinclude res/hangulpad.rc -O coff -o build/win-dll/version_res.o

LIBS_CLI=(-L"$WIN_PREFIX/lib" -lzip -lz -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic -lshell32)
LIBS_GUI=(-L"$WIN_PREFIX/lib" -lzip -lz -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic \
  -lcomdlg32 -luser32 -lgdi32 -lshell32 -lole32 -luuid -lwindowscodecs)

# Console CLI
"$WIN_CC" -o "$OUT_DIR/hp.exe" \
  build/win-dll/common.o build/win-dll/ir.o build/win-dll/xml.o build/win-dll/hwpx_package.o build/win-dll/hwpx_read.o \
  build/win-dll/hwpx_write.o build/win-dll/hwp.o build/win-dll/cli.o build/win-dll/version_res.o \
  "${LIBS_CLI[@]}"
x86_64-w64-mingw32-strip "$OUT_DIR/hp.exe"

# GUI — HWPX engine layout renderer (fonts/sizes). -municode → wWinMain.
"$WIN_CC" -mwindows -municode -o "$OUT_DIR/HangulPad.exe" \
  build/win-dll/common.o build/win-dll/ir.o build/win-dll/xml.o build/win-dll/hwpx_package.o build/win-dll/hwpx_read.o \
  build/win-dll/hwpx_write.o build/win-dll/hwp.o build/win-dll/gui_win.o build/win-dll/version_res.o \
  "${LIBS_GUI[@]}"
x86_64-w64-mingw32-strip "$OUT_DIR/HangulPad.exe"

cp -f "$WIN_PREFIX/bin/zlib1.dll" "$OUT_DIR/"
cp -f "$WIN_PREFIX/bin/libzip.dll" "$OUT_DIR/"
[[ -f /usr/x86_64-w64-mingw32/lib/libwinpthread-1.dll ]] && \
  cp -f /usr/x86_64-w64-mingw32/lib/libwinpthread-1.dll "$OUT_DIR/"
[[ -f /usr/lib/gcc/x86_64-w64-mingw32/13-win32/libgcc_s_seh-1.dll ]] && \
  cp -f /usr/lib/gcc/x86_64-w64-mingw32/13-win32/libgcc_s_seh-1.dll "$OUT_DIR/"

# Sample HWPX — GUI renders via HWPX engine (fonts/sizes), not page images.
mkdir -p "$OUT_DIR/documents"
DOC_NAME="공공부문-인공지능-윤리기준.hwpx"
if [[ -z "$SAMPLE_HWPX_SRC" ]]; then
  for cand in \
    "$ROOT/sample/documents/$DOC_NAME" \
    "$ROOT/sample/documents/sample.hwpx" \
    /home/ubuntu/.cursor/projects/workspace/uploads/_______________3650.hwpx \
    /home/ubuntu/.cursor/projects/workspace/uploads/_______________0f34.hwpx
  do
    if [[ -f "$cand" ]]; then SAMPLE_HWPX_SRC="$cand"; break; fi
  done
fi
if [[ -n "$SAMPLE_HWPX_SRC" && -f "$SAMPLE_HWPX_SRC" ]]; then
  cp -f "$SAMPLE_HWPX_SRC" "$OUT_DIR/documents/$DOC_NAME"
  cp -f "$SAMPLE_HWPX_SRC" "$OUT_DIR/documents/sample.hwpx"
else
  echo "WARN: sample HWPX not found; generating minimal sample" >&2
  if [[ -x "$ROOT/build/hp" ]]; then
    "$ROOT/build/hp" new "$OUT_DIR/documents/sample.hwpx" \
      --text "한글패드 샘플입니다." --title "샘플" || true
  fi
fi

# Corpus HWPX (nested tables / floats / containers) for fidelity checks
mkdir -p "$OUT_DIR/documents/corpus"
if [[ -d "$ROOT/sample/corpus" ]]; then
  shopt -s nullglob
  for f in "$ROOT/sample/corpus"/*.hwpx; do
    cp -f "$f" "$OUT_DIR/documents/corpus/"
  done
  shopt -u nullglob
fi

cat > "$OUT_DIR/사용법.txt" << EOF
한글패드 Windows 포터블 패키지 v${VERSION}

★ 창 프로그램 (권장)
  HangulPad.exe  더블클릭 → HWPX 엔진이 문서를 읽어
  글꼴·크기·정렬·표(중첩)·이미지·떠있는 개체·컨테이너·수식을
  반영해 PDF처럼 페이지로 표시합니다. (페이지 그림 파일이 아닙니다)

  이전/다음, 방향키, PageUp/Down, 스페이스, 마우스 휠로 이동
  메뉴: 파일 → 샘플 문서 열기 / HWPX 열기

★ 폴더 구조
  documents\\공공부문-인공지능-윤리기준.hwpx
  documents\\corpus\\  ← 중첩 표·떠있는 개체 비교용 HWPX

★ 콘솔 CLI
  hp.exe new demo.hwpx --text "한글패드"
  hp.exe open demo.hwpx
  hp.exe --version

중요: HangulPad.exe / hp.exe 와 모든 DLL 을 같은 폴더에 두세요.
EOF

cat > "$OUT_DIR/README.txt" << EOF
HangulPad v${VERSION} Windows x64 portable — HWPX engine layout viewer

Run HangulPad.exe: opens HWPX via the engine (fonts, tables including
nested, images, floats, containers, equations). Not JPEG page slides.
Keep all DLLs next to the EXE files. Samples: documents\\ and documents\\corpus\\
CLI: hp.exe
EOF

rm -f "$ZIP_OUT"
(cd "$ROOT/dist" && zip -r "$(basename "$ZIP_OUT")" windows-portable)
echo "Wrote $ZIP_OUT"
ls -la "$OUT_DIR" "$OUT_DIR/documents" "$ZIP_OUT"
file "$OUT_DIR/HangulPad.exe" "$OUT_DIR/hp.exe"
