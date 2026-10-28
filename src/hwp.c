/* HWP 5.x reader.
 *
 * An HWP 5.x file is a Compound File (cfb.c) whose streams hold binary records:
 *   FileHeader            signature, version, flags (compressed / password / distributed)
 *   DocInfo               fonts, character / paragraph shapes, border fills, BinData list
 *   BodyText/Section<n>   paragraphs: text, char-shape runs, saved line layout, controls
 *   BinData/BIN<hex>.ext  pictures
 * DocInfo and BodyText are raw-deflate compressed when the header says so.
 *
 * Rather than building the IR a second way, the records are translated into the
 * equivalent OWPML (HWPX) parts in memory — Contents/header.xml, section XML,
 * content.hpf, BinData — and handed to the HWPX reader (hp_hwpx_read_package).
 * HWP and HWPX are two encodings of the same document model (Hancom maps records
 * to elements one to one), so the same document read from either file yields the
 * same IR, and every layout rule in the reader and viewer applies to both.
 *
 * Record layouts follow Hancom's public "HWP 5.0 file format" document; where the
 * document is ambiguous the values were checked against Hancom-saved HWP/HWPX pairs.
 * Every read is bounds checked; damaged files fail with HP_ERR_INVALID. */
#include "hp/hwp.h"
#include "hp/hwpx.h"
#include "hp/version.h"

#include "aes128.h"
#include "cfb.h"
#include "hwpx_package.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

const unsigned char HP_CFB_MAGIC[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};

#define HWP_MAX_FILE ((size_t)512 * 1024 * 1024)
#define HWP_MAX_STREAM ((size_t)512 * 1024 * 1024)
#define HWP_MAX_DEPTH 48
#define TAG_DISTRIBUTE_DOC_DATA 0x1C

/* record tags (HWPTAG_BEGIN = 0x10) */
enum {
    TAG_ID_MAPPINGS = 0x11,
    TAG_BIN_DATA = 0x12,
    TAG_FACE_NAME = 0x13,
    TAG_BORDER_FILL = 0x14,
    TAG_CHAR_SHAPE = 0x15,
    TAG_PARA_SHAPE = 0x19,
    TAG_PARA_HEADER = 0x42,
    TAG_PARA_TEXT = 0x43,
    TAG_PARA_CHAR_SHAPE = 0x44,
    TAG_PARA_LINE_SEG = 0x45,
    TAG_CTRL_HEADER = 0x47,
    TAG_LIST_HEADER = 0x48,
    TAG_PAGE_DEF = 0x49,
    TAG_SHAPE_COMPONENT = 0x4C,
    TAG_TABLE = 0x4D,
    TAG_SC_PICTURE = 0x55,
    TAG_EQEDIT = 0x58
};

#define CHID(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/* ---------- byte reading ---------- */

typedef struct {
    const unsigned char *p;
    size_t n, i;
    int bad;
} rd;

static rd rd_make(const unsigned char *p, size_t n) {
    rd r;
    r.p = p;
    r.n = n;
    r.i = 0;
    r.bad = 0;
    return r;
}

static int rd_have(rd *r, size_t k) {
    if (r->i > r->n || r->n - r->i < k) {
        r->bad = 1;
        r->i = r->n;
        return 0;
    }
    return 1;
}

static unsigned r8(rd *r) {
    if (!rd_have(r, 1)) {
        return 0;
    }
    return r->p[r->i++];
}

static unsigned r16(rd *r) {
    unsigned v;
    if (!rd_have(r, 2)) {
        return 0;
    }
    v = (unsigned)r->p[r->i] | ((unsigned)r->p[r->i + 1] << 8);
    r->i += 2;
    return v;
}

static uint32_t r32(rd *r) {
    uint32_t v;
    if (!rd_have(r, 4)) {
        return 0;
    }
    v = (uint32_t)r->p[r->i] | ((uint32_t)r->p[r->i + 1] << 8) | ((uint32_t)r->p[r->i + 2] << 16) |
        ((uint32_t)r->p[r->i + 3] << 24);
    r->i += 4;
    return v;
}

static void rskip(rd *r, size_t k) {
    if (rd_have(r, k)) {
        r->i += k;
    }
}

static int rleft(const rd *r) {
    return r->i < r->n;
}

static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---------- XML output buffer ---------- */

typedef struct {
    char *s;
    size_t len, cap;
    int oom;
} xb;

static void xb_putn(xb *b, const char *s, size_t n) {
    if (b->oom) {
        return;
    }
    if (n > HWP_MAX_STREAM || b->len > HWP_MAX_STREAM - n) {
        b->oom = 1;
        return;
    }
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        char *g;
        while (b->len + n + 1 > cap) {
            cap *= 2;
        }
        g = (char *)realloc(b->s, cap);
        if (!g) {
            b->oom = 1;
            return;
        }
        b->s = g;
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}

static void xs(xb *b, const char *s) {
    xb_putn(b, s, strlen(s));
}

static void xf(xb *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) {
        b->oom = 1;
        return;
    }
    if ((size_t)n >= sizeof(tmp)) {
        char *big = (char *)malloc((size_t)n + 1);
        if (!big) {
            b->oom = 1;
            return;
        }
        va_start(ap, fmt);
        vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        xb_putn(b, big, (size_t)n);
        free(big);
        return;
    }
    xb_putn(b, tmp, (size_t)n);
}

/* One Unicode scalar as escaped UTF-8 (XML 1.0 cannot carry most C0 controls). */
static void x_char(xb *b, uint32_t cp) {
    char u[4];
    if (cp == '&') {
        xs(b, "&amp;");
        return;
    }
    if (cp == '<') {
        xs(b, "&lt;");
        return;
    }
    if (cp == '>') {
        xs(b, "&gt;");
        return;
    }
    if (cp == '"') {
        xs(b, "&quot;");
        return;
    }
    if ((cp < 0x20 && cp != '\t' && cp != '\n' && cp != '\r') || (cp >= 0xD800 && cp <= 0xDFFF) || cp == 0xFFFE ||
        cp == 0xFFFF || cp > 0x10FFFF) {
        cp = 0xFFFD;
    }
    if (cp < 0x80) {
        u[0] = (char)cp;
        xb_putn(b, u, 1);
    } else if (cp < 0x800) {
        u[0] = (char)(0xC0 | (cp >> 6));
        u[1] = (char)(0x80 | (cp & 0x3F));
        xb_putn(b, u, 2);
    } else if (cp < 0x10000) {
        u[0] = (char)(0xE0 | (cp >> 12));
        u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[2] = (char)(0x80 | (cp & 0x3F));
        xb_putn(b, u, 3);
    } else {
        u[0] = (char)(0xF0 | (cp >> 18));
        u[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        u[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[3] = (char)(0x80 | (cp & 0x3F));
        xb_putn(b, u, 4);
    }
}

/* UTF-16LE code units (count units) as escaped UTF-8; stops at NUL. */
static void x_utf16(xb *b, const unsigned char *p, size_t units) {
    size_t i;
    for (i = 0; i < units; i++) {
        uint32_t c = (uint32_t)p[i * 2] | ((uint32_t)p[i * 2 + 1] << 8);
        if (c == 0) {
            break;
        }
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < units) {
            uint32_t d = (uint32_t)p[i * 2 + 2] | ((uint32_t)p[i * 2 + 3] << 8);
            if (d >= 0xDC00 && d <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
                i++;
            }
        }
        x_char(b, c);
    }
}

/* HWP string: WORD length + WCHAR[length], as an escaped attribute value. */
static void x_hwpstr(xb *b, rd *r) {
    unsigned n = r16(r);
    if (!rd_have(r, (size_t)n * 2)) {
        return;
    }
    x_utf16(b, r->p + r->i, n);
    r->i += (size_t)n * 2;
}

static void skip_hwpstr(rd *r) {
    unsigned n = r16(r);
    rskip(r, (size_t)n * 2);
}

/* COLORREF 0x00BBGGRR → "#RRGGBB" (0xFFFFFFFF = none). */
static void x_color(xb *b, uint32_t c) {
    if (c == 0xFFFFFFFFu) {
        xs(b, "none");
        return;
    }
    if (c >> 24) {
        xf(b, "#%X%02X%02X%02X", (unsigned)(c >> 24), (unsigned)(c & 0xFF), (unsigned)((c >> 8) & 0xFF),
           (unsigned)((c >> 16) & 0xFF));
    } else {
        xf(b, "#%02X%02X%02X", (unsigned)(c & 0xFF), (unsigned)((c >> 8) & 0xFF), (unsigned)((c >> 16) & 0xFF));
    }
}

/* ---------- records ---------- */

typedef struct {
    unsigned tag, level;
    const unsigned char *data;
    size_t size;
    size_t end; /* index after this record's subtree */
} rec;

typedef struct {
    rec *r;
    size_t n;
} recs;

static hp_status parse_records(const unsigned char *d, size_t len, recs *out) {
    size_t i = 0, cap = 0, *stack = NULL, depth = 0, k;
    out->r = NULL;
    out->n = 0;
    while (i + 4 <= len) {
        uint32_t h = le32(d + i);
        size_t size = h >> 20;
        rec *g;
        i += 4;
        if (size == 0xFFF) {
            if (i + 4 > len) {
                break;
            }
            size = le32(d + i);
            i += 4;
        }
        if (size > len - i) {
            free(out->r);
            out->r = NULL;
            return HP_ERR_INVALID;
        }
        if (out->n == cap) {
            cap = cap ? cap * 2 : 256;
            g = (rec *)realloc(out->r, cap * sizeof(rec));
            if (!g) {
                free(out->r);
                out->r = NULL;
                return HP_ERR_NOMEM;
            }
            out->r = g;
        }
        out->r[out->n].tag = h & 0x3FF;
        out->r[out->n].level = (h >> 10) & 0x3FF;
        out->r[out->n].data = d + i;
        out->r[out->n].size = size;
        out->r[out->n].end = 0;
        out->n++;
        i += size;
    }
    /* Subtree ends: a record's subtree holds the following records with a deeper level. */
    stack = (size_t *)malloc((out->n + 1) * sizeof(size_t));
    if (!stack) {
        free(out->r);
        out->r = NULL;
        return HP_ERR_NOMEM;
    }
    for (k = 0; k < out->n; k++) {
        while (depth && out->r[stack[depth - 1]].level >= out->r[k].level) {
            out->r[stack[--depth]].end = k;
        }
        stack[depth++] = k;
    }
    while (depth) {
        out->r[stack[--depth]].end = out->n;
    }
    free(stack);
    return HP_OK;
}

/* raw deflate (no zlib header), as used by HWP streams */
static hp_status inflate_raw(const unsigned char *in, size_t in_len, unsigned char **out, size_t *out_len) {
    z_stream zs;
    size_t cap = in_len * 4 + 1024, len = 0;
    unsigned char *buf;
    int rc;
    *out = NULL;
    *out_len = 0;
    if (cap > HWP_MAX_STREAM) {
        cap = HWP_MAX_STREAM;
    }
    buf = (unsigned char *)malloc(cap);
    if (!buf) {
        return HP_ERR_NOMEM;
    }
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, -15) != Z_OK) {
        free(buf);
        return HP_ERR_NOMEM;
    }
    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)(in_len > 0x7FFFFFFF ? 0x7FFFFFFF : in_len);
    for (;;) {
        zs.next_out = buf + len;
        zs.avail_out = (uInt)((cap - len) > 0x7FFFFFFF ? 0x7FFFFFFF : (cap - len));
        rc = inflate(&zs, Z_NO_FLUSH);
        len = (size_t)((unsigned char *)zs.next_out - buf);
        if (rc == Z_STREAM_END) {
            break;
        }
        if (rc != Z_OK && rc != Z_BUF_ERROR) {
            inflateEnd(&zs);
            free(buf);
            return HP_ERR_INVALID;
        }
        if (zs.avail_out == 0) {
            unsigned char *g;
            if (cap >= HWP_MAX_STREAM) {
                inflateEnd(&zs);
                free(buf);
                return HP_ERR_INVALID;
            }
            cap = cap * 2 > HWP_MAX_STREAM ? HWP_MAX_STREAM : cap * 2;
            g = (unsigned char *)realloc(buf, cap);
            if (!g) {
                inflateEnd(&zs);
                free(buf);
                return HP_ERR_NOMEM;
            }
            buf = g;
        } else if (zs.avail_in == 0) {
            break; /* truncated stream: keep what was decoded */
        }
    }
    inflateEnd(&zs);
    *out = buf;
    *out_len = len;
    return HP_OK;
}

