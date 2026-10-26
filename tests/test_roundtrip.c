#include "hp/hwp.h"
#include "hp/hwpx.h"
#include "hp/ir.h"
#include "hp/version.h"
#include "../src/hwpx_package.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zip.h>

static int failures = 0;

#define EXPECT(cond, msg)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                        \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static const char *fixture_path(void) {
    const char *env = getenv("HP_FIXTURE");
    return env ? env : "tests/fixtures/simple_paragraph.hwpx";
}

static void test_write_read_korean(void) {
    hp_document *doc = hp_document_new();
    hp_document *loaded = NULL;
    char *text;
    const char *out = "tmp/hp-c-write-read.hwpx";

    EXPECT(doc != NULL, "alloc doc");
    hp_document_set_title(doc, "테스트");
    hp_section_add_paragraph_text(&doc->sections[0], "한글패드 엔진");
    hp_section_add_paragraph_text(&doc->sections[0], "Phase 1 HWPX roundtrip");
    EXPECT(hp_hwpx_write(doc, out) == HP_OK, "write");
    EXPECT(hp_hwpx_read(out, &loaded) == HP_OK, "read");
    EXPECT(loaded != NULL, "loaded");
    if (loaded) {
        EXPECT(strcmp(loaded->title, "테스트") == 0, "title");
        text = hp_document_plain_text(loaded);
        EXPECT(text != NULL, "plain text");
        if (text) {
            EXPECT(strcmp(text, "한글패드 엔진\nPhase 1 HWPX roundtrip") == 0, "body text");
            free(text);
        }
        hp_document_free(loaded);
    }
    hp_document_free(doc);
}

static void test_mimetype_first_stored(void) {
    hp_document *doc = hp_document_from_text("stored-check");
    const char *out = "tmp/hp-c-mimetype.hwpx";
    zip_t *za;
    int zerr = 0;
    zip_stat_t st;

    EXPECT(doc != NULL, "doc");
    EXPECT(hp_hwpx_write(doc, out) == HP_OK, "write");
    za = zip_open(out, ZIP_RDONLY, &zerr);
    EXPECT(za != NULL, "zip open");
    if (za) {
        EXPECT(zip_stat_index(za, 0, 0, &st) == 0, "stat0");
        EXPECT(strcmp(st.name, "mimetype") == 0, "first entry mimetype");
        EXPECT(st.comp_method == ZIP_CM_STORE, "mimetype stored");
        zip_close(za);
    }
    hp_document_free(doc);
}

static void test_fixture_roundtrip(void) {
    const char *in = fixture_path();
    const char *out = "tmp/hp-c-fixture-rt.hwpx";
    hp_document *doc = NULL;
    char *text;

    EXPECT(hp_hwpx_roundtrip(in, out, &doc) == HP_OK, "fixture roundtrip");
    EXPECT(doc != NULL, "doc");
    if (doc) {
        text = hp_document_plain_text(doc);
        EXPECT(text && strstr(text, "한글패드") != NULL, "fixture text");
        free(text);
        hp_document_free(doc);
    }
}

static void test_hwp_stub(void) {
    const char *path = "tmp/hp-c-fake.hwp";
    FILE *f = fopen(path, "wb");
    hp_document *doc = NULL;
    unsigned char pad[64] = {0};

    EXPECT(f != NULL, "create fake hwp");
    if (f) {
        fwrite(HP_CFB_MAGIC, 1, 8, f);
        fwrite(pad, 1, sizeof(pad), f);
        fclose(f);
    }
    EXPECT(hp_looks_like_hwp(path) == 1, "cfb detect");
    EXPECT(hp_hwp_read(path, &doc) == HP_ERR_UNSUPPORTED, "read stub");
    EXPECT(hp_hwp_write(NULL, path) == HP_ERR_UNSUPPORTED, "write unsupported");
}


/* ---------- lossless save ---------- */

static const char *rich_fixture(void) {
    const char *env = getenv("HP_RICH_FIXTURE");
    return env ? env : "tests/fixtures/float_nest_eq.hwpx";
}

