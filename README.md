# 한글패드 / HangulPad

가볍고 단순한 한글 워드 프로세서를 목표로 하는 오픈소스 프로젝트입니다.  
**엔진 우선:** HWPX 읽기·쓰기와 IR을 먼저 만들고, 데스크톱 UI는 이후 단계에서 붙입니다.

A lightweight Korean word processor (WordPad-simple). **Engine first:** HWPX read/write + internal IR now; desktop UI later.

> **Engine language:** **C11** (confirmed). Desktop UI later — Tauri 2 shell planned, talking to this C engine via FFI.

## Status (Phase 0–1 scaffold)

| Area | Status |
|------|--------|
| `hp` IR (`include/hp/ir.h`) | MVP (paragraph / run / section) |
| HWPX read/write | Minimal valid package + plain-text roundtrip |
| HWP 5.x | **Read stub** (CFB detect only) |
| CLI (`build/hp`) | `open` / `new` / `save` / `roundtrip` / `probe` |
| Desktop UI | Deferred |
| Full HWP record parse / HWP write | Deferred |

Default save format: **HWPX**.

## Layout

```
include/hp/   Public headers (ir, hwpx, hwp, common)
src/          Engine + CLI sources
tests/        Roundtrip tests + fixtures
Makefile      Build / test
```

## Build & test

Dependencies: C11 compiler, `libzip` (`libzip-dev` on Debian/Ubuntu).

```bash
make          # builds build/hp and runs tests
make cli      # debug binary → build/hp
make release  # stripped executable → dist/hp
make install  # install to /usr/local/bin/hp (PREFIX=... optional)
```

실행 파일:

| 경로 | 설명 |
|------|------|
| `build/hp` | 개발용 바이너리 |
| `dist/hp` | `make release` 후 배포용 (strip) |

### HWPX roundtrip

```bash
./build/hp new /tmp/a.hwpx --text "한글패드" --title demo
./build/hp roundtrip /tmp/a.hwpx /tmp/b.hwpx
./build/hp open /tmp/b.hwpx
```

## License

MIT OR Apache-2.0 — see [`LICENSE`](LICENSE), [`LICENSE-MIT`](LICENSE-MIT), [`LICENSE-APACHE`](LICENSE-APACHE).

## Attribution

See [`NOTICE`](NOTICE) for the Hancom HWP / OWPML attribution placeholder required when referencing Hancom's public format documentation.

## Non-goals (this PR)

- Full WordPad UI / desktop app shell
- Binary HWP write
- Tables, images, headers/footers, encryption
