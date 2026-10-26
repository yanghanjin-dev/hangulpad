/* HangulPad — in-memory copy of an HWPX (ZIP) package for lossless re-save.
 *
 * When a document is read from a file, every part of the package (XML, BinData,
 * Preview, settings, META-INF …) is kept byte-for-byte. hp_hwpx_write() then
 * writes those originals back and regenerates only what was actually changed.
 * Internal header (needs libzip); public code only sees `struct hp_package *`.
 */
#ifndef HP_HWPX_PACKAGE_H
#define HP_HWPX_PACKAGE_H

#include "hp/common.h"

#include <time.h>
#include <zip.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Refuse absurd packages instead of exhausting memory (zip bombs). */
#define HP_PACKAGE_MAX_ENTRIES 4096
#define HP_PACKAGE_MAX_ENTRY_BYTES ((zip_uint64_t)512 * 1024 * 1024)
#define HP_PACKAGE_MAX_TOTAL_BYTES ((zip_uint64_t)1024 * 1024 * 1024)

typedef struct hp_package_entry {
    char *name;
    unsigned char *data;
    size_t len;
    int stored;     /* 1 = original entry was STORE (uncompressed) */
    int is_dir;     /* directory entry ("foo/") */
    time_t mtime;
    int has_mtime;
} hp_package_entry;

typedef struct hp_package {
    hp_package_entry *entries; /* original order */
    size_t entry_count;
    char **section_parts;      /* section index → entry name (e.g. Contents/section0.xml) */
    size_t section_part_count;
    char *loaded_title;        /* doc->title as read; differs → content.hpf title is patched */
} hp_package;

hp_status hp_package_load_zip(zip_t *za, hp_package **out);
void hp_package_free(hp_package *pkg);
const hp_package_entry *hp_package_find(const hp_package *pkg, const char *name);
hp_status hp_package_add_section_part(hp_package *pkg, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* HP_HWPX_PACKAGE_H */
