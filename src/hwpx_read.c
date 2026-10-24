#include "hp/hwpx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zip.h>

#define HP_MAX_STYLE 512
#define HP_MAX_FONTS 64
#define HP_MAX_BIN 128
#define HP_MAX_FILL 256

typedef struct {
    uint32_t font_size_hwp;
    uint32_t color_rgb;
    int bold;
    int hangul_font_id;
    int valid;
} hp_char_style;

typedef struct {
    hp_align align;
    int valid;
} hp_para_style;

typedef struct {
    char *id;
    char *zip_path;
    unsigned char *data;
    size_t len;
} hp_bin_item;

typedef struct {
    char *faces[HP_MAX_FONTS];
    hp_char_style chars[HP_MAX_STYLE];
    hp_para_style paras[HP_MAX_STYLE];
    uint32_t fill_rgb[HP_MAX_FILL];
    int fill_valid[HP_MAX_FILL];
    hp_bin_item bins[HP_MAX_BIN];
    int bin_count;
} hp_style_tables;

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

static void style_tables_init(hp_style_tables *st) {
    memset(st, 0, sizeof(*st));
}

static void style_tables_free(hp_style_tables *st) {
    size_t i;
    if (!st) {
        return;
    }
    for (i = 0; i < HP_MAX_FONTS; i++) {
        free(st->faces[i]);
    }
    for (i = 0; i < (size_t)st->bin_count; i++) {
        free(st->bins[i].id);
        free(st->bins[i].zip_path);
        free(st->bins[i].data);
    }
}

static char *zip_read_entry(zip_t *za, const char *name) {
    zip_stat_t st;
    zip_file_t *zf;
    char *buf;
    zip_int64_t n;

    if (zip_stat(za, name, 0, &st) != 0) {
        return NULL;
    }
    zf = zip_fopen(za, name, 0);
    if (!zf) {
        return NULL;
    }
    buf = (char *)malloc((size_t)st.size + 1);
    if (!buf) {
        zip_fclose(zf);
        return NULL;
    }
    n = zip_fread(zf, buf, st.size);
    zip_fclose(zf);
    if (n < 0 || (zip_uint64_t)n != st.size) {
        free(buf);
        return NULL;
    }
    buf[st.size] = '\0';
    return buf;
}

static int zip_read_binary(zip_t *za, const char *name, unsigned char **out, size_t *out_len) {
    zip_stat_t st;
    zip_file_t *zf;
    unsigned char *buf;
    zip_int64_t n;
    *out = NULL;
    *out_len = 0;
    if (zip_stat(za, name, 0, &st) != 0) {
        return 0;
    }
    zf = zip_fopen(za, name, 0);
    if (!zf) {
        return 0;
    }
    buf = (unsigned char *)malloc((size_t)st.size);
    if (!buf) {
        zip_fclose(zf);
        return 0;
    }
    n = zip_fread(zf, buf, st.size);
    zip_fclose(zf);
    if (n < 0 || (zip_uint64_t)n != st.size) {
        free(buf);
        return 0;
    }
    *out = buf;
    *out_len = (size_t)st.size;
    return 1;
}

int hp_looks_like_hwpx(const char *path) {
    zip_t *za;
    int zerr = 0;
    char *mt;
    int ok = 0;
    if (!path) {
        return 0;
    }
    za = zip_open(path, ZIP_RDONLY, &zerr);
    if (!za) {
        return 0;
    }
    mt = zip_read_entry(za, "mimetype");
    if (mt) {
        size_t n = strlen(mt);
        while (n > 0 && (mt[n - 1] == '\n' || mt[n - 1] == '\r' || mt[n - 1] == ' ')) {
            mt[--n] = '\0';
        }
        ok = (strcmp(mt, HP_HWPX_MIMETYPE) == 0);
        free(mt);
    }
    zip_close(za);
    return ok;
}

static const char *local_name(const char *tag_lt) {
    const char *q = tag_lt + 1;
    const char *colon;
    if (*q == '/') {
        q++;
    }
    colon = strchr(q, ':');
    if (colon && colon < strchr(q, '>') && (!strchr(q, ' ') || colon < strchr(q, ' '))) {
        return colon + 1;
    }
    return q;
}

static int tag_is(const char *tag_lt, const char *name) {
    const char *ln = local_name(tag_lt);
    size_t n = strlen(name);
    return strncmp(ln, name, n) == 0 &&
           (ln[n] == ' ' || ln[n] == '>' || ln[n] == '/' || ln[n] == '\t' || ln[n] == '\n' ||
            ln[n] == '\r');
}

static int tag_is_close(const char *tag_lt, const char *name) {
    return tag_lt[1] == '/' && tag_is(tag_lt, name);
}

static const char *find_tag_open(const char *hay, const char *tag) {
    const char *p = hay;
    while ((p = strchr(p, '<')) != NULL) {
        if (p[1] != '/' && p[1] != '!' && p[1] != '?' && tag_is(p, tag)) {
            return p;
        }
        p++;
    }
    return NULL;
}

