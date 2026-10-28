/* HWP 5.x reader tests.
 *
 *   test_hwp [SOURCE_DIR]   (SOURCE_DIR holds sample/corpus; default ".")
 *
 * - The Hancom-saved HWP copies of the three corpus documents give the same plain
 *   text, sections, tables and pictures as their HWPX originals.
 * - An HWP document saves as HWPX (conversion) and reads back with the same text.
 * - The paragraph editor opens HWP documents and saves edits as HWPX.
 * - Encrypted / distribution-only files are refused; truncated and corrupted files
 *   fail cleanly (run under ASan/UBSan in CI). */
#include "hp/edit.h"
#include "hp/hwp.h"
#include "hp/hwpx.h"
#include "hp/ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define EXPECT(cond, msg)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                        \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static const char *source_dir = ".";
static const char *const stems[] = {"ethics", "gyuje-규제합리화", "mfds-프로프라놀롤"};

static void corpus_path(char *out, size_t n, const char *stem, const char *ext) {
    snprintf(out, n, "%s/sample/corpus/%s.%s", source_dir, stem, ext);
}

/* plain text without whitespace (line layout may differ, characters may not) */
static char *squeezed_text(const hp_document *doc) {
    char *t = hp_document_plain_text(doc), *a, *b;
    if (!t) {
        return NULL;
    }
    for (a = b = t; *a; a++) {
        if (*a != ' ' && *a != '\n' && *a != '\t' && *a != '\r') {
            *b++ = *a;
        }
    }
    *b = '\0';
    return t;
}

typedef struct {
    size_t tables, images, containers, equations;
} counts;

static void count_table(const hp_table *t, counts *c);

static void count_blocks(const hp_block *blocks, size_t n, counts *c) {
    size_t i;
    for (i = 0; i < n; i++) {
        if (blocks[i].kind == HP_BLOCK_TABLE && blocks[i].table) {
            count_table(blocks[i].table, c);
        } else if (blocks[i].kind == HP_BLOCK_IMAGE) {
            c->images++;
        } else if (blocks[i].kind == HP_BLOCK_CONTAINER) {
            c->containers++;
        } else if (blocks[i].kind == HP_BLOCK_EQUATION) {
            c->equations++;
        }
    }
}

static void count_table(const hp_table *t, counts *c) {
    size_t i, k;
    c->tables++;
    for (i = 0; i < t->cell_count; i++) {
        const hp_table_cell *cell = &t->cells[i];
        if (cell->image) {
            c->images++;
        }
        if (cell->container) {
            c->containers++;
        }
        for (k = 0; k < cell->nested_table_count; k++) {
            count_table(cell->nested_tables[k], c);
        }
    }
}

static counts doc_counts(const hp_document *doc) {
    counts c;
    size_t s;
    memset(&c, 0, sizeof(c));
    for (s = 0; s < doc->section_count; s++) {
        count_blocks(doc->sections[s].blocks, doc->sections[s].block_count, &c);
    }
    return c;
}

static void test_corpus_matches_hwpx(void) {
    size_t i;
    for (i = 0; i < sizeof(stems) / sizeof(stems[0]); i++) {
        char hwp[512], hwpx[512], msg[256];
        hp_document *a = NULL, *b = NULL;
        hp_status sa, sb;
        corpus_path(hwp, sizeof(hwp), stems[i], "hwp");
        corpus_path(hwpx, sizeof(hwpx), stems[i], "hwpx");
        EXPECT(hp_looks_like_hwp(hwp) == 1 && hp_looks_like_hwpx(hwp) == 0, "corpus: detected as HWP");
        sa = hp_hwp_read(hwp, &a);
        sb = hp_hwpx_read(hwpx, &b);
        snprintf(msg, sizeof(msg), "corpus %s: both formats read (%s / %s)", stems[i], hp_status_string(sa),
                 hp_status_string(sb));
        EXPECT(sa == HP_OK && sb == HP_OK && a && b, msg);
        if (a && b) {
            char *ta = squeezed_text(a), *tb = squeezed_text(b);
            counts ca = doc_counts(a), cb = doc_counts(b);
            snprintf(msg, sizeof(msg), "corpus %s: identical text (%zu / %zu bytes)", stems[i], ta ? strlen(ta) : 0,
                     tb ? strlen(tb) : 0);
            EXPECT(ta && tb && strcmp(ta, tb) == 0 && strlen(ta) > 1000, msg);
            snprintf(msg, sizeof(msg), "corpus %s: sections %zu / %zu", stems[i], a->section_count, b->section_count);
            EXPECT(a->section_count == b->section_count, msg);
            snprintf(msg, sizeof(msg), "corpus %s: tables %zu/%zu images %zu/%zu containers %zu/%zu", stems[i],
                     ca.tables, cb.tables, ca.images, cb.images, ca.containers, cb.containers);
            EXPECT(ca.tables == cb.tables && ca.images == cb.images && ca.containers == cb.containers &&
                       ca.equations == cb.equations && ca.tables > 0,
                   msg);
            EXPECT(a->sections[0].page.width == b->sections[0].page.width &&
                       a->sections[0].page.height == b->sections[0].page.height &&
                       a->sections[0].page.margin_left == b->sections[0].page.margin_left,
                   "corpus: page setup");
            free(ta);
            free(tb);
        }
        hp_document_free(a);
        hp_document_free(b);
    }
}