static unsigned char *zip_entry(zip_t *za, zip_uint64_t i, zip_uint64_t *len) {
    zip_stat_t st;
    zip_file_t *zf;
    unsigned char *buf;
    if (zip_stat_index(za, i, 0, &st) != 0) {
        return NULL;
    }
    buf = (unsigned char *)malloc((size_t)st.size + 1);
    zf = zip_fopen_index(za, i, 0);
    if (!buf || !zf) {
        free(buf);
        if (zf) {
            zip_fclose(zf);
        }
        return NULL;
    }
    if (st.size && zip_fread(zf, buf, st.size) != (zip_int64_t)st.size) {
        free(buf);
        buf = NULL;
    }
    zip_fclose(zf);
    if (buf) {
        buf[st.size] = '\0'; /* callers may strstr() the text */
    }
    *len = st.size;
    return buf;
}

/* Number of parts that differ between a and b, ignoring the part named `allowed`
 * (NULL = none). Also checks order, count and mimetype-first/STORED. -1 on error. */
static int package_diff(const char *a, const char *b, const char *allowed) {
    zip_t *za, *zb;
    int err = 0, diffs = 0;
    zip_int64_t n, i;
    zip_stat_t st;
    za = zip_open(a, ZIP_RDONLY, &err);
    zb = zip_open(b, ZIP_RDONLY, &err);
    if (!za || !zb) {
        if (za) {
            zip_close(za);
        }
        if (zb) {
            zip_close(zb);
        }
        return -1;
    }
    n = zip_get_num_entries(za, 0);
    if (n != zip_get_num_entries(zb, 0)) {
        diffs = -1;
    }
    if (diffs == 0 && zip_stat_index(zb, 0, 0, &st) == 0 &&
        (strcmp(st.name, "mimetype") != 0 || st.comp_method != ZIP_CM_STORE)) {
        diffs = -1;
    }
    for (i = 0; diffs >= 0 && i < n; i++) {
        const char *name_a = zip_get_name(za, (zip_uint64_t)i, 0);
        const char *name_b = zip_get_name(zb, (zip_uint64_t)i, 0);
        zip_uint64_t la = 0, lb = 0;
        unsigned char *da, *db;
        if (!name_a || !name_b || strcmp(name_a, name_b) != 0) {
            diffs = -1;
            break;
        }
        if (allowed && strcmp(name_a, allowed) == 0) {
            continue;
        }
        da = zip_entry(za, (zip_uint64_t)i, &la);
        db = zip_entry(zb, (zip_uint64_t)i, &lb);
        if (!da || !db || la != lb || memcmp(da, db, (size_t)la) != 0) {
            diffs++;
        }
        free(da);
        free(db);
    }
    zip_close(za);
    zip_close(zb);
    return diffs;
}

static size_t count_blocks(const hp_document *doc, hp_block_kind kind) {
    size_t s, b, n = 0;
    for (s = 0; s < doc->section_count; s++) {
        for (b = 0; b < doc->sections[s].block_count; b++) {
            if (doc->sections[s].blocks[b].kind == kind) {
                n++;
            }
        }
    }
    return n;
}

static void test_lossless_save(void) {
    const char *in = rich_fixture();
    const char *out = "tmp/hp-c-lossless.hwpx";
    hp_document *doc = NULL;
    EXPECT(hp_hwpx_read(in, &doc) == HP_OK, "lossless: read");
    if (!doc) {
        return;
    }
    EXPECT(doc->package != NULL, "lossless: package kept");
    EXPECT(hp_hwpx_write(doc, out) == HP_OK, "lossless: write");
    EXPECT(package_diff(in, out, NULL) == 0, "lossless: every part byte-identical");
    hp_document_free(doc);
}

