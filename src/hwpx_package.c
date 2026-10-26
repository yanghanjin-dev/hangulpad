#include "hwpx_package.h"

#include <stdlib.h>
#include <string.h>

static char *pkg_strdup(const char *s) {
    size_t n;
    char *out;
    if (!s) {
        return NULL;
    }
    n = strlen(s);
    out = (char *)malloc(n + 1);
    if (out) {
        memcpy(out, s, n + 1);
    }
    return out;
}

void hp_package_free(hp_package *pkg) {
    size_t i;
    if (!pkg) {
        return;
    }
    for (i = 0; i < pkg->entry_count; i++) {
        free(pkg->entries[i].name);
        free(pkg->entries[i].data);
    }
    free(pkg->entries);
    for (i = 0; i < pkg->section_part_count; i++) {
        free(pkg->section_parts[i]);
    }
    free(pkg->section_parts);
    free(pkg->loaded_title);
    free(pkg);
}

const hp_package_entry *hp_package_find(const hp_package *pkg, const char *name) {
    size_t i;
    if (!pkg || !name) {
        return NULL;
    }
    for (i = 0; i < pkg->entry_count; i++) {
        if (strcmp(pkg->entries[i].name, name) == 0) {
            return &pkg->entries[i];
        }
    }
    return NULL;
}

hp_status hp_package_add_section_part(hp_package *pkg, const char *name) {
    char **arr;
    char *copy;
    if (!pkg || !name) {
        return HP_ERR_INVALID;
    }
    copy = pkg_strdup(name);
    if (!copy) {
        return HP_ERR_NOMEM;
    }
    arr = (char **)realloc(pkg->section_parts, (pkg->section_part_count + 1) * sizeof(char *));
    if (!arr) {
        free(copy);
        return HP_ERR_NOMEM;
    }
    pkg->section_parts = arr;
    pkg->section_parts[pkg->section_part_count++] = copy;
    return HP_OK;
}

hp_status hp_package_load_zip(zip_t *za, hp_package **out) {
    hp_package *pkg;
    zip_int64_t n;
    zip_uint64_t i;
    zip_uint64_t total = 0;

    if (!za || !out) {
        return HP_ERR_INVALID;
    }
    *out = NULL;
    n = zip_get_num_entries(za, 0);
    if (n < 0 || n > HP_PACKAGE_MAX_ENTRIES) {
        return HP_ERR_INVALID;
    }
    pkg = (hp_package *)calloc(1, sizeof(*pkg));
    if (!pkg) {
        return HP_ERR_NOMEM;
    }
    if (n > 0) {
        pkg->entries = (hp_package_entry *)calloc((size_t)n, sizeof(hp_package_entry));
        if (!pkg->entries) {
            free(pkg);
            return HP_ERR_NOMEM;
        }
    }

    for (i = 0; i < (zip_uint64_t)n; i++) {
        zip_stat_t st;
        hp_package_entry *e = &pkg->entries[pkg->entry_count];
        size_t name_len;

        zip_stat_init(&st);
        if (zip_stat_index(za, i, 0, &st) != 0 || !(st.valid & ZIP_STAT_NAME)) {
            hp_package_free(pkg);
            return HP_ERR_INVALID;
        }
        e->name = pkg_strdup(st.name);
        if (!e->name) {
            hp_package_free(pkg);
            return HP_ERR_NOMEM;
        }
        pkg->entry_count++;
        name_len = strlen(e->name);
        e->is_dir = name_len > 0 && e->name[name_len - 1] == '/';
        e->stored = (st.valid & ZIP_STAT_COMP_METHOD) && st.comp_method == ZIP_CM_STORE;
        if (st.valid & ZIP_STAT_MTIME) {
            e->mtime = st.mtime;
            e->has_mtime = 1;
        }
        if (e->is_dir) {
            continue;
        }
        if (!(st.valid & ZIP_STAT_SIZE) || st.size > HP_PACKAGE_MAX_ENTRY_BYTES ||
            total + st.size > HP_PACKAGE_MAX_TOTAL_BYTES) {
            hp_package_free(pkg);
            return HP_ERR_INVALID;
        }
        total += st.size;
        /* +1 so zero-length parts still get a valid buffer */
        e->data = (unsigned char *)malloc((size_t)st.size + 1);
        if (!e->data) {
            hp_package_free(pkg);
            return HP_ERR_NOMEM;
        }
        if (st.size > 0) {
            zip_file_t *zf = zip_fopen_index(za, i, 0);
            zip_int64_t got;
            if (!zf) {
                hp_package_free(pkg);
                return HP_ERR_IO;
            }
            got = zip_fread(zf, e->data, st.size);
            zip_fclose(zf);
            if (got < 0 || (zip_uint64_t)got != st.size) {
                hp_package_free(pkg);
                return HP_ERR_IO;
            }
        }
        e->len = (size_t)st.size;
    }
    *out = pkg;
    return HP_OK;
}