/* Every picture of the HWP carries its decoded image bytes. */
static void test_pictures_have_data(void) {
    char hwp[512];
    hp_document *doc = NULL;
    size_t s, i, with = 0, total = 0;
    corpus_path(hwp, sizeof(hwp), "mfds-프로프라놀롤", "hwp");
    EXPECT(hp_hwp_read(hwp, &doc) == HP_OK, "pictures: read");
    for (s = 0; doc && s < doc->section_count; s++) {
        for (i = 0; i < doc->sections[s].block_count; i++) {
            const hp_block *b = &doc->sections[s].blocks[i];
            const hp_table *t = b->kind == HP_BLOCK_TABLE ? b->table : NULL;
            size_t k;
            if (b->kind == HP_BLOCK_IMAGE && b->image) {
                total++;
                with += b->image->data_len > 8;
            }
            for (k = 0; t && k < t->cell_count; k++) {
                if (t->cells[k].image) {
                    total++;
                    with += t->cells[k].image->data_len > 8;
                }
            }
        }
    }
    EXPECT(total > 0 && with == total, "pictures: every picture has its BinData bytes");
    hp_document_free(doc);
}

static void test_convert_to_hwpx(void) {
    char hwp[512];
    const char *out = "tmp/hp-hwp-converted.hwpx";
    hp_document *doc = NULL, *back = NULL;
    corpus_path(hwp, sizeof(hwp), "ethics", "hwp");
    EXPECT(hp_document_read(hwp, &doc) == HP_OK && doc, "convert: read via hp_document_read");
    if (!doc) {
        return;
    }
    EXPECT(hp_hwpx_write(doc, out) == HP_OK, "convert: save as HWPX");
    EXPECT(hp_looks_like_hwpx(out) == 1, "convert: output is HWPX");
    EXPECT(hp_document_read(out, &back) == HP_OK && back, "convert: read back");
    if (back) {
        char *a = squeezed_text(doc), *b = squeezed_text(back);
        counts ca = doc_counts(doc), cb = doc_counts(back);
        EXPECT(a && b && strcmp(a, b) == 0, "convert: same text after HWP -> HWPX");
        EXPECT(ca.tables == cb.tables && ca.images == cb.images, "convert: same tables and pictures");
        free(a);
        free(b);
    }
    hp_document_free(doc);
    hp_document_free(back);
}

static void test_editor_on_hwp(void) {
    char hwp[512];
    hp_editor *e = NULL;
    size_t i, target = (size_t)-1;
    hp_document *back = NULL;
    const char *out = "tmp/hp-hwp-edited.hwpx";
    corpus_path(hwp, sizeof(hwp), "mfds-프로프라놀롤", "hwp");
    EXPECT(hp_editor_open(hwp, &e) == HP_OK && e, "editor: open HWP");
    if (!e) {
        return;
    }
    EXPECT(hp_editor_count(e) > 10, "editor: paragraphs listed");
    for (i = 0; i < hp_editor_count(e); i++) {
        if (strstr(hp_editor_text(e, i), "프로프라놀롤")) {
            target = i;
            break;
        }
    }
    EXPECT(target != (size_t)-1, "editor: find paragraph");
    if (target != (size_t)-1) {
        char edited[4096];
        snprintf(edited, sizeof(edited), "%s [한글패드 편집]", hp_editor_text(e, target));
        EXPECT(hp_editor_set(e, target, edited) == HP_OK, "editor: set text");
        EXPECT(hp_editor_save(e, out) == HP_OK, "editor: save as HWPX");
        EXPECT(hp_hwpx_read(out, &back) == HP_OK && back, "editor: read saved");
        if (back) {
            char *t = hp_document_plain_text(back);
            EXPECT(t && strstr(t, "[한글패드 편집]"), "editor: edit kept");
            free(t);
        }
    }
    hp_document_free(back);
    hp_editor_free(e);
}