static const char *find_close_gt(const char *p) {
    int in_quot = 0;
    char qch = 0;
    while (*p) {
        if (in_quot) {
            if (*p == qch) {
                in_quot = 0;
            }
        } else if (*p == '"' || *p == '\'') {
            in_quot = 1;
            qch = *p;
        } else if (*p == '>') {
            return p;
        }
        p++;
    }
    return NULL;
}

/* Find matching close tag for an open element starting at open_lt. */
static const char *find_element_end(const char *open_lt, const char *name) {
    const char *gt = find_close_gt(open_lt);
    const char *p;
    int depth = 1;
    if (!gt) {
        return NULL;
    }
    if (gt > open_lt && gt[-1] == '/') {
        return gt + 1;
    }
    p = gt + 1;
    while ((p = strchr(p, '<')) != NULL) {
        if (tag_is_close(p, name)) {
            depth--;
            if (depth == 0) {
                const char *egt = find_close_gt(p);
                return egt ? egt + 1 : NULL;
            }
        } else if (p[1] != '/' && p[1] != '!' && p[1] != '?' && tag_is(p, name)) {
            const char *ogt = find_close_gt(p);
            if (ogt && ogt[-1] != '/') {
                depth++;
            }
        }
        p++;
    }
    return NULL;
}

static int attr_int(const char *tag_open, const char *name, int def) {
    char key[64];
    const char *p;
    size_t n = strlen(name);
    if (n + 3 >= sizeof(key)) {
        return def;
    }
    snprintf(key, sizeof(key), "%s=\"", name);
    p = strstr(tag_open, key);
    if (!p || p > find_close_gt(tag_open)) {
        return def;
    }
    return (int)strtol(p + n + 2, NULL, 10);
}

static int attr_str(const char *tag_open, const char *name, char *out, size_t out_sz) {
    char key[64];
    const char *p;
    const char *end;
    const char *gt;
    size_t n = strlen(name);
    size_t len;
    if (!out || out_sz == 0) {
        return 0;
    }
    out[0] = '\0';
    gt = find_close_gt(tag_open);
    if (!gt || n + 3 >= sizeof(key)) {
        return 0;
    }
    snprintf(key, sizeof(key), "%s=\"", name);
    p = strstr(tag_open, key);
    if (!p || p > gt) {
        return 0;
    }
    p += n + 2;
    end = strchr(p, '"');
    if (!end || end > gt) {
        return 0;
    }
    len = (size_t)(end - p);
    if (len >= out_sz) {
        len = out_sz - 1;
    }
    memcpy(out, p, len);
    out[len] = '\0';
    return 1;
}

static uint32_t parse_color(const char *s) {
    unsigned r = 0, g = 0, b = 0;
    if (!s || !s[0] || strcmp(s, "none") == 0) {
        return 0;
    }
    if (s[0] == '#' && strlen(s) >= 7) {
        sscanf(s + 1, "%02x%02x%02x", &r, &g, &b);
    }
    return (r << 16) | (g << 8) | b;
}

static char *extract_dc_field(const char *xml, const char *local) {
    const char *start = find_tag_open(xml, local);
    const char *gt;
    const char *end;
    size_t n;
    char *out;
    if (!start) {
        return hp_strdup("");
    }
    gt = find_close_gt(start);
    if (!gt || (gt > start && gt[-1] == '/')) {
        return hp_strdup("");
    }
    end = find_element_end(start, local);
    if (!end) {
        return hp_strdup("");
    }
    /* end points after </local>; content is gt+1 .. before </ */
    {
        const char *close = end;
        while (close > gt && !(close[0] == '<' && close[1] == '/')) {
            close--;
        }
        if (close <= gt) {
            return hp_strdup("");
        }
        n = (size_t)(close - (gt + 1));
    }
    out = (char *)malloc(n + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, gt + 1, n);
    out[n] = '\0';
    return out;
}

static char *clean_text_xml(const char *s, size_t n) {
    char *tmp = (char *)malloc(n + 1);
    char *out;
    size_t i = 0, j = 0;
    if (!tmp) {
        return NULL;
    }
    while (i < n) {
        if (s[i] == '<') {
            if (i + 6 < n && (strncmp(s + i, "<hp:tab", 7) == 0 || strncmp(s + i, "<hp:lineBreak", 13) == 0 ||
                              strncmp(s + i, "<hp:lb", 6) == 0)) {
                tmp[j++] = ' ';
            }
            while (i < n && s[i] != '>') {
                i++;
            }
            if (i < n) {
                i++;
            }
            continue;
        }
        if (s[i] == '&') {
            if (i + 5 <= n && strncmp(s + i, "&amp;", 5) == 0) {
                tmp[j++] = '&';
                i += 5;
            } else if (i + 4 <= n && strncmp(s + i, "&lt;", 4) == 0) {
                tmp[j++] = '<';
                i += 4;
            } else if (i + 4 <= n && strncmp(s + i, "&gt;", 4) == 0) {
                tmp[j++] = '>';
                i += 4;
            } else if (i + 6 <= n && strncmp(s + i, "&quot;", 6) == 0) {
                tmp[j++] = '"';
                i += 6;
            } else if (i + 6 <= n && strncmp(s + i, "&apos;", 6) == 0) {
                tmp[j++] = '\'';
                i += 6;
            } else {
                tmp[j++] = s[i++];
            }
        } else {
            tmp[j++] = s[i++];
        }
    }
    tmp[j] = '\0';
    out = hp_strdup(tmp);
    free(tmp);
    return out;
}

