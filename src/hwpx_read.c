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
    int32_t left_hwp;
    int32_t right_hwp;
    int32_t intent_hwp;
    int32_t prev_hwp;
    int32_t next_hwp;
    int line_spacing_percent;
    int valid;
} hp_para_style;

typedef struct {
    int left;
    int right;
    int top;
    int bottom;
    uint32_t color_left;
    uint32_t color_right;
    uint32_t color_top;
    uint32_t color_bottom;
    int width_px;
    int valid;
} hp_border_style;

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
    hp_border_style borders[HP_MAX_FILL];
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
    /* DISTRIBUTE / JUSTIFY */
    return HP_ALIGN_JUSTIFY;
}

static int32_t hc_margin_value(const char *margin_xml, const char *local, int32_t def) {
    char key[64];
    const char *p;
    if (!margin_xml) {
        return def;
    }
    snprintf(key, sizeof(key), ":%s ", local);
    p = strstr(margin_xml, key);
    if (!p) {
        snprintf(key, sizeof(key), "<%s ", local);
        p = strstr(margin_xml, key);
    }
    if (!p) {
        snprintf(key, sizeof(key), ":%s>", local);
        p = strstr(margin_xml, key);
    }
    if (!p) {
        return def;
    }
    p = strstr(p, "value=\"");
    if (!p) {
        return def;
    }
    return (int32_t)strtol(p + 7, NULL, 10);
}

