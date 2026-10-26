/* Platform-independent page/column flow. Fragments cover a block without gaps. */
#ifndef HP_PAGINATION_H
#define HP_PAGINATION_H
typedef struct hp_flow {
    int page, column, y, columns, height;
} hp_flow;
typedef struct hp_slice {
    int page, column, y, offset, height;
} hp_slice;
static inline void hp_flow_break(hp_flow *f, int page) {
    if (f->y == 0 && (f->column == 0 || !page)) return;
    f->y = 0;
    if (page || ++f->column >= f->columns) { f->page++; f->column = 0; }
}
static inline hp_slice hp_flow_take(hp_flow *f, int offset, int height,
                                    const int *ends, int count) {
    hp_slice slice;
    int room, limit, cut, i, next = height;
    if (f->y >= f->height) hp_flow_break(f, 0);
    room = f->height - f->y;
    limit = height-offset <= room ? height : offset+room;
    cut = offset;
    for (i=0; i<count; i++) {
        if (ends[i] > offset && ends[i] < next) next = ends[i];
        if (ends[i] > cut && ends[i] <= limit) cut = ends[i];
    }
    /* Keep a row/line together when it fits a fresh column. A row taller than
     * the entire column continues as clipped, contiguous slices. */
    if (cut == offset && next-offset <= f->height && next > limit && f->y > 0) {
        hp_flow_break(f, 0);
        return hp_flow_take(f, offset, height, ends, count);
    }
    if (cut == offset || height <= limit) cut = limit;
    slice.page=f->page; slice.column=f->column; slice.y=f->y;
    slice.offset=offset; slice.height=cut-offset;
    f->y += slice.height;
    return slice;
}
#endif