/* ---------- converter state ---------- */

typedef struct {
    unsigned type, compression;
    unsigned id;
    char ext[16];
} bin_entry;

typedef struct {
    hp_cfb *cfb;
    uint32_t version; /* 0xMMnnPPrr */
    int compressed;
    bin_entry *bins;
    size_t bin_count;
    int bf_shift; /* 1 when a default border fill is inserted as id 1 */
    int depth;
    xb *out;
    hp_status error;
    unsigned section_index;
} ctx;

static int ver_at_least(const ctx *c, unsigned a, unsigned b, unsigned d, unsigned e) {
    return c->version >= (((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)d << 8) | e);
}

static unsigned bf_ref(const ctx *c, unsigned id) {
    return id + (unsigned)c->bf_shift;
}

static const char *const LINE_TYPE[] = {"NONE", "SOLID", "DOT", "DASH", "DASH_DOT", "DASH_DOT_DOT", "LONG_DASH",
                                        "CIRCLE", "DOUBLE_SLIM", "SLIM_THICK", "THICK_SLIM", "SLIM_THICK_SLIM",
                                        "WAVE", "DOUBLEWAVE", "THICK_3D", "THICK_3D_REVERSE", "3D", "3D_REVERSE"};
static const char *const LINE_WIDTH[] = {"0.1 mm", "0.12 mm", "0.15 mm", "0.2 mm", "0.25 mm", "0.3 mm",
                                         "0.4 mm", "0.5 mm", "0.6 mm", "0.7 mm", "1.0 mm", "1.5 mm",
                                         "2.0 mm", "3.0 mm", "4.0 mm", "5.0 mm"};
/* char-shape underline / strikeout shapes (BorderType2) */
static const char *const LINE_TYPE2[] = {"SOLID", "DASH", "DOT", "DASH_DOT", "DASH_DOT_DOT", "LONG_DASH", "CIRCLE",
                                         "DOUBLE_SLIM", "SLIM_THICK", "THICK_SLIM", "SLIM_THICK_SLIM", "WAVE",
                                         "DOUBLEWAVE"};
static const char *const LANGS[7] = {"HANGUL", "LATIN", "HANJA", "JAPANESE", "OTHER", "SYMBOL", "USER"};
static const char *const LANG_ATTR[7] = {"hangul", "latin", "hanja", "japanese", "other", "symbol", "user"};

#define PICK(arr, i) ((i) < sizeof(arr) / sizeof((arr)[0]) ? (arr)[i] : (arr)[0])

/* ---------- header.xml (DocInfo) ---------- */

static void emit_border(xb *b, const char *name, rd *r) {
    unsigned type = r8(r), width = r8(r);
    uint32_t color = r32(r);
    xf(b, "<hh:%s type=\"%s\" width=\"%s\" color=\"", name, PICK(LINE_TYPE, type), PICK(LINE_WIDTH, width & 0x0F));
    x_color(b, color);
    xs(b, "\"/>");
}

static void emit_border_fill(xb *b, unsigned id, const rec *rc) {
    rd r = rd_make(rc->data, rc->size);
    unsigned props = r16(&r);
    uint32_t fill;
    xf(b, "<hh:borderFill id=\"%u\" threeD=\"%d\" shadow=\"%d\" centerLine=\"NONE\" breakCellSeparateLine=\"0\">", id,
       (int)(props & 1), (int)((props >> 1) & 1));
    xs(b, "<hh:slash type=\"NONE\" Crooked=\"0\" isCounter=\"0\"/><hh:backSlash type=\"NONE\" Crooked=\"0\" isCounter=\"0\"/>");
    emit_border(b, "leftBorder", &r);
    emit_border(b, "rightBorder", &r);
    emit_border(b, "topBorder", &r);
    emit_border(b, "bottomBorder", &r);
    emit_border(b, "diagonal", &r);
    fill = r32(&r);
    if (fill & 1) {
        uint32_t back = r32(&r), pattern = r32(&r);
        int hatch = (int)r32(&r);
        xs(b, "<hc:fillBrush><hc:winBrush faceColor=\"");
        x_color(b, back);
        xs(b, "\" hatchColor=\"");
        x_color(b, pattern);
        xs(b, "\" alpha=\"0\"");
        if (hatch >= 0 && hatch <= 5) {
            static const char *const hatches[] = {"HORIZONTAL", "VERTICAL", "BACK_SLASH", "SLASH", "CROSS", "CROSS_DIAGONAL"};
            xf(b, " hatchStyle=\"%s\"", hatches[hatch]);
        }
        xs(b, "/></hc:fillBrush>");
    }
    xs(b, "</hh:borderFill>");
}

static void emit_char_shape(const ctx *c, xb *b, unsigned id, const rec *rc) {
    rd r = rd_make(rc->data, rc->size);
    unsigned faces[7], ratio[7], relsz[7], i;
    int spacing[7], offset[7];
    int32_t height;
    uint32_t props, text_color, underline_color, shade_color, strike_color = 0;
    unsigned border_fill = 0;
    int shadow_x, shadow_y;
    for (i = 0; i < 7; i++) {
        faces[i] = r16(&r);
    }
    for (i = 0; i < 7; i++) {
        ratio[i] = r8(&r);
    }
    for (i = 0; i < 7; i++) {
        spacing[i] = (signed char)r8(&r);
    }
    for (i = 0; i < 7; i++) {
        relsz[i] = r8(&r);
    }
    for (i = 0; i < 7; i++) {
        offset[i] = (signed char)r8(&r);
    }
    height = (int32_t)r32(&r);
    props = r32(&r);
    shadow_x = (signed char)r8(&r);
    shadow_y = (signed char)r8(&r);
    text_color = r32(&r);
    underline_color = r32(&r);
    shade_color = r32(&r);
    (void)r32(&r); /* shadow color */
    if (rleft(&r) && ver_at_least(c, 5, 0, 2, 1)) {
        border_fill = r16(&r);
    }
    if (rleft(&r) && ver_at_least(c, 5, 0, 3, 0)) {
        strike_color = r32(&r);
    }
    xf(b, "<hh:charPr id=\"%u\" height=\"%d\" textColor=\"", id, (int)height);
    x_color(b, text_color);
    xs(b, "\" shadeColor=\"");
    x_color(b, shade_color);
    xf(b, "\" useFontSpace=\"%d\" useKerning=\"%d\" symMark=\"NONE\" borderFillIDRef=\"%u\">", (int)((props >> 25) & 1),
       (int)((props >> 30) & 1), bf_ref(c, border_fill));
    xs(b, "<hh:fontRef");
    for (i = 0; i < 7; i++) {
        xf(b, " %s=\"%u\"", LANG_ATTR[i], faces[i]);
    }
    xs(b, "/><hh:ratio");
    for (i = 0; i < 7; i++) {
        xf(b, " %s=\"%u\"", LANG_ATTR[i], ratio[i]);
    }
    xs(b, "/><hh:spacing");
    for (i = 0; i < 7; i++) {
        xf(b, " %s=\"%d\"", LANG_ATTR[i], spacing[i]);
    }
    xs(b, "/><hh:relSz");
    for (i = 0; i < 7; i++) {
        xf(b, " %s=\"%u\"", LANG_ATTR[i], relsz[i]);
    }
    xs(b, "/><hh:offset");
    for (i = 0; i < 7; i++) {
        xf(b, " %s=\"%d\"", LANG_ATTR[i], offset[i]);
    }
    xs(b, "/>");
    if (props & 1) {
        xs(b, "<hh:italic/>");
    }
    if (props & 2) {
        xs(b, "<hh:bold/>");
    }
    {
        unsigned ut = (props >> 2) & 3;
        xf(b, "<hh:underline type=\"%s\" shape=\"%s\" color=\"", ut == 1 ? "BOTTOM" : ut == 3 ? "TOP" : "NONE",
           PICK(LINE_TYPE2, (props >> 4) & 0x0F));
        x_color(b, underline_color);
        xs(b, "\"/>");
    }
    if ((props >> 18) & 7) {
        xf(b, "<hh:strikeout shape=\"%s\" color=\"", PICK(LINE_TYPE2, (props >> 26) & 0x0F));
        x_color(b, strike_color);
        xs(b, "\"/>");
    } else {
        xs(b, "<hh:strikeout shape=\"NONE\" color=\"#000000\"/>");
    }
    xf(b, "<hh:outline type=\"NONE\"/><hh:shadow type=\"NONE\" color=\"#B2B2B2\" offsetX=\"%d\" offsetY=\"%d\"/>", shadow_x,
       shadow_y);
    if (props & (1u << 15)) {
        xs(b, "<hh:supscript/>");
    }
    if (props & (1u << 16)) {
        xs(b, "<hh:subscript/>");
    }
    xs(b, "</hh:charPr>");
}

/* Paragraph margins are stored doubled; an odd value marks a "character" unit
 * (Hancom writes value (raw-1)/2 with unit CHAR in the HwpUnitChar branch). */
static void emit_margin_value(xb *b, const char *name, int32_t raw, int in_case) {
    if (!in_case) {
        xf(b, "<hc:%s value=\"%d\" unit=\"HWPUNIT\"/>", name, (int)raw);
    } else if (raw & 1) {
        xf(b, "<hc:%s value=\"%d\" unit=\"CHAR\"/>", name, (int)((raw - 1) / 2));
    } else {
        xf(b, "<hc:%s value=\"%d\" unit=\"HWPUNIT\"/>", name, (int)(raw / 2));
    }
}