static int border_width_to_px(const char *w) {
    double mm = 0.12;
    int px;
    if (w && w[0]) {
        mm = strtod(w, NULL);
        if (mm <= 0) {
            mm = 0.12;
        }
    }
    px = (int)(mm * 96.0 / 25.4 + 0.5);
    if (px < 1) {
        px = 1;
    }
    if (px > 4) {
        px = 4;
    }
    return px;
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
            const char *margin;
            const char *ls;
            char horiz[32];
            char mbuf[512];
            st->paras[id].align = HP_ALIGN_JUSTIFY;
            st->paras[id].left_hwp = 0;
            st->paras[id].right_hwp = 0;
            st->paras[id].intent_hwp = 0;
            st->paras[id].prev_hwp = 0;
            st->paras[id].next_hwp = 0;
            st->paras[id].line_spacing_percent = 160;
            if (al && al < end && attr_str(al, "horizontal", horiz, sizeof(horiz))) {
                st->paras[id].align = parse_align_name(horiz);
            }
            /* Prefer last <hh:margin> inside paraPr (switch/case duplicates). */
            margin = NULL;
            {
                const char *q = p;
                while (q < end && (q = find_tag_open(q, "margin")) != NULL && q < end) {
                    margin = q;
                    q = find_element_end(q, "margin");
                    if (!q) {
                        break;
                    }
                }
            }
            if (margin && margin < end) {
                const char *mend = find_element_end(margin, "margin");
                size_t n = mend && mend > margin ? (size_t)(mend - margin) : 0;
                if (n >= sizeof(mbuf)) {
                    n = sizeof(mbuf) - 1;
                }
                if (n > 0) {
                    memcpy(mbuf, margin, n);
                    mbuf[n] = '\0';
                    st->paras[id].intent_hwp = hc_margin_value(mbuf, "intent", 0);
                    st->paras[id].left_hwp = hc_margin_value(mbuf, "left", 0);
                    st->paras[id].right_hwp = hc_margin_value(mbuf, "right", 0);
                    st->paras[id].prev_hwp = hc_margin_value(mbuf, "prev", 0);
                    st->paras[id].next_hwp = hc_margin_value(mbuf, "next", 0);
                }
            }
            ls = NULL;
            {
                const char *q = p;
                while (q < end && (q = find_tag_open(q, "lineSpacing")) != NULL && q < end) {
                    ls = q;
                    q = find_close_gt(q);
                    if (!q) {
                        break;
                    }
                    q++;
                }
            }
            if (ls && ls < end) {
                char typ[16];
                int val = attr_int(ls, "value", 160);
                if (attr_str(ls, "type", typ, sizeof(typ)) && strcmp(typ, "PERCENT") == 0) {
                    st->paras[id].line_spacing_percent = val > 0 ? val : 160;
                } else if (val > 0) {
                    /* ABSOLUTE HWPUNIT — approximate as percent of 1000 */
                    st->paras[id].line_spacing_percent = (val * 100) / 1000;
                    if (st->paras[id].line_spacing_percent < 100) {
                        st->paras[id].line_spacing_percent = 100;
                    }
                }
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
            char typ[16];
            char width[32];
            const char *side;
            hp_border_style *br = &st->borders[id];
            memset(br, 0, sizeof(*br));
            br->width_px = 1;
            if (win && win < end && attr_str(win, "faceColor", color, sizeof(color))) {
                if (strcmp(color, "none") != 0) {
                    st->fill_rgb[id] = parse_color(color);
                    st->fill_valid[id] = 1;
                }
            }
            side = find_tag_open(p, "leftBorder");
            if (side && side < end && attr_str(side, "type", typ, sizeof(typ)) && strcmp(typ, "NONE") != 0) {
                br->left = 1;
                attr_str(side, "color", color, sizeof(color));
                br->color_left = parse_color(color);
                if (attr_str(side, "width", width, sizeof(width))) {
                    br->width_px = border_width_to_px(width);
                }
            }
            side = find_tag_open(p, "rightBorder");
            if (side && side < end && attr_str(side, "type", typ, sizeof(typ)) && strcmp(typ, "NONE") != 0) {
                br->right = 1;
                attr_str(side, "color", color, sizeof(color));
                br->color_right = parse_color(color);
                if (attr_str(side, "width", width, sizeof(width))) {
                    br->width_px = border_width_to_px(width);
                }
            }
            side = find_tag_open(p, "topBorder");
            if (side && side < end && attr_str(side, "type", typ, sizeof(typ)) && strcmp(typ, "NONE") != 0) {
                br->top = 1;
                attr_str(side, "color", color, sizeof(color));
                br->color_top = parse_color(color);
                if (attr_str(side, "width", width, sizeof(width))) {
                    br->width_px = border_width_to_px(width);
                }
            }
            side = find_tag_open(p, "bottomBorder");
            if (side && side < end && attr_str(side, "type", typ, sizeof(typ)) && strcmp(typ, "NONE") != 0) {
                br->bottom = 1;
                attr_str(side, "color", color, sizeof(color));
                br->color_bottom = parse_color(color);
                if (attr_str(side, "width", width, sizeof(width))) {
                    br->width_px = border_width_to_px(width);
                }
            }
            if (br->left || br->right || br->top || br->bottom) {
                br->valid = 1;
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

static void apply_para_style(hp_paragraph *para, const hp_style_tables *st, int para_pr_id) {
    if (!para) {
        return;
    }
    para->align = HP_ALIGN_JUSTIFY;
    para->indent_left_hwp = 0;
    para->indent_right_hwp = 0;
    para->indent_first_hwp = 0;
    para->space_before_hwp = 0;
    para->space_after_hwp = 0;
    para->line_spacing_percent = 160;
    if (!st || para_pr_id < 0 || para_pr_id >= HP_MAX_STYLE || !st->paras[para_pr_id].valid) {
        return;
    }
    para->align = st->paras[para_pr_id].align;
    para->indent_left_hwp = st->paras[para_pr_id].left_hwp;
    para->indent_right_hwp = st->paras[para_pr_id].right_hwp;
    para->indent_first_hwp = st->paras[para_pr_id].intent_hwp;
    para->space_before_hwp = st->paras[para_pr_id].prev_hwp;
    para->space_after_hwp = st->paras[para_pr_id].next_hwp;
    para->line_spacing_percent =
        st->paras[para_pr_id].line_spacing_percent > 0 ? st->paras[para_pr_id].line_spacing_percent : 160;
}

static void parse_linesegs(const char *p_open, const char *p_end, hp_paragraph *para) {
    const char *q;
    if (!para || !p_open || !p_end) {
        return;
    }
    para->segs = NULL;
    para->seg_count = 0;
    q = p_open;
    while (q < p_end && (q = find_tag_open(q, "lineseg")) != NULL && q < p_end) {
        hp_line_seg seg;
        hp_line_seg *arr;
        const char *gt = find_close_gt(q);
        memset(&seg, 0, sizeof(seg));
        seg.textpos = (uint32_t)attr_int(q, "textpos", 0);
        seg.vertpos_hwp = (uint32_t)attr_int(q, "vertpos", 0);
        seg.vertsize_hwp = (uint32_t)attr_int(q, "vertsize", 0);
        seg.textheight_hwp = (uint32_t)attr_int(q, "textheight", 0);
        seg.spacing_hwp = (uint32_t)attr_int(q, "spacing", 0);
        seg.horzpos_hwp = (uint32_t)attr_int(q, "horzpos", 0);
        seg.horzsize_hwp = (uint32_t)attr_int(q, "horzsize", 0);
        arr = (hp_line_seg *)realloc(para->segs, (para->seg_count + 1) * sizeof(hp_line_seg));
        if (!arr) {
            break;
        }
        para->segs = arr;
        para->segs[para->seg_count++] = seg;
        q = gt ? gt + 1 : q + 8;
    }
}

static void apply_border_style(hp_table_cell *cell, const hp_style_tables *st, int fill_id) {
    if (!cell || !st || fill_id < 0 || fill_id >= HP_MAX_FILL || !st->borders[fill_id].valid) {
        return;
    }
    cell->borders.left = st->borders[fill_id].left;
    cell->borders.right = st->borders[fill_id].right;
    cell->borders.top = st->borders[fill_id].top;
    cell->borders.bottom = st->borders[fill_id].bottom;
    cell->borders.color_left = st->borders[fill_id].color_left;
    cell->borders.color_right = st->borders[fill_id].color_right;
    cell->borders.color_top = st->borders[fill_id].color_top;
    cell->borders.color_bottom = st->borders[fill_id].color_bottom;
    cell->borders.width_px = st->borders[fill_id].width_px > 0 ? st->borders[fill_id].width_px : 1;
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
    sec->paragraphs[sec->paragraph_count].align = src->align;
    sec->paragraphs[sec->paragraph_count].indent_left_hwp = src->indent_left_hwp;
    sec->paragraphs[sec->paragraph_count].indent_right_hwp = src->indent_right_hwp;
    sec->paragraphs[sec->paragraph_count].indent_first_hwp = src->indent_first_hwp;
    sec->paragraphs[sec->paragraph_count].space_before_hwp = src->space_before_hwp;
    sec->paragraphs[sec->paragraph_count].space_after_hwp = src->space_after_hwp;
    sec->paragraphs[sec->paragraph_count].line_spacing_percent = src->line_spacing_percent;
    if (src->seg_count > 0 && src->segs) {
        sec->paragraphs[sec->paragraph_count].segs =
            (hp_line_seg *)malloc(src->seg_count * sizeof(hp_line_seg));
        if (sec->paragraphs[sec->paragraph_count].segs) {
            memcpy(sec->paragraphs[sec->paragraph_count].segs, src->segs,
                   src->seg_count * sizeof(hp_line_seg));
            sec->paragraphs[sec->paragraph_count].seg_count = src->seg_count;
        }
    }
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
    apply_para_style(out, styles, para_pr_id);
    parse_linesegs(p_open, p_end, out);
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

static hp_text_wrap parse_wrap(const char *s) {
    if (!s) {
        return HP_WRAP_TOP_AND_BOTTOM;
    }
    if (strcmp(s, "SQUARE") == 0) {
        return HP_WRAP_SQUARE;
    }
    if (strcmp(s, "BEHIND_TEXT") == 0) {
        return HP_WRAP_BEHIND;
    }
    if (strcmp(s, "IN_FRONT_OF_TEXT") == 0) {
        return HP_WRAP_IN_FRONT;
    }
    return HP_WRAP_TOP_AND_BOTTOM;
}

static uint32_t attr_u32(const char *tag_open, const char *name, uint32_t def) {
    char key[64];
    const char *p;
    const char *gt;
    size_t n = strlen(name);
    if (n + 3 >= sizeof(key)) {
        return def;
    }
    gt = find_close_gt(tag_open);
    if (!gt) {
        return def;
    }
    snprintf(key, sizeof(key), "%s=\"", name);
    p = strstr(tag_open, key);
    if (!p || p > gt) {
        return def;
    }
    return (uint32_t)strtoul(p + n + 2, NULL, 10);
}

static void parse_float_pos(const char *elem_open, const char *elem_end, const char *wrap_attr_src,
                            hp_float_pos *pos, int *has_pos) {
    const char *p;
    char wrap[32];
    char vrel[32];
    char hrel[32];
    memset(pos, 0, sizeof(*pos));
    pos->treat_as_char = 1;
    pos->wrap = HP_WRAP_TOP_AND_BOTTOM;
    pos->vert_rel_para = 1;
    pos->horz_rel_para = 1;
    *has_pos = 0;
    if (wrap_attr_src && attr_str(wrap_attr_src, "textWrap", wrap, sizeof(wrap))) {
        pos->wrap = parse_wrap(wrap);
        *has_pos = 1;
    }
    p = find_tag_open(elem_open, "pos");
    if (!p || p >= elem_end) {
        return;
    }
    *has_pos = 1;
    pos->treat_as_char = attr_int(p, "treatAsChar", 1);
    /* Offsets are HWPUNIT bit patterns — interpret as signed int32. */
    pos->vert_offset_hwp = hp_hwp_signed(attr_u32(p, "vertOffset", 0));
    pos->horz_offset_hwp = hp_hwp_signed(attr_u32(p, "horzOffset", 0));
    if (attr_str(p, "vertRelTo", vrel, sizeof(vrel))) {
        pos->vert_rel_para = (strcmp(vrel, "PARA") == 0);
    }
    if (attr_str(p, "horzRelTo", hrel, sizeof(hrel))) {
        pos->horz_rel_para = (strcmp(hrel, "PARA") == 0);
    }
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
    parse_float_pos(pic_open, end, pic_open, &image->pos, &image->has_pos);
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

static hp_equation *parse_equation(const char *eq_open) {
    const char *end = find_element_end(eq_open, "equation");
    const char *script;
    const char *gt;
    hp_equation *eq;
    if (!end) {
        /* try alternate tag names */
        end = find_element_end(eq_open, "script");
    }
    if (!end) {
        return NULL;
    }
    eq = (hp_equation *)calloc(1, sizeof(*eq));
    if (!eq) {
        return NULL;
    }
    eq->font_size_hwp = 1200;
    script = find_tag_open(eq_open, "script");
    if (script && script < end) {
        gt = find_close_gt(script);
        if (gt && gt[-1] != '/') {
            const char *close = find_element_end(script, "script");
            const char *c = close ? close : end;
            while (c > gt && !(c[0] == '<' && c[1] == '/')) {
                c--;
            }
            eq->script = clean_text_xml(gt + 1, (size_t)(c - (gt + 1)));
        }
    }
    if (!eq->script) {
        /* fallback: any text inside */
        gt = find_close_gt(eq_open);
        if (gt) {
            const char *c = end;
            while (c > gt && !(c[0] == '<' && c[1] == '/')) {
                c--;
            }
            eq->script = clean_text_xml(gt + 1, (size_t)(c - (gt + 1)));
        }
    }
    if (!eq->script || !eq->script[0]) {
        free(eq->script);
        eq->script = hp_strdup("[수식]");
    }
    return eq;
}

static hp_container *parse_container(const char *c_open, const hp_style_tables *styles) {
    const char *end = find_element_end(c_open, "container");
    const char *sz;
    const char *pic;
    hp_container *c;
    if (!end) {
        return NULL;
    }
    c = (hp_container *)calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    sz = find_tag_open(c_open, "sz");
    if (sz && sz < end) {
        c->width_hwp = (uint32_t)attr_int(sz, "width", 0);
        c->height_hwp = (uint32_t)attr_int(sz, "height", 0);
    }
    parse_float_pos(c_open, end, c_open, &c->pos, &c->has_pos);
    pic = c_open;
    while (pic < end && (pic = find_tag_open(pic, "pic")) != NULL && pic < end) {
        hp_image *im = parse_pic(pic, styles);
        const char *pend = find_element_end(pic, "pic");
        if (im) {
            hp_image **arr =
                (hp_image **)realloc(c->images, (c->image_count + 1) * sizeof(hp_image *));
            if (arr) {
                c->images = arr;
                c->images[c->image_count++] = im;
            } else {
                free(im->binary_id);
                free(im->data);
                free(im);
            }
        }
        if (!pend) {
            break;
        }
        pic = pend;
    }
    if (c->image_count == 0) {
        free(c);
        return NULL;
    }
    return c;
}

static hp_table *parse_table(const char *tbl_open, const hp_style_tables *styles);

static int cell_append_nested(hp_table_cell *cell, hp_table *nested) {
    hp_table **arr;
    if (!nested) {
        return 0;
    }
    arr = (hp_table **)realloc(cell->nested_tables,
                               (cell->nested_table_count + 1) * sizeof(hp_table *));
    if (!arr) {
        return -1;
    }
    cell->nested_tables = arr;
    cell->nested_tables[cell->nested_table_count++] = nested;
    return 0;
}

/* OWPML nests objects as tc/subList/p/run/{tbl,pic,container,equation}. */
static hp_status cell_harvest_objects(const char *from, const char *until, const hp_style_tables *styles,
                                      hp_table_cell *cell) {
    const char *inner = from;
    while (inner && inner < until) {
        const char *itbl = find_tag_open(inner, "tbl");
        const char *ipic = find_tag_open(inner, "pic");
        const char *icont = find_tag_open(inner, "container");
        const char *ieq = find_tag_open(inner, "equation");
        const char *inext = NULL;
        int ik = 0; /* 1 tbl 2 pic 3 container 4 equation */
        if (itbl && itbl < until) {
            inext = itbl;
            ik = 1;
        }
        if (ipic && ipic < until && (!inext || ipic < inext)) {
            inext = ipic;
            ik = 2;
        }
        if (icont && icont < until && (!inext || icont < inext)) {
            inext = icont;
            ik = 3;
        }
        if (ieq && ieq < until && (!inext || ieq < inext)) {
            inext = ieq;
            ik = 4;
        }
        if (!inext) {
            break;
        }
        if (ik == 1) {
            hp_table *nested = parse_table(inext, styles);
            const char *tend = find_element_end(inext, "tbl");
            if (cell_append_nested(cell, nested) != 0) {
                return HP_ERR_NOMEM;
            }
            inner = tend ? tend : inext + 4;
        } else if (ik == 2) {
            /* Skip pics that belong to a container we already/will parse as a unit */
            const char *pend = find_element_end(inext, "pic");
            if (!cell->container && !cell->image) {
                cell->image = parse_pic(inext, styles);
            }
            inner = pend ? pend : inext + 4;
        } else if (ik == 3) {
            const char *cend = find_element_end(inext, "container");
            if (!cell->container) {
                cell->container = parse_container(inext, styles);
            }
            /* Advance past whole container so child pics are not re-harvested */
            inner = cend ? cend : inext + 4;
        } else {
            const char *eend = find_element_end(inext, "equation");
            if (!cell->equation) {
                cell->equation = parse_equation(inext);
            }
            inner = eend ? eend : inext + 4;
        }
    }
    return HP_OK;
}

static hp_status parse_cell_content(const char *tc_open, const char *tc_end, const hp_style_tables *styles,
                                    hp_table_cell *cell) {
    const char *scan = find_close_gt(tc_open);
    if (scan) {
        scan++;
    }
    /* Walk in document order. Nested tbl/pic usually sit inside p/run, so p wins the
     * earliest-tag race — harvest objects from inside each paragraph. */
    while (scan && scan < tc_end) {
        const char *tbl = find_tag_open(scan, "tbl");
        const char *pic = find_tag_open(scan, "pic");
        const char *p = find_tag_open(scan, "p");
        const char *next = NULL;
        int kind = 0; /* 1 tbl 2 pic 3 p */

        if (tbl && tbl < tc_end) {
            next = tbl;
            kind = 1;
        }
        if (pic && pic < tc_end && (!next || pic < next)) {
            next = pic;
            kind = 2;
        }
        if (p && p < tc_end && (!next || p < next)) {
            next = p;
            kind = 3;
        }
        if (!next) {
            break;
        }

        if (kind == 1) {
            hp_table *nested = parse_table(next, styles);
            const char *tend = find_element_end(next, "tbl");
            if (cell_append_nested(cell, nested) != 0) {
                return HP_ERR_NOMEM;
            }
            scan = tend ? tend : next + 4;
            continue;
        }
        if (kind == 2) {
            if (!cell->image) {
                cell->image = parse_pic(next, styles);
            }
            {
                const char *pend = find_element_end(next, "pic");
                scan = pend ? pend : next + 4;
            }
            continue;
        }
        /* paragraph: pull nested tbl/pic out of p/run, then keep any text runs */
        {
            const char *pend = find_element_end(next, "p");
            hp_paragraph para;
            hp_paragraph *arr;
            if (!pend || pend > tc_end) {
                break;
            }
            if (cell_harvest_objects(next, pend, styles, cell) != HP_OK) {
                return HP_ERR_NOMEM;
            }
            if (parse_text_paragraph(next, pend, styles, &para) != HP_OK) {
                return HP_ERR_NOMEM;
            }
            if (para.run_count > 0) {
                arr = (hp_paragraph *)realloc(cell->paragraphs,
                                              (cell->paragraph_count + 1) * sizeof(*arr));
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
            } else {
                size_t i;
                for (i = 0; i < para.run_count; i++) {
                    free(para.runs[i].text);
                    free(para.runs[i].font_face);
                }
                free(para.runs);
            }
            scan = pend;
        }
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
    parse_float_pos(tbl_open, end, tbl_open, &table->pos, &table->has_pos);
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
            apply_border_style(&cell, styles, fill_id);
            {
                const char *cm = find_tag_open(tc, "cellMargin");
                if (cm && cm < tc_end) {
                    cell.pad_left_hwp = (uint32_t)attr_int(cm, "left", 140);
                    cell.pad_right_hwp = (uint32_t)attr_int(cm, "right", 140);
                    cell.pad_top_hwp = (uint32_t)attr_int(cm, "top", 140);
                    cell.pad_bottom_hwp = (uint32_t)attr_int(cm, "bottom", 140);
                } else {
                    cell.pad_left_hwp = cell.pad_right_hwp = 140;
                    cell.pad_top_hwp = cell.pad_bottom_hwp = 140;
                }
            }
            parse_cell_content(tc, tc_end, styles, &cell);
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

    /* page margins + 다단 */
    {
        const char *pg = find_tag_open(xml, "pagePr");
        const char *col = find_tag_open(xml, "colPr");
        if (pg) {
            const char *pend = find_element_end(pg, "pagePr");
            const char *mg;
            sec->page.width = (uint32_t)attr_int(pg, "width", (int)sec->page.width);
            sec->page.height = (uint32_t)attr_int(pg, "height", (int)sec->page.height);
            mg = find_tag_open(pg, "margin");
            if (mg && (!pend || mg < pend)) {
                sec->page.margin_left = (uint32_t)attr_int(mg, "left", (int)sec->page.margin_left);
                sec->page.margin_right = (uint32_t)attr_int(mg, "right", (int)sec->page.margin_right);
                sec->page.margin_top = (uint32_t)attr_int(mg, "top", (int)sec->page.margin_top);
                sec->page.margin_bottom = (uint32_t)attr_int(mg, "bottom", (int)sec->page.margin_bottom);
            }
        }
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
        } else if (tbl_depth == 0 && tag_is(p, "container") && p[1] != '/') {
            hp_container *c = parse_container(p, styles);
            if (c) {
                hp_block blk;
                memset(&blk, 0, sizeof(blk));
                blk.kind = HP_BLOCK_CONTAINER;
                blk.container = c;
                if (append_block(sec, &blk) != HP_OK) {
                    return HP_ERR_NOMEM;
                }
            }
            p = find_element_end(p, "container");
            if (!p) {
                break;
            }
            continue;
        } else if (tbl_depth == 0 && tag_is(p, "equation") && p[1] != '/') {
            hp_equation *eq = parse_equation(p);
            if (eq) {
                hp_block blk;
                memset(&blk, 0, sizeof(blk));
                blk.kind = HP_BLOCK_EQUATION;
                blk.equation = eq;
                if (append_block(sec, &blk) != HP_OK) {
                    return HP_ERR_NOMEM;
                }
            }
            p = find_element_end(p, "equation");
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
            apply_para_style(&pending, styles, para_pr_id);
            parse_linesegs(p, pend, &pending);
            align = pending.align;
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
                        apply_para_style(&pending, styles, para_pr_id);
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
                        apply_para_style(&pending, styles, para_pr_id);
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
                obj = find_tag_open(run, "container");
                if (obj && obj < run_end) {
                    if (pending.run_count > 0) {
                        hp_block blk;
                        memset(&blk, 0, sizeof(blk));
                        blk.kind = HP_BLOCK_PARAGRAPH;
                        blk.paragraph = pending;
                        append_flat_para(sec, &pending);
                        append_block(sec, &blk);
                        memset(&pending, 0, sizeof(pending));
                        apply_para_style(&pending, styles, para_pr_id);
                        emitted = 1;
                    }
                    {
                        hp_container *c = parse_container(obj, styles);
                        if (c) {
                            hp_block blk;
                            memset(&blk, 0, sizeof(blk));
                            blk.kind = HP_BLOCK_CONTAINER;
                            blk.container = c;
                            append_block(sec, &blk);
                            emitted = 1;
                        }
                    }
                    scan = run_end;
                    continue;
                }
                obj = find_tag_open(run, "equation");
                if (obj && obj < run_end) {
                    if (pending.run_count > 0) {
                        hp_block blk;
                        memset(&blk, 0, sizeof(blk));
                        blk.kind = HP_BLOCK_PARAGRAPH;
                        blk.paragraph = pending;
                        append_flat_para(sec, &pending);
                        append_block(sec, &blk);
                        memset(&pending, 0, sizeof(pending));
                        apply_para_style(&pending, styles, para_pr_id);
                        emitted = 1;
                    }
                    {
                        hp_equation *eq = parse_equation(obj);
                        if (eq) {
                            hp_block blk;
                            memset(&blk, 0, sizeof(blk));
                            blk.kind = HP_BLOCK_EQUATION;
                            blk.equation = eq;
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
