/* Saved line positions are relative to a page/cell, not to each paragraph.
 * Normalize the origin once and share the same geometry for measuring/drawing. */
#ifndef HP_LINE_LAYOUT_H
#define HP_LINE_LAYOUT_H
#include "hp/ir.h"
#include <limits.h>

typedef struct hp_line_cursor {
    int64_t origin;
    int64_t previous;
    int64_t bottom;
    int started;
} hp_line_cursor;

static inline int64_t hp_line_height(const hp_line_seg *seg) {
    int64_t height = seg->vertsize_hwp;
    if (height < seg->textheight_hwp) height = seg->textheight_hwp;
    height += (int32_t)seg->spacing_hwp;
    return height > 0 ? height : 1;
}

static inline int64_t hp_line_next(hp_line_cursor *cursor, const hp_line_seg *seg) {
    int64_t raw = (int32_t)seg->vertpos_hwp;
    int64_t top;
    if (!cursor->started) {
        cursor->origin = raw;
        cursor->started = 1;
    } else if (raw < cursor->previous) {
        /* The saved paragraph continues in another page/column. Keep its
         * local flow continuous rather than subtracting unsigned positions. */
        cursor->origin = raw - cursor->bottom;
    }
    top = raw - cursor->origin;
    if (top + hp_line_height(seg) > cursor->bottom)
        cursor->bottom = top + hp_line_height(seg);
    cursor->previous = raw;
    return top;
}

static inline int hp_line_px(int64_t value) {
    /* HWPUNIT = 1/7200 inch; page bitmap coordinates use 96 pixels/inch. */
    if (value <= 0) return 0;
    if (value > (int64_t)INT_MAX * 75) return INT_MAX;
    return (int)((value + 37) / 75);
}

static inline int hp_paragraph_lines_px(const hp_paragraph *para) {
    hp_line_cursor cursor = {0};
    size_t i;
    for (i = 0; i < para->seg_count; i++) hp_line_next(&cursor, &para->segs[i]);
    return hp_line_px(cursor.bottom);
}
#endif
