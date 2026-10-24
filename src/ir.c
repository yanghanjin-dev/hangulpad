#include "hp/ir.h"

#include <stdlib.h>
#include <string.h>

void hp_page_default(hp_page *page) {
    if (!page) {
        return;
    }
    /* A4 with ~20mm margins (HWPUNIT). */
    page->width = 59528;
    page->height = 84188;
    page->margin_left = 5669;
    page->margin_right = 5669;
    page->margin_top = 5669;
    page->margin_bottom = 4252;
}

uint32_t hp_hwp_to_px(uint32_t hwp) {
    /* 1 HWPUNIT = 1/7200 in; at 96 dpi → /75 */
    return (hwp + 37) / 75;
}

static char *hp_strdup(const char *s) {
    size_t n;
    char *out;
    if (!s) {
        s = "";
    }
    n = strlen(s);
    out = (char *)malloc(n + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, s, n + 1);
    return out;
}

static void hp_run_free(hp_run *run) {
    if (!run) {
        return;
    }
    free(run->text);
    free(run->font_face);
    run->text = NULL;
    run->font_face = NULL;
}

static void hp_paragraph_free(hp_paragraph *p) {
    size_t i;
    if (!p) {
        return;
    }
    for (i = 0; i < p->run_count; i++) {
        hp_run_free(&p->runs[i]);
    }
    free(p->runs);
    p->runs = NULL;
    p->run_count = 0;
}

static void hp_image_free(hp_image *img);

static void hp_table_free(hp_table *t) {
    size_t i, j;
    if (!t) {
        return;
    }
    for (i = 0; i < t->cell_count; i++) {
        for (j = 0; j < t->cells[i].paragraph_count; j++) {
            hp_paragraph_free(&t->cells[i].paragraphs[j]);
        }
        free(t->cells[i].paragraphs);
        if (t->cells[i].image) {
            hp_image_free(t->cells[i].image);
            t->cells[i].image = NULL;
        }
    }
    free(t->cells);
    free(t);
}

static void hp_image_free(hp_image *img) {
    if (!img) {
        return;
    }
    free(img->binary_id);
    free(img->data);
    free(img);
}

static void hp_block_free(hp_block *b) {
    if (!b) {
        return;
    }
    if (b->kind == HP_BLOCK_PARAGRAPH) {
        hp_paragraph_free(&b->paragraph);
    } else if (b->kind == HP_BLOCK_TABLE) {
        hp_table_free(b->table);
        b->table = NULL;
    } else if (b->kind == HP_BLOCK_IMAGE) {
        hp_image_free(b->image);
        b->image = NULL;
    }
}

static void hp_section_free(hp_section *s) {
    size_t i;
    if (!s) {
        return;
    }
    for (i = 0; i < s->block_count; i++) {
        hp_block_free(&s->blocks[i]);
    }
    free(s->blocks);
    s->blocks = NULL;
    s->block_count = 0;
    for (i = 0; i < s->paragraph_count; i++) {
        hp_paragraph_free(&s->paragraphs[i]);
    }
    free(s->paragraphs);
    s->paragraphs = NULL;
    s->paragraph_count = 0;
}

hp_document *hp_document_new(void) {
    hp_document *doc = (hp_document *)calloc(1, sizeof(*doc));
    hp_section *sec;
    if (!doc) {
        return NULL;
    }
    doc->title = hp_strdup("");
    doc->language = hp_strdup("ko");
    doc->sections = (hp_section *)calloc(1, sizeof(hp_section));
    if (!doc->title || !doc->language || !doc->sections) {
        hp_document_free(doc);
        return NULL;
    }
    doc->section_count = 1;
    sec = &doc->sections[0];
    hp_page_default(&sec->page);
    sec->column_count = 1;
    return doc;
}

