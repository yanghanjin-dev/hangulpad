#ifndef HP_EDIT_H
#define HP_EDIT_H
#include "hp/hwpx.h"
typedef struct hp_editor hp_editor;
hp_status hp_editor_open(const char *path, hp_editor **out);
void hp_editor_free(hp_editor *e);
size_t hp_editor_count(const hp_editor *e);
const char *hp_editor_text(const hp_editor *e, size_t i);
size_t hp_editor_section(const hp_editor *e, size_t i);
int hp_editor_is_deleted(const hp_editor *e, size_t i);
int hp_editor_can_delete(const hp_editor *e, size_t i);
hp_status hp_editor_set(hp_editor *e, size_t i, const char *text);
/* Insert after this paragraph and its nested contents; returns the new list index. */
hp_status hp_editor_insert(hp_editor *e, size_t i, size_t *inserted_index);
hp_status hp_editor_delete(hp_editor *e, size_t i);
hp_status hp_editor_save(hp_editor *e, const char *path);

/* Character / paragraph formatting of whole paragraphs (0.10.0).
 * `fields` says which members are meaningful. Sizes and lengths are HWPUNIT
 * (1 pt = 100). Changes are kept per paragraph and written on save as new
 * charPr/paraPr entries in Contents/header.xml; the paragraph's runs keep their
 * other differences (each run gets a copy of its own shape with the change). */
enum {
    HP_CF_FACE = 1, HP_CF_SIZE = 2, HP_CF_BOLD = 4, HP_CF_ITALIC = 8,
    HP_CF_UNDERLINE = 16, HP_CF_STRIKEOUT = 32, HP_CF_COLOR = 64
};
typedef struct hp_char_format {
    unsigned fields;
    char face[64];        /* UTF-8 */
    uint32_t size_hwp;    /* 1000 = 10 pt */
    int bold, italic, underline, strikeout;
    uint32_t color_rgb;   /* 0xRRGGBB */
} hp_char_format;
enum {
    HP_PF_ALIGN = 1, HP_PF_LINE_SPACING = 2, HP_PF_LEFT = 4, HP_PF_RIGHT = 8,
    HP_PF_INDENT = 16, HP_PF_BEFORE = 32, HP_PF_AFTER = 64
};
#define HP_ALIGN_DISTRIBUTE 4 /* in addition to hp_align (ir.h) */
typedef struct hp_para_format {
    unsigned fields;
    int align;                 /* hp_align or HP_ALIGN_DISTRIBUTE */
    int line_spacing_percent;  /* 160 = 160 % */
    int32_t left_hwp, right_hwp;
    int32_t indent_hwp;        /* > 0 first-line indent, < 0 hanging */
    int32_t before_hwp, after_hwp;
} hp_para_format;
/* Current format of paragraph i (its first run for characters), all fields set. */
hp_status hp_editor_get_format(const hp_editor *e, size_t i, hp_char_format *cf, hp_para_format *pf);
/* Merge the given fields into paragraph i. */
hp_status hp_editor_apply_char_format(hp_editor *e, size_t i, const hp_char_format *cf);
hp_status hp_editor_apply_para_format(hp_editor *e, size_t i, const hp_para_format *pf);
#endif