static hp_align parse_align_name(const char *a) {
    if (!a) {
        return HP_ALIGN_JUSTIFY;
    }
    if (strcmp(a, "CENTER") == 0) {
        return HP_ALIGN_CENTER;
    }
    if (strcmp(a, "RIGHT") == 0) {
        return HP_ALIGN_RIGHT;
    }
    if (strcmp(a, "LEFT") == 0) {
        return HP_ALIGN_LEFT;
    }
    return HP_ALIGN_JUSTIFY;
}

static void parse_header_styles(const char *xml, hp_style_tables *st) {
    const char *p;
    if (!xml || !st) {
        return;
    }

    p = xml;
    while ((p = find_tag_open(p, "fontface")) != NULL) {
        char lang[32];
        const char *gt = find_close_gt(p);
        const char *end;
        if (!gt) {
            break;
        }
        attr_str(p, "lang", lang, sizeof(lang));
        end = find_element_end(p, "fontface");
        if (!end) {
            break;
        }
        if (strcmp(lang, "HANGUL") == 0) {
            const char *q = gt + 1;
            while (q < end && (q = find_tag_open(q, "font")) != NULL && q < end) {
                int id = attr_int(q, "id", -1);
                char face[128];
                if (id >= 0 && id < HP_MAX_FONTS && attr_str(q, "face", face, sizeof(face))) {
                    free(st->faces[id]);
                    st->faces[id] = hp_strdup(face);
                }
                q = find_close_gt(q);
                if (!q) {
                    break;
                }
                q++;
            }
            break;
        }
        p = end;
    }

    p = xml;
    while ((p = find_tag_open(p, "charPr")) != NULL) {
        const char *end = find_element_end(p, "charPr");
        int id = attr_int(p, "id", -1);
        char color[32];
        if (!end) {
            break;
        }
        if (id >= 0 && id < HP_MAX_STYLE) {
            const char *fontref = find_tag_open(p, "fontRef");
            st->chars[id].font_size_hwp = (uint32_t)attr_int(p, "height", 1000);
            attr_str(p, "textColor", color, sizeof(color));
            st->chars[id].color_rgb = parse_color(color);
            st->chars[id].bold = 0;
            {
                const char *b = strstr(p, "<hh:bold");
                if (!b) {
                    b = strstr(p, "<bold");
                }
                if (b && b < end) {
                    st->chars[id].bold = 1;
                }
            }
            st->chars[id].hangul_font_id = 0;
            if (fontref && fontref < end) {
                st->chars[id].hangul_font_id = attr_int(fontref, "hangul", 0);
            }
            st->chars[id].valid = 1;
        }
        p = end;
    }

    p = xml;
    while ((p = find_tag_open(p, "paraPr")) != NULL) {
        const char *end = find_element_end(p, "paraPr");
        int id = attr_int(p, "id", -1);
        if (!end) {
            break;
        }
        if (id >= 0 && id < HP_MAX_STYLE) {
            const char *al = find_tag_open(p, "align");
            char horiz[32];
            st->paras[id].align = HP_ALIGN_JUSTIFY;
            if (al && al < end && attr_str(al, "horizontal", horiz, sizeof(horiz))) {
                st->paras[id].align = parse_align_name(horiz);
            }
            st->paras[id].valid = 1;
        }
        p = end;
    }

    p = xml;
    while ((p = find_tag_open(p, "borderFill")) != NULL) {
        const char *end = find_element_end(p, "borderFill");
        int id = attr_int(p, "id", -1);
        if (!end) {
            break;
        }
        if (id >= 0 && id < HP_MAX_FILL) {
            const char *fill = find_tag_open(p, "fillBrush");
            const char *win = fill && fill < end ? find_tag_open(fill, "winBrush") : NULL;
            char color[32];
            if (win && win < end && attr_str(win, "faceColor", color, sizeof(color))) {
                if (strcmp(color, "none") != 0) {
                    st->fill_rgb[id] = parse_color(color);
                    st->fill_valid[id] = 1;
                }
            }
        }
        p = end;
    }
}