hp_status hp_section_add_paragraph_text(hp_section *sec, const char *text) {
    hp_paragraph *paras;
    hp_run *runs;
    hp_block *blocks;
    if (!sec) {
        return HP_ERR_INVALID;
    }
    paras = (hp_paragraph *)realloc(sec->paragraphs, (sec->paragraph_count + 1) * sizeof(*paras));
    if (!paras) {
        return HP_ERR_NOMEM;
    }
    sec->paragraphs = paras;
    memset(&sec->paragraphs[sec->paragraph_count], 0, sizeof(hp_paragraph));
    runs = (hp_run *)calloc(1, sizeof(*runs));
    if (!runs) {
        return HP_ERR_NOMEM;
    }
    runs[0].text = hp_strdup(text ? text : "");
    if (!runs[0].text) {
        free(runs);
        return HP_ERR_NOMEM;
    }
    runs[0].font_face = NULL;
    runs[0].font_size_hwp = 1000;
    runs[0].color_rgb = 0;
    runs[0].bold = 0;
    sec->paragraphs[sec->paragraph_count].runs = runs;
    sec->paragraphs[sec->paragraph_count].run_count = 1;
    sec->paragraphs[sec->paragraph_count].align = HP_ALIGN_LEFT;

    blocks = (hp_block *)realloc(sec->blocks, (sec->block_count + 1) * sizeof(*blocks));
    if (!blocks) {
        return HP_ERR_NOMEM;
    }
    sec->blocks = blocks;
    memset(&sec->blocks[sec->block_count], 0, sizeof(hp_block));
    sec->blocks[sec->block_count].kind = HP_BLOCK_PARAGRAPH;
    {
        hp_paragraph *bp = &sec->blocks[sec->block_count].paragraph;
        hp_run *br = (hp_run *)calloc(1, sizeof(*br));
        if (!br) {
            return HP_ERR_NOMEM;
        }
        br[0].text = hp_strdup(text ? text : "");
        if (!br[0].text) {
            free(br);
            return HP_ERR_NOMEM;
        }
        br[0].font_size_hwp = 1000;
        bp->runs = br;
        bp->run_count = 1;
        bp->align = HP_ALIGN_LEFT;
    }
    sec->paragraph_count++;
    sec->block_count++;
    return HP_OK;
}

hp_document *hp_document_from_text(const char *text) {
    hp_document *doc = hp_document_new();
    if (!doc) {
        return NULL;
    }
    if (hp_section_add_paragraph_text(&doc->sections[0], text) != HP_OK) {
        hp_document_free(doc);
        return NULL;
    }
    return doc;
}

hp_status hp_document_set_title(hp_document *doc, const char *title) {
    char *copy;
    if (!doc) {
        return HP_ERR_INVALID;
    }
    copy = hp_strdup(title ? title : "");
    if (!copy) {
        return HP_ERR_NOMEM;
    }
    free(doc->title);
    doc->title = copy;
    return HP_OK;
}

void hp_document_free(hp_document *doc) {
    size_t i;
    if (!doc) {
        return;
    }
    free(doc->title);
    free(doc->language);
    for (i = 0; i < doc->section_count; i++) {
        hp_section_free(&doc->sections[i]);
    }
    free(doc->sections);
    free(doc);
}

static int para_blank(const hp_paragraph *para) {
    size_t ri;
    if (!para) {
        return 1;
    }
    for (ri = 0; ri < para->run_count; ri++) {
        const char *t = para->runs[ri].text ? para->runs[ri].text : "";
        while (*t) {
            if ((unsigned char)*t > 32) {
                return 0;
            }
            t++;
        }
    }
    return 1;
}

