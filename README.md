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
| Desktop UI (Windows) | `HangulPad.exe` engine layout (fonts, tables/nested, images, floats, containers, equations) |
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
| `build/hp` | Linux 개발용 |
| `dist/hp` | Linux 배포용 (`make release`) |
| `dist/hp.exe` | Windows x64 정적 빌드 (`make release-windows`) |
| `dist/hp-windows-x86_64-portable.zip` | **DLL 포함 포터블 패키지** (권장) |
| `dist/hangulpad-source.zip` | **소스 코드** (`make release-source`) |

### 다운로드 (릴리스 v0.1.0-windows)

- **Windows 포터블:** https://github.com/yanghanjin-dev/hangulpad/releases/download/v0.1.0-windows/hp-windows-x86_64-portable.zip  
- **소스 코드:** https://github.com/yanghanjin-dev/hangulpad/releases/download/v0.1.0-windows/hangulpad-source.zip  
- 또는 GitHub: https://github.com/yanghanjin-dev/hangulpad/archive/refs/heads/cursor/layout-metrics-fonts-956b.zip  

ZIP을 풀고 **`HangulPad.exe`** 실행 → HWPX를 **엔진으로 읽어** 글꼴·크기·들여쓰기·행간·표(중첩)·이미지·떠있는 개체·컨테이너·수식을 반영한 페이지 보기로 표시합니다 (그림 파일 슬라이드가 아님).  
- `documents/corpus/` 에 규제합리화·윤리·식약처 샘플 HWPX가 있습니다.  
- 이전/다음, 방향키, PageUp/Down, 스페이스, 마우스 휠로 넘깁니다.  
- `HangulPad.exe`와 DLL, `documents/` 를 함께 두세요.  
- CLI: 같은 폴더의 `hp.exe`  
- 소스: `make` / `make release-windows-portable` (의존성: `libzip`, Windows는 MinGW)

```bat
HangulPad.exe
hp.exe new demo.hwpx --text "한글패드"
```

Windows 정적/크로스 빌드 (개발용):

```bash
sudo apt-get install -y gcc-mingw-w64-x86-64 cmake curl
./scripts/build-win-deps.sh
make release-windows
```

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

- Tables/shapes/headers full fidelity (paragraph text styles are rendered)
- Binary HWP write
- Complete WordPad-class editing UI