static unsigned char *load(const char *path, size_t *len) {
    FILE *f = hp_fopen(path, "rb");
    unsigned char *buf;
    long n;
    *len = 0;
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (unsigned char *)malloc(n > 0 ? (size_t)n : 1);
    if (buf && n > 0 && fread(buf, 1, (size_t)n, f) == (size_t)n) {
        *len = (size_t)n;
    }
    fclose(f);
    return buf;
}

/* FileHeader is a small stream (mini stream): its signature appears verbatim. */
static unsigned char *file_header_in(unsigned char *buf, size_t len) {
    size_t i;
    for (i = 0; i + 40 <= len; i++) {
        if (memcmp(buf + i, "HWP Document File", 17) == 0) {
            return buf + i;
        }
    }
    return NULL;
}

static void test_refused_and_damaged(void) {
    char hwp[512];
    size_t len, i;
    unsigned char *orig, *copy, *fh;
    hp_document *doc = NULL;
    uint32_t seed = 12345;
    int ok = 0, failed = 0;
    corpus_path(hwp, sizeof(hwp), "ethics", "hwp");
    orig = load(hwp, &len);
    EXPECT(orig && len > 4096, "damage: load");
    if (!orig || len <= 4096) {
        free(orig);
        return;
    }
    copy = (unsigned char *)malloc(len);
    if (!copy) {
        free(orig);
        return;
    }
    /* not an HWP file */
    EXPECT(hp_hwp_read_memory((const unsigned char *)"PK\003\004 not a compound file", 26, &doc) == HP_ERR_NOT_HWP &&
               !doc,
           "refuse: not a compound file");
    /* password-protected and distribution-only flags */
    memcpy(copy, orig, len);
    fh = file_header_in(copy, len);
    EXPECT(fh != NULL, "refuse: FileHeader located");
    if (fh) {
        fh[36] |= 2;
        EXPECT(hp_hwp_read_memory(copy, len, &doc) == HP_ERR_UNSUPPORTED && !doc, "refuse: password");
        fh[36] = (unsigned char)((fh[36] & ~2) | 16);
        EXPECT(hp_hwp_read_memory(copy, len, &doc) == HP_ERR_UNSUPPORTED && !doc, "refuse: DRM");
        /* distribution flag without ViewText streams: damaged, not silently read as BodyText */
        fh[36] = (unsigned char)((fh[36] & ~16) | 4);
        EXPECT(hp_hwp_read_memory(copy, len, &doc) != HP_OK && !doc, "distribution flag needs ViewText");
    }
    /* truncated files */
    for (i = 1; i < 32; i++) {
        size_t cut = len * i / 32;
        if (hp_hwp_read_memory(orig, cut, &doc) == HP_OK) {
            ok++;
            hp_document_free(doc);
            doc = NULL;
        } else {
            failed++;
            EXPECT(doc == NULL, "damage: no document on failure");
        }
    }
    /* random byte damage */
    for (i = 0; i < 120; i++) {
        int k;
        memcpy(copy, orig, len);
        for (k = 0; k < 1 + (int)(i % 8); k++) {
            seed = seed * 1103515245u + 12345u;
            copy[(seed >> 4) % len] ^= (unsigned char)(1u << ((seed >> 24) & 7));
        }
        if (hp_hwp_read_memory(copy, len, &doc) == HP_OK) {
            char *t = hp_document_plain_text(doc);
            free(t);
            hp_document_free(doc);
            doc = NULL;
            ok++;
        } else {
            failed++;
        }
    }
    EXPECT(ok + failed == 151, "damage: every variant handled");
    printf("damaged variants: %d read, %d refused\n", ok, failed);
    free(copy);
    free(orig);
}

int main(int argc, char **argv) {
    if (argc > 1) {
        source_dir = argv[1];
    }
    test_corpus_matches_hwpx();
    test_pictures_have_data();
    test_convert_to_hwpx();
    test_editor_on_hwp();
    test_refused_and_damaged();
    if (failures) {
        fprintf(stderr, "%d hwp test(s) failed\n", failures);
        return 1;
    }
    puts("hwp tests passed");
    return 0;
}