char *hp_document_plain_text(const hp_document *doc) {
    size_t cap = 64;
    size_t len = 0;
    char *buf;
    size_t si, pi, ri, bi, ci;
    int first = 1;

    if (!doc) {
        return NULL;
    }
    buf = (char *)malloc(cap);
    if (!buf) {
        return NULL;
    }
    buf[0] = '\0';

    for (si = 0; si < doc->section_count; si++) {
        const hp_section *sec = &doc->sections[si];
        /* Prefer rich blocks when present */
        if (sec->block_count > 0) {
            for (bi = 0; bi < sec->block_count; bi++) {
                const hp_block *bl = &sec->blocks[bi];
                if (bl->kind == HP_BLOCK_PARAGRAPH) {
                    const hp_paragraph *para = &bl->paragraph;
                    if (para_blank(para)) {
                        continue;
                    }
                    if (!first) {
                        if (len + 2 > cap) {
                            char *nbuf;
                            cap *= 2;
                            nbuf = (char *)realloc(buf, cap);
                            if (!nbuf) {
                                free(buf);
                                return NULL;
                            }
                            buf = nbuf;
                        }
                        buf[len++] = '\n';
                        buf[len] = '\0';
                    }
                    first = 0;
                    for (ri = 0; ri < para->run_count; ri++) {
                        const char *t = para->runs[ri].text ? para->runs[ri].text : "";
                        size_t n = strlen(t);
                        while (len + n + 1 > cap) {
                            char *nbuf;
                            cap *= 2;
                            nbuf = (char *)realloc(buf, cap);
                            if (!nbuf) {
                                free(buf);
                                return NULL;
                            }
                            buf = nbuf;
                        }
                        memcpy(buf + len, t, n);
                        len += n;
                        buf[len] = '\0';
                    }
                } else if (bl->kind == HP_BLOCK_TABLE && bl->table) {
                    for (ci = 0; ci < bl->table->cell_count; ci++) {
                        const hp_table_cell *cell = &bl->table->cells[ci];
                        for (pi = 0; pi < cell->paragraph_count; pi++) {
                            const hp_paragraph *para = &cell->paragraphs[pi];
                            if (para_blank(para)) {
                                continue;
                            }
                            if (!first) {
                                if (len + 2 > cap) {
                                    char *nbuf;
                                    cap *= 2;
                                    nbuf = (char *)realloc(buf, cap);
                                    if (!nbuf) {
                                        free(buf);
                                        return NULL;
                                    }
                                    buf = nbuf;
                                }
                                buf[len++] = '\n';
                                buf[len] = '\0';
                            }
                            first = 0;
                            for (ri = 0; ri < para->run_count; ri++) {
                                const char *t = para->runs[ri].text ? para->runs[ri].text : "";
                                size_t n = strlen(t);
                                while (len + n + 1 > cap) {
                                    char *nbuf;
                                    cap *= 2;
                                    nbuf = (char *)realloc(buf, cap);
                                    if (!nbuf) {
                                        free(buf);
                                        return NULL;
                                    }
                                    buf = nbuf;
                                }
                                memcpy(buf + len, t, n);
                                len += n;
                                buf[len] = '\0';
                            }
                        }
                    }
                }
            }
            continue;
        }
        for (pi = 0; pi < sec->paragraph_count; pi++) {
            const hp_paragraph *para = &sec->paragraphs[pi];
            if (para_blank(para)) {
                continue;
            }
            if (!first) {
                if (len + 2 > cap) {
                    char *nbuf;
                    cap *= 2;
                    nbuf = (char *)realloc(buf, cap);
                    if (!nbuf) {
                        free(buf);
                        return NULL;
                    }
                    buf = nbuf;
                }
                buf[len++] = '\n';
                buf[len] = '\0';
            }
            first = 0;
            for (ri = 0; ri < para->run_count; ri++) {
                const char *t = para->runs[ri].text ? para->runs[ri].text : "";
                size_t n = strlen(t);
                while (len + n + 1 > cap) {
                    char *nbuf;
                    cap *= 2;
                    nbuf = (char *)realloc(buf, cap);
                    if (!nbuf) {
                        free(buf);
                        return NULL;
                    }
                    buf = nbuf;
                }
                memcpy(buf + len, t, n);
                len += n;
                buf[len] = '\0';
            }
        }
    }
    return buf;
}
