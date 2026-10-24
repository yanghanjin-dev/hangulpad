# Sample corpus (HWPX + PDF pairs)

Used to hillclimb the HangulPad layout engine against PDF ground truth.

| Pair | HWPX | PDF |
|------|------|-----|
| 공공부문 인공지능 윤리기준 | `documents/공공부문-인공지능-윤리기준.hwpx` | `corpus/ethics.pdf` |
| 규제합리화위원회 보도자료 | `corpus/gyuje-규제합리화.hwpx` | `corpus/gyuje-규제합리화.pdf` |
| 식약처 프로프라놀롤 안내 | `corpus/mfds-프로프라놀롤.hwpx` | `corpus/mfds-프로프라놀롤.pdf` |

Engine reads HWPX structure (paragraph styles, tables, BinData images, column count).
PDFs are reference only for visual comparison — not used at runtime.
