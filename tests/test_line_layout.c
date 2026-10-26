#include "../src/line_layout.h"
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line layout failure: %s:%d\n", __FILE__, __LINE__); return 1; } } while (0)
int main(void) {
    hp_line_seg segs[3] = {0};
    hp_paragraph para = {0};
    hp_line_cursor cursor = {0};
    para.segs = segs;
    para.seg_count = 2;
    segs[0].vertpos_hwp = 20366;
    segs[1].vertpos_hwp = 22746;
    segs[0].vertsize_hwp = segs[1].vertsize_hwp = 1400;
    segs[0].textheight_hwp = segs[1].textheight_hwp = 1400;
    segs[0].spacing_hwp = segs[1].spacing_hwp = 980;
    CHECK(hp_line_next(&cursor, &segs[0]) == 0);
    CHECK(hp_line_next(&cursor, &segs[1]) == 2380);
    CHECK(hp_paragraph_lines_px(&para) == hp_line_px(4760));
    /* Identical paragraph at another page/cell origin has identical height. */
    segs[0].vertpos_hwp += 40000;
    segs[1].vertpos_hwp += 40000;
    CHECK(hp_paragraph_lines_px(&para) == hp_line_px(4760));
    /* A page reset must not wrap to an enormous unsigned height. */
    para.seg_count = 3;
    segs[2] = segs[0];
    segs[2].vertpos_hwp = 0;
    CHECK(hp_paragraph_lines_px(&para) == hp_line_px(7140));
    /* Empty lines still retain their saved line advance. */
    para.seg_count = 1;
    segs[0].textpos = 0;
    CHECK(hp_paragraph_lines_px(&para) == hp_line_px(2380));
    segs[0].spacing_hwp = (uint32_t)-200;
    CHECK(hp_paragraph_lines_px(&para) == hp_line_px(1200));
    /* Same-height fragments belong to one physical line, not two advances. */
    para.seg_count = 2;
    segs[1] = segs[0];
    CHECK(hp_paragraph_lines_px(&para) == hp_line_px(1200));
    /* Signed positions and large values must not overflow pixel conversion. */
    segs[0].vertpos_hwp = segs[1].vertpos_hwp = (uint32_t)-100;
    CHECK(hp_paragraph_lines_px(&para) == hp_line_px(1200));
    CHECK(hp_line_px(INT64_MAX) == INT_MAX);
    puts("line layout tests passed");
    return 0;
}
