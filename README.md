# 한글패드 / HangulPad

가볍고 단순한 한글 워드 프로세서를 목표로 하는 오픈소스 프로젝트입니다.  
**엔진 우선:** HWPX 읽기·쓰기와 IR을 먼저 만들고, Tauri 데스크톱 UI는 이후 단계에서 붙입니다.

A lightweight Korean word processor (WordPad-simple). **Engine first:** HWPX read/write + internal IR now; Tauri UI later.

## Status (Phase 0–1 scaffold)

| Area | Status |
|------|--------|
| `hp-ir` internal document model | MVP (paragraph / run / section) |
| `hp-hwpx` HWPX read/write | Minimal valid package + plain-text roundtrip |
| `hp-hwp` HWP 5.x | **Read stub** (CFB detect only) |
| `hp-cli` (`hp`) | `open` / `new` / `save` / `roundtrip` / `probe` |
| Tauri WordPad UI | Deferred |
| Full HWP record parse / HWP write | Deferred |

Default save format: **HWPX**.

## Workspace

```
crates/
  hp-ir/     Internal document model
  hp-hwpx/   HWPX (OWPML) codec
  hp-hwp/    HWP 5.x codec (stub)
  hp-cli/    Engine CLI binary `hp`
```

## Build & test

```bash
cargo test --workspace
cargo clippy --workspace --all-targets -- -D warnings
cargo run -p hp-cli -- --help
```

### HWPX roundtrip

```bash
cargo run -p hp-cli -- new /tmp/a.hwpx --text "한글패드" --title demo
cargo run -p hp-cli -- roundtrip /tmp/a.hwpx /tmp/b.hwpx
cargo run -p hp-cli -- open /tmp/b.hwpx
```

## License

MIT OR Apache-2.0 — see [`LICENSE`](LICENSE), [`LICENSE-MIT`](LICENSE-MIT), [`LICENSE-APACHE`](LICENSE-APACHE).

## Attribution

See [`NOTICE`](NOTICE) for the Hancom HWP / OWPML attribution placeholder required when referencing Hancom's public format documentation.

## Non-goals (this PR)

- Full WordPad UI / Tauri app shell
- Binary HWP write
- Tables, images, headers/footers, encryption (IR slots reserved later)