static void emit_para_shape(const ctx *c, xb *b, unsigned id, const rec *rc) {
    static const char *const aligns[] = {"JUSTIFY", "LEFT", "RIGHT", "CENTER", "DISTRIBUTE", "DISTRIBUTE_SPACE"};
    static const char *const valigns[] = {"BASELINE", "TOP", "CENTER", "BOTTOM"};
    static const char *const heads[] = {"NONE", "OUTLINE", "NUMBER", "BULLET"};
    static const char *const ls_types[] = {"PERCENT", "FIXED", "BETWEEN_LINES", "AT_LEAST"};
    rd r = rd_make(rc->data, rc->size);
    uint32_t p1 = r32(&r), p2 = 0, line2 = 0;
    int32_t left = (int32_t)r32(&r), right = (int32_t)r32(&r), indent = (int32_t)r32(&r);
    int32_t prev = (int32_t)r32(&r), next = (int32_t)r32(&r), line1 = (int32_t)r32(&r);
    unsigned tab = r16(&r), head = r16(&r), border = r16(&r);
    int bl = (int16_t)r16(&r), br = (int16_t)r16(&r), bt = (int16_t)r16(&r), bb = (int16_t)r16(&r);
    unsigned ls_type = p1 & 3;
    int32_t line;
    int k;
    if (rleft(&r) && ver_at_least(c, 5, 0, 1, 7)) {
        p2 = r32(&r);
    }
    if (rleft(&r) && ver_at_least(c, 5, 0, 2, 5)) {
        (void)r32(&r);
        line2 = r32(&r);
    }
    line = ver_at_least(c, 5, 0, 2, 5) ? (int32_t)line2 : line1;
    xf(b, "<hh:paraPr id=\"%u\" tabPrIDRef=\"%u\" condense=\"%u\" fontLineHeight=\"%d\" snapToGrid=\"%d\" "
          "suppressLineNumbers=\"0\" checked=\"0\">",
       id, tab, (unsigned)((p1 >> 9) & 0x7F), (int)((p1 >> 22) & 1), (int)((p1 >> 8) & 1));
    xf(b, "<hh:align horizontal=\"%s\" vertical=\"%s\"/>", PICK(aligns, (p1 >> 2) & 7), PICK(valigns, (p1 >> 20) & 3));
    xf(b, "<hh:heading type=\"%s\" idRef=\"%u\" level=\"%u\"/>", PICK(heads, (p1 >> 23) & 3), head,
       (unsigned)((p1 >> 25) & 7));
    xf(b, "<hh:breakSetting breakLatinWord=\"%s\" breakNonLatinWord=\"%s\" widowOrphan=\"%d\" keepWithNext=\"%d\" "
          "keepLines=\"%d\" pageBreakBefore=\"%d\" lineWrap=\"%s\"/>",
       ((p1 >> 5) & 3) == 0 ? "KEEP_WORD" : ((p1 >> 5) & 3) == 1 ? "HYPHENATION" : "BREAK_WORD",
       (p1 >> 7) & 1 ? "BREAK_WORD" : "KEEP_WORD", (int)((p1 >> 16) & 1), (int)((p1 >> 17) & 1),
       (int)((p1 >> 18) & 1), (int)((p1 >> 19) & 1), (p2 & 3) ? "SQUEEZE" : "BREAK");
    xs(b, "<hp:switch><hp:case hp:required-namespace=\"http://www.hancom.co.kr/hwpml/2016/HwpUnitChar\">");
    for (k = 1; k >= 0; k--) {
        xs(b, "<hh:margin>");
        emit_margin_value(b, "intent", indent, k);
        emit_margin_value(b, "left", left, k);
        emit_margin_value(b, "right", right, k);
        xf(b, "<hc:prev value=\"%d\" unit=\"HWPUNIT\"/>", (int)(k ? prev / 2 : prev));
        xf(b, "<hc:next value=\"%d\" unit=\"HWPUNIT\"/>", (int)(k ? next / 2 : next));
        xs(b, "</hh:margin>");
        xf(b, "<hh:lineSpacing type=\"%s\" value=\"%d\" unit=\"HWPUNIT\"/>", PICK(ls_types, ls_type),
           (int)(k && ls_type != 0 ? line / 2 : line));
        xs(b, k ? "</hp:case><hp:default>" : "</hp:default></hp:switch>");
    }
    xf(b, "<hh:border borderFillIDRef=\"%u\" offsetLeft=\"%d\" offsetRight=\"%d\" offsetTop=\"%d\" offsetBottom=\"%d\" "
          "connect=\"%d\" ignoreMargin=\"%d\"/>",
       bf_ref(c, border), bl, br, bt, bb, (int)((p1 >> 28) & 1), (int)((p1 >> 29) & 1));
    xf(b, "<hh:autoSpacing eAsianEng=\"%d\" eAsianNum=\"%d\"/></hh:paraPr>", (int)((p2 >> 4) & 1), (int)((p2 >> 5) & 1));
}

static hp_status emit_header(ctx *c, const recs *di, unsigned section_count, xb *b) {
    uint32_t counts[18];
    size_t i, face_index = 0;
    unsigned lang, n_border = 0, n_char = 0, n_para = 0, k;
    int zero_ref = 0;
    memset(counts, 0, sizeof(counts));
    for (i = 0; i < di->n; i++) {
        if (di->r[i].tag == TAG_ID_MAPPINGS) {
            rd r = rd_make(di->r[i].data, di->r[i].size);
            for (k = 0; k < 18 && rleft(&r); k++) {
                counts[k] = r32(&r);
            }
            break;
        }
    }
    /* BinData list (pictures refer to it by 1-based index). */
    for (i = 0; i < di->n; i++) {
        if (di->r[i].tag == TAG_BIN_DATA) {
            rd r = rd_make(di->r[i].data, di->r[i].size);
            bin_entry e;
            bin_entry *g;
            unsigned props = r16(&r);
            memset(&e, 0, sizeof(e));
            e.type = props & 0x0F;
            e.compression = (props >> 4) & 3;
            if (e.type == 0) {
                skip_hwpstr(&r);
                skip_hwpstr(&r);
            } else {
                unsigned n, j;
                e.id = r16(&r);
                n = r16(&r);
                for (j = 0; j < n && rd_have(&r, 2); j++) {
                    unsigned ch = r16(&r);
                    if (j < sizeof(e.ext) - 1 && ch > 32 && ch < 127 && ch != '/' && ch != '\\') {
                        e.ext[j] = (char)ch;
                    }
                }
            }
            g = (bin_entry *)realloc(c->bins, (c->bin_count + 1) * sizeof(*g));
            if (!g) {
                return HP_ERR_NOMEM;
            }
            c->bins = g;
            c->bins[c->bin_count++] = e;
        } else if (di->r[i].tag == TAG_CHAR_SHAPE) {
            rd r = rd_make(di->r[i].data, di->r[i].size);
            rskip(&r, 14 + 7 * 4 + 4 + 4 + 2 + 16);
            if (rleft(&r) && ver_at_least(c, 5, 0, 2, 1) && r16(&r) == 0) {
                zero_ref = 1;
            } else if (!ver_at_least(c, 5, 0, 2, 1)) {
                zero_ref = 1;
            }
        } else if (di->r[i].tag == TAG_PARA_SHAPE) {
            rd r = rd_make(di->r[i].data, di->r[i].size);
            rskip(&r, 4 + 6 * 4 + 4);
            if (r16(&r) == 0) {
                zero_ref = 1;
            }
        }
    }
    c->bf_shift = zero_ref;

    xs(b, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\" ?>"
          "<hh:head xmlns:ha=\"http://www.hancom.co.kr/hwpml/2011/app\" "
          "xmlns:hp=\"http://www.hancom.co.kr/hwpml/2011/paragraph\" "
          "xmlns:hp10=\"http://www.hancom.co.kr/hwpml/2016/paragraph\" "
          "xmlns:hs=\"http://www.hancom.co.kr/hwpml/2011/section\" "
          "xmlns:hc=\"http://www.hancom.co.kr/hwpml/2011/core\" "
          "xmlns:hh=\"http://www.hancom.co.kr/hwpml/2011/head\" ");
    xf(b, "version=\"1.4\" secCnt=\"%u\"><hh:beginNum page=\"1\" footnote=\"1\" endnote=\"1\" pic=\"1\" tbl=\"1\" "
          "equation=\"1\"/><hh:refList>",
       section_count);

    /* fonts: FACE_NAME records in language order, counts from ID_MAPPINGS */
    xs(b, "<hh:fontfaces itemCnt=\"7\">");
    {
        size_t fi = 0;
        for (lang = 0; lang < 7; lang++) {
            uint32_t n = counts[1 + lang], id = 0;
            xf(b, "<hh:fontface lang=\"%s\" fontCnt=\"%u\">", LANGS[lang], (unsigned)n);
            for (; fi < di->n && id < n; fi++) {
                rd r;
                if (di->r[fi].tag != TAG_FACE_NAME) {
                    continue;
                }
                r = rd_make(di->r[fi].data, di->r[fi].size);
                (void)r8(&r);
                xf(b, "<hh:font id=\"%u\" face=\"", (unsigned)id);
                x_hwpstr(b, &r);
                xs(b, "\" type=\"TTF\" isEmbedded=\"0\"/>");
                id++;
                face_index++;
            }
            xs(b, "</hh:fontface>");
        }
    }
    xs(b, "</hh:fontfaces>");

    xs(b, "<hh:borderFills>");
    if (c->bf_shift) {
        xs(b, "<hh:borderFill id=\"1\" threeD=\"0\" shadow=\"0\" centerLine=\"NONE\" breakCellSeparateLine=\"0\">"
              "<hh:slash type=\"NONE\" Crooked=\"0\" isCounter=\"0\"/><hh:backSlash type=\"NONE\" Crooked=\"0\" isCounter=\"0\"/>"
              "<hh:leftBorder type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/>"
              "<hh:rightBorder type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/>"
              "<hh:topBorder type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/>"
              "<hh:bottomBorder type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/>"
              "<hh:diagonal type=\"SOLID\" width=\"0.1 mm\" color=\"#000000\"/></hh:borderFill>");
    }
    for (i = 0; i < di->n; i++) {
        if (di->r[i].tag == TAG_BORDER_FILL) {
            n_border++;
            emit_border_fill(b, bf_ref(c, n_border), &di->r[i]);
        }
    }
    xs(b, "</hh:borderFills><hh:charProperties>");
    for (i = 0; i < di->n; i++) {
        if (di->r[i].tag == TAG_CHAR_SHAPE) {
            emit_char_shape(c, b, n_char++, &di->r[i]);
        }
    }
    xs(b, "</hh:charProperties><hh:paraProperties>");
    for (i = 0; i < di->n; i++) {
        if (di->r[i].tag == TAG_PARA_SHAPE) {
            emit_para_shape(c, b, n_para++, &di->r[i]);
        }
    }
    xs(b, "</hh:paraProperties></hh:refList></hh:head>");
    (void)face_index;
    return b->oom ? HP_ERR_NOMEM : HP_OK;
}

/* ---------- section XML (BodyText) ---------- */

static void emit_paragraph(ctx *c, const recs *rs, size_t pi);

/* Paragraphs of a list (cell, caption, text box, header ...): consecutive
 * PARA_HEADER siblings starting at `first`, stopping at the declared count, the
 * "last in list" flag or the end of the parent's subtree. Returns the next index. */
static size_t emit_para_list(ctx *c, const recs *rs, size_t first, size_t limit, long count) {
    size_t j = first;
    long done = 0;
    unsigned level;
    if (first >= limit) {
        return first;
    }
    level = rs->r[first].level;
    while (j < limit && rs->r[j].tag == TAG_PARA_HEADER && rs->r[j].level == level && (count <= 0 || done < count)) {
        uint32_t nchars = rs->r[j].size >= 4 ? le32(rs->r[j].data) : 0;
        emit_paragraph(c, rs, j);
        done++;
        j = rs->r[j].end > j ? rs->r[j].end : j + 1;
        if (nchars & 0x80000000u) {
            break;
        }
    }
    return j;
}

/* LIST_HEADER at index lh: its paragraphs follow it as siblings. */
static size_t emit_sublist(ctx *c, const recs *rs, size_t lh, size_t limit, const char *vert_align, uint32_t text_width,
                           uint32_t text_height) {
    rd r = rd_make(rs->r[lh].data, rs->r[lh].size);
    long count = (long)(r32(&r) & 0xFFFF);
    uint32_t props = r32(&r);
    static const char *const dirs[] = {"HORIZONTAL", "VERTICAL", "VERTICALALL", "HORIZONTAL"};
    static const char *const wraps[] = {"BREAK", "SQUEEZE", "KEEP", "BREAK"};
    static const char *const valigns[] = {"TOP", "CENTER", "BOTTOM", "TOP"};
    size_t next;
    xf(c->out, "<hp:subList id=\"\" textDirection=\"%s\" lineWrap=\"%s\" vertAlign=\"%s\" linkListIDRef=\"0\" "
               "linkListNextIDRef=\"0\" textWidth=\"%u\" textHeight=\"%u\" hasTextRef=\"0\" hasNumRef=\"0\">",
       dirs[props & 3], wraps[(props >> 3) & 3], vert_align ? vert_align : valigns[(props >> 5) & 3],
       (unsigned)text_width, (unsigned)text_height);
    next = emit_para_list(c, rs, lh + 1, limit, count);
    xs(c->out, "</hp:subList>");
    return next;
}

typedef struct {
    uint32_t props;
    uint32_t y, x, width, height;
    int32_t z;
    unsigned margin[4];
    uint32_t instance;
    int prevent_page_break;
    int valid;
} common_obj;

static void read_common(const rec *ch, common_obj *o) {
    rd r = rd_make(ch->data, ch->size);
    memset(o, 0, sizeof(*o));
    rskip(&r, 4); /* ctrl id */
    o->props = r32(&r);
    o->y = r32(&r);
    o->x = r32(&r);
    o->width = r32(&r);
    o->height = r32(&r);
    o->z = (int32_t)r32(&r);
    o->margin[0] = r16(&r);
    o->margin[1] = r16(&r);
    o->margin[2] = r16(&r);
    o->margin[3] = r16(&r);
    o->instance = r32(&r);
    if (r.n - r.i > 4) {
        o->prevent_page_break = (int)(r32(&r) & 1);
    }
    o->valid = !r.bad;
}