static void load_bindata(zip_t *za, hp_style_tables *st) {
    zip_int64_t n = zip_get_num_entries(za, 0);
    zip_int64_t i;
    for (i = 0; i < n && st->bin_count < HP_MAX_BIN; i++) {
        const char *name = zip_get_name(za, (zip_uint64_t)i, 0);
        const char *base;
        char id[64];
        unsigned char *data = NULL;
        size_t len = 0;
        if (!name || strncmp(name, "BinData/", 8) != 0) {
            continue;
        }
        base = name + 8;
        {
            const char *dot = strrchr(base, '.');
            size_t idlen = dot ? (size_t)(dot - base) : strlen(base);
            if (idlen >= sizeof(id)) {
                idlen = sizeof(id) - 1;
            }
            memcpy(id, base, idlen);
            id[idlen] = '\0';
        }
        if (!zip_read_binary(za, name, &data, &len)) {
            continue;
        }
        st->bins[st->bin_count].id = hp_strdup(id);
        st->bins[st->bin_count].zip_path = hp_strdup(name);
        st->bins[st->bin_count].data = data;
        st->bins[st->bin_count].len = len;
        st->bin_count++;
    }
}

static const hp_bin_item *find_bin(const hp_style_tables *st, const char *id) {
    int i;
    if (!st || !id) {
        return NULL;
    }
    for (i = 0; i < st->bin_count; i++) {
        if (st->bins[i].id && strcmp(st->bins[i].id, id) == 0) {
            return &st->bins[i];
        }
    }
    return NULL;
}

static void apply_char_style(hp_run *run, const hp_style_tables *st, int char_id) {
    run->font_size_hwp = 1000;
    run->color_rgb = 0;
    run->bold = 0;
    run->font_face = NULL;
    if (!st || char_id < 0 || char_id >= HP_MAX_STYLE || !st->chars[char_id].valid) {
        return;
    }
    run->font_size_hwp = st->chars[char_id].font_size_hwp;
    run->color_rgb = st->chars[char_id].color_rgb;
    run->bold = st->chars[char_id].bold;
    {
        int fid = st->chars[char_id].hangul_font_id;
        if (fid >= 0 && fid < HP_MAX_FONTS && st->faces[fid]) {
            run->font_face = hp_strdup(st->faces[fid]);
        }
    }
}

static hp_status append_block(hp_section *sec, hp_block *blk) {
    hp_block *n = (hp_block *)realloc(sec->blocks, (sec->block_count + 1) * sizeof(*n));
    if (!n) {
        return HP_ERR_NOMEM;
    }
    sec->blocks = n;
    sec->blocks[sec->block_count++] = *blk;
    memset(blk, 0, sizeof(*blk));
    return HP_OK;
}

static hp_status append_flat_para(hp_section *sec, const hp_paragraph *src) {
    hp_paragraph *paras;
    hp_run *runs;
    size_t i;
    if (!src || src->run_count == 0) {
        return HP_OK;
    }
    paras = (hp_paragraph *)realloc(sec->paragraphs, (sec->paragraph_count + 1) * sizeof(*paras));
    if (!paras) {
        return HP_ERR_NOMEM;
    }
    sec->paragraphs = paras;
    memset(&sec->paragraphs[sec->paragraph_count], 0, sizeof(hp_paragraph));
    runs = (hp_run *)calloc(src->run_count, sizeof(hp_run));
    if (!runs) {
        return HP_ERR_NOMEM;
    }
    for (i = 0; i < src->run_count; i++) {
        runs[i].text = hp_strdup(src->runs[i].text ? src->runs[i].text : "");
        runs[i].font_face = src->runs[i].font_face ? hp_strdup(src->runs[i].font_face) : NULL;
        runs[i].font_size_hwp = src->runs[i].font_size_hwp;
        runs[i].color_rgb = src->runs[i].color_rgb;
        runs[i].bold = src->runs[i].bold;
    }
    sec->paragraphs[sec->paragraph_count].runs = runs;
    sec->paragraphs[sec->paragraph_count].run_count = src->run_count;
    sec->paragraphs[sec->paragraph_count].align = src->align;
    sec->paragraph_count++;
    return HP_OK;
}

