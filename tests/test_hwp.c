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
#include "../src/hwpx_package.h"

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

/* text of one converted part */
static char *part_text(const hp_document *doc, const char *name) {
    const hp_package_entry *p = hp_package_find(doc->package, name);
    char *s;
    if (!p) {
        return NULL;
    }
    s = (char *)malloc(p->len + 1);
    if (s) {
        memcpy(s, p->data, p->len);
        s[p->len] = '\0';
    }
    return s;
}

/* 0.11.2: section, table and font details 한글 writes into its own HWPX of the
 * same document (values from sample/corpus/ethics.hwpx) */
static void test_converted_details(void) {
    char hwp[512], *head, *s0, *s1;
    hp_document *doc = NULL;
    corpus_path(hwp, sizeof(hwp), "ethics", "hwp");
    EXPECT(hp_hwp_read(hwp, &doc) == HP_OK, "details: read");
    if (!doc) {
        return;
    }
    head = part_text(doc, "Contents/header.xml");
    s0 = part_text(doc, "Contents/section0.xml");
    s1 = part_text(doc, "Contents/section1.xml");
    EXPECT(s0 && strstr(s0, "<hp:lineNumberShape restartType=\"0\" countBy=\"0\" distance=\"0\" startNumber=\"0\"/>"),
           "details: line number shape");
    EXPECT(s0 && strstr(s0, "<hp:footNotePr><hp:autoNumFormat type=\"DIGIT\" userChar=\"\" prefixChar=\"\" "
                            "suffixChar=\")\" supscript=\"0\"/><hp:noteLine length=\"-1\" type=\"SOLID\" "
                            "width=\"0.12 mm\" color=\"#000000\"/><hp:noteSpacing betweenNotes=\"285\" "
                            "belowLine=\"565\" aboveLine=\"850\"/>"),
           "details: footnote shape");
    EXPECT(s0 && strstr(s0, "<hp:noteLine length=\"14692344\"") && strstr(s0, "place=\"END_OF_DOCUMENT\""),
           "details: endnote shape");
    EXPECT(s0 && strstr(s0, "<hp:pageBorderFill type=\"ODD\" borderFillIDRef=\"1\" textBorder=\"PAPER\" "
                            "headerInside=\"0\" footerInside=\"0\" fillArea=\"PAPER\"><hp:offset left=\"1417\""),
           "details: page border fills");
    EXPECT(s1 && strstr(s1, "<hp:cellzone startRowAddr=\"2\" startColAddr=\"0\" endRowAddr=\"9\" endColAddr=\"6\""),
           "details: table cell zones");
    EXPECT(head && strstr(head, "face=\"한양신명조\" type=\"HFT\""), "details: HFT font kind");
    EXPECT(head && strstr(head, "face=\"맑은 고딕\" type=\"TTF\" isEmbedded=\"0\"><hh:typeInfo familyType=\"FCAT_GOTHIC\" "
                                "weight=\"5\" proportion=\"3\" contrast=\"2\" strokeVariation=\"0\" armStyle=\"0\" "
                                "letterform=\"2\" midline=\"0\" xHeight=\"4\"/></hh:font>"),
           "details: font type information");
    EXPECT(head && strstr(head, "<hh:substFont face=\"한컴바탕\" type=\"TTF\""), "details: substitute font");
    free(head);
    free(s0);
    free(s1);
    hp_document_free(doc);
}

/* 0.12.0: headers/footers with page number fields, footnotes, paragraph
 * numbers, and saved line positions that count controls as 8 units. The
 * section is replaced in a copy of ethics.hwpx (its header has numbering 1). */