static void emit_object_attrs(ctx *c, const common_obj *o) {
    static const char *const wraps[] = {"SQUARE", "TOP_AND_BOTTOM", "BEHIND_TEXT", "IN_FRONT_OF_TEXT"};
    static const char *const flows[] = {"BOTH_SIDES", "LEFT_ONLY", "RIGHT_ONLY", "LARGEST_ONLY"};
    static const char *const numbering[] = {"NONE", "PICTURE", "TABLE", "EQUATION"};
    if (o) {
        xf(c->out, " id=\"%u\" zOrder=\"%d\" numberingType=\"%s\" textWrap=\"%s\" textFlow=\"%s\"", (unsigned)o->instance,
           (int)o->z, numbering[(o->props >> 26) & 3], PICK(wraps, (o->props >> 21) & 7), flows[(o->props >> 24) & 3]);
    } else {
        xs(c->out, " id=\"0\" zOrder=\"0\" numberingType=\"NONE\" textWrap=\"TOP_AND_BOTTOM\" textFlow=\"BOTH_SIDES\"");
    }
    xs(c->out, " lock=\"0\" dropcapstyle=\"None\"");
}

static void emit_object_placement(ctx *c, const common_obj *o) {
    static const char *const wrel[] = {"PAPER", "PAGE", "COLUMN", "PARA", "ABSOLUTE"};
    static const char *const hrel_h[] = {"PAPER", "PAGE", "ABSOLUTE", "ABSOLUTE"};
    static const char *const vrel[] = {"PAPER", "PAGE", "PARA", "PARA"};
    static const char *const hrel[] = {"PAPER", "PAGE", "COLUMN", "PARA"};
    static const char *const valign[] = {"TOP", "CENTER", "BOTTOM", "INSIDE", "OUTSIDE"};
    static const char *const halign[] = {"LEFT", "CENTER", "RIGHT", "INSIDE", "OUTSIDE"};
    uint32_t p = o->props;
    xf(c->out, "<hp:sz width=\"%u\" widthRelTo=\"%s\" height=\"%u\" heightRelTo=\"%s\" protect=\"%d\"/>",
       (unsigned)o->width, PICK(wrel, (p >> 15) & 7), (unsigned)o->height, hrel_h[(p >> 18) & 3], (int)((p >> 20) & 1));
    xf(c->out, "<hp:pos treatAsChar=\"%d\" affectLSpacing=\"%d\" flowWithText=\"%d\" allowOverlap=\"%d\" "
               "holdAnchorAndSO=\"%d\" vertRelTo=\"%s\" horzRelTo=\"%s\" vertAlign=\"%s\" horzAlign=\"%s\" "
               "vertOffset=\"%u\" horzOffset=\"%u\"/>",
       (int)(p & 1), (int)((p >> 2) & 1), (int)((p >> 13) & 1), (int)((p >> 14) & 1), o->prevent_page_break,
       vrel[(p >> 3) & 3], hrel[(p >> 8) & 3], PICK(valign, (p >> 5) & 7), PICK(halign, (p >> 10) & 7),
       (unsigned)o->y, (unsigned)o->x);
    xf(c->out, "<hp:outMargin left=\"%u\" right=\"%u\" top=\"%u\" bottom=\"%u\"/>", o->margin[0], o->margin[1],
       o->margin[2], o->margin[3]);
}

/* Caption LIST_HEADER (before the table body / shape component). */
static void emit_caption(ctx *c, const recs *rs, size_t lh, size_t limit) {
    static const char *const sides[] = {"LEFT", "RIGHT", "TOP", "BOTTOM"};
    rd r = rd_make(rs->r[lh].data, rs->r[lh].size);
    uint32_t cprops, width, last;
    unsigned gap;
    rskip(&r, 8);
    cprops = r32(&r);
    width = r32(&r);
    gap = r16(&r);
    last = r32(&r);
    xf(c->out, "<hp:caption side=\"%s\" fullSz=\"%d\" width=\"%u\" gap=\"%u\" lastWidth=\"%u\">", sides[cprops & 3],
       (int)((cprops >> 2) & 1), (unsigned)width, gap, (unsigned)last);
    emit_sublist(c, rs, lh, limit, NULL, last, 0);
    xs(c->out, "</hp:caption>");
}

static void emit_table(ctx *c, const recs *rs, size_t ci) {
    static const char *const breaks[] = {"NONE", "TABLE", "CELL", "CELL"};
    const rec *ch = &rs->r[ci];
    common_obj o;
    size_t j, table_rec = (size_t)-1, caption = (size_t)-1;
    rd t;
    uint32_t tprops;
    unsigned rows, cols, spacing, pad[4], border, row, *per_row = NULL;
    read_common(ch, &o);
    for (j = ci + 1; j < ch->end; j = rs->r[j].end > j ? rs->r[j].end : j + 1) {
        if (rs->r[j].tag == TAG_TABLE) {
            table_rec = j;
            break;
        }
        if (rs->r[j].tag == TAG_LIST_HEADER && caption == (size_t)-1) {
            caption = j;
        }
    }
    if (table_rec == (size_t)-1) {
        return;
    }
    t = rd_make(rs->r[table_rec].data, rs->r[table_rec].size);
    tprops = r32(&t);
    rows = r16(&t);
    cols = r16(&t);
    spacing = r16(&t);
    pad[0] = r16(&t);
    pad[1] = r16(&t);
    pad[2] = r16(&t);
    pad[3] = r16(&t);
    if (rows > 0 && rows <= 65535) {
        per_row = (unsigned *)calloc(rows, sizeof(unsigned));
        for (row = 0; per_row && row < rows; row++) {
            per_row[row] = r16(&t);
        }
    }
    border = r16(&t);
    xs(c->out, "<hp:tbl");
    emit_object_attrs(c, &o);
    xf(c->out, " pageBreak=\"%s\" repeatHeader=\"%d\" rowCnt=\"%u\" colCnt=\"%u\" cellSpacing=\"%u\" "
               "borderFillIDRef=\"%u\" noAdjust=\"0\">",
       breaks[tprops & 3], (int)((tprops >> 2) & 1), rows, cols, spacing, bf_ref(c, border));
    emit_object_placement(c, &o);
    if (caption != (size_t)-1 && (o.props >> 29) & 1) {
        emit_caption(c, rs, caption, ch->end);
    }
    xf(c->out, "<hp:inMargin left=\"%u\" right=\"%u\" top=\"%u\" bottom=\"%u\"/>", pad[0], pad[1], pad[2], pad[3]);
    /* cells: LIST_HEADER siblings after the TABLE record, grouped per row */
    {
        unsigned in_row = 0, cur_row = 0;
        int open = 0;
        for (j = table_rec + 1; j < ch->end && !c->out->oom;) {
            const rec *lh = &rs->r[j];
            rd r;
            uint32_t props;
            unsigned col_i, row_i, cspan, rspan, m[4], cell_bf;
            uint32_t w, h;
            static const char *const valigns[] = {"TOP", "CENTER", "BOTTOM", "TOP"};
            if (lh->tag != TAG_LIST_HEADER) {
                j = lh->end > j ? lh->end : j + 1;
                continue;
            }
            r = rd_make(lh->data, lh->size);
            rskip(&r, 4);
            props = r32(&r);
            col_i = r16(&r);
            row_i = r16(&r);
            cspan = r16(&r);
            rspan = r16(&r);
            w = r32(&r);
            h = r32(&r);
            m[0] = r16(&r);
            m[1] = r16(&r);
            m[2] = r16(&r);
            m[3] = r16(&r);
            cell_bf = r16(&r);
            while (per_row && cur_row < rows && in_row >= per_row[cur_row]) {
                if (open) {
                    xs(c->out, "</hp:tr>");
                    open = 0;
                }
                cur_row++;
                in_row = 0;
            }
            if (!open) {
                xs(c->out, "<hp:tr>");
                open = 1;
            }
            xf(c->out, "<hp:tc name=\"\" header=\"%d\" hasMargin=\"%d\" protect=\"%d\" editable=\"%d\" dirty=\"0\" "
                       "borderFillIDRef=\"%u\">",
               (int)((props >> 18) & 1), (int)((props >> 16) & 1), (int)((props >> 17) & 1), (int)((props >> 19) & 1),
               bf_ref(c, cell_bf));
            j = emit_sublist(c, rs, j, ch->end, valigns[(props >> 5) & 3], 0, 0);
            xf(c->out, "<hp:cellAddr colAddr=\"%u\" rowAddr=\"%u\"/><hp:cellSpan colSpan=\"%u\" rowSpan=\"%u\"/>"
                       "<hp:cellSz width=\"%u\" height=\"%u\"/>"
                       "<hp:cellMargin left=\"%u\" right=\"%u\" top=\"%u\" bottom=\"%u\"/></hp:tc>",
               col_i, row_i, cspan, rspan, (unsigned)w, (unsigned)h, m[0], m[1], m[2], m[3]);
            in_row++;
        }
        if (open) {
            xs(c->out, "</hp:tr>");
        }
    }
    free(per_row);
    xs(c->out, "</hp:tbl>");
}

/* ---- drawing objects (gso) ---- */

static double rdouble(rd *r) {
    double v = 0;
    unsigned char b[8];
    if (!rd_have(r, 8)) {
        return 0;
    }
    memcpy(b, r->p + r->i, 8);
    r->i += 8;
    {
        /* little-endian IEEE-754 */
        uint64_t u = 0;
        int k;
        for (k = 7; k >= 0; k--) {
            u = (u << 8) | b[k];
        }
        memcpy(&v, &u, sizeof(v));
    }
    return v;
}

static void emit_matrix(xb *b, const char *name, rd *r) {
    double e[6];
    int k;
    for (k = 0; k < 6; k++) {
        e[k] = rdouble(r);
    }
    xf(b, "<hc:%s e1=\"%g\" e2=\"%g\" e3=\"%g\" e4=\"%g\" e5=\"%g\" e6=\"%g\"/>", name, e[0], e[1], e[2], e[3], e[4], e[5]);
}

static const char *shape_element(uint32_t id) {
    switch (id) {
    case CHID('$', 'p', 'i', 'c'):
        return "pic";
    case CHID('$', 'r', 'e', 'c'):
        return "rect";
    case CHID('$', 'e', 'l', 'l'):
        return "ellipse";
    case CHID('$', 'a', 'r', 'c'):
        return "arc";
    case CHID('$', 'p', 'o', 'l'):
        return "polygon";
    case CHID('$', 'c', 'u', 'r'):
        return "curve";
    case CHID('$', 'l', 'i', 'n'):
        return "line";
    case CHID('$', 'c', 'o', 'n'):
        return "container";
    case CHID('$', 'o', 'l', 'e'):
        return "ole";
    case CHID('$', 'c', 'o', 'l'):
        return "connectLine";
    case CHID('$', 't', 'a', 't'):
        return "textart";
    default:
        return NULL;
    }
}

static const bin_entry *find_bin_item(const ctx *c, unsigned item) {
    size_t i;
    if (item >= 1 && item <= c->bin_count) {
        return &c->bins[item - 1];
    }
    for (i = 0; i < c->bin_count; i++) {
        if (c->bins[i].id == item) {
            return &c->bins[i];
        }
    }
    return NULL;
}