static hp_status parse_text_paragraph(const char *p_open, const char *p_end, const hp_style_tables *styles,
                                      hp_paragraph *out) {
    const char *q;
    int para_pr_id;
    memset(out, 0, sizeof(*out));
    para_pr_id = attr_int(p_open, "paraPrIDRef", 0);
    out->align = HP_ALIGN_JUSTIFY;
    if (styles && para_pr_id >= 0 && para_pr_id < HP_MAX_STYLE && styles->paras[para_pr_id].valid) {
        out->align = styles->paras[para_pr_id].align;
    }
    q = find_close_gt(p_open);
    if (!q) {
        return HP_OK;
    }
    q++;
    while (q < p_end && (q = find_tag_open(q, "run")) != NULL && q < p_end) {
        const char *run_end = find_element_end(q, "run");
        int char_id;
        const char *tq;
        if (!run_end || run_end > p_end) {
            break;
        }
        /* Skip runs that are only tbl/pic controls */
        if (find_tag_open(q, "tbl") && find_tag_open(q, "tbl") < run_end) {
            q = run_end;
            continue;
        }
        if (find_tag_open(q, "pic") && find_tag_open(q, "pic") < run_end) {
            q = run_end;
            continue;
        }
        char_id = attr_int(q, "charPrIDRef", 0);
        tq = q;
        while (tq < run_end && (tq = find_tag_open(tq, "t")) != NULL && tq < run_end) {
            const char *t_gt = find_close_gt(tq);
            const char *t_end;
            char *text;
            hp_run *runs;
            if (!t_gt || t_gt >= run_end) {
                break;
            }
            if (t_gt[-1] == '/') {
                text = hp_strdup("");
                tq = t_gt + 1;
            } else {
                const char *close;
                t_end = find_element_end(tq, "t");
                if (!t_end || t_end > run_end) {
                    break;
                }
                close = t_end;
                while (close > t_gt && !(close[0] == '<' && close[1] == '/')) {
                    close--;
                }
                text = clean_text_xml(t_gt + 1, (size_t)(close - (t_gt + 1)));
                tq = t_end;
            }
            if (!text) {
                return HP_ERR_NOMEM;
            }
            runs = (hp_run *)realloc(out->runs, (out->run_count + 1) * sizeof(hp_run));
            if (!runs) {
                free(text);
                return HP_ERR_NOMEM;
            }
            out->runs = runs;
            memset(&out->runs[out->run_count], 0, sizeof(hp_run));
            out->runs[out->run_count].text = text;
            apply_char_style(&out->runs[out->run_count], styles, char_id);
            out->run_count++;
        }
        q = run_end;
    }
    return HP_OK;
}

static hp_image *parse_pic(const char *pic_open, const hp_style_tables *styles) {
    const char *end = find_element_end(pic_open, "pic");
    const char *img;
    const char *sz;
    char bin_id[64];
    hp_image *image;
    const hp_bin_item *bin;
    if (!end) {
        return NULL;
    }
    img = find_tag_open(pic_open, "img");
    if (!img || img >= end) {
        return NULL;
    }
    if (!attr_str(img, "binaryItemIDRef", bin_id, sizeof(bin_id))) {
        return NULL;
    }
    image = (hp_image *)calloc(1, sizeof(*image));
    if (!image) {
        return NULL;
    }
    image->binary_id = hp_strdup(bin_id);
    sz = find_tag_open(pic_open, "sz");
    if (sz && sz < end) {
        image->width_hwp = (uint32_t)attr_int(sz, "width", 0);
        image->height_hwp = (uint32_t)attr_int(sz, "height", 0);
    } else {
        const char *org = find_tag_open(pic_open, "orgSz");
        if (org && org < end) {
            image->width_hwp = (uint32_t)attr_int(org, "width", 0);
            image->height_hwp = (uint32_t)attr_int(org, "height", 0);
        }
    }
    bin = find_bin(styles, bin_id);
    if (bin && bin->data && bin->len) {
        image->data = (unsigned char *)malloc(bin->len);
        if (image->data) {
            memcpy(image->data, bin->data, bin->len);
            image->data_len = bin->len;
        }
    }
    return image;
}

static hp_status parse_cell_paragraphs(const char *tc_open, const char *tc_end, const hp_style_tables *styles,
                                       hp_table_cell *cell) {
    const char *p = tc_open;
    /* Prefer first picture in the cell (logos in header tables, etc.). */
    {
        const char *pic = find_tag_open(tc_open, "pic");
        if (pic && pic < tc_end && !cell->image) {
            cell->image = parse_pic(pic, styles);
        }
    }
    while (p < tc_end && (p = find_tag_open(p, "p")) != NULL && p < tc_end) {
        const char *pend = find_element_end(p, "p");
        hp_paragraph para;
        hp_paragraph *arr;
        if (!pend || pend > tc_end) {
            break;
        }
        if (parse_text_paragraph(p, pend, styles, &para) != HP_OK) {
            return HP_ERR_NOMEM;
        }
        arr = (hp_paragraph *)realloc(cell->paragraphs, (cell->paragraph_count + 1) * sizeof(*arr));
        if (!arr) {
            size_t i;
            for (i = 0; i < para.run_count; i++) {
                free(para.runs[i].text);
                free(para.runs[i].font_face);
            }
            free(para.runs);
            return HP_ERR_NOMEM;
        }
        cell->paragraphs = arr;
        cell->paragraphs[cell->paragraph_count++] = para;
        p = pend;
    }
    return HP_OK;
}