static void test_subtexts(void) {
    static const char section[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\" ?>"
        "<hs:sec xmlns:hp=\"http://www.hancom.co.kr/hwpml/2011/paragraph\" "
        "xmlns:hs=\"http://www.hancom.co.kr/hwpml/2011/section\">"
        "<hp:p id=\"0\" paraPrIDRef=\"0\" styleIDRef=\"0\"><hp:run charPrIDRef=\"0\">"
        "<hp:secPr id=\"\" outlineShapeIDRef=\"1\"><hp:pagePr landscape=\"WIDELY\" width=\"59528\" height=\"84186\">"
        "<hp:margin header=\"4252\" footer=\"4252\" gutter=\"0\" left=\"8504\" right=\"8504\" top=\"5668\" bottom=\"4252\"/>"
        "</hp:pagePr><hp:footNotePr><hp:autoNumFormat type=\"DIGIT\" userChar=\"\" prefixChar=\"\" suffixChar=\")\" "
        "supscript=\"1\"/><hp:noteLine length=\"-1\" type=\"SOLID\" width=\"0.12 mm\" color=\"#000000\"/>"
        "<hp:noteSpacing betweenNotes=\"283\" belowLine=\"567\" aboveLine=\"850\"/></hp:footNotePr>"
        "<hp:endNotePr><hp:autoNumFormat type=\"ROMAN_SMALL\" userChar=\"\" prefixChar=\"\" suffixChar=\")\" "
        "supscript=\"0\"/></hp:endNotePr></hp:secPr>"
        "<hp:ctrl><hp:header id=\"1\" applyPageType=\"BOTH\"><hp:subList id=\"\" vertAlign=\"BOTTOM\">"
        "<hp:p id=\"0\" paraPrIDRef=\"0\" styleIDRef=\"0\"><hp:run charPrIDRef=\"0\"><hp:t>머리말 시험</hp:t></hp:run></hp:p>"
        "<hp:p id=\"0\" paraPrIDRef=\"0\" styleIDRef=\"0\"><hp:run charPrIDRef=\"0\"><hp:tbl id=\"9\" rowCnt=\"1\" "
        "colCnt=\"1\" cellSpacing=\"0\" borderFillIDRef=\"3\"><hp:sz width=\"20000\" height=\"1500\"/>"
        "<hp:pos treatAsChar=\"1\"/><hp:tr><hp:tc borderFillIDRef=\"3\"><hp:subList id=\"\">"
        "<hp:p id=\"0\" paraPrIDRef=\"0\" styleIDRef=\"0\"><hp:run charPrIDRef=\"0\"><hp:t>머리 표</hp:t></hp:run></hp:p>"
        "</hp:subList><hp:cellAddr colAddr=\"0\" rowAddr=\"0\"/><hp:cellSpan colSpan=\"1\" rowSpan=\"1\"/>"
        "<hp:cellSz width=\"20000\" height=\"1500\"/></hp:tc></hp:tr></hp:tbl></hp:run></hp:p>"
        "</hp:subList></hp:header></hp:ctrl>"
        "<hp:ctrl><hp:footer id=\"2\" applyPageType=\"ODD\"><hp:subList id=\"\" vertAlign=\"TOP\">"
        "<hp:p id=\"0\" paraPrIDRef=\"0\" styleIDRef=\"0\"><hp:run charPrIDRef=\"0\"><hp:t>- </hp:t>"
        "<hp:ctrl><hp:autoNum num=\"1\" numType=\"PAGE\"/></hp:ctrl><hp:t> -</hp:t></hp:run></hp:p>"
        "</hp:subList></hp:footer></hp:ctrl><hp:t>본문</hp:t></hp:run></hp:p>"
        "<hp:p id=\"0\" paraPrIDRef=\"35\" styleIDRef=\"0\"><hp:run charPrIDRef=\"0\"><hp:t>첫째</hp:t></hp:run></hp:p>"
        "<hp:p id=\"0\" paraPrIDRef=\"36\" styleIDRef=\"0\"><hp:run charPrIDRef=\"0\"><hp:t>아래</hp:t></hp:run></hp:p>"
        "<hp:p id=\"0\" paraPrIDRef=\"35\" styleIDRef=\"0\"><hp:run charPrIDRef=\"0\"><hp:t>둘째</hp:t></hp:run></hp:p>"
        "<hp:p id=\"0\" paraPrIDRef=\"0\" styleIDRef=\"0\"><hp:run charPrIDRef=\"0\"><hp:t>각주가</hp:t>"
        "<hp:ctrl><hp:footNote number=\"1\" instId=\"1\"><hp:subList id=\"\" vertAlign=\"TOP\">"
        "<hp:p id=\"0\" paraPrIDRef=\"0\" styleIDRef=\"0\"><hp:run charPrIDRef=\"0\">"
        "<hp:ctrl><hp:autoNum num=\"1\" numType=\"FOOTNOTE\"/></hp:ctrl><hp:t> 각주 내용</hp:t></hp:run></hp:p>"
        "</hp:subList></hp:footNote></hp:ctrl><hp:t> 있는 문단</hp:t></hp:run></hp:p>"
        "<hp:p id=\"0\" paraPrIDRef=\"0\" styleIDRef=\"0\"><hp:run charPrIDRef=\"0\"><hp:t>미주가</hp:t>"
        "<hp:ctrl><hp:endNote number=\"1\" instId=\"2\"><hp:subList id=\"\" vertAlign=\"TOP\">"
        "<hp:p id=\"0\" paraPrIDRef=\"0\" styleIDRef=\"0\"><hp:run charPrIDRef=\"0\">"
        "<hp:ctrl><hp:autoNum num=\"1\" numType=\"ENDNOTE\"/></hp:ctrl><hp:t> 미주 내용</hp:t></hp:run></hp:p>"
        "</hp:subList></hp:endNote></hp:ctrl><hp:t> 있는 문단</hp:t></hp:run></hp:p>"
        "<hp:p id=\"0\" paraPrIDRef=\"0\" styleIDRef=\"0\"><hp:run charPrIDRef=\"0\"><hp:t>가나다</hp:t>"
        "<hp:ctrl><hp:fieldBegin id=\"7\" type=\"CLICK_HERE\" name=\"x\"/></hp:ctrl><hp:t>라마</hp:t>"
        "<hp:ctrl><hp:fieldEnd beginIDRef=\"7\"/></hp:ctrl><hp:t>바사<hp:tab width=\"4000\" leader=\"0\" type=\"1\"/>"
        "자</hp:t></hp:run><hp:linesegarray>"
        "<hp:lineseg textpos=\"0\" vertpos=\"0\" vertsize=\"1000\" textheight=\"1000\" baseline=\"850\" spacing=\"600\" "
        "horzpos=\"0\" horzsize=\"42520\" flags=\"393216\"/>"
        "<hp:lineseg textpos=\"21\" vertpos=\"1600\" vertsize=\"1000\" textheight=\"1000\" baseline=\"850\" spacing=\"600\" "
        "horzpos=\"0\" horzsize=\"42520\" flags=\"393216\"/>"
        "<hp:lineseg textpos=\"31\" vertpos=\"3200\" vertsize=\"1000\" textheight=\"1000\" baseline=\"850\" spacing=\"600\" "
        "horzpos=\"0\" horzsize=\"42520\" flags=\"393216\"/></hp:linesegarray></hp:p></hs:sec>";
    char src[512];
    hp_document *d = NULL;
    const hp_section *sec;
    size_t i;
    corpus_path(src, sizeof(src), "ethics", "hwpx");
    EXPECT(hp_hwpx_read(src, &d) == HP_OK, "subtexts: read source");
    if (!d) {
        return;
    }
    for (i = 0; i < d->package->entry_count; i++) {
        hp_package_entry *p = &d->package->entries[i];
        if (!strcmp(p->name, "Contents/section0.xml")) {
            free(p->data);
            p->len = strlen(section);
            p->data = (unsigned char *)malloc(p->len + 1);
            if (p->data) {
                memcpy(p->data, section, p->len + 1);
            }
        }
    }
    EXPECT(hp_hwpx_write(d, "tmp/subtexts.hwpx") == HP_OK, "subtexts: write");
    hp_document_free(d);
    d = NULL;
    EXPECT(hp_hwpx_read("tmp/subtexts.hwpx", &d) == HP_OK && d && d->section_count >= 1, "subtexts: reread");
    if (!d) {
        return;
    }
    sec = &d->sections[0];
    EXPECT(sec->subtext_count == 3, "subtexts: header, footer and footnote");
    if (sec->subtext_count == 3) {
        const hp_subtext *h = &sec->subtexts[0], *f = &sec->subtexts[1], *n = &sec->subtexts[2];
        EXPECT(h->kind == HP_SUB_HEADER && h->vert_align == 2 && h->apply == 0 && h->block_count == 2 && h->blocks[1].kind == HP_BLOCK_TABLE &&
                   !strcmp(h->blocks[0].paragraph.runs[0].text, "머리말 시험"),
               "subtexts: header text and table, bottom aligned");
        EXPECT(f->kind == HP_SUB_FOOTER && f->apply == 2 && f->block_count == 1 &&
                   f->blocks[0].paragraph.run_count == 3 && f->blocks[0].paragraph.runs[1].field == 1,
               "subtexts: odd-page footer with a page number field");
        EXPECT(n->kind == HP_SUB_FOOTNOTE && n->block == 4 && n->block_count == 1 &&
                   !strcmp(n->blocks[0].paragraph.runs[0].text, "1)"),
               "subtexts: footnote numbered 1) in its text, anchored to its paragraph");
    }
    EXPECT(sec->block_count == 7, "subtexts: body blocks");
    if (sec->block_count == 7) {
        const hp_paragraph *a = &sec->blocks[1].paragraph, *b = &sec->blocks[2].paragraph,
                           *c = &sec->blocks[3].paragraph, *fn = &sec->blocks[4].paragraph,
                           *en = &sec->blocks[5].paragraph, *fl = &sec->blocks[6].paragraph;
        const hp_section *last = &d->sections[d->section_count - 1];
        const hp_block *note = last->block_count ? &last->blocks[last->block_count - 1] : NULL;
        EXPECT(en->run_count == 3 && !strcmp(en->runs[1].text, "i)"), "endnote: reference number (own format)");
        EXPECT(note && note->note_separator && note->kind == HP_BLOCK_PARAGRAPH &&
                   !strcmp(note->paragraph.runs[0].text, "i)") && !strcmp(note->paragraph.runs[1].text, " 미주 내용"),
               "endnote: text after the last section, below a separator");
        EXPECT(!strcmp(a->runs[0].text, "1. ") && a->generated_prefix == 3, "numbers: 1.");
        EXPECT(!strcmp(b->runs[0].text, "1.1. ") && b->generated_prefix == 5, "numbers: 1.1.");
        EXPECT(!strcmp(c->runs[0].text, "2. "), "numbers: 2. (next at level 1)");
        EXPECT(fn->run_count == 3 && !strcmp(fn->runs[1].text, "1)") && fn->runs[1].script == 1 && en->runs[1].script == 0,
               "footnote: reference number in the text, superscript as its format says");
        /* 가나다[8]라마[8]바사[tab 8]자: saved 21 -> 5 (바), 31 -> 8 (자) */
        EXPECT(fl->seg_count == 3 && fl->segs[1].textpos == 5 && fl->segs[2].textpos == 8,
               "saved lines: controls and tabs count 8 units");
    }
    hp_document_free(d);
}

/* 0.12.1: HWPML 2.x (한글 XML, e.g. 국가법령정보센터 downloads named .hwp) */
static void test_hwpml(void) {
    static const char doc[] =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!DOCTYPE HWPML [\n\t<!ENTITY nbsp\t\"&#160;\">\n]>\n"
        "<HWPML Version=\"2.1\"><HEAD SecCnt=\"1\"><DOCSUMMARY><TITLE>법 &amp; 시험</TITLE></DOCSUMMARY>"
        "<MAPPINGTABLE><BINDATALIST Count=\"1\"><BINITEM Type=\"Embedding\" BinData=\"1\" Format=\"png\"/></BINDATALIST>"
        "<FACENAMELIST><FONTFACE Lang=\"Hangul\" Count=\"1\"><FONT Id=\"0\" Type=\"ttf\" Name=\"굴림\"/></FONTFACE>"
        "</FACENAMELIST><BORDERFILLLIST Count=\"1\"><BORDERFILL Id=\"1\" ThreeD=\"false\" Shadow=\"false\">"
        "<LEFTBORDER Type=\"Solid\" Width=\"0.12mm\" Color=\"0\"/><RIGHTBORDER Type=\"None\" Width=\"0.1mm\" Color=\"0\"/>"
        "<TOPBORDER Type=\"None\" Width=\"0.1mm\" Color=\"0\"/><BOTTOMBORDER Type=\"None\" Width=\"0.1mm\" Color=\"0\"/>"
        "<FILLBRUSH><WINDOWBRUSH FaceColor=\"255\" HatchColor=\"0\" Alpha=\"0\"/></FILLBRUSH></BORDERFILL></BORDERFILLLIST>"
        "<CHARSHAPELIST Count=\"2\"><CHARSHAPE Id=\"0\" Height=\"1000\" TextColor=\"0\" ShadeColor=\"4294967295\">"
        "<FONTID Hangul=\"0\" Latin=\"0\" Hanja=\"0\" Japanese=\"0\" Other=\"0\" Symbol=\"0\" User=\"0\"/></CHARSHAPE>"
        "<CHARSHAPE Id=\"1\" Height=\"1200\" TextColor=\"255\" ShadeColor=\"4294967295\"><FONTID Hangul=\"0\" Latin=\"0\" "
        "Hanja=\"0\" Japanese=\"0\" Other=\"0\" Symbol=\"0\" User=\"0\"/><BOLD/></CHARSHAPE></CHARSHAPELIST>"
        "<PARASHAPELIST Count=\"1\"><PARASHAPE Id=\"0\" Align=\"Justify\" HeadingType=\"None\" Heading=\"0\" Level=\"0\">"
        "<PARAMARGIN Indent=\"-2620\" Left=\"4000\" Right=\"0\" Prev=\"0\" Next=\"0\" LineSpacingType=\"Percent\" "
        "LineSpacing=\"130\"/></PARASHAPE></PARASHAPELIST></MAPPINGTABLE></HEAD>"
        "<BODY><SECTION Id=\"0\"><P ParaShape=\"0\"><TEXT CharShape=\"0\"><SECDEF OutlineShape=\"1\">"
        "<PAGEDEF Landscape=\"0\" Width=\"59528\" Height=\"84188\" GutterType=\"LeftOnly\"><PAGEMARGIN Left=\"4252\" "
        "Right=\"4252\" Top=\"5669\" Bottom=\"4252\" Header=\"3600\" Footer=\"3600\" Gutter=\"0\"/></PAGEDEF>"
        "<FOOTNOTESHAPE><AUTONUMFORMAT Type=\"Digit\" SuffixChar=\")\" Superscript=\"false\"/>"
        "<NOTELINE Length=\"5cm\" Type=\"Solid\" Width=\"0.12mm\" Color=\"0\"/></FOOTNOTESHAPE></SECDEF>"
        "<FOOTER ApplyPageType=\"Both\"><PARALIST VertAlign=\"Bottom\"><P ParaShape=\"0\"><TEXT CharShape=\"0\">"
        "<CHAR>- </CHAR><AUTONUM Number=\"1\" NumberType=\"Page\"/><CHAR> / </CHAR>"
        "<AUTONUM Number=\"1\" NumberType=\"TotalPage\"/></TEXT></P></PARALIST></FOOTER>"
        "<CHAR>제1조(목적)&nbsp;이 법은</CHAR></TEXT><TEXT CharShape=\"1\"><CHAR>굵게</CHAR>"
        "<FOOTNOTE><PARALIST><P ParaShape=\"0\"><TEXT CharShape=\"0\"><AUTONUM Number=\"1\" NumberType=\"Footnote\"/>"
        "<CHAR> 각주</CHAR></TEXT></P></PARALIST></FOOTNOTE></TEXT></P>"
        "<P ParaShape=\"0\"><TEXT CharShape=\"0\"><TABLE RowCount=\"1\" ColCount=\"1\" CellSpacing=\"0\" BorderFill=\"1\">"
        "<SHAPEOBJECT InstId=\"5\" ZOrder=\"0\" NumberingType=\"Table\"><SIZE Width=\"20000\" Height=\"1500\"/>"
        "<POSITION TreatAsChar=\"true\" VertRelTo=\"Para\" HorzRelTo=\"Para\"/></SHAPEOBJECT><ROW><CELL ColAddr=\"0\" "
        "RowAddr=\"0\" ColSpan=\"1\" RowSpan=\"1\" Width=\"20000\" Height=\"1500\" BorderFill=\"1\"><PARALIST>"
        "<P ParaShape=\"0\"><TEXT CharShape=\"0\"><CHAR>셀</CHAR></TEXT></P></PARALIST></CELL></ROW></TABLE></TEXT></P>"
        "<P ParaShape=\"0\"><TEXT CharShape=\"0\"><PICTURE><SHAPEOBJECT InstId=\"6\" ZOrder=\"1\" NumberingType=\"Figure\" "
        "TextWrap=\"TopAndBottom\"><SIZE Width=\"2834\" Height=\"2834\"/><POSITION TreatAsChar=\"true\"/></SHAPEOBJECT>"
        "<SHAPECOMPONENT OriWidth=\"2834\" OriHeight=\"2834\" CurWidth=\"2834\" CurHeight=\"2834\"/>"
        "<IMAGE Bright=\"0\" Contrast=\"0\" Effect=\"RealPic\" BinItem=\"1\"/></PICTURE></TEXT></P></SECTION></BODY>"
        "<TAIL><BINDATASTORAGE><BINDATA Size=\"16\" Encoding=\"Base64\" Id=\"1\">iVBORw0KGgoAAAANSUhEUg==</BINDATA>"
        "</BINDATASTORAGE></TAIL></HWPML>";
    hp_document *d = NULL;
    const hp_section *sec;
    EXPECT(hp_hwpml_read_memory((const unsigned char *)doc, strlen(doc), &d) == HP_OK && d, "hwpml: read");
    if (!d) {
        return;
    }
    EXPECT(d->title && !strcmp(d->title, "법 & 시험"), "hwpml: title");
    sec = &d->sections[0];
    EXPECT(sec->page.margin_top == 5669 && sec->page.footer_height == 3600, "hwpml: page definition");
    EXPECT(sec->block_count == 3, "hwpml: paragraph, table, picture");
    if (sec->block_count == 3) {
        const hp_paragraph *p = &sec->blocks[0].paragraph;
        EXPECT(p->run_count == 3 && !strcmp(p->runs[0].text, "제1조(목적)\xC2\xA0이 법은") && p->runs[1].bold &&
                   p->runs[1].font_size_hwp == 1200 && p->runs[1].color_rgb == 0xFF0000 && !strcmp(p->runs[2].text, "1)"),
               "hwpml: text, &nbsp;, character shapes, footnote number");
        EXPECT(p->indent_first_hwp == -1310 && p->indent_left_hwp == 2000 && p->line_spacing_percent == 130,
               "hwpml: paragraph margins are doubled HWPUNITs");
        EXPECT(sec->blocks[1].kind == HP_BLOCK_TABLE && sec->blocks[1].table &&
                   !strcmp(sec->blocks[1].table->cells[0].paragraphs[0].runs[0].text, "셀"),
               "hwpml: table");
        EXPECT(sec->blocks[2].kind == HP_BLOCK_IMAGE && sec->blocks[2].image && sec->blocks[2].image->data_len == 16 &&
                   !memcmp(sec->blocks[2].image->data, "\x89PNG", 4),
               "hwpml: Base64 picture");
    }
    EXPECT(sec->subtext_count == 2 && sec->subtexts[0].kind == HP_SUB_FOOTER && sec->subtexts[1].kind == HP_SUB_FOOTNOTE,
           "hwpml: footer and footnote");
    if (sec->subtext_count == 2) {
        const hp_paragraph *f = &sec->subtexts[0].blocks[0].paragraph;
        EXPECT(sec->subtexts[0].vert_align == 2 && f->run_count == 4 && f->runs[1].field == 1 && f->runs[3].field == 2,
               "hwpml: footer page number and total pages");
    }
    EXPECT(sec->footnote_shape.line_length_hwp == 14173, "hwpml: separator length in cm");
    hp_document_free(d);
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
    test_converted_details();
    test_subtexts();
    test_hwpml();
    test_refused_and_damaged();
    if (failures) {
        fprintf(stderr, "%d hwp test(s) failed\n", failures);
        return 1;
    }
    puts("hwp tests passed");
    return 0;
}