static void emit_picture_body(ctx *c, const rec *pr) {
    rd r = rd_make(pr->data, pr->size);
    int32_t pts[8], clip[4];
    unsigned m[4], item, k;
    int bright, contrast, effect;
    uint32_t dim_w = 0, dim_h = 0;
    const bin_entry *bin;
    (void)r32(&r); /* border color */
    (void)r32(&r); /* border width */
    (void)r32(&r); /* border props */
    for (k = 0; k < 8; k++) {
        pts[k] = (int32_t)r32(&r);
    }
    for (k = 0; k < 4; k++) {
        clip[k] = (int32_t)r32(&r);
    }
    for (k = 0; k < 4; k++) {
        m[k] = r16(&r);
    }
    bright = (signed char)r8(&r);
    contrast = (signed char)r8(&r);
    effect = (int)r8(&r);
    item = r16(&r);
    if (rleft(&r)) {
        (void)r8(&r); /* border alpha */
    }
    if (rleft(&r)) {
        (void)r32(&r); /* instance id */
    }
    if (rleft(&r)) {
        /* picture effects (variable length), then the original image size */
        uint32_t fx = r32(&r);
        int ok = 1;
        if (fx & 1) { /* shadow: 12 x 4 bytes + color */
            rskip(&r, 11 * 4);
            if (r32(&r) != 0) {
                ok = 0;
            } else {
                uint32_t n;
                rskip(&r, 4);
                n = r32(&r);
                rskip(&r, (size_t)n * 8);
            }
        }
        if (ok && (fx & 2)) { /* neon */
            rskip(&r, 8);
            if (r32(&r) != 0) {
                ok = 0;
            } else {
                uint32_t n;
                rskip(&r, 4);
                n = r32(&r);
                rskip(&r, (size_t)n * 8);
            }
        }
        if (ok && (fx & 4)) { /* soft edge */
            rskip(&r, 4);
        }
        if (ok && (fx & 8)) { /* reflection */
            rskip(&r, 15 * 4);
        }
        if (ok && r.n - r.i >= 8) {
            dim_w = r32(&r);
            dim_h = r32(&r);
        }
    }
    xs(c->out, "<hp:imgRect>");
    for (k = 0; k < 4; k++) {
        xf(c->out, "<hc:pt%u x=\"%d\" y=\"%d\"/>", k, (int)pts[k * 2], (int)pts[k * 2 + 1]);
    }
    xf(c->out, "</hp:imgRect><hp:imgClip left=\"%d\" right=\"%d\" top=\"%d\" bottom=\"%d\"/>", (int)clip[0], (int)clip[2],
       (int)clip[1], (int)clip[3]);
    xf(c->out, "<hp:inMargin left=\"%u\" right=\"%u\" top=\"%u\" bottom=\"%u\"/>", m[0], m[1], m[2], m[3]);
    xf(c->out, "<hp:imgDim dimwidth=\"%u\" dimheight=\"%u\"/>", (unsigned)dim_w, (unsigned)dim_h);
    bin = find_bin_item(c, item);
    xf(c->out, "<hc:img binaryItemIDRef=\"image%u\" bright=\"%d\" contrast=\"%d\" effect=\"%s\" alpha=\"0\"/>",
       bin ? bin->id : item, bright, contrast,
       effect == 1 ? "GRAY_SCALE" : effect == 2 ? "BLACK_WHITE" : "REAL_PIC");
    xs(c->out, "<hp:effects/>");
}

static void emit_shape(ctx *c, const recs *rs, size_t si, const common_obj *top, size_t caption, size_t caption_limit) {
    const rec *sc = &rs->r[si];
    rd r = rd_make(sc->data, sc->size);
    uint32_t id, props, org_w, org_h, cur_w, cur_h;
    int32_t ox, oy, rcx, rcy;
    unsigned group, angle, mcount, k;
    const char *name;
    size_t j;
    if (++c->depth > HWP_MAX_DEPTH) {
        c->depth--;
        return;
    }
    id = r32(&r);
    if (top) {
        (void)r32(&r); /* the top-level component repeats its id */
    }
    name = shape_element(id);
    if (!name) {
        c->depth--;
        return;
    }
    ox = (int32_t)r32(&r);
    oy = (int32_t)r32(&r);
    group = r16(&r);
    (void)r16(&r);
    org_w = r32(&r);
    org_h = r32(&r);
    cur_w = r32(&r);
    cur_h = r32(&r);
    props = r32(&r);
    angle = r16(&r);
    rcx = (int32_t)r32(&r);
    rcy = (int32_t)r32(&r);
    mcount = r16(&r);
    xf(c->out, "<hp:%s", name);
    emit_object_attrs(c, top);
    xf(c->out, " href=\"\" groupLevel=\"%u\" instid=\"0\"", group);
    if (strcmp(name, "pic") == 0) {
        xf(c->out, " reverse=\"%d\"", (int)((props >> 24) & 1));
    }
    xs(c->out, ">");
    xf(c->out, "<hp:offset x=\"%u\" y=\"%u\"/><hp:orgSz width=\"%u\" height=\"%u\"/>", (unsigned)ox, (unsigned)oy,
       (unsigned)org_w, (unsigned)org_h);
    xf(c->out, "<hp:curSz width=\"%u\" height=\"%u\"/>", (unsigned)(cur_w == org_w ? 0 : cur_w),
       (unsigned)(cur_h == org_h ? 0 : cur_h));
    xf(c->out, "<hp:flip horizontal=\"%d\" vertical=\"%d\"/>", (int)(props & 1), (int)((props >> 1) & 1));
    xf(c->out, "<hp:rotationInfo angle=\"%u\" centerX=\"%d\" centerY=\"%d\" rotateimage=\"1\"/>", angle, (int)rcx,
       (int)rcy);
    xs(c->out, "<hp:renderingInfo>");
    emit_matrix(c->out, "transMatrix", &r);
    for (k = 0; k < mcount && k < 64 && !r.bad; k++) {
        emit_matrix(c->out, "scaMatrix", &r);
        emit_matrix(c->out, "rotMatrix", &r);
    }
    xs(c->out, "</hp:renderingInfo>");

    if (strcmp(name, "container") == 0) {
        for (j = si + 1; j < sc->end; j = rs->r[j].end > j ? rs->r[j].end : j + 1) {
            if (rs->r[j].tag == TAG_SHAPE_COMPONENT) {
                emit_shape(c, rs, j, NULL, (size_t)-1, 0);
            }
        }
    } else {
        for (j = si + 1; j < sc->end; j = rs->r[j].end > j ? rs->r[j].end : j + 1) {
            if (rs->r[j].tag == TAG_SC_PICTURE && strcmp(name, "pic") == 0) {
                emit_picture_body(c, &rs->r[j]);
            } else if (rs->r[j].tag == TAG_LIST_HEADER) {
                /* text box */
                rd t = rd_make(rs->r[j].data, rs->r[j].size);
                unsigned m[4];
                uint32_t last;
                rskip(&t, 8);
                m[0] = r16(&t);
                m[1] = r16(&t);
                m[2] = r16(&t);
                m[3] = r16(&t);
                last = r32(&t);
                xf(c->out, "<hp:drawText lastWidth=\"%u\" name=\"\" editable=\"0\">", (unsigned)last);
                {
                    size_t next = emit_sublist(c, rs, j, sc->end, NULL, 0, 0);
                    xf(c->out, "<hp:textMargin left=\"%u\" right=\"%u\" top=\"%u\" bottom=\"%u\"/></hp:drawText>", m[0],
                       m[1], m[2], m[3]);
                    if (next > j + 1) {
                        j = next - 1; /* paragraphs consumed; continue after them */
                        continue;
                    }
                }
            }
        }
    }
    if (top) {
        emit_object_placement(c, top);
        if (caption != (size_t)-1 && (top->props >> 29) & 1) {
            emit_caption(c, rs, caption, caption_limit);
        }
    }
    xf(c->out, "</hp:%s>", name);
    c->depth--;
}

static void emit_gso(ctx *c, const recs *rs, size_t ci) {
    const rec *ch = &rs->r[ci];
    common_obj o;
    size_t j, caption = (size_t)-1;
    read_common(ch, &o);
    for (j = ci + 1; j < ch->end; j = rs->r[j].end > j ? rs->r[j].end : j + 1) {
        if (rs->r[j].tag == TAG_LIST_HEADER && caption == (size_t)-1) {
            caption = j;
        } else if (rs->r[j].tag == TAG_SHAPE_COMPONENT) {
            emit_shape(c, rs, j, &o, caption, ch->end);
            return;
        }
    }
}

static void emit_equation(ctx *c, const recs *rs, size_t ci) {
    const rec *ch = &rs->r[ci];
    common_obj o;
    size_t j;
    read_common(ch, &o);
    for (j = ci + 1; j < ch->end; j = rs->r[j].end > j ? rs->r[j].end : j + 1) {
        if (rs->r[j].tag == TAG_EQEDIT) {
            rd r = rd_make(rs->r[j].data, rs->r[j].size);
            uint32_t props = r32(&r), size, color;
            size_t script_at = r.i;
            int baseline = 0;
            skip_hwpstr(&r);
            size = r32(&r);
            color = r32(&r);
            if (rleft(&r)) {
                baseline = (int16_t)r16(&r);
            }
            xs(c->out, "<hp:equation");
            emit_object_attrs(c, &o);
            xf(c->out, " version=\"Equation Version 60\" baseLine=\"%d\" textColor=\"", baseline);
            x_color(c->out, color);
            xf(c->out, "\" baseUnit=\"%u\" lineMode=\"%s\" font=\"HYhwpEQ\">", (unsigned)size, (props & 1) ? "LINE" : "CHAR");
            emit_object_placement(c, &o);
            xs(c->out, "<hp:script>");
            r.i = script_at;
            r.bad = 0;
            x_hwpstr(c->out, &r);
            xs(c->out, "</hp:script></hp:equation>");
            return;
        }
    }
}

static void emit_secd(ctx *c, const recs *rs, size_t ci) {
    const rec *ch = &rs->r[ci];
    rd r = rd_make(ch->data, ch->size);
    uint32_t props, tab;
    unsigned gap, outline, page_start, pic_start, tbl_start, eq_start;
    size_t j;
    rskip(&r, 4);
    props = r32(&r);
    gap = r16(&r);
    (void)r16(&r);
    (void)r16(&r);
    tab = r32(&r);
    outline = r16(&r);
    page_start = r16(&r);
    pic_start = r16(&r);
    tbl_start = r16(&r);
    eq_start = r16(&r);
    xf(c->out, "<hp:secPr id=\"\" textDirection=\"HORIZONTAL\" spaceColumns=\"%u\" tabStop=\"%u\" tabStopVal=\"%u\" "
               "tabStopUnit=\"HWPUNIT\" outlineShapeIDRef=\"%u\" memoShapeIDRef=\"0\" textVerticalWidthHead=\"0\" "
               "masterPageCnt=\"0\">",
       gap, (unsigned)tab, (unsigned)(tab / 2), outline);
    xf(c->out, "<hp:grid lineGrid=\"0\" charGrid=\"0\" wonggojiFormat=\"%d\"/>", (int)((props >> 22) & 1));
    xf(c->out, "<hp:startNum pageStartsOn=\"BOTH\" page=\"%u\" pic=\"%u\" tbl=\"%u\" equation=\"%u\"/>", page_start,
       pic_start, tbl_start, eq_start);
    xf(c->out, "<hp:visibility hideFirstHeader=\"%d\" hideFirstFooter=\"%d\" hideFirstMasterPage=\"%d\" border=\"%s\" "
               "fill=\"%s\" hideFirstPageNum=\"%d\" hideFirstEmptyLine=\"%d\" showLineNumber=\"0\"/>",
       (int)(props & 1), (int)((props >> 1) & 1), (int)((props >> 2) & 1), (props >> 3) & 1 ? "HIDE_FIRST" : "SHOW_ALL",
       (props >> 4) & 1 ? "HIDE_FIRST" : "SHOW_ALL", (int)((props >> 5) & 1), (int)((props >> 19) & 1));
    for (j = ci + 1; j < ch->end; j = rs->r[j].end > j ? rs->r[j].end : j + 1) {
        if (rs->r[j].tag == TAG_PAGE_DEF) {
            rd p = rd_make(rs->r[j].data, rs->r[j].size);
            uint32_t v[9], attr;
            int k;
            for (k = 0; k < 9; k++) {
                v[k] = r32(&p);
            }
            attr = r32(&p);
            xf(c->out, "<hp:pagePr landscape=\"%s\" width=\"%u\" height=\"%u\" gutterType=\"%s\">",
               (attr & 1) ? "NARROWLY" : "WIDELY", (unsigned)v[0], (unsigned)v[1],
               ((attr >> 1) & 3) == 1 ? "LEFT_RIGHT" : ((attr >> 1) & 3) == 2 ? "TOP_BOTTOM" : "LEFT_ONLY");
            xf(c->out, "<hp:margin header=\"%u\" footer=\"%u\" gutter=\"%u\" left=\"%u\" right=\"%u\" top=\"%u\" "
                       "bottom=\"%u\"/></hp:pagePr>",
               (unsigned)v[6], (unsigned)v[7], (unsigned)v[8], (unsigned)v[2], (unsigned)v[3], (unsigned)v[4],
               (unsigned)v[5]);
            break;
        }
    }
    xs(c->out, "</hp:secPr>");
}