static hp_table *parse_table(const char *tbl_open, const hp_style_tables *styles) {
    const char *end = find_element_end(tbl_open, "tbl");
    const char *sz;
    const char *tr;
    hp_table *table;
    if (!end) {
        return NULL;
    }
    table = (hp_table *)calloc(1, sizeof(*table));
    if (!table) {
        return NULL;
    }
    table->row_count = attr_int(tbl_open, "rowCnt", 0);
    table->col_count = attr_int(tbl_open, "colCnt", 0);
    sz = find_tag_open(tbl_open, "sz");
    if (sz && sz < end) {
        table->width_hwp = (uint32_t)attr_int(sz, "width", 0);
        table->height_hwp = (uint32_t)attr_int(sz, "height", 0);
    }
    tr = tbl_open;
    while (tr < end && (tr = find_tag_open(tr, "tr")) != NULL && tr < end) {
        const char *tr_end = find_element_end(tr, "tr");
        const char *tc;
        if (!tr_end || tr_end > end) {
            break;
        }
        tc = tr;
        while (tc < tr_end && (tc = find_tag_open(tc, "tc")) != NULL && tc < tr_end) {
            const char *tc_end = find_element_end(tc, "tc");
            hp_table_cell cell;
            hp_table_cell *arr;
            const char *addr;
            const char *span;
            const char *csz;
            int fill_id;
            if (!tc_end || tc_end > tr_end) {
                break;
            }
            memset(&cell, 0, sizeof(cell));
            cell.fill_rgb = 0xFFFFFFFFu;
            cell.has_fill = 0;
            addr = find_tag_open(tc, "cellAddr");
            span = find_tag_open(tc, "cellSpan");
            csz = find_tag_open(tc, "cellSz");
            if (addr && addr < tc_end) {
                cell.col = attr_int(addr, "colAddr", 0);
                cell.row = attr_int(addr, "rowAddr", 0);
            }
            if (span && span < tc_end) {
                cell.colspan = attr_int(span, "colSpan", 1);
                cell.rowspan = attr_int(span, "rowSpan", 1);
            } else {
                cell.colspan = 1;
                cell.rowspan = 1;
            }
            if (csz && csz < tc_end) {
                cell.width_hwp = (uint32_t)attr_int(csz, "width", 0);
                cell.height_hwp = (uint32_t)attr_int(csz, "height", 0);
            }
            fill_id = attr_int(tc, "borderFillIDRef", -1);
            if (fill_id >= 0 && fill_id < HP_MAX_FILL && styles && styles->fill_valid[fill_id]) {
                cell.fill_rgb = styles->fill_rgb[fill_id];
                cell.has_fill = 1;
            }
            if (parse_cell_paragraphs(tc, tc_end, styles, &cell) != HP_OK) {
                /* continue with what we have */
            }
            arr = (hp_table_cell *)realloc(table->cells, (table->cell_count + 1) * sizeof(*arr));
            if (!arr) {
                break;
            }
            table->cells = arr;
            table->cells[table->cell_count++] = cell;
            tc = tc_end;
        }
        tr = tr_end;
    }
    return table;
}