static void test_append_keeps_rest(void) {
    const char *in = rich_fixture();
    const char *out = "tmp/hp-c-append.hwpx";
    hp_document *doc = NULL, *back = NULL;
    size_t tables, images, last;
    char *text;
    char part[64];
    EXPECT(hp_hwpx_read(in, &doc) == HP_OK, "append: read");
    if (!doc) {
        return;
    }
    tables = count_blocks(doc, HP_BLOCK_TABLE);
    images = count_blocks(doc, HP_BLOCK_IMAGE) + count_blocks(doc, HP_BLOCK_CONTAINER);
    last = doc->section_count - 1;
    EXPECT(hp_section_add_paragraph_text(&doc->sections[last], "덧붙인 문단 <&>") == HP_OK, "append: add");
    EXPECT(hp_hwpx_write(doc, out) == HP_OK, "append: write");
    snprintf(part, sizeof(part), "Contents/section%zu.xml", last);
    EXPECT(package_diff(in, out, part) == 0, "append: other parts untouched");
    EXPECT(package_diff(in, out, NULL) == 1, "append: section changed");
    EXPECT(hp_hwpx_read(out, &back) == HP_OK, "append: reread");
    if (back) {
        EXPECT(count_blocks(back, HP_BLOCK_TABLE) == tables, "append: tables kept");
        EXPECT(count_blocks(back, HP_BLOCK_IMAGE) + count_blocks(back, HP_BLOCK_CONTAINER) == images,
               "append: images kept");
        text = hp_document_plain_text(back);
        EXPECT(text && strstr(text, "덧붙인 문단 <&>") != NULL, "append: new text present");
        free(text);
        hp_document_free(back);
    }
    hp_document_free(doc);
}

static void test_title_patch(void) {
    const char *in = rich_fixture();
    const char *out = "tmp/hp-c-title.hwpx";
    hp_document *doc = NULL, *back = NULL;
    EXPECT(hp_hwpx_read(in, &doc) == HP_OK, "title: read");
    if (!doc) {
        return;
    }
    hp_document_set_title(doc, "새 제목 & 테스트");
    EXPECT(hp_hwpx_write(doc, out) == HP_OK, "title: write");
    EXPECT(package_diff(in, out, "Contents/content.hpf") == 0, "title: only content.hpf changed");
    EXPECT(hp_hwpx_read(out, &back) == HP_OK, "title: reread");
    if (back) {
        EXPECT(back->title && strstr(back->title, "새 제목") != NULL, "title: new title read back");
        hp_document_free(back);
    }
    hp_document_free(doc);
}

static void copy_file(const char *from, const char *to) {
    FILE *a = fopen(from, "rb"), *b = fopen(to, "wb");
    char buf[8192];
    size_t n;
    if (a && b) {
        while ((n = fread(buf, 1, sizeof(buf), a)) > 0) {
            fwrite(buf, 1, n, b);
        }
    }
    if (a) {
        fclose(a);
    }
    if (b) {
        fclose(b);
    }
}

static void test_inplace_save(void) {
    const char *path = "tmp/hp-c-inplace.hwpx";
    hp_document *doc = NULL;
    copy_file(rich_fixture(), path);
    EXPECT(hp_hwpx_read(path, &doc) == HP_OK, "inplace: read");
    if (!doc) {
        return;
    }
    EXPECT(hp_hwpx_write(doc, path) == HP_OK, "inplace: write over source");
    EXPECT(package_diff(rich_fixture(), path, NULL) == 0, "inplace: identical");
    hp_document_free(doc);
}

static void test_failed_save_keeps_target(void) {
    const char *path = "tmp/hp-c-keep.hwpx";
    hp_document *doc = NULL;
    copy_file(rich_fixture(), path);
    EXPECT(hp_hwpx_read(path, &doc) == HP_OK, "keep: read");
    if (!doc) {
        return;
    }
    /* Section count no longer matches the package → write must refuse, not clobber. */
    doc->section_count++;
    EXPECT(hp_hwpx_write(doc, path) == HP_ERR_UNSUPPORTED, "keep: refused");
    doc->section_count--;
    EXPECT(package_diff(rich_fixture(), path, NULL) == 0, "keep: target untouched");
    hp_document_free(doc);
}