static void emit_cold(ctx *c, const rec *ch) {
    rd r = rd_make(ch->data, ch->size);
    unsigned props, count, gap = 0;
    static const char *const types[] = {"NEWSPAPER", "BALANCED_NEWSPAPER", "PARALLEL", "NEWSPAPER"};
    static const char *const layouts[] = {"LEFT", "RIGHT", "MIRROR", "LEFT"};
    rskip(&r, 4);
    props = r16(&r);
    count = (props >> 2) & 0xFF;
    if (count < 2 || ((props >> 12) & 1)) {
        gap = r16(&r);
    }
    xf(c->out, "<hp:ctrl><hp:colPr id=\"\" type=\"%s\" layout=\"%s\" colCount=\"%u\" sameSz=\"%d\" sameGap=\"%u\"/></hp:ctrl>",
       types[props & 3], layouts[(props >> 10) & 3], count ? count : 1, (int)((props >> 12) & 1), gap);
}

static void emit_note_or_header(ctx *c, const recs *rs, size_t ci, const char *element, const char *attrs) {
    const rec *ch = &rs->r[ci];
    size_t j;
    xf(c->out, "<hp:ctrl><hp:%s %s>", element, attrs);
    for (j = ci + 1; j < ch->end; j = rs->r[j].end > j ? rs->r[j].end : j + 1) {
        if (rs->r[j].tag == TAG_LIST_HEADER) {
            uint32_t w = 0, h = 0;
            if (strcmp(element, "header") == 0 || strcmp(element, "footer") == 0) {
                rd r = rd_make(rs->r[j].data, rs->r[j].size);
                rskip(&r, 8);
                w = r32(&r);
                h = r32(&r);
            }
            emit_sublist(c, rs, j, ch->end, NULL, w, h);
            break;
        }
    }
    xf(c->out, "</hp:%s></hp:ctrl>", element);
}

static void emit_control(ctx *c, const recs *rs, size_t ci) {
    const rec *ch = &rs->r[ci];
    uint32_t id;
    rd r;
    if (ch->size < 4) {
        return;
    }
    id = le32(ch->data);
    r = rd_make(ch->data, ch->size);
    rskip(&r, 4);
    if (++c->depth > HWP_MAX_DEPTH) {
        c->depth--;
        return;
    }
    switch (id) {
    case CHID('s', 'e', 'c', 'd'):
        emit_secd(c, rs, ci);
        break;
    case CHID('c', 'o', 'l', 'd'):
        emit_cold(c, ch);
        break;
    case CHID('t', 'b', 'l', ' '):
        emit_table(c, rs, ci);
        break;
    case CHID('g', 's', 'o', ' '):
        emit_gso(c, rs, ci);
        break;
    case CHID('e', 'q', 'e', 'd'):
        emit_equation(c, rs, ci);
        break;
    case CHID('h', 'e', 'a', 'd'):
    case CHID('f', 'o', 'o', 't'): {
        uint32_t places = r32(&r) & 3;
        char attrs[64];
        snprintf(attrs, sizeof(attrs), "id=\"\" applyPageType=\"%s\"", places == 1 ? "EVEN" : places == 2 ? "ODD" : "BOTH");
        emit_note_or_header(c, rs, ci, id == CHID('h', 'e', 'a', 'd') ? "header" : "footer", attrs);
        break;
    }
    case CHID('f', 'n', ' ', ' '):
    case CHID('e', 'n', ' ', ' '): {
        char attrs[48];
        snprintf(attrs, sizeof(attrs), "number=\"%u\" suffixChar=\"41\" instId=\"0\"", (unsigned)r32(&r));
        emit_note_or_header(c, rs, ci, id == CHID('f', 'n', ' ', ' ') ? "footNote" : "endNote", attrs);
        break;
    }
    case CHID('p', 'g', 'n', 'p'): {
        static const char *const pos[] = {"NONE", "TOP_LEFT", "TOP_CENTER", "TOP_RIGHT", "BOTTOM_LEFT", "BOTTOM_CENTER",
                                          "BOTTOM_RIGHT", "OUTSIDE_TOP", "OUTSIDE_BOTTOM", "INSIDE_TOP", "INSIDE_BOTTOM"};
        uint32_t props = r32(&r);
        unsigned side;
        rskip(&r, 4);
        side = r16(&r);
        xf(c->out, "<hp:ctrl><hp:pageNum pos=\"%s\" formatType=\"DIGIT\" sideChar=\"", PICK(pos, (props >> 8) & 0x0F));
        if (side) {
            x_char(c->out, side);
        }
        xs(c->out, "\"/></hp:ctrl>");
        break;
    }
    case CHID('p', 'g', 'h', 'd'): {
        uint32_t p = r32(&r);
        xf(c->out, "<hp:ctrl><hp:pageHiding hideHeader=\"%d\" hideFooter=\"%d\" hideMasterPage=\"%d\" hideBorder=\"%d\" "
                   "hideFill=\"%d\" hidePageNum=\"%d\"/></hp:ctrl>",
           (int)(p & 1), (int)((p >> 1) & 1), (int)((p >> 2) & 1), (int)((p >> 3) & 1), (int)((p >> 4) & 1),
           (int)((p >> 5) & 1));
        break;
    }
    case CHID('n', 'w', 'n', 'o'):
    case CHID('a', 't', 'n', 'o'): {
        static const char *const kinds[] = {"PAGE", "FOOTNOTE", "ENDNOTE", "PICTURE", "TABLE", "EQUATION"};
        uint32_t p = r32(&r);
        unsigned num = r16(&r);
        if (id == CHID('n', 'w', 'n', 'o')) {
            xf(c->out, "<hp:ctrl><hp:newNum num=\"%u\" numType=\"%s\"/></hp:ctrl>", num, PICK(kinds, p & 0x0F));
        } else {
            xf(c->out, "<hp:ctrl><hp:autoNum num=\"%u\" numType=\"%s\"/></hp:ctrl>", num, PICK(kinds, p & 0x0F));
        }
        break;
    }
    case CHID('b', 'o', 'k', 'm'):
        xs(c->out, "<hp:ctrl><hp:bookmark name=\"\"/></hp:ctrl>");
        break;
    case CHID('t', 'c', 'm', 't'):
        emit_note_or_header(c, rs, ci, "hiddenComment", "");
        break;
    default:
        if ((id >> 24) == '%') {
            static const struct {
                uint32_t id;
                const char *type;
            } fields[] = {{CHID('%', 'h', 'l', 'k'), "HYPERLINK"}, {CHID('%', 'c', 'l', 'k'), "CLICK_HERE"},
                          {CHID('%', 'b', 'm', 'k'), "BOOKMARK"},  {CHID('%', 'd', 't', 'e'), "DATE"},
                          {CHID('%', 'd', 'd', 't'), "DOC_DATE"},  {CHID('%', 'p', 'a', 't'), "PATH"},
                          {CHID('%', 'm', 'm', 'g'), "MAILMERGE"}, {CHID('%', 'x', 'r', 'f'), "CROSSREF"},
                          {CHID('%', 'f', 'm', 'u'), "FORMULA"},   {CHID('%', 's', 'm', 'r'), "SUMMARY"},
                          {CHID('%', 'u', 's', 'r'), "USER_INFO"}, {CHID('%', '%', 'm', 'e'), "MEMO"}};
            const char *type = "UNKNOWN";
            size_t k;
            for (k = 0; k < sizeof(fields) / sizeof(fields[0]); k++) {
                if (fields[k].id == id) {
                    type = fields[k].type;
                }
            }
            xf(c->out, "<hp:ctrl><hp:fieldBegin id=\"0\" type=\"%s\" name=\"\" editable=\"0\" dirty=\"0\" zorder=\"0\" "
                       "fieldid=\"0\"/></hp:ctrl>",
               type);
        }
        break;
    }
    c->depth--;
}

static int is_extended(unsigned ch) {
    return ch == 1 || ch == 2 || ch == 3 || ch == 11 || ch == 12 || ch == 14 || ch == 15 || ch == 16 || ch == 17 ||
           ch == 18 || ch == 21 || ch == 22 || ch == 23;
}

static int is_inline(unsigned ch) {
    return (ch >= 4 && ch <= 9) || ch == 19 || ch == 20;
}

typedef struct {
    int run_open;     /* "<hp:run ..." written; '>' pending until content */
    int run_content;
    int t_open;
    int after_ctrl;   /* last thing in this run was a control */
} run_state;

static void run_begin(ctx *c, run_state *s, uint32_t char_id) {
    xf(c->out, "<hp:run charPrIDRef=\"%u\"", (unsigned)char_id);
    s->run_open = 1;
    s->run_content = 0;
    s->t_open = 0;
    s->after_ctrl = 0;
}

static void run_content(ctx *c, run_state *s) {
    if (!s->run_content) {
        xs(c->out, ">");
        s->run_content = 1;
    }
}

static void t_open(ctx *c, run_state *s) {
    run_content(c, s);
    if (!s->t_open) {
        xs(c->out, "<hp:t>");
        s->t_open = 1;
    }
    s->after_ctrl = 0;
}

static void t_close(ctx *c, run_state *s) {
    if (s->t_open) {
        xs(c->out, "</hp:t>");
        s->t_open = 0;
    }
}

static void run_end(ctx *c, run_state *s) {
    if (!s->run_open) {
        return;
    }
    t_close(c, s);
    xs(c->out, s->run_content ? "</hp:run>" : "/>");
    s->run_open = 0;
}

