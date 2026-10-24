# HangulPad engine (C11) — Phase 0–1
CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
CPPFLAGS += -Iinclude
LDFLAGS +=
LDLIBS += $(shell pkg-config --libs libzip 2>/dev/null || echo -lzip)
PREFIX ?= /usr/local
VERSION ?= 0.1.0

LIB_SRCS := src/common.c src/ir.c src/hwpx_read.c src/hwpx_write.c src/hwp.c
LIB_OBJS := $(LIB_SRCS:src/%.c=build/%.o)

.PHONY: all clean test cli check release release-windows install uninstall dist

all: cli test

build:
	mkdir -p build build/win

dist:
	mkdir -p dist

build/%.o: src/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/cli.o: src/cli.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/test_roundtrip.o: tests/test_roundtrip.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

cli: build/hp
build/hp: build/cli.o $(LIB_OBJS)
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

# Stripped release binary under dist/
release: dist/hp
dist/hp: build/hp | dist
	cp -f build/hp dist/hp
	strip dist/hp
	@echo "executable: $$(pwd)/dist/hp"
	@file dist/hp
	@ls -la dist/hp

# Cross-compile Windows x86_64 console EXE (needs mingw + deps in WIN_PREFIX).
# Default: /tmp/win-prefix with static libzip + zlib (see scripts/build-win-deps.sh).
WIN_CC ?= x86_64-w64-mingw32-gcc
WIN_PREFIX ?= /tmp/win-prefix
WIN_CFLAGS ?= -std=c11 -Wall -Wextra -O2 -Iinclude -I$(WIN_PREFIX)/include
WIN_LIBS ?= $(WIN_PREFIX)/lib/libzip.a $(WIN_PREFIX)/lib/libz.a -lbcrypt

release-windows: dist/hp.exe
dist/hp.exe: $(LIB_SRCS) src/cli.c | dist build
	@command -v $(WIN_CC) >/dev/null || { echo "missing $(WIN_CC); install gcc-mingw-w64-x86-64"; exit 1; }
	@test -f $(WIN_PREFIX)/lib/libzip.a || { echo "missing $(WIN_PREFIX)/lib/libzip.a — run scripts/build-win-deps.sh first"; exit 1; }
	$(WIN_CC) $(WIN_CFLAGS) -c src/common.c -o build/win/common.o
	$(WIN_CC) $(WIN_CFLAGS) -c src/ir.c -o build/win/ir.o
	$(WIN_CC) $(WIN_CFLAGS) -c src/hwpx_read.c -o build/win/hwpx_read.o
	$(WIN_CC) $(WIN_CFLAGS) -c src/hwpx_write.c -o build/win/hwpx_write.o
	$(WIN_CC) $(WIN_CFLAGS) -c src/hwp.c -o build/win/hwp.o
	$(WIN_CC) $(WIN_CFLAGS) -c src/cli.c -o build/win/cli.o
	$(WIN_CC) -static -o dist/hp.exe build/win/common.o build/win/ir.o build/win/hwpx_read.o build/win/hwpx_write.o build/win/hwp.o build/win/cli.o $(WIN_LIBS)
	x86_64-w64-mingw32-strip dist/hp.exe
	@echo "executable: $$(pwd)/dist/hp.exe"
	@file dist/hp.exe
	@ls -la dist/hp.exe

install: dist/hp
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 dist/hp $(DESTDIR)$(PREFIX)/bin/hp

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/hp

test: build/test_roundtrip
	./build/test_roundtrip

build/test_roundtrip: build/test_roundtrip.o $(LIB_OBJS)
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

check: test

clean:
	rm -rf build dist

# Regenerate the checked-in HWPX fixture (optional).
fixture: build/hp
	./build/hp new tests/fixtures/simple_paragraph.hwpx --text "한글패드" --title simple_paragraph
