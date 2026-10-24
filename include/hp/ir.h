/* HangulPad internal document model (IR) */
#ifndef HP_IR_H
#define HP_IR_H

#include "hp/common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum hp_align {
    HP_ALIGN_LEFT = 0,
    HP_ALIGN_CENTER = 1,
    HP_ALIGN_RIGHT = 2,
    HP_ALIGN_JUSTIFY = 3
} hp_align;

typedef enum hp_block_kind {
    HP_BLOCK_PARAGRAPH = 0,
    HP_BLOCK_TABLE = 1,
    HP_BLOCK_IMAGE = 2
} hp_block_kind;

typedef struct hp_run {
    char *text;             /* UTF-8; ownership by run */
    char *font_face;        /* UTF-8 Hangul face name; may be NULL */
    uint32_t font_size_hwp; /* 1/100 pt (HWP height) — 1000 = 10pt */
    uint32_t color_rgb;     /* 0x00RRGGBB */
    int bold;
} hp_run;

typedef struct hp_paragraph {
    hp_run *runs;
    size_t run_count;
    hp_align align;
} hp_paragraph;

typedef struct hp_table_cell {
    int row;
    int col;
    int rowspan;
    int colspan;
    uint32_t width_hwp;
    uint32_t height_hwp;
    uint32_t fill_rgb; /* 0x00RRGGBB; 0xFFFFFFFF = none */
    int has_fill;
    hp_paragraph *paragraphs;
    size_t paragraph_count;
    struct hp_image *image; /* optional image inside cell */
} hp_table_cell;

typedef struct hp_table {
    int row_count;
    int col_count;
    uint32_t width_hwp;
    uint32_t height_hwp;
    hp_table_cell *cells;
    size_t cell_count;
} hp_table;

typedef struct hp_image {
    char *binary_id; /* e.g. "image1" */
    unsigned char *data;
    size_t data_len;
    uint32_t width_hwp;
    uint32_t height_hwp;
} hp_image;

typedef struct hp_block {
    hp_block_kind kind;
    hp_paragraph paragraph; /* HP_BLOCK_PARAGRAPH */
    hp_table *table;        /* HP_BLOCK_TABLE (owned) */
    hp_image *image;        /* HP_BLOCK_IMAGE (owned) */
} hp_block;

typedef struct hp_page {
    uint32_t width;
    uint32_t height;
    uint32_t margin_left;
    uint32_t margin_right;
    uint32_t margin_top;
    uint32_t margin_bottom;
} hp_page;

typedef struct hp_section {
    hp_page page;
    int column_count; /* 다단; usually 1 */
    uint32_t column_gap_hwp;
    /* Rich layout stream (paragraphs, tables, images in doc order). */
    hp_block *blocks;
    size_t block_count;
    /* Flat text paragraphs (compat for write / plain_text). */
    hp_paragraph *paragraphs;
    size_t paragraph_count;
} hp_section;

typedef struct hp_document {
    char *title;
    char *language;
    hp_section *sections;
    size_t section_count;
} hp_document;

void hp_page_default(hp_page *page);
hp_document *hp_document_new(void);
hp_document *hp_document_from_text(const char *text);
void hp_document_free(hp_document *doc);

hp_status hp_document_set_title(hp_document *doc, const char *title);
hp_status hp_section_add_paragraph_text(hp_section *sec, const char *text);

/* Caller frees returned string with free(). */
char *hp_document_plain_text(const hp_document *doc);

/* HWPUNIT → CSS/px at 96dpi: px = hwp / 75 */
uint32_t hp_hwp_to_px(uint32_t hwp);

#ifdef __cplusplus
}
#endif

#endif /* HP_IR_H */