static void test_rewrite_section(void) {
    const char *in = rich_fixture();
    const char *out = "tmp/hp-c-rewrite.hwpx";
    hp_document *doc = NULL;
    copy_file(in, out);
    EXPECT(hp_hwpx_read(in, &doc) == HP_OK, "rewrite: read");
    if (!doc) {
        return;
    }
    doc->sections[0].needs_rewrite = 1;
    EXPECT(hp_hwpx_write(doc, out) == HP_ERR_UNSUPPORTED, "rewrite: refused");
    EXPECT(package_diff(in, out, NULL) == 0, "rewrite: target untouched");
    hp_document_free(doc);
}

static void test_removed_paragraph_refused(void) {
    const char *in = rich_fixture(), *out = "tmp/hp-c-remove-refused.hwpx";
    hp_document *doc = NULL;
    copy_file(in, out);
    EXPECT(hp_hwpx_read(in, &doc) == HP_OK, "remove: read");
    if (!doc) return;
    /* Simulate a count reduction even if the fixture's first section is empty. */
    doc->sections[0].loaded_paragraph_count = doc->sections[0].paragraph_count + 1;
    EXPECT(hp_hwpx_write(doc, out) == HP_ERR_UNSUPPORTED, "remove: refused");
    EXPECT(package_diff(in, out, NULL) == 0, "remove: target untouched");
    hp_document_free(doc);
}

static void test_append_failure_keeps_target(void) {
    const char *in = rich_fixture(), *out = "tmp/hp-c-append-refused.hwpx";
    hp_document *doc = NULL;
    hp_package_entry *entry = NULL;
    size_t i;
    copy_file(in, out);
    EXPECT(hp_hwpx_read(in, &doc) == HP_OK, "append failure: read");
    if (!doc) return;
    EXPECT(hp_section_add_paragraph_text(&doc->sections[0], "must not replace tables") == HP_OK,
           "append failure: add paragraph");
    for (i = 0; i < doc->package->entry_count; i++) {
        if (strcmp(doc->package->entries[i].name, doc->package->section_parts[0]) == 0) {
            entry = &doc->package->entries[i];
            break;
        }
    }
    EXPECT(entry != NULL, "append failure: section part");
    if (entry) {
        /* Force insertion failure by removing the saved XML's closing tag. */
        memset(entry->data, ' ', entry->len);
        EXPECT(hp_hwpx_write(doc, out) == HP_ERR_UNSUPPORTED, "append failure: refused");
        EXPECT(package_diff(in, out, NULL) == 0, "append failure: target untouched");
    }
    hp_document_free(doc);
}

/* New documents must be packaged like Hangul's own files (Hangul reports
 * "damaged" otherwise): same part list/order, full paths in content.hpf. */
static void test_fresh_package_shape(void) {
    static const char *const expected[] = {"mimetype", "version.xml", "Contents/header.xml",
                                           "Contents/section0.xml", "Preview/PrvText.txt", "settings.xml",
                                           "META-INF/container.rdf", "Contents/content.hpf",
                                           "META-INF/container.xml", "META-INF/manifest.xml"};
    const char *out = "tmp/hp-c-fresh-shape.hwpx";
    hp_document *doc = hp_document_from_text("새 문서");
    zip_t *za;
    int err = 0;
    size_t i;
    zip_uint64_t len = 0;
    unsigned char *hpf, *header;
    EXPECT(doc && hp_hwpx_write(doc, out) == HP_OK, "fresh: write");
    hp_document_free(doc);
    za = zip_open(out, ZIP_RDONLY, &err);
    EXPECT(za != NULL, "fresh: open");
    if (!za) {
        return;
    }
    EXPECT(zip_get_num_entries(za, 0) == (zip_int64_t)(sizeof(expected) / sizeof(expected[0])), "fresh: part count");
    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        const char *name = zip_get_name(za, i, 0);
        EXPECT(name && strcmp(name, expected[i]) == 0, "fresh: part order");
    }
    hpf = zip_entry(za, 7, &len);
    EXPECT(hpf && strstr((char *)hpf, "href=\"Contents/header.xml\"") != NULL, "fresh: hpf full header path");
    EXPECT(hpf && strstr((char *)hpf, "href=\"Contents/section0.xml\"") != NULL, "fresh: hpf full section path");
    EXPECT(hpf && strstr((char *)hpf, "<opf:title>") != NULL, "fresh: opf:title");
    free(hpf);
    header = zip_entry(za, 2, &len);
    EXPECT(header && strstr((char *)header, "lang=\"USER\"") != NULL, "fresh: 7 font languages");
    free(header);
    zip_close(za);
}

