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
    int italic;
    int underline;
    int strikeout;
    int use_font_space; /* 0: HWP half-em spaces, 1: physical font space advance */
    uint32_t tab_width_hwp; /* a dedicated tab run retains its saved advance */
    int tab_leader;
    int tab_align_right;
    /* Header/footer fields (0.12.0): 1 = current page number, 2 = total pages;
     * text holds a placeholder that the layout replaces per page. */
    int field;
    /* Original per-script faces; font_face remains the default/Hangul face. */
    char script_faces[7][256];
    /* HANGUL, LATIN, HANJA, JAPANESE, OTHER, SYMBOL, USER; zero ratio = 100%. */
    int width_percent[7];
    int spacing_percent[7];
} hp_run;

/* One line from hp:lineseg (Hangul-saved layout hint). */
typedef struct hp_line_seg {
    uint32_t textpos;       /* char index into concatenated paragraph text */
    uint32_t vertpos_hwp;   /* saved page/cell position; normalize to paragraph origin */
    uint32_t vertsize_hwp;
    uint32_t textheight_hwp;
    uint32_t baseline_hwp;
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
    uint32_t inline_prefix_width_hwp; /* leading inline table, rendered on first text line */
    int object_anchor_only; /* blank text on a separately rendered object line */
    uint32_t mark_size_hwp; /* character size of the paragraph mark of a paragraph without runs (0 = unknown) */
    uint32_t source_p; /* 1 + index of the source <p> in its section, document order (0 = unknown); 0.11.0 */
    /* Characters at the start that are not in the source text: the paragraph
     * number / bullet (0.12.0). Editor offsets skip them. */
    uint32_t generated_prefix;
    hp_line_seg *segs;
    size_t seg_count;
} hp_paragraph;

typedef enum hp_border_line {
    HP_BORDER_SOLID=0, HP_BORDER_DASH, HP_BORDER_DOT,
    HP_BORDER_DASH_DOT, HP_BORDER_DASH_DOT_DOT, HP_BORDER_LONG_DASH, HP_BORDER_DOUBLE_SLIM
} hp_border_line;
typedef struct hp_borders {
    int left;
    int right;
    int top;
    int bottom; /* nonzero = visible side; line pattern is stored separately */
    hp_border_line style_left, style_right, style_top, style_bottom;
    uint32_t color_left;
    uint32_t color_right;
    uint32_t color_top;
    uint32_t color_bottom;
    int width_px;
    int width_left, width_right, width_top, width_bottom;
} hp_borders;

typedef struct hp_image {
    char *binary_id;
    unsigned char *data;
    size_t data_len;
    uint32_t width_hwp;
    uint32_t height_hwp;
    uint32_t clip_left, clip_top, clip_right, clip_bottom;
    uint32_t image_dim_width, image_dim_height;
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
    int is_header;
    int vertical_align; /* 0=TOP, 1=CENTER, 2=BOTTOM */
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
    size_t *nested_before_paragraph; /* source order; NULL means after all text */
    /* Grouped drawings / equations anchored in the cell. */
    struct hp_container *container;
    hp_equation *equation;
} hp_table_cell;

typedef struct hp_table {
    int row_count;
    int col_count;
    int repeat_header;
    int has_anchor_align;
    hp_align anchor_align; /* inline tables inherit their containing paragraph */
    int32_t anchor_left_hwp, anchor_right_hwp;
    uint32_t outer_left_hwp, outer_right_hwp;
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
    int page_break_before;
    int column_break_before;
    int saved_break_before; /* saved page/column coordinates restarted */
    int has_saved_y;
    uint32_t saved_y_hwp; /* position of the source paragraph's first line */
    int page_number_reset; /* newNum PAGE: affects the page containing this block */
    int hide_page_number; /* pageHiding: this page only */
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
    uint32_t header_height;
    uint32_t footer_height;
} hp_page;

/* Text kept outside the body flow (0.12.0): headers and footers, footnotes. */
typedef enum hp_subtext_kind {
    HP_SUB_HEADER = 0,
    HP_SUB_FOOTER = 1,
    HP_SUB_FOOTNOTE = 2
} hp_subtext_kind;

typedef struct hp_subtext {
    hp_subtext_kind kind;
    int apply;      /* header/footer: 0 every page, 1 even pages, 2 odd pages */
    int vert_align; /* 0 top, 1 center, 2 bottom */
    size_t block;   /* section block the control belongs to: applies from its page */
    hp_paragraph *paragraphs;
    size_t paragraph_count;
} hp_subtext;

/* Footnote separator and spacing (secPr footNotePr), HWPUNIT. */
typedef struct hp_note_shape {
    int32_t line_length_hwp; /* -1: a third of the column */
    int line_type;           /* 0 none, 1 solid */
    uint32_t line_color;
    uint32_t above_hwp, below_hwp, between_hwp;
} hp_note_shape;

typedef struct hp_section {
    hp_page page;
    int page_number_start; /* 0 continues from the preceding section */
    int hide_first_page_number;
    /* -1 absent/inherit, 0 NONE, 1..3 top L/C/R, 4..6 bottom L/C/R,
     * 7/8 top/bottom outside, 9/10 top/bottom inside. */
    int page_number_position;
    char page_number_side[16];
    int column_count;
    uint32_t column_gap_hwp;
    hp_block *blocks;
    size_t block_count;
    hp_paragraph *paragraphs;
    size_t paragraph_count;
    /* headers, footers and footnotes in document order (0.12.0) */
    hp_subtext *subtexts;
    size_t subtext_count;
    hp_note_shape footnote_shape;
    /* Edit tracking for lossless save (documents read from a file):
     * - paragraphs[loaded_paragraph_count..] were added after reading; they are
     *   inserted at the end of the original section XML, everything else is kept.
     * - needs_rewrite = 1 after changing existing content in this section;
     *   saving is refused with HP_ERR_UNSUPPORTED until lossless rewriting
     *   is implemented. Removing loaded paragraphs is also refused. */
    size_t loaded_paragraph_count;
    int needs_rewrite;
} hp_section;

struct hp_package; /* original HWPX parts (internal, see src/hwpx_package.h) */

typedef struct hp_document {
    char *title;
    char *language;
    hp_section *sections;
    size_t section_count;
    /* Set when read from an HWPX file; NULL for new documents. hp_hwpx_write()
     * re-emits the original parts byte-for-byte and replaces only changed ones. */
    struct hp_package *package;
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



