/* irdump — print the complete IR of an HWPX file as deterministic text.
 * Used for regression checks: run old and new readers, diff the output.
 *   irdump <file.hwpx>            full dump
 *   irdump --flat <file.hwpx>     only the flat sec->paragraphs list */
#include "hp/hwpx.h"
#include "hp/hwp.h"
#include "hp/ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/win_utf8.h"
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static void ind(int d) {
    int i;
    for (i = 0; i < d; i++) {
        fputs("  ", stdout);
    }
}

static void qstr(const char *s) {
    putchar('"');
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            printf("\\%c", c);
        } else if (c == '\n') {
            fputs("\\n", stdout);
        } else if (c == '\t') {
            fputs("\\t", stdout);
        } else if (c < 0x20) {
            printf("\\x%02x", c);
        } else {
            putchar(c);
        }
    }
    putchar('"');
}

static unsigned long fnv(const unsigned char *d, size_t n) {
    unsigned long h = 2166136261UL;
    size_t i;
    for (i = 0; i < n; i++) {
        h = (h ^ d[i]) * 16777619UL;
        h &= 0xffffffffUL;
    }
    return h;
}

static void dump_pos(int d, const hp_float_pos *p, int has) {
    ind(d);
    if (!has) {
        puts("pos: none");
        return;
    }
    printf("pos: char=%d v=%d h=%d wrap=%d vrel_para=%d hrel_para=%d\n", p->treat_as_char, p->vert_offset_hwp,
           p->horz_offset_hwp, (int)p->wrap, p->vert_rel_para, p->horz_rel_para);
}

static void dump_para(int d, const hp_paragraph *p) {
    size_t i;
    ind(d);
    printf("para align=%d indent l=%d r=%d first=%d space before=%d after=%d line=%d%%\n", (int)p->align,
           p->indent_left_hwp, p->indent_right_hwp, p->indent_first_hwp, p->space_before_hwp, p->space_after_hwp,
           p->line_spacing_percent);
    if(p->object_anchor_only) {ind(d+1);puts("object-anchor-only=1");}
    if(p->inline_prefix_width_hwp) {ind(d+1);printf("inline-prefix=%u\n",p->inline_prefix_width_hwp);}
    for (i = 0; i < p->seg_count; i++) {
        const hp_line_seg *s = &p->segs[i];
        ind(d + 1);
        printf("seg textpos=%u vpos=%u vsize=%u th=%u sp=%u hpos=%u hsize=%u\n", s->textpos, s->vertpos_hwp,
               s->vertsize_hwp, s->textheight_hwp, s->spacing_hwp, s->horzpos_hwp, s->horzsize_hwp);
    }
    for (i = 0; i < p->run_count; i++) {
        const hp_run *r = &p->runs[i];
        ind(d + 1);
        fputs("run ", stdout);
        qstr(r->text);
        fputs(" font=", stdout);
        qstr(r->font_face);
        printf(" size=%u color=%06x bold=%d\n", r->font_size_hwp, r->color_rgb, r->bold);
        {
            size_t lang; int changed=0;
            for(lang=0;lang<7;lang++) if((r->width_percent[lang] && r->width_percent[lang]!=100) || r->spacing_percent[lang]) changed=1;
            if(changed) {
                ind(d+2);fputs("metrics width=",stdout);
                for(lang=0;lang<7;lang++)printf("%s%d",lang ? "," : "",r->width_percent[lang] ? r->width_percent[lang] : 100);
                fputs(" spacing=",stdout);
                for(lang=0;lang<7;lang++)printf("%s%d",lang ? "," : "",r->spacing_percent[lang]);
                putchar('\n');
            }
        }
    }
}

static void dump_image(int d, const hp_image *im) {
    ind(d);
    if (!im) {
        puts("image: null");
        return;
    }
    fputs("image id=", stdout);
    qstr(im->binary_id);
    printf(" bytes=%lu hash=%08lx w=%u h=%u\n", (unsigned long)im->data_len,
           im->data ? fnv(im->data, im->data_len) : 0UL, im->width_hwp, im->height_hwp);
    dump_pos(d + 1, &im->pos, im->has_pos);
}

static void dump_eq(int d, const hp_equation *e) {
    ind(d);
    fputs("equation script=", stdout);
    qstr(e->script);
    printf(" size=%u\n", e->font_size_hwp);
}

static void dump_container(int d, const hp_container *c) {
    size_t i;
    ind(d);
    printf("container w=%u h=%u images=%lu\n", c->width_hwp, c->height_hwp, (unsigned long)c->image_count);
    dump_pos(d + 1, &c->pos, c->has_pos);
    for (i = 0; i < c->image_count; i++) {
        dump_image(d + 1, c->images[i]);
    }
}

static void dump_table(int d, const hp_table *t);