static hp_status parse_section_xml(const char *xml, hp_section *sec, const hp_style_tables *styles) {
    const char *p;
    int tbl_depth = 0;

    hp_page_default(&sec->page);
    sec->column_count = 1;
    sec->column_gap_hwp = 0;
    sec->blocks = NULL;
    sec->block_count = 0;
    sec->paragraphs = NULL;
    sec->paragraph_count = 0;

    /* 다단 from colPr */
    {
        const char *col = find_tag_open(xml, "colPr");
        if (col) {
            int cc = attr_int(col, "colCount", 1);
            if (cc < 1) {
                cc = 1;
            }
            sec->column_count = cc;
            sec->column_gap_hwp = (uint32_t)attr_int(col, "sameGap", 0);
        }
    }

    p = xml;
    while ((p = strchr(p, '<')) != NULL) {
        if (tag_is(p, "tbl") && p[1] != '/') {
            const char *gt = find_close_gt(p);
            if (tbl_depth == 0) {
                hp_table *table = parse_table(p, styles);
                if (table) {
                    hp_block blk;
                    memset(&blk, 0, sizeof(blk));
                    blk.kind = HP_BLOCK_TABLE;
                    blk.table = table;
                    if (append_block(sec, &blk) != HP_OK) {
                        return HP_ERR_NOMEM;
                    }
                }
                p = find_element_end(p, "tbl");
                if (!p) {
                    break;
                }
                continue;
            }
            if (!gt || gt[-1] != '/') {
                tbl_depth++;
            }
        } else if (tag_is_close(p, "tbl")) {
            if (tbl_depth > 0) {
                tbl_depth--;
            }
        } else if (tbl_depth == 0 && tag_is(p, "pic") && p[1] != '/') {
            hp_image *image = parse_pic(p, styles);
            if (image) {
                hp_block blk;
                memset(&blk, 0, sizeof(blk));
                blk.kind = HP_BLOCK_IMAGE;
                blk.image = image;
                if (append_block(sec, &blk) != HP_OK) {
                    return HP_ERR_NOMEM;
                }
            }
            p = find_element_end(p, "pic");
            if (!p) {
                break;
            }
            continue;
        } else if (tbl_depth == 0 && tag_is(p, "p") && p[1] != '/') {
            const char *pend = find_element_end(p, "p");
            const char *scan;
            int para_pr_id;
            hp_align align = HP_ALIGN_JUSTIFY;
            hp_paragraph pending;
            int emitted = 0;
            if (!pend) {
                break;
            }
            memset(&pending, 0, sizeof(pending));
            para_pr_id = attr_int(p, "paraPrIDRef", 0);
            if (styles && para_pr_id >= 0 && para_pr_id < HP_MAX_STYLE &&
                styles->paras[para_pr_id].valid) {
                align = styles->paras[para_pr_id].align;
            }
            pending.align = align;
            scan = find_close_gt(p);
            if (scan) {
                scan++;
            }
            while (scan && scan < pend) {
                const char *run = find_tag_open(scan, "run");
                const char *run_end;
                const char *obj;
                if (!run || run >= pend) {
                    break;
                }
                run_end = find_element_end(run, "run");
                if (!run_end || run_end > pend) {
                    break;
                }
                obj = find_tag_open(run, "tbl");
                if (obj && obj < run_end) {
                    if (pending.run_count > 0) {
                        hp_block blk;
                        memset(&blk, 0, sizeof(blk));
                        blk.kind = HP_BLOCK_PARAGRAPH;
                        blk.paragraph = pending;
                        append_flat_para(sec, &pending);
                        append_block(sec, &blk);
                        memset(&pending, 0, sizeof(pending));
                        pending.align = align;
                        emitted = 1;
                    }
                    {
                        hp_table *table = parse_table(obj, styles);
                        if (table) {
                            hp_block blk;
                            memset(&blk, 0, sizeof(blk));
                            blk.kind = HP_BLOCK_TABLE;
                            blk.table = table;
                            append_block(sec, &blk);
                            emitted = 1;
                        }
                    }
                    scan = run_end;
                    continue;
                }
                obj = find_tag_open(run, "pic");
                if (obj && obj < run_end) {
                    if (pending.run_count > 0) {
                        hp_block blk;
                        memset(&blk, 0, sizeof(blk));
                        blk.kind = HP_BLOCK_PARAGRAPH;
                        blk.paragraph = pending;
                        append_flat_para(sec, &pending);
                        append_block(sec, &blk);
                        memset(&pending, 0, sizeof(pending));
                        pending.align = align;
                        emitted = 1;
                    }
                    {
                        hp_image *image = parse_pic(obj, styles);
                        if (image) {
                            hp_block blk;
                            memset(&blk, 0, sizeof(blk));
                            blk.kind = HP_BLOCK_IMAGE;
                            blk.image = image;
                            append_block(sec, &blk);
                            emitted = 1;
                        }
                    }
                    scan = run_end;
                    continue;
                }
                /* Text run: append into pending via temporary parse of just this run's <t> */
                {
                    int char_id = attr_int(run, "charPrIDRef", 0);
                    const char *tq = run;
                    while (tq < run_end && (tq = find_tag_open(tq, "t")) != NULL && tq < run_end) {
                        const char *t_gt = find_close_gt(tq);
                        const char *t_end;
                        const char *close;
                        char *text;
                        hp_run *runs;
                        if (!t_gt || t_gt >= run_end) {
                            break;
                        }
                        if (t_gt[-1] == '/') {
                            tq = t_gt + 1;
                            continue;
                        }
                        t_end = find_element_end(tq, "t");
                        if (!t_end || t_end > run_end) {
                            break;
                        }
                        close = t_end;
                        while (close > t_gt && !(close[0] == '<' && close[1] == '/')) {
                            close--;
                        }
                        text = clean_text_xml(t_gt + 1, (size_t)(close - (t_gt + 1)));
                        if (!text) {
                            return HP_ERR_NOMEM;
                        }
                        runs = (hp_run *)realloc(pending.runs, (pending.run_count + 1) * sizeof(hp_run));
                        if (!runs) {
                            free(text);
                            return HP_ERR_NOMEM;
                        }
                        pending.runs = runs;
                        memset(&pending.runs[pending.run_count], 0, sizeof(hp_run));
                        pending.runs[pending.run_count].text = text;
                        apply_char_style(&pending.runs[pending.run_count], styles, char_id);
                        pending.run_count++;
                        tq = t_end;
                    }
                }
                scan = run_end;
            }
            if (pending.run_count > 0) {
                hp_block blk;
                memset(&blk, 0, sizeof(blk));
                blk.kind = HP_BLOCK_PARAGRAPH;
                blk.paragraph = pending;
                append_flat_para(sec, &pending);
                append_block(sec, &blk);
                emitted = 1;
            } else if (!emitted) {
                hp_block blk;
                hp_run *runs = (hp_run *)calloc(1, sizeof(hp_run));
                memset(&blk, 0, sizeof(blk));
                blk.kind = HP_BLOCK_PARAGRAPH;
                if (runs) {
                    runs[0].text = hp_strdup("");
                    runs[0].font_size_hwp = 1000;
                    blk.paragraph.runs = runs;
                    blk.paragraph.run_count = 1;
                    blk.paragraph.align = align;
                }
                append_block(sec, &blk);
            }
            p = pend;
            continue;
        }
        p++;
    }
    return HP_OK;
}