static void test_version_consistent(void) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%d.%d.%d", HP_VERSION_MAJOR, HP_VERSION_MINOR, HP_VERSION_PATCH);
    EXPECT(strcmp(buf, HP_VERSION_STRING) == 0, "version: string matches numbers");
    snprintf(buf, sizeof(buf), "%d, %d, %d, 0", HP_VERSION_MAJOR, HP_VERSION_MINOR, HP_VERSION_PATCH);
    EXPECT(strcmp(buf, HP_VERSION_COMMAS) == 0, "version: appVersion matches numbers");
    EXPECT(strcmp(hp_version(), HP_VERSION_STRING) == 0, "version: hp_version()");
}

/* tests/fixtures/xml_features.hwpx (see make_xml_features.py): XML constructs the
 * string-search reader of <= 0.2.0 got wrong. */
static void test_xml_features(void) {
    hp_document *doc = NULL;
    char *text;
    const hp_section *s0;
    const hp_table *tbl = NULL;
    size_t i;
    EXPECT(hp_hwpx_read("tests/fixtures/xml_features.hwpx", &doc) == HP_OK, "xmlf: read");
    if (!doc) {
        return;
    }
    EXPECT(doc->section_count == 2, "xmlf: two sections from content.hpf");
    EXPECT(strcmp(doc->title, "XML 기능 & 테스트") == 0, "xmlf: title entity decoded");
    EXPECT(strcmp(doc->language, "ko") == 0, "xmlf: language");
    text = hp_document_plain_text(doc);
    EXPECT(text && strstr(text, "숫자참조 가나 끝"), "xmlf: numeric character references");
    EXPECT(text && strstr(text, "<raw> & text"), "xmlf: CDATA");
    EXPECT(text && strstr(text, "주석 사이"), "xmlf: comment skipped, fwSpace -> space");
    EXPECT(text && strstr(text, "표 앞 글자") && strstr(text, "표 뒤 글자"), "xmlf: text beside a table kept");
    EXPECT(text && !strstr(text, "머리말"), "xmlf: header control is not body text");
    EXPECT(text && strstr(text, "둘째 구역 & <끝>"), "xmlf: second section");
    free(text);
    s0 = &doc->sections[0];
    for (i = 0; i < s0->block_count; i++) {
        if (s0->blocks[i].kind == HP_BLOCK_TABLE) {
            tbl = s0->blocks[i].table;
        }
    }
    EXPECT(tbl && tbl->cell_count == 2, "xmlf: table with 2 cells");
    if (tbl && tbl->cell_count == 2) {
        const hp_table_cell *c1 = &tbl->cells[1];
        EXPECT(c1->col == 1 && c1->width_hwp == 10000, "xmlf: cell keeps its own address/size (not the nested one)");
        EXPECT(c1->nested_table_count == 1, "xmlf: nested table");
        EXPECT(tbl->cells[0].pad_left_hwp == 100, "xmlf: cell margin");
    }
    EXPECT(s0->blocks[0].paragraph.runs && s0->blocks[1].paragraph.runs[0].bold == 1 &&
               s0->blocks[1].paragraph.runs[0].font_size_hwp == 1600,
           "xmlf: char shape applied");
    hp_document_free(doc);
}

