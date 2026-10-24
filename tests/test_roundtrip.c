#include "hp/hwp.h"
#include "hp/hwpx.h"
#include "hp/ir.h"

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
    const char *out = "/tmp/hp-c-write-read.hwpx";

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
    const char *out = "/tmp/hp-c-mimetype.hwpx";
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
    const char *out = "/tmp/hp-c-fixture-rt.hwpx";
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
    const char *path = "/tmp/hp-c-fake.hwp";
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

int main(void) {
    test_write_read_korean();
    test_mimetype_first_stored();
    test_fixture_roundtrip();
    test_hwp_stub();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    puts("all tests passed");
    return 0;
}
