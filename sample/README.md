# Sample: 공공부문 인공지능 윤리기준

- `documents/*.hwpx` — HWPX sample
- `pages/00.jpg` … `16.jpg` — 17 pages rasterized from the reference PDF
  `공공부문 인공지능 윤리기준.pdf` (A4) so HangulPad can show the document
  like a PDF viewer.

```bash
pdftoppm -jpeg -r 120 ethics.pdf /tmp/page
# rename page-01.jpg → 00.jpg … in order
```
