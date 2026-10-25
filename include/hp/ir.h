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
    HP_BLOCK_IMAGE = 2,
    HP_BLOCK_EQUATION = 3,
    HP_BLOCK_CONTAINER = 4
} hp_block_kind;

typedef enum hp_text_wrap {
    HP_WRAP_TOP_AND_BOTTOM = 0,
    HP_WRAP_SQUARE = 1,
    HP_WRAP_BEHIND = 2,
    HP_WRAP_IN_FRONT = 3,
    HP_WRAP_INLINE = 4
} hp_text_wrap;

/* Floating / anchored object placement (HWP pos). */
typedef struct hp_float_pos {
    int treat_as_char;     /* 1 = inline character */
    int32_t vert_offset_hwp;
    int32_t horz_offset_hwp;
    hp_text_wrap wrap;
    int vert_rel_para;     /* 1=PARA, 0=PAGE/PAPER */
    int horz_rel_para;
} hp_float_pos;

typedef struct hp_run {
    char *text;
    char *font_face;
    uint32_t font_size_hwp;
    uint32_t color_rgb;
    int bold;
} hp_run;

/* One line from hp:lineseg (Hangul-saved layout hint). */
typedef struct hp_line_seg {
    uint32_t textpos;       /* char index into concatenated paragraph text */
    uint32_t vertpos_hwp;   /* offset from paragraph top */
    uint32_t vertsize_hwp;
    uint32_t textheight_hwp;
    uint32_t spacing_hwp;   /* advance beyond textheight → line pitch */
    uint32_t horzpos_hwp;
    uint32_t horzsize_hwp;
} hp_line_seg;

typedef struct hp_paragraph {
    hp_run *runs;
    size_t run_count;
    hp_align align;
    int32_t indent_left_hwp;
    int32_t indent_right_hwp;
    int32_t indent_first_hwp; /* first-line / hanging (intent) */
    int32_t space_before_hwp;
    int32_t space_after_hwp;
    int line_spacing_percent; /* 160 = 160%; 0 → treat as 160 */
    hp_line_seg *segs;
    size_t seg_count;
} hp_paragraph;

typedef struct hp_borders {
    int left;
    int right;
    int top;
    int bottom; /* 1 = solid side */
    uint32_t color_left;
    uint32_t color_right;
    uint32_t color_top;
    uint32_t color_bottom;
    int width_px;
} hp_borders;

typedef struct hp_image {
    char *binary_id;
    unsigned char *data;
    size_t data_len;
    uint32_t width_hwp;
    uint32_t height_hwp;
    hp_float_pos pos;
    int has_pos;
} hp_image;

typedef struct hp_equation {
    char *script; /* UTF-8 script or plaintext fallback */
    uint32_t font_size_hwp;
} hp_equation;

struct hp_table; /* fwd */

typedef struct hp_table_cell {
    int row;
    int col;
    int rowspan;
    int colspan;
    uint32_t width_hwp;
    uint32_t height_hwp;
    uint32_t fill_rgb;
    int has_fill;
    hp_borders borders;
    uint32_t pad_left_hwp;
    uint32_t pad_right_hwp;
    uint32_t pad_top_hwp;
    uint32_t pad_bottom_hwp;
    hp_paragraph *paragraphs;
    size_t paragraph_count;
    hp_image *image;
    /* Nested tables directly inside this cell (gyuje etc.). */
    struct hp_table **nested_tables;
    size_t nested_table_count;
    /* Grouped drawings / equations anchored in the cell. */
    struct hp_container *container;
    hp_equation *equation;
} hp_table_cell;

typedef struct hp_table {
    int row_count;
    int col_count;
    uint32_t width_hwp;
    uint32_t height_hwp;
    hp_table_cell *cells;
    size_t cell_count;
    hp_float_pos pos;
    int has_pos;
} hp_table;

/* Grouped drawings (hp:container) — ordered child images. */
typedef struct hp_container {
    uint32_t width_hwp;
    uint32_t height_hwp;
    hp_image **images;
    size_t image_count;
    hp_float_pos pos;
    int has_pos;
} hp_container;

typedef struct hp_block {
    hp_block_kind kind;
    hp_paragraph paragraph;
    hp_table *table;
    hp_image *image;
    hp_equation *equation;
    hp_container *container;
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
    int column_count;
    uint32_t column_gap_hwp;
    hp_block *blocks;
    size_t block_count;
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

char *hp_document_plain_text(const hp_document *doc);
uint32_t hp_hwp_to_px(uint32_t hwp);
int32_t hp_hwp_signed(uint32_t raw); /* interpret HWPUNIT as signed */

#ifdef __cplusplus
}
#endif

#endif /* HP_IR_H */
