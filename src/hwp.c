#include "hp/hwp.h"

#include <stdio.h>
#include <string.h>

const unsigned char HP_CFB_MAGIC[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};

int hp_looks_like_hwp(const char *path) {
    unsigned char magic[8];
    size_t n;
    FILE *f;

    if (!path) {
        return 0;
    }
    f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    n = fread(magic, 1, 8, f);
    fclose(f);
    if (n < 8) {
        return 0;
    }
    return memcmp(magic, HP_CFB_MAGIC, 8) == 0;
}

hp_status hp_hwp_read(const char *path, hp_document **out_doc) {
    if (out_doc) {
        *out_doc = NULL;
    }
    if (!path || !out_doc) {
        return HP_ERR_INVALID;
    }
    if (!hp_looks_like_hwp(path)) {
        return HP_ERR_NOT_HWP;
    }
    /* Phase 1 stub: CFB detected, full record parse deferred. */
    return HP_ERR_UNSUPPORTED;
}

hp_status hp_hwp_write(const hp_document *doc, const char *path) {
    (void)doc;
    (void)path;
    return HP_ERR_UNSUPPORTED;
}