static void emit_paragraph(ctx *c, const recs *rs, size_t pi) {
    const rec *ph = &rs->r[pi];
    rd h = rd_make(ph->data, ph->size);
    const rec *text = NULL, *shapes = NULL, *segs = NULL;
    size_t *ctrls = NULL, ctrl_count = 0, ctrl_next = 0, j;
    unsigned para_shape, style, split;
    uint32_t instance;
    size_t n_units = 0, i, pair_count = 1, k = 0;
    uint32_t pos = 0;
    run_state s;
    memset(&s, 0, sizeof(s));
    if (++c->depth > HWP_MAX_DEPTH) {
        c->depth--;
        return;
    }
    (void)r32(&h);
    (void)r32(&h);
    para_shape = r16(&h);
    style = r8(&h);
    split = r8(&h);
    rskip(&h, 6);
    instance = r32(&h);
    for (j = pi + 1; j < ph->end; j = rs->r[j].end > j ? rs->r[j].end : j + 1) {
        switch (rs->r[j].tag) {
        case TAG_PARA_TEXT:
            if (!text) {
                text = &rs->r[j];
            }
            break;
        case TAG_PARA_CHAR_SHAPE:
            if (!shapes) {
                shapes = &rs->r[j];
            }
            break;
        case TAG_PARA_LINE_SEG:
            if (!segs) {
                segs = &rs->r[j];
            }
            break;
        case TAG_CTRL_HEADER: {
            size_t *g = (size_t *)realloc(ctrls, (ctrl_count + 1) * sizeof(size_t));
            if (!g) {
                c->error = HP_ERR_NOMEM;
                free(ctrls);
                c->depth--;
                return;
            }
            ctrls = g;
            ctrls[ctrl_count++] = j;
            break;
        }
        default:
            break;
        }
    }
    if (text) {
        n_units = text->size / 2;
    }
    if (shapes && shapes->size >= 8) {
        pair_count = shapes->size / 8;
    }
#define PAIR_POS(x) (shapes && shapes->size >= 8 ? le32(shapes->data + (x) * 8) : 0u)
#define PAIR_ID(x) (shapes && shapes->size >= 8 ? le32(shapes->data + (x) * 8 + 4) : 0u)
    xf(c->out, "<hp:p id=\"%u\" paraPrIDRef=\"%u\" styleIDRef=\"%u\" pageBreak=\"%d\" columnBreak=\"%d\" merged=\"0\">",
       (unsigned)instance, para_shape, style, (int)((split >> 2) & 1), (int)((split >> 3) & 1));
    run_begin(c, &s, PAIR_ID(0));
    for (i = 0; i < n_units && !c->out->oom;) {
        unsigned ch = (unsigned)text->data[i * 2] | ((unsigned)text->data[i * 2 + 1] << 8);
        while (k + 1 < pair_count && pos >= PAIR_POS(k + 1)) {
            run_end(c, &s);
            k++;
            run_begin(c, &s, PAIR_ID(k));
        }
        if (ch >= 32) {
            uint32_t cp = ch;
            size_t used = 1;
            if (ch >= 0xD800 && ch <= 0xDBFF && i + 1 < n_units) {
                unsigned lo = (unsigned)text->data[i * 2 + 2] | ((unsigned)text->data[i * 2 + 3] << 8);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((ch - 0xD800) << 10) + (lo - 0xDC00);
                    used = 2;
                }
            }
            t_open(c, &s);
            x_char(c->out, cp);
            i += used;
            pos += (uint32_t)used;
        } else if (is_extended(ch)) {
            t_close(c, &s);
            run_content(c, &s);
            if (ctrl_next < ctrl_count) {
                emit_control(c, rs, ctrls[ctrl_next++]);
            }
            s.after_ctrl = 1;
            i += 8;
            pos += 8;
        } else if (is_inline(ch)) {
            if (ch == 9 && i + 7 < n_units) {
                const unsigned char *a = text->data + (i + 1) * 2;
                t_open(c, &s);
                xf(c->out, "<hp:tab width=\"%u\" leader=\"%u\" type=\"%u\"/>", (unsigned)le32(a), (unsigned)a[4],
                   (unsigned)a[5]);
            } else if (ch == 4) {
                t_close(c, &s);
                run_content(c, &s);
                xs(c->out, "<hp:ctrl><hp:fieldEnd beginIDRef=\"0\" fieldid=\"0\"/></hp:ctrl>");
                s.after_ctrl = 1;
            }
            i += 8;
            pos += 8;
        } else {
            switch (ch) {
            case 10:
                t_open(c, &s);
                xs(c->out, "<hp:lineBreak/>");
                break;
            case 13:
                if (s.after_ctrl && !s.t_open) {
                    xs(c->out, "<hp:t/>");
                    s.after_ctrl = 0;
                }
                break;
            case 24:
                t_open(c, &s);
                xs(c->out, "<hp:hyphen/>");
                break;
            case 30:
                t_open(c, &s);
                xs(c->out, "<hp:nbSpace/>");
                break;
            case 31:
                t_open(c, &s);
                xs(c->out, "<hp:fwSpace/>");
                break;
            default:
                break;
            }
            i++;
            pos++;
        }
    }
    run_end(c, &s);
    for (k = k + 1; k < pair_count && !c->out->oom; k++) {
        run_begin(c, &s, PAIR_ID(k));
        run_end(c, &s);
    }
#undef PAIR_POS
#undef PAIR_ID
    if (segs && segs->size >= 36) {
        size_t n = segs->size / 36;
        xs(c->out, "<hp:linesegarray>");
        for (i = 0; i < n; i++) {
            const unsigned char *d = segs->data + i * 36;
            xf(c->out, "<hp:lineseg textpos=\"%u\" vertpos=\"%d\" vertsize=\"%d\" textheight=\"%d\" baseline=\"%d\" "
                       "spacing=\"%d\" horzpos=\"%d\" horzsize=\"%d\" flags=\"%u\"/>",
               (unsigned)le32(d), (int)(int32_t)le32(d + 4), (int)(int32_t)le32(d + 8), (int)(int32_t)le32(d + 12),
               (int)(int32_t)le32(d + 16), (int)(int32_t)le32(d + 20), (int)(int32_t)le32(d + 24),
               (int)(int32_t)le32(d + 28), (unsigned)le32(d + 32));
        }
        xs(c->out, "</hp:linesegarray>");
    }
    xs(c->out, "</hp:p>");
    free(ctrls);
    c->depth--;
}

static hp_status emit_section(ctx *c, const recs *rs, xb *b) {
    size_t i;
    c->out = b;
    c->depth = 0;
    xs(b, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\" ?>"
          "<hs:sec xmlns:ha=\"http://www.hancom.co.kr/hwpml/2011/app\" "
          "xmlns:hp=\"http://www.hancom.co.kr/hwpml/2011/paragraph\" "
          "xmlns:hp10=\"http://www.hancom.co.kr/hwpml/2016/paragraph\" "
          "xmlns:hs=\"http://www.hancom.co.kr/hwpml/2011/section\" "
          "xmlns:hc=\"http://www.hancom.co.kr/hwpml/2011/core\" "
          "xmlns:hh=\"http://www.hancom.co.kr/hwpml/2011/head\">");
    /* Top-level paragraphs (level 0). Anything else at level 0 (memo lists,
     * master pages) is not body text. */
    for (i = 0; i < rs->n && !b->oom && c->error == HP_OK;) {
        if (rs->r[i].tag == TAG_PARA_HEADER && rs->r[i].level == 0) {
            emit_paragraph(c, rs, i);
        }
        i = rs->r[i].end > i ? rs->r[i].end : i + 1;
    }
    xs(b, "</hs:sec>");
    if (c->error != HP_OK) {
        return c->error;
    }
    return b->oom ? HP_ERR_NOMEM : HP_OK;
}

/* ---------- summary information (title) ---------- */

static char *summary_title(const unsigned char *d, size_t len) {
    size_t sec, count, k;
    xb b;
    memset(&b, 0, sizeof(b));
    if (len < 48) {
        return NULL;
    }
    sec = le32(d + 44);
    if (sec + 8 > len) {
        return NULL;
    }
    count = le32(d + sec + 4);
    for (k = 0; k < count && k < 4096; k++) {
        size_t at = sec + 8 + k * 8, off;
        uint32_t pid, type;
        if (at + 8 > len) {
            return NULL;
        }
        pid = le32(d + at);
        off = sec + le32(d + at + 4);
        if (pid != 2 || off + 8 > len) {
            continue;
        }
        type = le32(d + off);
        if (type == 0x1F) { /* VT_LPWSTR */
            size_t chars = le32(d + off + 4);
            if (chars > (len - off - 8) / 2) {
                return NULL;
            }
            x_utf16(&b, d + off + 8, chars);
        } else if (type == 0x1E) { /* VT_LPSTR: keep ASCII only */
            size_t bytes = le32(d + off + 4), i;
            if (bytes > len - off - 8) {
                return NULL;
            }
            for (i = 0; i < bytes && d[off + 8 + i]; i++) {
                if (d[off + 8 + i] < 0x80) {
                    x_char(&b, d[off + 8 + i]);
                }
            }
        }
        if (b.oom) {
            free(b.s);
            return NULL;
        }
        return b.s;
    }
    return NULL;
}

/* ---------- top level ---------- */