hp_status hp_hwpx_read(const char *path, hp_document **out_doc) {
    zip_t *za;
    int zerr = 0;
    char *mimetype = NULL;
    char *hpf = NULL;
    char *header_xml = NULL;
    hp_document *doc = NULL;
    hp_status st = HP_OK;
    char *title = NULL;
    char *lang = NULL;
    hp_style_tables styles;
    int sec_i;

    style_tables_init(&styles);
    if (!path || !out_doc) {
        return HP_ERR_INVALID;
    }
    *out_doc = NULL;

    za = zip_open(path, ZIP_RDONLY, &zerr);
    if (!za) {
        return HP_ERR_IO;
    }

    mimetype = zip_read_entry(za, "mimetype");
    if (!mimetype) {
        st = HP_ERR_NOT_HWPX;
        goto done;
    }
    {
        size_t n = strlen(mimetype);
        while (n > 0 &&
               (mimetype[n - 1] == '\n' || mimetype[n - 1] == '\r' || mimetype[n - 1] == ' ')) {
            mimetype[--n] = '\0';
        }
        if (strcmp(mimetype, HP_HWPX_MIMETYPE) != 0) {
            st = HP_ERR_NOT_HWPX;
            goto done;
        }
    }

    hpf = zip_read_entry(za, "Contents/content.hpf");
    title = hpf ? extract_dc_field(hpf, "title") : hp_strdup("");
    lang = hpf ? extract_dc_field(hpf, "language") : hp_strdup("ko");
    if (!title || !lang) {
        st = HP_ERR_NOMEM;
        goto done;
    }
    if (lang[0] == '\0') {
        free(lang);
        lang = hp_strdup("ko");
    }

    header_xml = zip_read_entry(za, "Contents/header.xml");
    if (header_xml) {
        parse_header_styles(header_xml, &styles);
    }
    load_bindata(za, &styles);

    doc = hp_document_new();
    if (!doc) {
        st = HP_ERR_NOMEM;
        goto done;
    }
    free(doc->title);
    free(doc->language);
    doc->title = title;
    doc->language = lang;
    title = NULL;
    lang = NULL;

    {
        size_t i;
        for (i = 0; i < doc->sections[0].paragraph_count; i++) {
            size_t r;
            for (r = 0; r < doc->sections[0].paragraphs[i].run_count; r++) {
                free(doc->sections[0].paragraphs[i].runs[r].text);
                free(doc->sections[0].paragraphs[i].runs[r].font_face);
            }
            free(doc->sections[0].paragraphs[i].runs);
        }
        free(doc->sections[0].paragraphs);
        free(doc->sections[0].blocks);
        free(doc->sections);
        doc->sections = NULL;
        doc->section_count = 0;
    }

    for (sec_i = 0; sec_i < 64; sec_i++) {
        char name[64];
        char *section_xml;
        hp_section *secs;
        snprintf(name, sizeof(name), "Contents/section%d.xml", sec_i);
        section_xml = zip_read_entry(za, name);
        if (!section_xml) {
            if (sec_i == 0) {
                st = HP_ERR_INVALID;
                hp_document_free(doc);
                doc = NULL;
                goto done;
            }
            break;
        }
        secs = (hp_section *)realloc(doc->sections, (doc->section_count + 1) * sizeof(hp_section));
        if (!secs) {
            free(section_xml);
            st = HP_ERR_NOMEM;
            hp_document_free(doc);
            doc = NULL;
            goto done;
        }
        doc->sections = secs;
        memset(&doc->sections[doc->section_count], 0, sizeof(hp_section));
        st = parse_section_xml(section_xml, &doc->sections[doc->section_count], &styles);
        free(section_xml);
        if (st != HP_OK) {
            hp_document_free(doc);
            doc = NULL;
            goto done;
        }
        doc->section_count++;
    }

    *out_doc = doc;
    doc = NULL;

done:
    style_tables_free(&styles);
    free(mimetype);
    free(hpf);
    free(header_xml);
    free(title);
    free(lang);
    if (doc) {
        hp_document_free(doc);
    }
    zip_close(za);
    return st;
}

hp_status hp_hwpx_roundtrip(const char *input, const char *output, hp_document **out_doc) {
    hp_document *doc = NULL;
    hp_status st = hp_hwpx_read(input, &doc);
    if (st != HP_OK) {
        return st;
    }
    st = hp_hwpx_write(doc, output);
    if (st != HP_OK) {
        hp_document_free(doc);
        return st;
    }
    if (out_doc) {
        *out_doc = doc;
    } else {
        hp_document_free(doc);
    }
    return HP_OK;
}