static void dump_cell(int d, const hp_table_cell *c) {
    size_t i;
    if(c->is_header) {ind(d);puts("header-cell=1");}
    ind(d);
    printf("cell r=%d c=%d rs=%d cs=%d w=%u h=%u fill=%d:%06x pad=%u,%u,%u,%u\n", c->row, c->col, c->rowspan,
           c->colspan, c->width_hwp, c->height_hwp, c->has_fill, c->has_fill ? c->fill_rgb : 0, c->pad_left_hwp,
           c->pad_right_hwp, c->pad_top_hwp, c->pad_bottom_hwp);
    ind(d + 1);
    printf("borders l=%d r=%d t=%d b=%d colors=%06x,%06x,%06x,%06x width=%d\n", c->borders.left, c->borders.right,
           c->borders.top, c->borders.bottom, c->borders.color_left, c->borders.color_right, c->borders.color_top,
           c->borders.color_bottom, c->borders.width_px);
    for (i = 0; i < c->paragraph_count; i++) {
        dump_para(d + 1, &c->paragraphs[i]);
    }
    if (c->image) {
        dump_image(d + 1, c->image);
    }
    for (i = 0; i < c->nested_table_count; i++) {
        dump_table(d + 1, c->nested_tables[i]);
    }
    if (c->container) {
        dump_container(d + 1, c->container);
    }
    if (c->equation) {
        dump_eq(d + 1, c->equation);
    }
}

static void dump_table(int d, const hp_table *t) {
    size_t i;
    ind(d);
    if (!t) {
        puts("table: null");
        return;
    }
    printf("table rows=%d cols=%d w=%u h=%u cells=%lu\n", t->row_count, t->col_count, t->width_hwp, t->height_hwp,
           (unsigned long)t->cell_count);
    if(t->repeat_header) {ind(d+1);puts("repeat-header=1");}
    dump_pos(d + 1, &t->pos, t->has_pos);
    for (i = 0; i < t->cell_count; i++) {
        dump_cell(d + 1, &t->cells[i]);
    }
}

static void dump_block(int d, const hp_block *b) {
    if(b->page_break_before || b->column_break_before || b->saved_break_before) {
        ind(d);printf("breaks page=%d column=%d saved=%d\n",b->page_break_before,b->column_break_before,b->saved_break_before);
    }
    switch (b->kind) {
    case HP_BLOCK_PARAGRAPH:
        dump_para(d, &b->paragraph);
        break;
    case HP_BLOCK_TABLE:
        dump_table(d, b->table);
        break;
    case HP_BLOCK_IMAGE:
        dump_image(d, b->image);
        break;
    case HP_BLOCK_EQUATION:
        dump_eq(d, b->equation);
        break;
    case HP_BLOCK_CONTAINER:
        dump_container(d, b->container);
        break;
    default:
        ind(d);
        printf("block kind=%d\n", (int)b->kind);
    }
}

int main(int argc, char **argv) {
    hp_document *doc = NULL;
    hp_status st;
    size_t s, i;
    int flat = 0;
    const char *path;
    hp_utf8_args(&argc, &argv); /* Korean sample names on Windows */
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY); /* LF output, byte-identical to Linux golden files */
#endif
    if (argc == 3 && strcmp(argv[1], "--flat") == 0) {
        flat = 1;
        path = argv[2];
    } else if (argc == 2) {
        path = argv[1];
    } else {
        fprintf(stderr, "usage: irdump [--flat] <file.hwpx>\n");
        return 2;
    }
    st = hp_document_read(path, &doc);
    if (st != HP_OK) {
        printf("error: %s\n", hp_status_string(st));
        return 1;
    }
    fputs("title=", stdout);
    qstr(doc->title);
    fputs(" lang=", stdout);
    qstr(doc->language);
    printf(" sections=%lu\n", (unsigned long)doc->section_count);
    for (s = 0; s < doc->section_count; s++) {
        const hp_section *sec = &doc->sections[s];
        printf("section %lu page=%ux%u margin l=%u r=%u t=%u b=%u cols=%d gap=%u blocks=%lu paras=%lu\n",
               (unsigned long)s, sec->page.width, sec->page.height, sec->page.margin_left, sec->page.margin_right,
               sec->page.margin_top, sec->page.margin_bottom, sec->column_count, sec->column_gap_hwp,
               (unsigned long)sec->block_count, (unsigned long)sec->paragraph_count);
        if (flat) {
            for (i = 0; i < sec->paragraph_count; i++) {
                dump_para(1, &sec->paragraphs[i]);
            }
        } else {
            for (i = 0; i < sec->block_count; i++) {
                dump_block(1, &sec->blocks[i]);
            }
            puts("  -- flat paragraphs --");
            for (i = 0; i < sec->paragraph_count; i++) {
                dump_para(1, &sec->paragraphs[i]);
            }
        }
    }
    hp_document_free(doc);
    return 0;
}
