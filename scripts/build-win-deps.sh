#!/usr/bin/env bash
# Cross-build static zlib + libzip for MinGW-w64 (x86_64) into WIN_PREFIX.
set -euo pipefail

WIN_PREFIX="${WIN_PREFIX:-/tmp/win-prefix}"
WORKDIR="${WORKDIR:-/tmp/win-deps}"
ZLIB_VER="${ZLIB_VER:-1.3.1}"
LIBZIP_VER="${LIBZIP_VER:-1.10.1}"

mkdir -p "$WIN_PREFIX"/{bin,include,lib/pkgconfig} "$WORKDIR"
cd "$WORKDIR"

if [[ ! -f "$WIN_PREFIX/lib/libz.a" ]]; then
  curl -fsSL -o "zlib-${ZLIB_VER}.tar.gz" \
    "https://github.com/madler/zlib/releases/download/v${ZLIB_VER}/zlib-${ZLIB_VER}.tar.gz"
  tar xf "zlib-${ZLIB_VER}.tar.gz"
  make -C "zlib-${ZLIB_VER}" -f win32/Makefile.gcc \
    PREFIX=x86_64-w64-mingw32- \
    BINARY_PATH="$WIN_PREFIX/bin" \
    INCLUDE_PATH="$WIN_PREFIX/include" \
    LIBRARY_PATH="$WIN_PREFIX/lib" \
    SHARED_MODE=0 install
fi

if [[ ! -f "$WIN_PREFIX/lib/libzip.a" ]]; then
  curl -fsSL -o "libzip-${LIBZIP_VER}.tar.xz" \
    "https://github.com/nih-at/libzip/releases/download/v${LIBZIP_VER}/libzip-${LIBZIP_VER}.tar.xz"
  tar xf "libzip-${LIBZIP_VER}.tar.xz"
  cmake -S "libzip-${LIBZIP_VER}" -B "libzip-${LIBZIP_VER}/build" \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
    -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres \
    -DZLIB_INCLUDE_DIR="$WIN_PREFIX/include" \
    -DZLIB_LIBRARY="$WIN_PREFIX/lib/libz.a" \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TOOLS=OFF \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_DOC=OFF \
    -DENABLE_COMMONCRYPTO=OFF \
    -DENABLE_GNUTLS=OFF \
    -DENABLE_MBEDTLS=OFF \
    -DENABLE_OPENSSL=OFF \
    -DENABLE_WINDOWS_CRYPTO=OFF \
    -DENABLE_BZIP2=OFF \
    -DENABLE_LZMA=OFF \
    -DENABLE_ZSTD=OFF \
    -DCMAKE_INSTALL_PREFIX="$WIN_PREFIX"
  cmake --build "libzip-${LIBZIP_VER}/build" -j"$(nproc)"
  cmake --install "libzip-${LIBZIP_VER}/build"
fi

echo "Windows deps ready under $WIN_PREFIX"
ls -la "$WIN_PREFIX/lib/libz.a" "$WIN_PREFIX/lib/libzip.a"
