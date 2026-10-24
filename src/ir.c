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
    run->text = NULL;
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

static void hp_section_free(hp_section *s) {
    size_t i;
    if (!s) {
        return;
    }
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
    return doc;
}

hp_status hp_section_add_paragraph_text(hp_section *sec, const char *text) {
    hp_paragraph *paras;
    hp_run *runs;
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
    sec->paragraphs[sec->paragraph_count].runs = runs;
    sec->paragraphs[sec->paragraph_count].run_count = 1;
    sec->paragraph_count++;
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

char *hp_document_plain_text(const hp_document *doc) {
    size_t cap = 64;
    size_t len = 0;
    char *buf;
    size_t si, pi, ri;
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
        for (pi = 0; pi < sec->paragraph_count; pi++) {
            const hp_paragraph *para = &sec->paragraphs[pi];
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
