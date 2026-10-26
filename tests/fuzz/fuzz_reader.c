/* libFuzzer: whole HWPX reader. Input = section0.xml, packaged with the header.xml of
 * a real document (HP_FUZZ_SOURCE, default sample/corpus/ethics.hwpx). */
#include "hp/hwpx.h"
#include "hp/ir.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zip.h>
static char *header; static size_t header_len;
static char path[64];
int LLVMFuzzerInitialize(int *argc, char ***argv) {
    const char *src = getenv("HP_FUZZ_SOURCE") ? getenv("HP_FUZZ_SOURCE") : "sample/corpus/ethics.hwpx";
    int err = 0; zip_t *za = zip_open(src, ZIP_RDONLY, &err); zip_stat_t st; zip_file_t *zf;
    (void)argc; (void)argv;
    if (!za || zip_stat(za, "Contents/header.xml", 0, &st) != 0 || !(zf = zip_fopen(za, "Contents/header.xml", 0))) abort();
    header_len = (size_t)st.size; header = malloc(header_len);
    if (zip_fread(zf, header, header_len) != (zip_int64_t)header_len) abort();
    zip_fclose(zf); zip_close(za);
    snprintf(path, sizeof path, "/dev/shm/fz-%d.hwpx", (int)getpid());
    return 0;
}
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    int err = 0; zip_t *za = zip_open(path, ZIP_CREATE | ZIP_TRUNCATE, &err);
    hp_document *doc = NULL;
    static const char mime[] = "application/hwp+zip";
    zip_file_add(za, "mimetype", zip_source_buffer(za, mime, strlen(mime), 0), 0);
    zip_file_add(za, "Contents/header.xml", zip_source_buffer(za, header, header_len, 0), 0);
    zip_file_add(za, "Contents/section0.xml", zip_source_buffer(za, data, size, 0), 0);
    zip_set_file_compression(za, 1, ZIP_CM_STORE, 0); zip_set_file_compression(za, 2, ZIP_CM_STORE, 0);
    zip_close(za);
    if (hp_hwpx_read(path, &doc) == HP_OK) {
        char *t = hp_document_plain_text(doc); free(t);
        hp_document_free(doc);
    }
    return 0;
}