int hp_looks_like_hwp(const char *path) {
    unsigned char magic[8];
    size_t n;
    FILE *f;

    if (!path) {
        return 0;
    }
    f = hp_fopen(path, "rb");
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

static hp_status read_file(const char *path, unsigned char **out, size_t *out_len) {
    FILE *f = hp_fopen(path, "rb");
    unsigned char *buf = NULL;
    size_t len = 0, cap = 0;
    *out = NULL;
    *out_len = 0;
    if (!f) {
        return HP_ERR_IO;
    }
    for (;;) {
        size_t got;
        if (len == cap) {
            unsigned char *g;
            if (cap >= HWP_MAX_FILE) {
                free(buf);
                fclose(f);
                return HP_ERR_INVALID;
            }
            cap = cap ? cap * 2 : 1 << 16;
            g = (unsigned char *)realloc(buf, cap);
            if (!g) {
                free(buf);
                fclose(f);
                return HP_ERR_NOMEM;
            }
            buf = g;
        }
        got = fread(buf + len, 1, cap - len, f);
        len += got;
        if (got == 0) {
            break;
        }
    }
    if (ferror(f)) {
        free(buf);
        fclose(f);
        return HP_ERR_IO;
    }
    fclose(f);
    *out = buf;
    *out_len = len;
    return HP_OK;
}

/* Stream content, inflated when `compressed`. */
static hp_status read_stream(const hp_cfb *cfb, const char *name, int compressed, unsigned char **out, size_t *out_len) {
    long e = hp_cfb_find(cfb, name);
    unsigned char *raw;
    size_t raw_len;
    hp_status st;
    *out = NULL;
    *out_len = 0;
    if (e < 0 || !hp_cfb_is_stream(cfb, e)) {
        return HP_ERR_INVALID;
    }
    st = hp_cfb_read(cfb, e, &raw, &raw_len);
    if (st != HP_OK || !compressed) {
        *out = raw;
        *out_len = raw_len;
        return st;
    }
    st = inflate_raw(raw, raw_len, out, out_len);
    free(raw);
    return st;
}

/* Distribution-only (배포용) documents keep the body in ViewText/Section<n>: a
 * DISTRIBUTE_DOC_DATA record whose 256 bytes are scrambled with an MSVC rand()
 * stream seeded by its first 4 bytes and hide the AES-128 key, then the AES-ECB
 * encrypted (and possibly compressed) section records. */
static hp_status decode_distributed(const unsigned char *raw, size_t len, int compressed, unsigned char **out,
                                    size_t *out_len) {
    unsigned char head[256], key[16], *body;
    uint32_t seed, rnd;
    unsigned char mask = 0;
    int i, run = 0;
    size_t body_len;
    hp_status st;
    *out = NULL;
    *out_len = 0;
    if (len < 4 + 256 || (le32(raw) & 0x3FF) != TAG_DISTRIBUTE_DOC_DATA || (le32(raw) >> 20) != 256) {
        return HP_ERR_INVALID;
    }
    memcpy(head, raw + 4, 256);
    seed = le32(head);
    rnd = seed;
    for (i = 0; i < 256; i++) {
        if (run == 0) {
            rnd = rnd * 214013u + 2531011u;
            mask = (unsigned char)((rnd >> 16) & 0xFF);
            rnd = rnd * 214013u + 2531011u;
            run = (int)(((rnd >> 16) & 0x0F) + 1);
        }
        if (i >= 4) {
            head[i] ^= mask;
        }
        run--;
    }
    memcpy(key, head + 4 + (seed & 0x0F), 16);
    body_len = len - 260;
    body = (unsigned char *)malloc(body_len ? body_len : 1);
    if (!body) {
        return HP_ERR_NOMEM;
    }
    memcpy(body, raw + 260, body_len);
    hp_aes128_ecb_decrypt(key, body, body_len);
    if (!compressed) {
        *out = body;
        *out_len = body_len;
        return HP_OK;
    }
    st = inflate_raw(body, body_len, out, out_len);
    free(body);
    return st;
}

static const char *media_type(const char *ext) {
    char low[16];
    size_t i;
    for (i = 0; ext[i] && i < sizeof(low) - 1; i++) {
        low[i] = (char)(ext[i] >= 'A' && ext[i] <= 'Z' ? ext[i] + 32 : ext[i]);
    }
    low[i] = '\0';
    if (strcmp(low, "png") == 0) {
        return "image/png";
    }
    if (strcmp(low, "jpg") == 0 || strcmp(low, "jpeg") == 0) {
        return "image/jpg";
    }
    if (strcmp(low, "bmp") == 0) {
        return "image/bmp";
    }
    if (strcmp(low, "gif") == 0) {
        return "image/gif";
    }
    if (strcmp(low, "wmf") == 0) {
        return "image/x-wmf";
    }
    if (strcmp(low, "emf") == 0) {
        return "image/x-emf";
    }
    return "application/octet-stream";
}

static hp_status add_text_part(hp_package *pkg, const char *name, xb *b) {
    hp_status st;
    if (b->oom) {
        return HP_ERR_NOMEM;
    }
    st = hp_package_add_entry(pkg, name, b->s ? b->s : "", b->len, 0);
    free(b->s);
    memset(b, 0, sizeof(*b));
    return st;
}

hp_status hp_hwp_read(const char *path, hp_document **out_doc) {
    unsigned char *file = NULL;
    size_t file_len = 0;
    hp_status st;
    if (out_doc) {
        *out_doc = NULL;
    }
    if (!path || !out_doc) {
        return HP_ERR_INVALID;
    }
    if (!hp_looks_like_hwp(path)) {
        return HP_ERR_NOT_HWP;
    }
    st = read_file(path, &file, &file_len);
    if (st != HP_OK) {
        return st;
    }
    st = hp_hwp_read_memory(file, file_len, out_doc);
    free(file);
    return st;
}

hp_status hp_hwp_read_memory(const unsigned char *file, size_t file_len, hp_document **out_doc) {
    unsigned char *header = NULL, *docinfo = NULL;
    size_t header_len = 0, docinfo_len = 0, i;
    hp_cfb *cfb = NULL;
    recs di = {NULL, 0};
    hp_package *pkg = NULL;
    ctx c;
    xb b, hpf;
    unsigned sections = 0;
    char *title = NULL;
    hp_status st;
    uint32_t flags;
    int distributed;
    const char *body_storage;

    memset(&c, 0, sizeof(c));
    memset(&b, 0, sizeof(b));
    memset(&hpf, 0, sizeof(hpf));
    if (out_doc) {
        *out_doc = NULL;
    }
    if (!file || !out_doc) {
        return HP_ERR_INVALID;
    }
    st = hp_cfb_open(file, file_len, &cfb);
    if (st != HP_OK) {
        goto done;
    }
    /* FileHeader: "HWP Document File" + version + flags */
    st = read_stream(cfb, "FileHeader", 0, &header, &header_len);
    if (st != HP_OK || header_len < 40 || memcmp(header, "HWP Document File", 17) != 0) {
        st = st != HP_OK ? st : HP_ERR_NOT_HWP;
        goto done;
    }
    c.version = le32(header + 32);
    flags = le32(header + 36);
    c.compressed = (int)(flags & 1);
    if ((flags & 2) || (flags & 16)) {
        /* password-protected or DRM: the content cannot be read without the key */
        st = HP_ERR_UNSUPPORTED;
        goto done;
    }
    distributed = (flags & 4) != 0;
    c.cfb = cfb;
    st = read_stream(cfb, "DocInfo", c.compressed, &docinfo, &docinfo_len);
    if (st == HP_OK) {
        st = parse_records(docinfo, docinfo_len, &di);
    }
    if (st != HP_OK) {
        goto done;
    }
    body_storage = distributed ? "ViewText" : "BodyText";
    while (sections < 4096) {
        char name[48];
        snprintf(name, sizeof(name), "%s/Section%u", body_storage, sections);
        if (hp_cfb_find(cfb, name) < 0) {
            break;
        }
        sections++;
    }
    if (sections == 0) {
        st = HP_ERR_INVALID;
        goto done;
    }

    pkg = (hp_package *)calloc(1, sizeof(*pkg));
    if (!pkg) {
        st = HP_ERR_NOMEM;
        goto done;
    }
    st = hp_package_add_entry(pkg, "mimetype", HP_HWPX_MIMETYPE, strlen(HP_HWPX_MIMETYPE), 1);
    if (st != HP_OK) {
        goto done;
    }
    xs(&b, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\" ?>"
           "<hv:HCFVersion xmlns:hv=\"http://www.hancom.co.kr/hwpml/2011/version\" tagetApplication=\"WORDPROCESSOR\" "
           "major=\"5\" minor=\"1\" micro=\"1\" buildNumber=\"0\" os=\"1\" xmlVersion=\"1.4\" application=\"HangulPad\" "
           "appVersion=\"" HP_VERSION_STRING "\"/>");
    st = add_text_part(pkg, "version.xml", &b);
    if (st == HP_OK) {
        xs(&b, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\" ?>"
               "<ocf:container xmlns:ocf=\"urn:oasis:names:tc:opendocument:xmlns:container\" "
               "xmlns:hpf=\"http://www.hancom.co.kr/schema/2011/hpf\"><ocf:rootfiles>"
               "<ocf:rootfile full-path=\"Contents/content.hpf\" media-type=\"application/hwpml-package+xml\"/>"
               "</ocf:rootfiles></ocf:container>");
        st = add_text_part(pkg, "META-INF/container.xml", &b);
    }
    if (st != HP_OK) {
        goto done;
    }

    /* title */
    {
        unsigned char *si = NULL;
        size_t si_len = 0;
        if (read_stream(cfb, "\005HwpSummaryInformation", 0, &si, &si_len) == HP_OK) {
            title = summary_title(si, si_len);
        }
        free(si);
    }

    /* header.xml */
    st = emit_header(&c, &di, sections, &b);
    if (st == HP_OK) {
        st = add_text_part(pkg, "Contents/header.xml", &b);
    }
    if (st != HP_OK) {
        goto done;
    }

    /* content.hpf (manifest + spine) is written as parts are added */
    xs(&hpf, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\" ?>"
             "<opf:package xmlns:opf=\"http://www.idpf.org/2007/opf/\" version=\"\" unique-identifier=\"\" id=\"\">"
             "<opf:metadata><opf:title>");
    if (title) {
        xs(&hpf, title);
    }
    xs(&hpf, "</opf:title><opf:language>ko</opf:language></opf:metadata><opf:manifest>"
             "<opf:item id=\"header\" href=\"Contents/header.xml\" media-type=\"application/xml\"/>");

    /* pictures */
    for (i = 0; i < c.bin_count; i++) {
        const bin_entry *e = &c.bins[i];
        char stream[64], part[64];
        unsigned char *data = NULL;
        size_t len = 0;
        int compressed;
        if (e->type == 0 || !e->ext[0]) {
            continue; /* linked file */
        }
        snprintf(stream, sizeof(stream), "BinData/BIN%04X.%s", e->id, e->ext);
        compressed = e->compression == 1 || (e->compression == 0 && c.compressed);
        if (read_stream(cfb, stream, compressed, &data, &len) != HP_OK) {
            free(data);
            data = NULL;
            /* mislabelled compression: fall back to the stored bytes */
            if (!compressed || read_stream(cfb, stream, 0, &data, &len) != HP_OK) {
                free(data);
                continue;
            }
        }
        snprintf(part, sizeof(part), "BinData/image%u.%s", e->id, e->ext);
        st = hp_package_add_entry(pkg, part, data, len, 1);
        free(data);
        if (st != HP_OK) {
            goto done;
        }
        xf(&hpf, "<opf:item id=\"image%u\" href=\"%s\" media-type=\"%s\" isEmbeded=\"1\"/>", e->id, part,
           media_type(e->ext));
    }

    /* sections */
    for (i = 0; i < sections; i++) {
        char name[48], part[48];
        unsigned char *body = NULL;
        size_t body_len = 0;
        recs rs = {NULL, 0};
        snprintf(name, sizeof(name), "%s/Section%u", body_storage, (unsigned)i);
        if (distributed) {
            unsigned char *raw = NULL;
            size_t raw_len = 0;
            st = read_stream(cfb, name, 0, &raw, &raw_len);
            if (st == HP_OK) {
                st = decode_distributed(raw, raw_len, c.compressed, &body, &body_len);
            }
            free(raw);
        } else {
            st = read_stream(cfb, name, c.compressed, &body, &body_len);
        }
        if (st == HP_OK) {
            st = parse_records(body, body_len, &rs);
        }
        if (st == HP_OK) {
            c.error = HP_OK;
            c.section_index = (unsigned)i;
            st = emit_section(&c, &rs, &b);
        }
        free(rs.r);
        free(body);
        if (st == HP_OK) {
            snprintf(part, sizeof(part), "Contents/section%u.xml", (unsigned)i);
            st = add_text_part(pkg, part, &b);
            xf(&hpf, "<opf:item id=\"section%u\" href=\"%s\" media-type=\"application/xml\"/>", (unsigned)i, part);
        }
        if (st != HP_OK) {
            goto done;
        }
    }
    xs(&hpf, "</opf:manifest><opf:spine><opf:itemref idref=\"header\" linear=\"yes\"/>");
    for (i = 0; i < sections; i++) {
        xf(&hpf, "<opf:itemref idref=\"section%u\" linear=\"yes\"/>", (unsigned)i);
    }
    xs(&hpf, "</opf:spine></opf:package>");
    st = add_text_part(pkg, "Contents/content.hpf", &hpf);
    if (st != HP_OK) {
        goto done;
    }
    pkg->read_only = distributed;
    st = hp_hwpx_read_package(pkg, out_doc);
    pkg = NULL; /* owned by the document (or freed) now */

done:
    free(b.s);
    free(hpf.s);
    free(title);
    free(di.r);
    free(docinfo);
    free(header);
    free(c.bins);
    hp_package_free(pkg);
    hp_cfb_close(cfb);
    return st;
}

int hp_document_is_read_only(const hp_document *doc) {
    return doc && doc->package && doc->package->read_only;
}

hp_status hp_document_read(const char *path, hp_document **out_doc) {
    if (out_doc) {
        *out_doc = NULL;
    }
    if (!path || !out_doc) {
        return HP_ERR_INVALID;
    }
    if (hp_looks_like_hwp(path)) {
        return hp_hwp_read(path, out_doc);
    }
    return hp_hwpx_read(path, out_doc);
}

#ifdef HP_FUZZING
/* Fuzzing hook: uncompressed DocInfo and one section's records, bypassing the
 * compound file and deflate so mutations reach the record parser.
 * Input: 4-byte little-endian DocInfo length, DocInfo, section records. */
int hp_hwp_fuzz_records(const unsigned char *data, size_t size) {
    ctx c;
    recs di = {NULL, 0}, rs = {NULL, 0};
    xb b, hpf;
    hp_package *pkg;
    hp_document *doc = NULL;
    size_t dl;
    if (size < 4) {
        return 0;
    }
    dl = le32(data);
    if (dl > size - 4) {
        return 0;
    }
    memset(&c, 0, sizeof(c));
    memset(&b, 0, sizeof(b));
    memset(&hpf, 0, sizeof(hpf));
    c.version = 0x05000500u;
    pkg = (hp_package *)calloc(1, sizeof(*pkg));
    if (!pkg || parse_records(data + 4, dl, &di) != HP_OK ||
        parse_records(data + 4 + dl, size - 4 - dl, &rs) != HP_OK) {
        goto out;
    }
    hp_package_add_entry(pkg, "mimetype", HP_HWPX_MIMETYPE, strlen(HP_HWPX_MIMETYPE), 1);
    if (emit_header(&c, &di, 1, &b) != HP_OK || add_text_part(pkg, "Contents/header.xml", &b) != HP_OK) {
        goto out;
    }
    if (emit_section(&c, &rs, &b) != HP_OK || add_text_part(pkg, "Contents/section0.xml", &b) != HP_OK) {
        goto out;
    }
    xs(&hpf, "<opf:package><opf:manifest><opf:item id=\"s\" href=\"Contents/section0.xml\"/></opf:manifest>"
             "<opf:spine><opf:itemref idref=\"s\"/></opf:spine></opf:package>");
    if (add_text_part(pkg, "Contents/content.hpf", &hpf) != HP_OK) {
        goto out;
    }
    if (hp_hwpx_read_package(pkg, &doc) == HP_OK) {
        char *t = hp_document_plain_text(doc);
        free(t);
        hp_document_free(doc);
    }
    pkg = NULL;
out:
    hp_package_free(pkg);
    free(b.s);
    free(hpf.s);
    free(di.r);
    free(rs.r);
    free(c.bins);
    return 0;
}
#endif

hp_status hp_hwp_write(const hp_document *doc, const char *path) {
    (void)doc;
    (void)path;
    return HP_ERR_UNSUPPORTED;
}
