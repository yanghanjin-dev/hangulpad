# HangulPad engine (C11) — Phase 0–1
CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
CPPFLAGS += -Iinclude
LDFLAGS +=
LDLIBS += $(shell pkg-config --libs libzip 2>/dev/null || echo -lzip)

LIB_SRCS := src/common.c src/ir.c src/hwpx_read.c src/hwpx_write.c src/hwp.c
LIB_OBJS := $(LIB_SRCS:src/%.c=build/%.o)

.PHONY: all clean test cli check

all: cli test

build:
	mkdir -p build

build/%.o: src/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/cli.o: src/cli.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/test_roundtrip.o: tests/test_roundtrip.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

cli: build/hp
build/hp: build/cli.o $(LIB_OBJS)
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

test: build/test_roundtrip
	./build/test_roundtrip

build/test_roundtrip: build/test_roundtrip.o $(LIB_OBJS)
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

check: test

clean:
	rm -rf build

# Regenerate the checked-in HWPX fixture (optional).
fixture: build/hp
	./build/hp new tests/fixtures/simple_paragraph.hwpx --text "한글패드" --title simple_paragraph