static void test_malformed_xml_rejected(void) {
    /* a section that is not well-formed must fail cleanly, not half-parse */
    const char *path = "tmp/hp-c-badxml.hwpx";
    zip_t *za;
    int err = 0;
    hp_document *doc = NULL;
    static const char bad[] = "<hs:sec xmlns:hs=\"s\" xmlns:hp=\"p\"><hp:p><hp:run><hp:t>x</hp:run></hp:p></hs:sec>";
    static const char mime[] = "application/hwp+zip";
    remove(path);
    za = zip_open(path, ZIP_CREATE | ZIP_TRUNCATE, &err);
    EXPECT(za != NULL, "badxml: create");
    if (!za) {
        return;
    }
    zip_file_add(za, "mimetype", zip_source_buffer(za, mime, strlen(mime), 0), 0);
    zip_file_add(za, "Contents/section0.xml", zip_source_buffer(za, bad, strlen(bad), 0), 0);
    zip_close(za);
    EXPECT(hp_hwpx_read(path, &doc) == HP_ERR_XML && doc == NULL, "badxml: HP_ERR_XML");
}

/* Korean (non-ASCII) file names: UTF-8 paths must work on Windows too, where the
 * C runtime expects the ANSI code page (libzip and hp_fopen take UTF-8). */
static void test_unicode_paths(void) {
    const char *path = "tmp/한글 파일 이름.hwpx";
    const char *hwp = "tmp/가짜 문서.hwp";
    hp_document *doc = hp_document_from_text("유니코드 경로");
    hp_document *back = NULL;
    char *text;
    FILE *f;
    unsigned char pad[64] = {0};
    EXPECT(doc && hp_hwpx_write(doc, path) == HP_OK, "unicode: write");
    hp_document_free(doc);
    EXPECT(hp_looks_like_hwpx(path) == 1, "unicode: probe hwpx");
    EXPECT(hp_hwpx_read(path, &back) == HP_OK && back, "unicode: read");
    if (back) {
        text = hp_document_plain_text(back);
        EXPECT(text && strstr(text, "유니코드 경로"), "unicode: text");
        free(text);
        EXPECT(hp_hwpx_write(back, path) == HP_OK, "unicode: lossless save in place");
        hp_document_free(back);
    }
    f = hp_fopen(hwp, "wb");
    EXPECT(f != NULL, "unicode: hp_fopen create");
    if (f) {
        fwrite(HP_CFB_MAGIC, 1, 8, f);
        fwrite(pad, 1, sizeof(pad), f);
        fclose(f);
    }
    EXPECT(hp_looks_like_hwp(hwp) == 1, "unicode: probe hwp");
}

