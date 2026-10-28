# Sample corpus (HWPX + HWP + PDF)

Used to hillclimb the HangulPad layout engine against PDF ground truth.

| Pair | HWPX | PDF |
|------|------|-----|
| 공공부문 인공지능 윤리기준 | `documents/공공부문-인공지능-윤리기준.hwpx` | `corpus/ethics.pdf` |
| 규제합리화위원회 보도자료 | `corpus/gyuje-규제합리화.hwpx` | `corpus/gyuje-규제합리화.pdf` |
| 식약처 프로프라놀롤 안내 | `corpus/mfds-프로프라놀롤.hwpx` | `corpus/mfds-프로프라놀롤.pdf` |

`corpus/*.hwp` are the same three documents saved by Hancom Office 2026-10-29 as HWP 5.0.5.0
(compressed). They check the HWP 5.x reader against the HWPX reader: plain text must be identical,
and the full IR is kept in `tests/golden/hwp-*.txt`. Hancom re-numbers character shapes when it
saves, so run boundaries and ±1 HWPUNIT values may legitimately differ from the HWPX files.

Engine reads HWPX structure (paragraph styles, tables, BinData images, column count).
PDFs are reference only for visual comparison — not used at runtime.
