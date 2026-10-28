/* Read-only Compound File Binary reader (see cfb.h). Format: [MS-CFB]. */
#include "cfb.h"

#include <stdlib.h>
#include <string.h>

#define CFB_FREE 0xFFFFFFFFu
#define CFB_END 0xFFFFFFFEu
#define CFB_HEADER 512u
#define CFB_DIR_ENTRY 128u
#define CFB_NAME_MAX 32

typedef struct {
    char name[CFB_NAME_MAX * 3 + 1]; /* UTF-8 */
    unsigned type;                    /* 1 storage, 2 stream, 5 root */
    uint32_t left, right, child;
    uint32_t start;
    uint64_t size;
    long parent;
} cfb_entry;

struct hp_cfb {
    const unsigned char *data;
    size_t len;
    uint32_t sector_size;
    uint32_t mini_size;
    uint32_t mini_cutoff;
    uint32_t sector_count; /* sectors that exist in the file */
    uint32_t *fat;
    uint32_t fat_count;
    uint32_t *minifat;
    uint32_t minifat_count;
    unsigned char *ministream;
    size_t ministream_len;
    cfb_entry *entries;
    uint32_t entry_count;
};

static uint16_t rd16(const unsigned char *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static const unsigned char *sector_ptr(const hp_cfb *c, uint32_t sector) {
    if (sector >= c->sector_count) {
        return NULL;
    }
    return c->data + (size_t)(sector + 1) * c->sector_size;
}

/* Follow a FAT chain and copy `size` bytes (or the whole chain when size is
 * UINT64_MAX). Rejects loops, out-of-range sectors and short chains. */
static hp_status read_chain(const hp_cfb *c, const uint32_t *fat, uint32_t fat_count, uint32_t start, uint64_t size,
                            int mini, unsigned char **out, size_t *out_len) {
    uint32_t unit = mini ? c->mini_size : c->sector_size;
    uint32_t limit = mini ? (uint32_t)(c->ministream_len / c->mini_size) : c->sector_count;
    uint32_t sector = start, steps = 0;
    size_t cap, len = 0;
    unsigned char *buf;
    int whole = size == UINT64_MAX;

    *out = NULL;
    *out_len = 0;
    if (!whole && size > (uint64_t)c->len + c->ministream_len) {
        return HP_ERR_INVALID; /* larger than anything this file could hold */
    }
    if (!whole && size == 0) {
        *out = (unsigned char *)malloc(1);
        return *out ? HP_OK : HP_ERR_NOMEM;
    }
    cap = whole ? 4096 : (size_t)size;
    buf = (unsigned char *)malloc(cap ? cap : 1);
    if (!buf) {
        return HP_ERR_NOMEM;
    }
    while (sector != CFB_END) {
        const unsigned char *src;
        size_t take;
        if (sector >= limit || sector >= fat_count || ++steps > limit) {
            free(buf);
            return HP_ERR_INVALID;
        }
        src = mini ? c->ministream + (size_t)sector * unit : sector_ptr(c, sector);
        if (!src) {
            free(buf);
            return HP_ERR_INVALID;
        }
        take = unit;
        if (!whole) {
            if (len >= (size_t)size) {
                break;
            }
            if (take > (size_t)size - len) {
                take = (size_t)size - len;
            }
        } else if (len + take > cap) {
            unsigned char *g;
            while (len + take > cap) {
                cap *= 2;
            }
            g = (unsigned char *)realloc(buf, cap);
            if (!g) {
                free(buf);
                return HP_ERR_NOMEM;
            }
            buf = g;
        }
        memcpy(buf + len, src, take);
        len += take;
        sector = fat[sector];
    }
    if (!whole && len < (size_t)size) {
        free(buf);
        return HP_ERR_INVALID; /* chain shorter than the declared size */
    }
    *out = buf;
    *out_len = len;
    return HP_OK;
}

static void utf16_name(const unsigned char *p, unsigned bytes, char *out) {
    unsigned i, n = bytes >= 2 ? bytes / 2 - 1 : 0; /* without the terminating NUL */
    char *o = out;
    if (n > CFB_NAME_MAX - 1) {
        n = CFB_NAME_MAX - 1;
    }
    for (i = 0; i < n; i++) {
        unsigned ch = rd16(p + i * 2);
        if (ch < 0x80) {
            *o++ = (char)ch;
        } else if (ch < 0x800) {
            *o++ = (char)(0xC0 | (ch >> 6));
            *o++ = (char)(0x80 | (ch & 0x3F));
        } else {
            *o++ = (char)(0xE0 | (ch >> 12));
            *o++ = (char)(0x80 | ((ch >> 6) & 0x3F));
            *o++ = (char)(0x80 | (ch & 0x3F));
        }
    }
    *o = '\0';
}

/* Record each entry's parent by walking the sibling trees below every storage. */
static void link_parents(hp_cfb *c, uint32_t node, long parent, uint32_t depth) {
    cfb_entry *e;
    if (node >= c->entry_count || depth > c->entry_count) {
        return;
    }
    e = &c->entries[node];
    if (e->parent != -2) {
        return; /* already visited: cycle or shared node */
    }
    e->parent = parent;
    link_parents(c, e->left, parent, depth + 1);
    link_parents(c, e->right, parent, depth + 1);
    if (e->type == 1) {
        link_parents(c, e->child, (long)node, depth + 1);
    }
}

void hp_cfb_close(hp_cfb *cfb) {
    if (!cfb) {
        return;
    }
    free(cfb->fat);
    free(cfb->minifat);
    free(cfb->ministream);
    free(cfb->entries);
    free(cfb);
}

hp_status hp_cfb_open(const unsigned char *data, size_t len, hp_cfb **out) {
    static const unsigned char magic[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    hp_cfb *c;
    unsigned shift, mini_shift;
    uint32_t fat_sectors, dir_start, minifat_start, minifat_sectors, difat_start, difat_sectors;
    uint32_t per_sector, i, filled = 0;
    unsigned char *dir = NULL, *bytes = NULL;
    size_t dir_len = 0, bytes_len = 0;
    hp_status st;

    if (!out) {
        return HP_ERR_INVALID;
    }
    *out = NULL;
    if (!data || len < CFB_HEADER || memcmp(data, magic, 8) != 0) {
        return HP_ERR_NOT_HWP;
    }
    shift = rd16(data + 30);
    mini_shift = rd16(data + 32);
    if ((shift != 9 && shift != 12) || mini_shift != 6 || rd16(data + 28) != 0xFFFE) {
        return HP_ERR_INVALID;
    }
    c = (hp_cfb *)calloc(1, sizeof(*c));
    if (!c) {
        return HP_ERR_NOMEM;
    }
    c->data = data;
    c->len = len;
    c->sector_size = 1u << shift;
    c->mini_size = 1u << mini_shift;
    c->mini_cutoff = rd32(data + 56);
    if (len >= c->sector_size) {
        c->sector_count = (uint32_t)((len - c->sector_size) / c->sector_size);
    }
    fat_sectors = rd32(data + 44);
    dir_start = rd32(data + 48);
    minifat_start = rd32(data + 60);
    minifat_sectors = rd32(data + 64);
    difat_start = rd32(data + 68);
    difat_sectors = rd32(data + 72);
    per_sector = c->sector_size / 4;
    if (fat_sectors == 0 || fat_sectors > c->sector_count || difat_sectors > c->sector_count ||
        minifat_sectors > c->sector_count) {
        hp_cfb_close(c);
        return HP_ERR_INVALID;
    }

    /* FAT: sector numbers from the header DIFAT (109) and the DIFAT chain. */
    c->fat_count = fat_sectors * per_sector;
    c->fat = (uint32_t *)malloc((size_t)c->fat_count * sizeof(uint32_t));
    if (!c->fat) {
        hp_cfb_close(c);
        return HP_ERR_NOMEM;
    }
    {
        uint32_t difat_sector = difat_start, difat_seen = 0, k;
        for (k = 0; k < 109 && filled < fat_sectors; k++) {
            uint32_t s = rd32(data + 76 + k * 4);
            const unsigned char *p = sector_ptr(c, s);
            uint32_t j;
            if (!p) {
                hp_cfb_close(c);
                return HP_ERR_INVALID;
            }
            for (j = 0; j < per_sector; j++) {
                c->fat[filled * per_sector + j] = rd32(p + j * 4);
            }
            filled++;
        }
        while (filled < fat_sectors) {
            const unsigned char *d = sector_ptr(c, difat_sector);
            if (!d || ++difat_seen > difat_sectors + 1) {
                hp_cfb_close(c);
                return HP_ERR_INVALID;
            }
            for (k = 0; k + 1 < per_sector && filled < fat_sectors; k++) {
                const unsigned char *p = sector_ptr(c, rd32(d + k * 4));
                uint32_t j;
                if (!p) {
                    hp_cfb_close(c);
                    return HP_ERR_INVALID;
                }
                for (j = 0; j < per_sector; j++) {
                    c->fat[filled * per_sector + j] = rd32(p + j * 4);
                }
                filled++;
            }
            difat_sector = rd32(d + (per_sector - 1) * 4);
        }
    }

    /* Directory */
    st = read_chain(c, c->fat, c->fat_count, dir_start, UINT64_MAX, 0, &dir, &dir_len);
    if (st != HP_OK || dir_len < CFB_DIR_ENTRY) {
        free(dir);
        hp_cfb_close(c);
        return st != HP_OK ? st : HP_ERR_INVALID;
    }
    c->entry_count = (uint32_t)(dir_len / CFB_DIR_ENTRY);
    c->entries = (cfb_entry *)calloc(c->entry_count, sizeof(cfb_entry));
    if (!c->entries) {
        free(dir);
        hp_cfb_close(c);
        return HP_ERR_NOMEM;
    }
    for (i = 0; i < c->entry_count; i++) {
        const unsigned char *p = dir + (size_t)i * CFB_DIR_ENTRY;
        cfb_entry *e = &c->entries[i];
        unsigned name_len = rd16(p + 64);
        e->type = p[66];
        if (name_len > 64) {
            name_len = 64;
        }
        utf16_name(p, name_len, e->name);
        e->left = rd32(p + 68);
        e->right = rd32(p + 72);
        e->child = rd32(p + 76);
        e->start = rd32(p + 116);
        e->size = rd32(p + 120);
        if (shift == 12) {
            e->size |= (uint64_t)rd32(p + 124) << 32;
        }
        e->parent = -2;
    }
    free(dir);
    if (c->entries[0].type != 5) {
        hp_cfb_close(c);
        return HP_ERR_INVALID;
    }
    c->entries[0].parent = -1;
    link_parents(c, c->entries[0].child, 0, 0);

    /* Mini FAT and mini stream (root entry's data). */
    if (minifat_sectors && minifat_start != CFB_END) {
        st = read_chain(c, c->fat, c->fat_count, minifat_start, UINT64_MAX, 0, &bytes, &bytes_len);
        if (st != HP_OK) {
            hp_cfb_close(c);
            return st;
        }
        c->minifat_count = (uint32_t)(bytes_len / 4);
        c->minifat = (uint32_t *)malloc((c->minifat_count ? c->minifat_count : 1) * sizeof(uint32_t));
        if (!c->minifat) {
            free(bytes);
            hp_cfb_close(c);
            return HP_ERR_NOMEM;
        }
        for (i = 0; i < c->minifat_count; i++) {
            c->minifat[i] = rd32(bytes + i * 4);
        }
        free(bytes);
        bytes = NULL;
    }
    if (c->entries[0].size > 0 && c->entries[0].start != CFB_END) {
        if (c->entries[0].size > len) {
            hp_cfb_close(c);
            return HP_ERR_INVALID;
        }
        st = read_chain(c, c->fat, c->fat_count, c->entries[0].start, c->entries[0].size, 0, &c->ministream,
                        &c->ministream_len);
        if (st != HP_OK) {
            hp_cfb_close(c);
            return st;
        }
    }
    *out = c;
    return HP_OK;
}

static int name_eq(const char *a, const char *b, size_t blen) {
    size_t i;
    for (i = 0; i < blen; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'a' && x <= 'z') {
            x = (unsigned char)(x - 32);
        }
        if (y >= 'a' && y <= 'z') {
            y = (unsigned char)(y - 32);
        }
        if (x != y || x == 0) {
            return 0;
        }
    }
    return a[blen] == '\0';
}

long hp_cfb_find(const hp_cfb *cfb, const char *path) {
    long parent = 0;
    const char *seg = path;
    if (!cfb || !path) {
        return -1;
    }
    while (*seg) {
        const char *slash = strchr(seg, '/');
        size_t n = slash ? (size_t)(slash - seg) : strlen(seg);
        long found = -1;
        uint32_t i;
        for (i = 1; i < cfb->entry_count; i++) {
            const cfb_entry *e = &cfb->entries[i];
            if (e->parent == parent && (e->type == 1 || e->type == 2) && name_eq(e->name, seg, n)) {
                found = (long)i;
                break;
            }
        }
        if (found < 0) {
            return -1;
        }
        parent = found;
        seg += n;
        if (*seg == '/') {
            seg++;
        }
    }
    return parent == 0 ? -1 : parent;
}

int hp_cfb_is_stream(const hp_cfb *cfb, long entry) {
    return cfb && entry > 0 && (uint32_t)entry < cfb->entry_count && cfb->entries[entry].type == 2;
}

hp_status hp_cfb_read(const hp_cfb *cfb, long entry, unsigned char **out, size_t *out_len) {
    const cfb_entry *e;
    if (!out || !out_len) {
        return HP_ERR_INVALID;
    }
    *out = NULL;
    *out_len = 0;
    if (!hp_cfb_is_stream(cfb, entry)) {
        return HP_ERR_INVALID;
    }
    e = &cfb->entries[entry];
    if (e->size > cfb->len) {
        return HP_ERR_INVALID;
    }
    if (e->size < cfb->mini_cutoff) {
        if (!cfb->minifat && e->size) {
            return HP_ERR_INVALID;
        }
        return read_chain(cfb, cfb->minifat, cfb->minifat_count, e->start, e->size, 1, out, out_len);
    }
    return read_chain(cfb, cfb->fat, cfb->fat_count, e->start, e->size, 0, out, out_len);
}