static void test_source_formatting(void) {
    const char *path="tmp/source-formatting.hwpx";
    const char *header="<head><fontface lang='HANGUL'><font id='0' face='Arial'/></fontface>"
        "<fontface lang='LATIN'><font id='0' face='Times New Roman'/></fontface>"
        "<charPr id='0' height='1000'><fontRef hangul='0' latin='0'/><italic/>"
        "<underline type='BOTTOM'/><strikeout shape='3D' color='none'/></charPr>"
        "<charPr id='1' height='1000'><fontRef hangul='0' latin='0'/><strikeout shape='SOLID' color='#000000'/></charPr></head>";
    const char *section="<sec><secPr><startNum page='7'/><visibility hideFirstPageNum='1'/></secPr>"
        "<pagePr width='59528' height='84186'><margin header='2834' footer='4252'/></pagePr>"
        "<p><run charPrIDRef='0'><ctrl><pageNum pos='TOP_RIGHT' sideChar='-'/><newNum numType='PAGE' num='3'/><pageHiding hidePageNum='1'/></ctrl><t>A</t></run><linesegarray><lineseg textpos='0' vertpos='100' vertsize='1000' textheight='1000' baseline='850'/></linesegarray></p>"
        "<p><run><tbl rowCnt='1' colCnt='2'><sz width='20000' height='3000'/><inMargin left='0' right='0' top='0' bottom='0'/>"
        "<tr><tc hasMargin='0'><subList vertAlign='CENTER'><p><run charPrIDRef='1'><t>B</t></run></p></subList>"
        "<cellAddr rowAddr='0' colAddr='0'/><cellSz width='10000' height='3000'/><cellMargin left='510'/></tc>"
        "<tc hasMargin='1'><subList vertAlign='BOTTOM'><p><run><pic><img binaryItemIDRef='image1'/>"
        "<sz width='6000' height='5400'/><imgClip left='0' top='0' right='5545' bottom='5400'/><imgDim dimwidth='19800' dimheight='5400'/>"
        "</pic></run></p></subList><cellAddr rowAddr='0' colAddr='1'/><cellSz width='10000' height='3000'/><cellMargin left='510'/></tc>"
        "</tr></tbl></run></p></sec>";
    int err;zip_t *za=zip_open(path,ZIP_CREATE|ZIP_TRUNCATE,&err);hp_document *doc=NULL;
    EXPECT(za!=NULL,"format: create fixture");if(!za)return;
    zip_file_add(za,"mimetype",zip_source_buffer(za,"application/hwp+zip",strlen("application/hwp+zip"),0),0);
    zip_file_add(za,"Contents/header.xml",zip_source_buffer(za,header,strlen(header),0),0);
    zip_file_add(za,"Contents/section0.xml",zip_source_buffer(za,section,strlen(section),0),0);
    EXPECT(zip_close(za)==0,"format: close fixture");
    EXPECT(hp_hwpx_read(path,&doc)==HP_OK,"format: read fixture");if(!doc)return;
    {
        const hp_section *s=&doc->sections[0];const hp_run *r=&s->blocks[0].paragraph.runs[0];
        const hp_table *t=s->blocks[1].table;
        EXPECT(strcmp(r->script_faces[0],"Arial")==0 && strcmp(r->script_faces[1],"Times New Roman")==0,"format: script-local font IDs");
        EXPECT(r->italic && r->underline && !r->strikeout,"format: color none disables strikeout");
        EXPECT(s->paragraphs[0].runs[0].italic && strcmp(s->paragraphs[0].runs[0].script_faces[1],"Times New Roman")==0,"format: cloned run retains formatting");
        EXPECT(s->page.header_height==2834 && s->page.footer_height==4252,"format: header/footer space");
        EXPECT(s->page_number_start==7 && s->hide_first_page_number && s->page_number_position==3 && strcmp(s->page_number_side,"-")==0,"page numbers: section settings");
        EXPECT(s->blocks[0].page_number_reset==3 && s->blocks[0].hide_page_number,"page numbers: paragraph controls");
        EXPECT(s->blocks[0].paragraph.segs[0].baseline_hwp==850 && s->blocks[0].saved_y_hwp==100,"format: source baseline and origin");
        EXPECT(t && t->cells[0].pad_left_hwp==0 && t->cells[1].pad_left_hwp==510,"format: inherited versus explicit cell margins");
        EXPECT(t->cells[0].vertical_align==1 && t->cells[1].vertical_align==2,"format: cell vertical alignment");
        EXPECT(t->cells[0].paragraphs[0].runs[0].strikeout,"format: explicit strikeout");
        EXPECT(t->cells[1].image && t->cells[1].image->image_dim_width==19800 && t->cells[1].image->clip_right==5545,"format: picture crop coordinate space");
    }
    hp_document_free(doc);
}

int main(void) {
    test_source_formatting();
    test_write_read_korean();
    test_mimetype_first_stored();
    test_fixture_roundtrip();
    test_hwp_stub();
    test_lossless_save();
    test_append_keeps_rest();
    test_title_patch();
    test_inplace_save();
    test_failed_save_keeps_target();
    test_rewrite_section();
    test_removed_paragraph_refused();
    test_append_failure_keeps_target();
    test_fresh_package_shape();
    test_version_consistent();
    test_xml_features();
    test_malformed_xml_rejected();
    test_unicode_paths();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    puts("all tests passed");
    return 0;
}
