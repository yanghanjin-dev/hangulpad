#ifndef HP_TABLE_PAGINATION_H
#define HP_TABLE_PAGINATION_H
#include "hp/ir.h"
#include <stdlib.h>

typedef struct hp_ink_band { int top, bottom; } hp_ink_band;
typedef struct hp_cut_map {
    hp_ink_band *bands;
    size_t count, capacity;
    int failed;
} hp_cut_map;

static inline void hp_cut_add(hp_cut_map *map, int top, int bottom) {
    hp_ink_band *next;
    if (!map || map->failed || bottom <= top) return;
    if (map->count == map->capacity) {
        size_t capacity = map->capacity ? map->capacity*2 : 64;
        next = (hp_ink_band *)realloc(map->bands,capacity*sizeof(*next));
        if (!next) { map->failed=1;return; }
        map->bands=next;map->capacity=capacity;
    }
    map->bands[map->count++]=(hp_ink_band){top,bottom};
}
static int hp_cut_compare(const void *a, const void *b) {
    const hp_ink_band *x=(const hp_ink_band *)a,*y=(const hp_ink_band *)b;
    return x->top<y->top ? -1 : x->top>y->top;
}
static inline void hp_cut_normalize(hp_cut_map *map) {
    size_t i,n=0;
    if(!map->count)return;
    qsort(map->bands,map->count,sizeof(*map->bands),hp_cut_compare);
    for(i=0;i<map->count;i++) {
        hp_ink_band band=map->bands[i];
        /* A boundary where two ink bands touch is still a valid cut. */
        if(n && band.top<map->bands[n-1].bottom) {
            if(band.bottom>map->bands[n-1].bottom)map->bands[n-1].bottom=band.bottom;
        } else map->bands[n++]=band;
    }
    map->count=n;
}
static inline int hp_cut_before(const hp_cut_map *map,int start,int limit) {
    size_t i=map->count;
    int cut=limit;
    while(i--)if(map->bands[i].top<cut && cut<map->bands[i].bottom)cut=map->bands[i].top;
    return cut>start ? cut : start;
}
static inline int hp_table_header_rows(const hp_table *table) {
    size_t i;
    int end=0,changed;
    if(!table || !table->repeat_header)return 0;
    do {
        changed=0;
        for(i=0;i<table->cell_count;i++) {
            const hp_table_cell *cell=&table->cells[i];
            int bottom=cell->row+(cell->rowspan>0 ? cell->rowspan : 1);
            if(cell->is_header && cell->row>=0 && cell->row<=end && bottom>end) {
                end=bottom;changed=1;
            }
        }
    } while(changed);
    if(end<=0 || end>=table->row_count)return 0;
    /* Never repeat body data belonging to a merge across the header boundary. */
    for(i=0;i<table->cell_count;i++) {
        const hp_table_cell *cell=&table->cells[i];
        if(cell->row<end && cell->row+cell->rowspan>end)return 0;
    }
    return end;
}
#endif
