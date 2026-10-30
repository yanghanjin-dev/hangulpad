/* HWPX (OWPML) reader — built on the DOM parser in xml.c.
 *
 * Reads Contents/header.xml (fonts, char/para shapes, border fills), BinData,
 * and each section into the IR: paragraphs with runs, tables (nested), pictures,
 * grouped drawings (containers), equations, float positions and Hangul's saved
 * line layout (linesegs). The whole package is also kept (hp_package) so that a
 * later save can be lossless. */
#include "hp/hwpx.h"

#include "hwpx_package.h"
#include "xml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zip.h>

/* Style ids above this are ignored (a hostile file could otherwise make us
 * allocate gigabytes for id="2000000000"). Real documents stay far below. */
#define HP_MAX_STYLE_ID 65535

typedef struct {
    uint32_t font_size_hwp;
    uint32_t color_rgb;
    int bold;
    int italic, underline, strikeout;
    int use_font_space;
    int font_ids[7];
    int width_percent[7];
    int spacing_percent[7];
    int valid;
} hp_char_style;

typedef struct {
    hp_align align;
    int page_break_before;
    int32_t left_hwp;
    int32_t right_hwp;
    int32_t intent_hwp;
    int32_t prev_hwp;
    int32_t next_hwp;
    int line_spacing_percent;
    int heading_type;  /* 0 none, 1 outline, 2 number, 3 bullet */
    int heading_id;    /* numbering / bullet id */
    int heading_level; /* 0-based */
    int valid;
} hp_para_style;

/* <hh:numbering>: per level the format ("^1." etc.), number style and start */
typedef struct {
    char *format[10];
    int num_format[10];
    int start[10];
    int char_id[10]; /* -1: the paragraph's own character shape */
    int valid;
} hp_numbering_def;

/* How footnote numbers are written (secPr autoNumFormat). */
typedef struct {
    int num_format;
    char prefix[16], suffix[16];
} hp_note_format;

/* Mutable state while reading the sections of one document (0.12.0). */
typedef struct {
    int (*counters)[10]; /* per numbering id: last number per level, 0 = unused */
    size_t counter_count;
    int outline_id;      /* the section's outlineShapeIDRef */
    hp_note_format footnote;
    int note_count;      /* footnotes so far (numbering is continuous) */
    int current_note;    /* number of the note whose text is being read, 0 outside */
    hp_section *sec;     /* section being read */
} hp_read_state;

typedef struct {
    int left;
    int right;
    int top;
    int bottom;
    hp_border_line style_left,style_right,style_top,style_bottom;
    uint32_t color_left;
    uint32_t color_right;
    uint32_t color_top;
    uint32_t color_bottom;
    int width_px;
    int width_left, width_right, width_top, width_bottom;
    int valid;
    uint32_t fill_rgb;
    int fill_valid;
} hp_border_style;

typedef struct {
    const char *id;            /* points into id_buf */
    char id_buf[64];
    const unsigned char *data; /* owned by the package */
    size_t len;
} hp_bin_item;

/* Growable tables indexed by the ids used in header.xml. */
typedef struct {
    char **faces[7];
    size_t face_count[7];
    hp_char_style *chars;
    size_t char_count;
    hp_para_style *paras;
    size_t para_count;
    hp_border_style *borders;
    size_t border_count;
    hp_bin_item *bins;
    size_t bin_count;
    /* start offsets of the section's <p> elements in document order (ascending),
     * to record each paragraph's source index (hp_paragraph.source_p) */
    size_t *p_starts;
    size_t p_count;
    hp_numbering_def *numberings;
    size_t numbering_count;
    char **bullets; /* UTF-8 bullet character per bullet id */
    size_t bullet_count;
    hp_read_state *state; /* the tables are read-only; this is not */
} hp_style_tables;

static uint32_t p_index(const hp_style_tables *st, const hp_xml_node *p) {
    size_t lo = 0, hi = st->p_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (st->p_starts[mid] < p->start) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < st->p_count && st->p_starts[lo] == p->start ? (uint32_t)lo + 1 : 0;
}

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

/* Make tbl[id] addressable (zero-filled growth). Returns 0 if id is out of range
 * or allocation fails. */
static int ensure_slot(void **tbl, size_t *count, size_t elem, long long id) {
    size_t need;
    void *grown;
    if (id < 0 || id > HP_MAX_STYLE_ID) {
        return 0;
    }
    need = (size_t)id + 1;
    if (need <= *count) {
        return 1;
    }
    grown = realloc(*tbl, need * elem);
    if (!grown) {
        return 0;
    }
    memset((char *)grown + *count * elem, 0, (need - *count) * elem);
    *tbl = grown;
    *count = need;
    return 1;
}

static void style_tables_free(hp_style_tables *st) {
    size_t i,lang;
    for(lang=0;lang<7;lang++) {
        for (i = 0; i < st->face_count[lang]; i++)free(st->faces[lang][i]);
        free(st->faces[lang]);
    }
    free(st->chars);
    free(st->paras);
    free(st->borders);
    free(st->bins);
    free(st->p_starts);
    for (i = 0; i < st->numbering_count; i++) {
        int k;
        for (k = 0; k < 10; k++) {
            free(st->numberings[i].format[k]);
        }
    }
    free(st->numberings);
    for (i = 0; i < st->bullet_count; i++) {
        free(st->bullets[i]);
    }
    free(st->bullets);
    memset(st, 0, sizeof(*st));
}

int hp_looks_like_hwpx(const char *path) {
    zip_t *za;
    int zerr = 0;
    zip_stat_t st;
    zip_file_t *zf;
    char buf[64];
    zip_int64_t n;
    int ok = 0;
    if (!path) {
        return 0;
    }
    za = zip_open(path, ZIP_RDONLY, &zerr);
    if (!za) {
        return 0;
    }
    if (zip_stat(za, "mimetype", 0, &st) == 0 && st.size < sizeof(buf) && (zf = zip_fopen(za, "mimetype", 0))) {
        n = zip_fread(zf, buf, sizeof(buf) - 1);
        zip_fclose(zf);
        if (n >= 0) {
            buf[n] = '\0';
            while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) {
                buf[--n] = '\0';
            }
            ok = strcmp(buf, HP_HWPX_MIMETYPE) == 0;
        }
    }
    zip_close(za);
    return ok;
}

/* ---------- small helpers ---------- */

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
    return HP_ALIGN_JUSTIFY; /* DISTRIBUTE / JUSTIFY */
}

static hp_border_line border_line_style(const char *s){
    if(!s)return HP_BORDER_SOLID;
    if(strcmp(s,"DASH")==0)return HP_BORDER_DASH;
    if(strcmp(s,"DOT")==0)return HP_BORDER_DOT;
    if(strcmp(s,"DASH_DOT")==0)return HP_BORDER_DASH_DOT;
    if(strcmp(s,"DASH_DOT_DOT")==0)return HP_BORDER_DASH_DOT_DOT;
    if(strcmp(s,"LONG_DASH")==0)return HP_BORDER_LONG_DASH;
    if(strcmp(s,"DOUBLE_SLIM")==0)return HP_BORDER_DOUBLE_SLIM;
    return HP_BORDER_SOLID;
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

static long long attr_l(const hp_xml_node *el, const char *name, long long def) {
    return hp_xml_attr_long(el, name, def);
}

static uint32_t attr_u32(const hp_xml_node *el, const char *name, uint32_t def) {
    return (uint32_t)hp_xml_attr_ulong(el, name, def);
}

/* Last descendant element with this name (switch/case blocks repeat elements;
 * the hp:default branch comes last). */
static hp_xml_node *find_last(const hp_xml_node *scope, const char *name) {
    hp_xml_node *n, *last = NULL;
    for (n = hp_xml_find(scope, name); n; n = hp_xml_find_next(scope, n, name)) {
        last = n;
    }
    return last;
}

/* Growable string buffer */
typedef struct {
    char *s;
    size_t len, cap;
    int oom;
} sbuf;

static void sb_putn(sbuf *b, const char *s, size_t n) {
    if (b->oom) {
        return;
    }
    if (n > SIZE_MAX-b->len-1) {b->oom=1;return;}
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        char *g;
        while (b->len + n + 1 > cap) {
            if(cap>SIZE_MAX/2){cap=b->len+n+1;break;}
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

static char *sb_finish(sbuf *b) {
    if (b->oom) {
        free(b->s);
        return NULL;
    }
    if (!b->s) {
        return hp_strdup("");
    }
    return b->s;
}

/* Text of an element: all descendant text. Inline controls inside <hp:t> become
 * plain characters: tab / line break / fixed-width space / non-breaking space -> ' ',
 * soft hyphen -> '-'. */
static void collect_text(const hp_xml_node *el, sbuf *b) {
    const hp_xml_node *c;
    for (c = el->first_child; c; c = c->next) {
        if (c->type == HP_XML_TEXT) {
            sb_putn(b, c->text, c->text_len);
        } else {
            if (strcmp(c->name, "tab") == 0 || strcmp(c->name, "lineBreak") == 0 || strcmp(c->name, "lb") == 0 ||
                strcmp(c->name, "fwSpace") == 0 || strcmp(c->name, "nbSpace") == 0) {
                sb_putn(b, " ", 1);
            } else if (strcmp(c->name, "hyphen") == 0) {
                sb_putn(b, "-", 1);
            }
            collect_text(c, b);
        }
    }
}

static char *element_text(const hp_xml_node *el) {
    sbuf b = {0};
    collect_text(el, &b);
    return sb_finish(&b);
}

static int is_empty_element_tag(const hp_xml_node *el) {
    return el->inner_start == el->end; /* written as <x/> */
}

/* ---------- automatic numbers (0.12.0) ---------- */

static const char *const NUMBER_FORMATS[] = {
    "DIGIT",           "CIRCLED_DIGIT",           "ROMAN_CAPITAL", "ROMAN_SMALL",          "LATIN_CAPITAL",
    "LATIN_SMALL",     "CIRCLED_LATIN_CAPITAL",   "CIRCLED_LATIN_SMALL", "HANGUL_SYLLABLE", "CIRCLED_HANGUL_SYLLABLE",
    "HANGUL_JAMO",     "CIRCLED_HANGUL_JAMO",     "HANGUL_PHONETIC", "IDEOGRAPH",           "CIRCLED_IDEOGRAPH"};

static int number_format_code(const char *s) {
    size_t i;
    for (i = 0; s && i < sizeof(NUMBER_FORMATS) / sizeof(NUMBER_FORMATS[0]); i++) {
        if (strcmp(s, NUMBER_FORMATS[i]) == 0) {
            return (int)i;
        }
    }
    return 0;
}

static void sb_codepoint(sbuf *b, uint32_t cp) {
    char u[4];
    if (cp < 0x80) {
        u[0] = (char)cp;
        sb_putn(b, u, 1);
    } else if (cp < 0x800) {
        u[0] = (char)(0xC0 | (cp >> 6));
        u[1] = (char)(0x80 | (cp & 0x3F));
        sb_putn(b, u, 2);
    } else {
        u[0] = (char)(0xE0 | (cp >> 12));
        u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[2] = (char)(0x80 | (cp & 0x3F));
        sb_putn(b, u, 3);
    }
}

/* n written in one of NUMBER_FORMATS (as 한글 counts: 가나다…하, ㄱㄴ…ㅎ, ①…) */
static void format_number(sbuf *b, int n, int fmt) {
    static const uint16_t syllables[14] = {0xAC00, 0xB098, 0xB2E4, 0xB77C, 0xB9C8, 0xBC14, 0xC0AC,
                                           0xC544, 0xC790, 0xCC28, 0xCE74, 0xD0C0, 0xD30C, 0xD558};
    static const uint16_t jamo[14] = {0x3131, 0x3134, 0x3137, 0x3139, 0x3141, 0x3142, 0x3145,
                                      0x3147, 0x3148, 0x314A, 0x314B, 0x314C, 0x314D, 0x314E};
    static const uint16_t phonetic[10] = {0xC77C, 0xC774, 0xC0BC, 0xC0AC, 0xC624, 0xC721, 0xCE60, 0xD314, 0xAD6C, 0xC2ED};
    static const uint16_t ideograph[10] = {0x4E00, 0x4E8C, 0x4E09, 0x56DB, 0x4E94, 0x516D, 0x4E03, 0x516B, 0x4E5D, 0x5341};
    char digits[16];
    if (n < 1) {
        n = 1;
    }
    switch (fmt) {
    case 1: /* ① … ⑳ ㉑ … ㉟ ㊱ … ㊿ */
        if (n <= 20) {
            sb_codepoint(b, 0x2460 + (uint32_t)n - 1);
            return;
        }
        if (n <= 35) {
            sb_codepoint(b, 0x3251 + (uint32_t)n - 21);
            return;
        }
        if (n <= 50) {
            sb_codepoint(b, 0x32B1 + (uint32_t)n - 36);
            return;
        }
        break;
    case 2:
    case 3: {
        static const int values[13] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
        static const char *const upper[13] = {"M", "CM", "D", "CD", "C", "XC", "L", "XL", "X", "IX", "V", "IV", "I"};
        static const char *const lower[13] = {"m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i"};
        int i, v = n > 3999 ? 3999 : n;
        for (i = 0; i < 13; i++) {
            while (v >= values[i]) {
                sb_putn(b, fmt == 2 ? upper[i] : lower[i], strlen(upper[i]));
                v -= values[i];
            }
        }
        return;
    }
    case 4:
    case 5:
        sb_codepoint(b, (fmt == 4 ? 'A' : 'a') + (uint32_t)((n - 1) % 26));
        return;
    case 6:
    case 7:
        sb_codepoint(b, (fmt == 6 ? 0x24B6 : 0x24D0) + (uint32_t)((n - 1) % 26));
        return;
    case 8:
        sb_codepoint(b, syllables[(n - 1) % 14]);
        return;
    case 9:
        sb_codepoint(b, 0x326E + (uint32_t)((n - 1) % 14));
        return;
    case 10:
        sb_codepoint(b, jamo[(n - 1) % 14]);
        return;
    case 11:
        sb_codepoint(b, 0x3260 + (uint32_t)((n - 1) % 14));
        return;
    case 12:
    case 13:
        if (n <= 10) {
            sb_codepoint(b, fmt == 12 ? phonetic[n - 1] : ideograph[n - 1]);
            return;
        }
        break;
    case 14:
        if (n <= 10) {
            sb_codepoint(b, 0x3280 + (uint32_t)n - 1);
            return;
        }
        break;
    default:
        break;
    }
    snprintf(digits, sizeof(digits), "%d", n);
    sb_putn(b, digits, strlen(digits));
}

/* ---------- header.xml ---------- */

static void parse_header_styles(const hp_xml_node *root, hp_style_tables *st) {
    hp_xml_node *n;

    /* Font IDs are local to each script's fontface table. */
    for (n = hp_xml_find(root, "fontface"); n; n = hp_xml_find_after(root, n, "fontface")) {
        const char *lang = hp_xml_attr(n, "lang");
        hp_xml_node *f;
        static const char *const names[]={"HANGUL","LATIN","HANJA","JAPANESE","OTHER","SYMBOL","USER"};
        int li;
        for(li=0;li<7;li++)if(lang && strcmp(lang,names[li])==0)break;
        if(li==7)continue;
        for (f = hp_xml_find(n, "font"); f; f = hp_xml_find_after(n, f, "font")) {
            long long id = attr_l(f, "id", -1);
            const char *face = hp_xml_attr(f, "face");
            if (face && ensure_slot((void **)&st->faces[li], &st->face_count[li], sizeof(char *), id)) {
                free(st->faces[li][id]);
                st->faces[li][id] = hp_strdup(face);
            }
        }
    }

    /* character shapes */
    for (n = hp_xml_find(root, "charPr"); n; n = hp_xml_find_after(root, n, "charPr")) {
        long long id = attr_l(n, "id", -1);
        hp_char_style *cs;
        hp_xml_node *fontref;
        if (!ensure_slot((void **)&st->chars, &st->char_count, sizeof(hp_char_style), id)) {
            continue;
        }
        cs = &st->chars[id];
        cs->font_size_hwp = (uint32_t)attr_l(n, "height", 1000);
        cs->color_rgb = parse_color(hp_xml_attr(n, "textColor"));
        cs->bold = hp_xml_find(n, "bold") != NULL;
        cs->use_font_space = attr_l(n,"useFontSpace",0) != 0;
        cs->italic = hp_xml_child(n,"italic") != NULL;
        {
            const hp_xml_node *u=hp_xml_child(n,"underline"),*s=hp_xml_child(n,"strikeout");
            const char *ut=hp_xml_attr(u,"type"),*ss=hp_xml_attr(s,"shape"),*sc=hp_xml_attr(s,"color");
            cs->underline=ut && strcmp(ut,"NONE")!=0;
            /* Legacy producers emit shape=3D even with strikeout disabled.
             * Render the explicit single-line style, not that placeholder. */
            cs->strikeout=ss && strcmp(ss,"SOLID")==0 && sc && strcmp(sc,"none")!=0;
        }
        fontref = hp_xml_find(n, "fontRef");
        {
            static const char *const langs[] = {"hangul", "latin", "hanja", "japanese", "other", "symbol", "user"};
            const hp_xml_node *ratio = hp_xml_child(n, "ratio");
            const hp_xml_node *spacing = hp_xml_child(n, "spacing");
            size_t li;
            for (li = 0; li < 7; li++) {
                cs->font_ids[li]=(int)attr_l(fontref,langs[li],0);
                long long width = attr_l(ratio, langs[li], 100);
                long long space = attr_l(spacing, langs[li], 0);
                cs->width_percent[li] = width >= 1 && width <= 1000 ? (int)width : 100;
                cs->spacing_percent[li] = space >= -100 && space <= 100 ? (int)space : 0;
            }
        }
        cs->valid = 1;
    }

    /* paragraph shapes */
    for (n = hp_xml_find(root, "paraPr"); n; n = hp_xml_find_after(root, n, "paraPr")) {
        long long id = attr_l(n, "id", -1);
        hp_para_style *ps;
        hp_xml_node *al, *margin, *ls,*metric_scope,*branch;
        if (!ensure_slot((void **)&st->paras, &st->para_count, sizeof(hp_para_style), id)) {
            continue;
        }
        ps = &st->paras[id];
        memset(ps, 0, sizeof(*ps));
        ps->align = HP_ALIGN_JUSTIFY;
        ps->line_spacing_percent = 160;
        ps->page_break_before = (int)attr_l(hp_xml_find(n, "breakSetting"), "pageBreakBefore", 0) != 0;
        {
            const hp_xml_node *hd = hp_xml_child(n, "heading");
            const char *type = hp_xml_attr(hd, "type");
            long long ref = attr_l(hd, "idRef", 0), level = attr_l(hd, "level", 0);
            ps->heading_type = !type ? 0 : !strcmp(type, "OUTLINE") ? 1 : !strcmp(type, "NUMBER") ? 2 : !strcmp(type, "BULLET") ? 3 : 0;
            ps->heading_id = ref >= 0 && ref <= HP_MAX_STYLE_ID ? (int)ref : 0;
            ps->heading_level = level >= 0 && level < 10 ? (int)level : 0;
        }
        al = hp_xml_find(n, "align");
        if (al && hp_xml_attr(al, "horizontal")) {
            ps->align = parse_align_name(hp_xml_attr(al, "horizontal"));
        }
        metric_scope=n;
        /* Supported HwpUnitChar alternatives use normal HWPUNIT values. The
         * legacy default duplicates paragraph margins at a different scale. */
        for(branch=hp_xml_find(n,"case");branch;branch=hp_xml_find_after(n,branch,"case")) {
            const char *required=hp_xml_attr(branch,"required-namespace");
            if(required && strcmp(required,"http://www.hancom.co.kr/hwpml/2016/HwpUnitChar")==0) {
                metric_scope=branch;break;
            }
        }
        margin = find_last(metric_scope, "margin");
        if (margin) {
            ps->intent_hwp = (int32_t)attr_l(hp_xml_child(margin, "intent"), "value", 0);
            ps->left_hwp = (int32_t)attr_l(hp_xml_child(margin, "left"), "value", 0);
            ps->right_hwp = (int32_t)attr_l(hp_xml_child(margin, "right"), "value", 0);
            ps->prev_hwp = (int32_t)attr_l(hp_xml_child(margin, "prev"), "value", 0);
            ps->next_hwp = (int32_t)attr_l(hp_xml_child(margin, "next"), "value", 0);
        }
        ls = find_last(metric_scope, "lineSpacing");
        if (ls) {
            const char *type = hp_xml_attr(ls, "type");
            /* 64-bit and clamped: hostile values must not overflow (fuzzing, v0.9.0). */
            long long val = attr_l(ls, "value", 160);
            if (type && strcmp(type, "PERCENT") == 0) {
                ps->line_spacing_percent = val > 0 ? (int)(val > 5000 ? 5000 : val) : 160;
            } else if (val > 0) {
                /* FIXED/AT_LEAST/BETWEEN_LINES (HWPUNIT): approximate as % of 10pt */
                long long pct = val > 1000000 ? 100000 : (val * 100) / 1000;
                ps->line_spacing_percent = (int)(pct > 5000 ? 5000 : pct);
                if (ps->line_spacing_percent < 100) {
                    ps->line_spacing_percent = 100;
                }
            }
        }
        ps->valid = 1;
    }

    /* border fills (cell borders + background) */
    for (n = hp_xml_find(root, "borderFill"); n; n = hp_xml_find_after(root, n, "borderFill")) {
        static const char *const sides[4] = {"leftBorder", "rightBorder", "topBorder", "bottomBorder"};
        long long id = attr_l(n, "id", -1);
        hp_border_style *br;
        hp_xml_node *fill, *win;
        int i;
        if (!ensure_slot((void **)&st->borders, &st->border_count, sizeof(hp_border_style), id)) {
            continue;
        }
        br = &st->borders[id];
        memset(br, 0, sizeof(*br));
        br->width_px = 1;
        fill = hp_xml_find(n, "fillBrush");
        win = fill ? hp_xml_find(fill, "winBrush") : NULL;
        if (win && hp_xml_attr(win, "faceColor") && strcmp(hp_xml_attr(win, "faceColor"), "none") != 0) {
            br->fill_rgb = parse_color(hp_xml_attr(win, "faceColor"));
            br->fill_valid = 1;
        }
        for (i = 0; i < 4; i++) {
            hp_xml_node *side = hp_xml_find(n, sides[i]);
            const char *type = side ? hp_xml_attr(side, "type") : NULL;
            uint32_t color;
            if (!type || strcmp(type, "NONE") == 0) {
                continue;
            }
            color = parse_color(hp_xml_attr(side, "color"));
            switch (i) {
            case 0:
                br->left = 1;
                br->style_left = border_line_style(type);
                br->width_left = border_width_to_px(hp_xml_attr(side, "width"));
                br->color_left = color;
                break;
            case 1:
                br->right = 1;
                br->style_right = border_line_style(type);
                br->width_right = border_width_to_px(hp_xml_attr(side, "width"));
                br->color_right = color;
                break;
            case 2:
                br->top = 1;
                br->style_top = border_line_style(type);
                br->width_top = border_width_to_px(hp_xml_attr(side, "width"));
                br->color_top = color;
                break;
            default:
                br->bottom = 1;
                br->style_bottom = border_line_style(type);
                br->width_bottom = border_width_to_px(hp_xml_attr(side, "width"));
                br->color_bottom = color;
                break;
            }
            if (hp_xml_attr(side, "width")) {
                br->width_px = border_width_to_px(hp_xml_attr(side, "width"));
            }
        }
        br->valid = br->left || br->right || br->top || br->bottom;
    }

    /* paragraph numbering (also outline numbering) and bullets, 0.12.0 */
    for (n = hp_xml_find(root, "numbering"); n; n = hp_xml_find_after(root, n, "numbering")) {
        long long id = attr_l(n, "id", -1);
        hp_numbering_def *nd;
        const hp_xml_node *h;
        int k;
        if (!ensure_slot((void **)&st->numberings, &st->numbering_count, sizeof(hp_numbering_def), id)) {
            continue;
        }
        nd = &st->numberings[id];
        for (k = 0; k < 10; k++) {
            free(nd->format[k]);
            nd->format[k] = NULL;
            nd->start[k] = 1;
            nd->num_format[k] = 0;
            nd->char_id[k] = -1;
        }
        for (h = hp_xml_child(n, "paraHead"); h; h = hp_xml_next(h, "paraHead")) {
            long long level = attr_l(h, "level", 0), start = attr_l(h, "start", 1), ch = attr_l(h, "charPrIDRef", -1);
            if (level < 1 || level > 10) {
                continue;
            }
            nd->format[level - 1] = element_text(h);
            nd->num_format[level - 1] = number_format_code(hp_xml_attr(h, "numFormat"));
            nd->start[level - 1] = start >= 0 && start < 100000 ? (int)start : 1;
            nd->char_id[level - 1] = ch >= 0 && ch <= HP_MAX_STYLE_ID ? (int)ch : -1;
        }
        nd->valid = 1;
    }
    for (n = hp_xml_find(root, "bullet"); n; n = hp_xml_find_after(root, n, "bullet")) {
        long long id = attr_l(n, "id", -1);
        const char *ch = hp_xml_attr(n, "char");
        if (ch && ensure_slot((void **)&st->bullets, &st->bullet_count, sizeof(char *), id)) {
            free(st->bullets[id]);
            st->bullets[id] = hp_strdup(ch);
        }
    }
}

/* BinData/<id>.<ext> parts, in package order (data stays owned by the package). */
static hp_status load_bindata(const hp_package *pkg, hp_style_tables *st) {
    size_t i;
    for (i = 0; i < pkg->entry_count; i++) {
        const hp_package_entry *e = &pkg->entries[i];
        const char *base, *dot;
        size_t idlen;
        hp_bin_item *grown;
        if (e->is_dir || strncmp(e->name, "BinData/", 8) != 0 || e->len == 0) {
            continue;
        }
        grown = (hp_bin_item *)realloc(st->bins, (st->bin_count + 1) * sizeof(hp_bin_item));
        if (!grown) {
            return HP_ERR_NOMEM;
        }
        st->bins = grown;
        base = e->name + 8;
        dot = strrchr(base, '.');
        idlen = dot ? (size_t)(dot - base) : strlen(base);
        if (idlen >= sizeof(st->bins[0].id_buf)) {
            idlen = sizeof(st->bins[0].id_buf) - 1;
        }
        memcpy(st->bins[st->bin_count].id_buf, base, idlen);
        st->bins[st->bin_count].id_buf[idlen] = '\0';
        st->bins[st->bin_count].data = e->data;
        st->bins[st->bin_count].len = e->len;
        st->bin_count++;
    }
    /* ids point into the (now stable) array */
    for (i = 0; i < st->bin_count; i++) {
        st->bins[i].id = st->bins[i].id_buf;
    }
    return HP_OK;
}

static const hp_bin_item *find_bin(const hp_style_tables *st, const char *id) {
    size_t i;
    if (!id) {
        return NULL;
    }
    for (i = 0; i < st->bin_count; i++) {
        if (strcmp(st->bins[i].id, id) == 0) {
            return &st->bins[i];
        }
    }
    return NULL;
}

static void apply_char_style(hp_run *run, const hp_style_tables *st, long long char_id) {
    const hp_char_style *cs;
    run->font_size_hwp = 1000;
    run->color_rgb = 0;
    run->bold = 0;
    run->font_face = NULL;
    if (char_id < 0 || (size_t)char_id >= st->char_count || !st->chars[char_id].valid) {
        return;
    }
    cs = &st->chars[char_id];
    run->font_size_hwp = cs->font_size_hwp;
    run->color_rgb = cs->color_rgb;
    run->bold = cs->bold;
    run->use_font_space = cs->use_font_space;
    run->italic=cs->italic;run->underline=cs->underline;run->strikeout=cs->strikeout;
    memcpy(run->width_percent, cs->width_percent, sizeof(run->width_percent));
    memcpy(run->spacing_percent, cs->spacing_percent, sizeof(run->spacing_percent));
    {
        int li;
        for(li=0;li<7;li++) {
            int id=cs->font_ids[li];
            if(id>=0 && (size_t)id<st->face_count[li] && st->faces[li][id]) {
                strncpy(run->script_faces[li],st->faces[li][id],sizeof(run->script_faces[li])-1);
            }
        }
        if(run->script_faces[0][0])run->font_face=hp_strdup(run->script_faces[0]);
    }
}

static void apply_para_style(hp_paragraph *para, const hp_style_tables *st, long long id) {
    const hp_para_style *ps;
    para->align = HP_ALIGN_JUSTIFY;
    para->indent_left_hwp = 0;
    para->indent_right_hwp = 0;
    para->indent_first_hwp = 0;
    para->space_before_hwp = 0;
    para->space_after_hwp = 0;
    para->line_spacing_percent = 160;
    if (id < 0 || (size_t)id >= st->para_count || !st->paras[id].valid) {
        return;
    }
    ps = &st->paras[id];
    para->align = ps->align;
    para->indent_left_hwp = ps->left_hwp;
    para->indent_right_hwp = ps->right_hwp;
    para->indent_first_hwp = ps->intent_hwp;
    para->space_before_hwp = ps->prev_hwp;
    para->space_after_hwp = ps->next_hwp;
    para->line_spacing_percent = ps->line_spacing_percent > 0 ? ps->line_spacing_percent : 160;
}

static void apply_border_style(hp_table_cell *cell, const hp_style_tables *st, long long fill_id) {
    const hp_border_style *b;
    if (fill_id < 0 || (size_t)fill_id >= st->border_count) {
        return;
    }
    b = &st->borders[fill_id];
    if (b->fill_valid) {
        cell->fill_rgb = b->fill_rgb;
        cell->has_fill = 1;
    }
    if (!b->valid) {
        return;
    }
    cell->borders.left = b->left;
    cell->borders.style_left = b->style_left;
    cell->borders.width_left = b->width_left;
    cell->borders.right = b->right;
    cell->borders.style_right = b->style_right;
    cell->borders.width_right = b->width_right;
    cell->borders.top = b->top;
    cell->borders.style_top = b->style_top;
    cell->borders.width_top = b->width_top;
    cell->borders.bottom = b->bottom;
    cell->borders.style_bottom = b->style_bottom;
    cell->borders.width_bottom = b->width_bottom;
    cell->borders.color_left = b->color_left;
    cell->borders.color_right = b->color_right;
    cell->borders.color_top = b->color_top;
    cell->borders.color_bottom = b->color_bottom;
    cell->borders.width_px = b->width_px > 0 ? b->width_px : 1;
}

/* Free what a parsed paragraph owns and reset it. */
static void paragraph_release(hp_paragraph *para) {
    size_t i;
    for (i = 0; i < para->run_count; i++) {
        free(para->runs[i].text);
        free(para->runs[i].font_face);
    }
    free(para->runs);
    free(para->segs);
    memset(para, 0, sizeof(*para));
}

/* Hangul's saved line layout: the paragraph's own <hp:linesegarray> only (paragraphs
 * nested in tables inside it have their own). */
static void parse_linesegs(const hp_xml_node *p, hp_paragraph *para) {
    hp_xml_node *ls;
    para->segs = NULL;
    para->seg_count = 0;
    for (ls = hp_xml_child(hp_xml_child(p, "linesegarray"), "lineseg"); ls; ls = hp_xml_next(ls, "lineseg")) {
        hp_line_seg seg;
        hp_line_seg *arr;
        seg.textpos = attr_u32(ls, "textpos", 0);
        seg.vertpos_hwp = attr_u32(ls, "vertpos", 0);
        seg.vertsize_hwp = attr_u32(ls, "vertsize", 0);
        seg.textheight_hwp = attr_u32(ls, "textheight", 0);
        seg.baseline_hwp = attr_u32(ls,"baseline",0);
        seg.spacing_hwp = attr_u32(ls, "spacing", 0);
        seg.horzpos_hwp = attr_u32(ls, "horzpos", 0);
        seg.horzsize_hwp = attr_u32(ls, "horzsize", 0);
        arr = (hp_line_seg *)realloc(para->segs, (para->seg_count + 1) * sizeof(hp_line_seg));
        if (!arr) {
            break;
        }
        para->segs = arr;
        para->segs[para->seg_count++] = seg;
    }
}

static hp_status add_run(hp_paragraph *para, char *text, const hp_style_tables *st, long long char_id) {
    hp_run *runs;
    if (!text) {
        return HP_ERR_NOMEM;
    }
    runs = (hp_run *)realloc(para->runs, (para->run_count + 1) * sizeof(hp_run));
    if (!runs) {
        free(text);
        return HP_ERR_NOMEM;
    }
    para->runs = runs;
    memset(&para->runs[para->run_count], 0, sizeof(hp_run));
    para->runs[para->run_count].text = text;
    apply_char_style(&para->runs[para->run_count], st, char_id);
    para->run_count++;
    return HP_OK;
}

/* Objects that become their own blocks (or cell objects). */
static int is_object(const hp_xml_node *n) {
    return hp_xml_is(n, "tbl") || hp_xml_is(n, "pic") || hp_xml_is(n, "container") || hp_xml_is(n, "equation");
}

/* Text of a run element that is not an object: <hp:t> itself, or the <hp:t>s inside
 * e.g. a drawing's text box. Controls (<hp:ctrl>: page numbers, columns, headers,
 * footers, footnotes) are not body text and are skipped. Returns NULL for none. */
static int run_child_has_text(const hp_xml_node *n) {
    return !hp_xml_is(n, "ctrl") && (hp_xml_is(n, "t") || hp_xml_find(n, "t") != NULL);
}

/* ---------- saved line positions (0.12.0) ----------
 * <hp:lineseg textpos> counts as HWP text does: every control (field,
 * footnote, header, column/section definition, object) and every tab takes 8
 * units, other characters one UTF-16 unit. The IR text has one character per
 * glyph and its own characters for footnote numbers and paragraph numbers, so
 * the saved positions are mapped onto it. */
typedef struct {
    uint32_t stream;
    int32_t shift; /* stream - IR position from here on */
} pos_mark;

typedef struct {
    pos_mark *v;
    size_t n, cap;
    uint32_t stream, chars;
} pos_track;

static uint32_t utf16_units(const char *s) {
    uint32_t n = 0;
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c & 0xC0) != 0x80) {
            n += c >= 0xF0 ? 2 : 1;
        }
    }
    return n;
}

static uint32_t para_units(const hp_paragraph *para) {
    uint32_t n = 0;
    size_t i;
    for (i = 0; i < para->run_count; i++) {
        n += utf16_units(para->runs[i].text);
    }
    return n;
}

static void pos_advance(pos_track *t, uint32_t units, uint32_t chars) {
    t->stream += units;
    t->chars += chars;
    if (units == chars) {
        return;
    }
    if (t->n == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 8;
        pos_mark *g = (pos_mark *)realloc(t->v, cap * sizeof(*g));
        if (!g) {
            return; /* positions stay approximate */
        }
        t->v = g;
        t->cap = cap;
    }
    t->v[t->n].stream = t->stream;
    t->v[t->n].shift = (int32_t)(t->stream - t->chars);
    t->n++;
}

/* units of one <hp:t> */
static void pos_text(pos_track *t, const hp_xml_node *el) {
    const hp_xml_node *c;
    for (c = el->first_child; c; c = c->next) {
        if (c->type == HP_XML_TEXT) {
            char *s = (char *)malloc(c->text_len + 1);
            uint32_t n;
            if (!s) {
                continue;
            }
            memcpy(s, c->text, c->text_len);
            s[c->text_len] = '\0';
            n = utf16_units(s);
            free(s);
            pos_advance(t, n, n);
        } else if (hp_xml_is(c, "tab")) {
            pos_advance(t, 8, 1);
        } else if (hp_xml_is(c, "lineBreak") || hp_xml_is(c, "lb") || hp_xml_is(c, "fwSpace") ||
                   hp_xml_is(c, "nbSpace") || hp_xml_is(c, "hyphen")) {
            pos_advance(t, 1, 1);
        } else {
            char *s = element_text(c);
            uint32_t n = utf16_units(s);
            free(s);
            pos_advance(t, n ? n : 8, n); /* markers without text are controls */
        }
    }
}

static void pos_apply(hp_line_seg *segs, size_t count, const pos_track *t) {
    size_t i, k;
    for (i = 0; i < count; i++) {
        uint32_t at = segs[i].textpos;
        int32_t shift = 0;
        if (at == 0) {
            continue; /* the first line starts with any paragraph number */
        }
        for (k = 0; k < t->n && t->v[k].stream <= at; k++) {
            shift = t->v[k].shift;
        }
        segs[i].textpos = shift > 0 && (uint32_t)shift > at ? 0 : (uint32_t)((int64_t)at - shift);
    }
}

/* ---------- paragraph numbers, notes, headers and footers (0.12.0) ---------- */

/* The number or bullet 한글 shows before a paragraph with a numbered paragraph
 * shape; advances that numbering's counters. NULL for none. */
static char *heading_prefix(const hp_style_tables *st, long long para_pr_id, int *char_id) {
    hp_read_state *rs = st->state;
    const hp_para_style *ps;
    const hp_numbering_def *nd;
    sbuf b = {0};
    const char *p;
    int id, level, k, *c;
    *char_id = -1;
    if (!rs || para_pr_id < 0 || (size_t)para_pr_id >= st->para_count || !st->paras[para_pr_id].valid) {
        return NULL;
    }
    ps = &st->paras[para_pr_id];
    if (ps->heading_type == 3) {
        id = ps->heading_id;
        if ((size_t)id >= st->bullet_count || !st->bullets[id] || !st->bullets[id][0]) {
            return NULL;
        }
        sb_putn(&b, st->bullets[id], strlen(st->bullets[id]));
        sb_putn(&b, " ", 1);
        return sb_finish(&b);
    }
    if (ps->heading_type != 1 && ps->heading_type != 2) {
        return NULL;
    }
    id = ps->heading_type == 1 ? rs->outline_id : ps->heading_id;
    level = ps->heading_level;
    if (id < 0 || (size_t)id >= st->numbering_count || !st->numberings[id].valid) {
        return NULL;
    }
    nd = &st->numberings[id];
    if ((size_t)id >= rs->counter_count) {
        int(*g)[10] = (int(*)[10])realloc(rs->counters, st->numbering_count * sizeof(*g));
        if (!g) {
            return NULL;
        }
        memset(g + rs->counter_count, 0, (st->numbering_count - rs->counter_count) * sizeof(*g));
        rs->counters = g;
        rs->counter_count = st->numbering_count;
    }
    c = rs->counters[id];
    c[level] = c[level] ? c[level] + 1 : nd->start[level];
    for (k = level + 1; k < 10; k++) {
        c[k] = 0;
    }
    if (!nd->format[level]) {
        return NULL;
    }
    /* "^n" is level n's number; '^' before anything else is dropped */
    for (p = nd->format[level]; *p;) {
        if (*p == '^') {
            p++;
            if (*p >= '1' && *p <= '9') {
                int lv = *p++ - '0';
                if (lv == 1 && *p == '0') {
                    lv = 10;
                    p++;
                }
                format_number(&b, c[lv - 1] ? c[lv - 1] : nd->start[lv - 1], nd->num_format[lv - 1]);
            }
            continue;
        }
        sb_putn(&b, p, 1);
        p++;
    }
    if (!b.len) {
        free(b.s);
        return NULL;
    }
    sb_putn(&b, " ", 1); /* the gap before the text (textOffset) */
    *char_id = nd->char_id[level];
    return sb_finish(&b);
}

/* Put the paragraph number first; saved lines after the first move with it. */
static hp_status add_heading_prefix(hp_paragraph *para, const hp_xml_node *p, const hp_style_tables *st,
                                    long long para_pr_id, pos_track *pt) {
    int char_id;
    char *prefix = heading_prefix(st, para_pr_id, &char_id);
    hp_status rc;
    if (!prefix) {
        return HP_OK;
    }
    para->generated_prefix = utf16_units(prefix);
    pos_advance(pt, 0, para->generated_prefix);
    rc = add_run(para, prefix, st,
                 char_id >= 0 && (size_t)char_id < st->char_count && st->chars[char_id].valid
                     ? char_id
                     : attr_l(hp_xml_child(p, "run"), "charPrIDRef", 0));
    return rc;
}

static hp_status parse_cell_paragraph(const hp_xml_node *p, const hp_style_tables *st, hp_paragraph *out);

static void note_marker(sbuf *b, const hp_note_format *f, int number) {
    sb_putn(b, f->prefix, strlen(f->prefix));
    format_number(b, number, f->num_format);
    sb_putn(b, f->suffix, strlen(f->suffix));
}

/* Paragraphs of a header, footer or note (<hp:subList>), text only. */
static hp_status parse_subtext(const hp_xml_node *el, const hp_style_tables *st, hp_subtext *sub) {
    const hp_xml_node *list = hp_xml_child(el, "subList"), *p;
    const char *va = hp_xml_attr(list, "vertAlign");
    sub->vert_align = !va ? 0 : !strcmp(va, "CENTER") ? 1 : !strcmp(va, "BOTTOM") ? 2 : 0;
    for (p = hp_xml_child(list, "p"); p; p = hp_xml_next(p, "p")) {
        hp_paragraph q, *g;
        if (parse_cell_paragraph(p, st, &q) != HP_OK) {
            return HP_ERR_NOMEM;
        }
        g = (hp_paragraph *)realloc(sub->paragraphs, (sub->paragraph_count + 1) * sizeof(*g));
        if (!g) {
            paragraph_release(&q);
            return HP_ERR_NOMEM;
        }
        sub->paragraphs = g;
        sub->paragraphs[sub->paragraph_count++] = q;
    }
    return HP_OK;
}

static hp_status add_subtext(hp_section *sec, hp_subtext *sub) {
    hp_subtext *g = (hp_subtext *)realloc(sec->subtexts, (sec->subtext_count + 1) * sizeof(*g));
    if (!g) {
        size_t i;
        for (i = 0; i < sub->paragraph_count; i++) {
            paragraph_release(&sub->paragraphs[i]);
        }
        free(sub->paragraphs);
        return HP_ERR_NOMEM;
    }
    sec->subtexts = g;
    sec->subtexts[sec->subtext_count++] = *sub;
    return HP_OK;
}

/* One control inside <hp:ctrl>: footnote / endnote (number in the text, note
 * text kept aside), header / footer, automatic number. */
static hp_status add_control(hp_paragraph *para, const hp_xml_node *e, const hp_style_tables *st, long long char_id) {
    hp_read_state *rs = st->state;
    if (!rs) {
        return HP_OK;
    }
    if (hp_xml_is(e, "footNote") || hp_xml_is(e, "endNote")) {
        long long n = attr_l(e, "number", 0);
        int number = n > 0 && n < 100000 ? (int)n : rs->note_count + 1, saved = rs->current_note;
        sbuf b = {0};
        hp_subtext sub;
        hp_status rc;
        if (number > rs->note_count) {
            rs->note_count = number;
        }
        note_marker(&b, &rs->footnote, number);
        rc = add_run(para, sb_finish(&b), st, char_id);
        if (rc != HP_OK || !rs->sec) {
            return rc;
        }
        /* endnotes are shown as footnotes for now (on the page of the reference) */
        memset(&sub, 0, sizeof(sub));
        sub.kind = HP_SUB_FOOTNOTE;
        sub.block = rs->sec->block_count;
        rs->current_note = number;
        rc = parse_subtext(e, st, &sub);
        rs->current_note = saved;
        return rc == HP_OK ? add_subtext(rs->sec, &sub) : rc;
    }
    if (hp_xml_is(e, "header") || hp_xml_is(e, "footer")) {
        const char *apply = hp_xml_attr(e, "applyPageType");
        hp_subtext sub;
        hp_status rc;
        if (!rs->sec) {
            return HP_OK;
        }
        memset(&sub, 0, sizeof(sub));
        sub.kind = hp_xml_is(e, "header") ? HP_SUB_HEADER : HP_SUB_FOOTER;
        sub.apply = !apply ? 0 : !strcmp(apply, "EVEN") ? 1 : !strcmp(apply, "ODD") ? 2 : 0;
        sub.block = rs->sec->block_count;
        rc = parse_subtext(e, st, &sub);
        return rc == HP_OK ? add_subtext(rs->sec, &sub) : rc;
    }
    if (hp_xml_is(e, "autoNum")) {
        const char *type = hp_xml_attr(e, "numType");
        if (type && (!strcmp(type, "PAGE") || !strcmp(type, "TOTAL_PAGE"))) {
            hp_status rc = add_run(para, hp_strdup("1"), st, char_id);
            if (rc == HP_OK) {
                para->runs[para->run_count - 1].field = !strcmp(type, "PAGE") ? 1 : 2;
            }
            return rc;
        }
        if (type && (!strcmp(type, "FOOTNOTE") || !strcmp(type, "ENDNOTE")) && rs->current_note > 0) {
            sbuf b = {0};
            note_marker(&b, &rs->footnote, rs->current_note);
            return add_run(para, sb_finish(&b), st, char_id);
        }
    }
    return HP_OK;
}

/* A run child that is a control holder: each control counts 8 saved units. */
static hp_status add_ctrl(hp_paragraph *para, const hp_xml_node *ctrl, const hp_style_tables *st, long long char_id,
                          pos_track *pt) {
    const hp_xml_node *e;
    for (e = hp_xml_child(ctrl, NULL); e; e = hp_xml_next(e, NULL)) {
        uint32_t before = para_units(para);
        hp_status rc = add_control(para, e, st, char_id);
        if (rc != HP_OK) {
            return rc;
        }
        pos_advance(pt, 8, para_units(para) - before);
    }
    return HP_OK;
}

/* ---------- objects ---------- */

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

/* Placement from the object's own <hp:pos> (a direct child; nested objects have theirs). */
static void parse_float_pos(const hp_xml_node *el, hp_float_pos *pos, int *has_pos) {
    const hp_xml_node *p;
    const char *rel;
    memset(pos, 0, sizeof(*pos));
    pos->treat_as_char = 1;
    pos->wrap = HP_WRAP_TOP_AND_BOTTOM;
    pos->vert_rel_para = 1;
    pos->horz_rel_para = 1;
    *has_pos = 0;
    if (hp_xml_attr(el, "textWrap")) {
        pos->wrap = parse_wrap(hp_xml_attr(el, "textWrap"));
        *has_pos = 1;
    }
    p = hp_xml_child(el, "pos");
    if (!p) {
        return;
    }
    *has_pos = 1;
    pos->treat_as_char = (int)attr_l(p, "treatAsChar", 1);
    /* Offsets are HWPUNIT bit patterns — interpret as signed int32. */
    pos->vert_offset_hwp = hp_hwp_signed(attr_u32(p, "vertOffset", 0));
    pos->horz_offset_hwp = hp_hwp_signed(attr_u32(p, "horzOffset", 0));
    if ((rel = hp_xml_attr(p, "vertRelTo")) != NULL) {
        pos->vert_rel_para = strcmp(rel, "PARA") == 0;
    }
    if ((rel = hp_xml_attr(p, "horzRelTo")) != NULL) {
        pos->horz_rel_para = strcmp(rel, "PARA") == 0;
    }
}

/* Size from the object's own <hp:sz> (pictures fall back to <hp:orgSz>). */
static void parse_size(const hp_xml_node *el, uint32_t *w, uint32_t *h, int use_org_fallback) {
    const hp_xml_node *sz = hp_xml_child(el, "sz");
    if (!sz && use_org_fallback) {
        sz = hp_xml_child(el, "orgSz");
    }
    if (sz) {
        *w = (uint32_t)attr_l(sz, "width", 0);
        *h = (uint32_t)attr_l(sz, "height", 0);
    }
}

static hp_image *parse_pic(const hp_xml_node *pic, const hp_style_tables *st) {
    const hp_xml_node *img = hp_xml_child(pic, "img");
    const char *bin_id = img ? hp_xml_attr(img, "binaryItemIDRef") : NULL;
    hp_image *image;
    const hp_bin_item *bin;
    if (!bin_id) {
        return NULL;
    }
    image = (hp_image *)calloc(1, sizeof(*image));
    if (!image) {
        return NULL;
    }
    image->binary_id = hp_strdup(bin_id);
    parse_size(pic, &image->width_hwp, &image->height_hwp, 1);
    {
        const hp_xml_node *clip=hp_xml_child(pic,"imgClip"),*dim=hp_xml_child(pic,"imgDim");
        image->clip_left=attr_u32(clip,"left",0);image->clip_top=attr_u32(clip,"top",0);
        image->clip_right=attr_u32(clip,"right",0);image->clip_bottom=attr_u32(clip,"bottom",0);
        image->image_dim_width=attr_u32(dim,"dimwidth",0);image->image_dim_height=attr_u32(dim,"dimheight",0);
    }
    parse_float_pos(pic, &image->pos, &image->has_pos);
    bin = find_bin(st, bin_id);
    if (bin && bin->len) {
        image->data = (unsigned char *)malloc(bin->len);
        if (image->data) {
            memcpy(image->data, bin->data, bin->len);
            image->data_len = bin->len;
        }
    }
    return image;
}

static void image_free(hp_image *im) {
    if (im) {
        free(im->binary_id);
        free(im->data);
        free(im);
    }
}

static hp_equation *parse_equation(const hp_xml_node *eqn) {
    hp_equation *eq = (hp_equation *)calloc(1, sizeof(*eq));
    const hp_xml_node *script;
    if (!eq) {
        return NULL;
    }
    eq->font_size_hwp = 1200;
    script = hp_xml_find(eqn, "script");
    if (script && !is_empty_element_tag(script)) {
        eq->script = element_text(script);
    }
    if (!eq->script) {
        eq->script = element_text(eqn);
    }
    if (!eq->script || !eq->script[0]) {
        free(eq->script);
        eq->script = hp_strdup("[수식]");
    }
    return eq;
}

static hp_container *parse_container(const hp_xml_node *cn, const hp_style_tables *st) {
    hp_container *c = (hp_container *)calloc(1, sizeof(*c));
    hp_xml_node *pic;
    if (!c) {
        return NULL;
    }
    parse_size(cn, &c->width_hwp, &c->height_hwp, 0);
    parse_float_pos(cn, &c->pos, &c->has_pos);
    for (pic = hp_xml_find(cn, "pic"); pic; pic = hp_xml_find_after(cn, pic, "pic")) {
        hp_image *im = parse_pic(pic, st);
        hp_image **arr;
        if (!im) {
            continue;
        }
        arr = (hp_image **)realloc(c->images, (c->image_count + 1) * sizeof(hp_image *));
        if (!arr) {
            image_free(im);
            continue;
        }
        c->images = arr;
        c->images[c->image_count++] = im;
    }
    if (c->image_count == 0) {
        free(c->images);
        free(c);
        return NULL;
    }
    return c;
}

static hp_table *parse_table(const hp_xml_node *tbl, const hp_style_tables *st);

static int cell_append_nested(hp_table_cell *cell, hp_table *nested) {
    hp_table **arr;size_t *positions;
    if (!nested) {
        return 0;
    }
    arr = (hp_table **)realloc(cell->nested_tables, (cell->nested_table_count + 1) * sizeof(hp_table *));
    if (!arr) {
        return -1;
    }
    cell->nested_tables = arr;
    positions=(size_t *)realloc(cell->nested_before_paragraph,(cell->nested_table_count+1)*sizeof(size_t));
    if(!positions)return -1;
    cell->nested_before_paragraph=positions;
    positions[cell->nested_table_count]=cell->paragraph_count;
    cell->nested_tables[cell->nested_table_count++] = nested;
    return 0;
}

static int is_blank(const char *s) {
    for (; s && *s; s++) {
        if (*s != ' ' && *s != '\t' && *s != '\n') {
            return 0;
        }
    }
    return 1;
}

static int run_has_object(const hp_xml_node *run) {
    const hp_xml_node *c;
    for (c = hp_xml_child(run, NULL); c; c = hp_xml_next(c, NULL)) {
        if (is_object(c)) {
            return 1;
        }
    }
    return 0;
}

/* Text in a run that also holds an object sits on the object's line; only visible
 * text is kept (a lone space must not turn into an extra blank paragraph). */
static hp_status add_text_piece(hp_paragraph *para, char *text, const hp_style_tables *st, long long char_id,
                                int beside_object) {
    if (text && beside_object && is_blank(text)) {
        free(text);
        return HP_OK;
    }
    return add_run(para, text, st, char_id);
}

/* Add the text of one non-object run child as runs of `para`. Empty <hp:t/> adds an
 * empty run only when keep_empty is set (cells keep blank lines that way). */
static hp_status add_child_text(hp_paragraph *para, const hp_xml_node *child, const hp_style_tables *st,
                                long long char_id, int keep_empty, int beside_object) {
    hp_xml_node *t;
    if (hp_xml_is(child, "t")) {
        if (hp_xml_child(child,"tab")) {
            const hp_xml_node *part;
            for(part=child->first_child;part;part=part->next) {
                hp_status rc=HP_OK;
                if(part->type==HP_XML_TEXT) {
                    sbuf text={0};sb_putn(&text,part->text,part->text_len);
                    rc=add_text_piece(para,sb_finish(&text),st,char_id,0);
                } else if(hp_xml_is(part,"tab")) {
                    rc=add_run(para,hp_strdup("\t"),st,char_id);
                    if(rc==HP_OK){para->runs[para->run_count-1].tab_width_hwp=attr_u32(part,"width",0);
                        para->runs[para->run_count-1].tab_leader=(int)attr_l(part,"leader",0);
                        para->runs[para->run_count-1].tab_align_right=attr_l(part,"type",0)==2;}
                } else if(hp_xml_is(part,"lineBreak") || hp_xml_is(part,"lb") || hp_xml_is(part,"fwSpace") || hp_xml_is(part,"nbSpace"))
                    rc=add_text_piece(para,hp_strdup(" "),st,char_id,0);
                else if(hp_xml_is(part,"hyphen"))rc=add_text_piece(para,hp_strdup("-"),st,char_id,0);
                else rc=add_text_piece(para,element_text(part),st,char_id,0);
                if(rc!=HP_OK)return rc;
            }
            return HP_OK;
        }
        if (is_empty_element_tag(child)) {
            return keep_empty && !beside_object ? add_run(para, hp_strdup(""), st, char_id) : HP_OK;
        }
        return add_text_piece(para, element_text(child), st, char_id, beside_object);
    }
    for (t = hp_xml_find(child, "t"); t; t = hp_xml_find_after(child, t, "t")) {
        hp_status rc;
        if (is_empty_element_tag(t)) {
            continue;
        }
        rc = add_text_piece(para, element_text(t), st, char_id, beside_object);
        if (rc != HP_OK) {
            return rc;
        }
    }
    return HP_OK;
}

/* Text paragraph inside a table cell (its objects are collected by
 * cell_harvest_objects). */
/* Text of one run child, keeping the saved-position map in step. */
static hp_status add_run_child(hp_paragraph *para, const hp_xml_node *c, const hp_style_tables *st,
                               long long char_id, int keep_empty, int beside, pos_track *pt) {
    uint32_t before = para_units(para), chars = pt->chars, added;
    hp_status rc;
    if (hp_xml_is(c, "ctrl")) {
        return add_ctrl(para, c, st, char_id, pt);
    }
    if (!run_child_has_text(c)) {
        pos_advance(pt, 8, 0); /* secPr and other controls */
        return HP_OK;
    }
    rc = add_child_text(para, c, st, char_id, keep_empty, beside);
    if (rc != HP_OK) {
        return rc;
    }
    added = para_units(para) - before;
    if (hp_xml_is(c, "t")) {
        pos_text(pt, c);
        if (pt->chars - chars != added) {
            /* blank text beside an object is dropped */
            pt->chars = chars;
            pos_advance(pt, 0, added);
        }
    } else {
        pos_advance(pt, 8, added); /* a drawing's text */
    }
    return HP_OK;
}

static hp_status parse_cell_paragraph(const hp_xml_node *p, const hp_style_tables *st, hp_paragraph *out) {
    hp_xml_node *run;
    int object=0,leading=1,prefix_count=0,object_count=0;size_t i;
    pos_track pt = {0};
    memset(out, 0, sizeof(*out));
    apply_para_style(out, st, attr_l(p, "paraPrIDRef", 0));
    parse_linesegs(p, out);
    if (add_heading_prefix(out, p, st, attr_l(p, "paraPrIDRef", 0), &pt) != HP_OK) {
        paragraph_release(out);
        free(pt.v);
        return HP_ERR_NOMEM;
    }
    for (run = hp_xml_child(p, "run"); run; run = hp_xml_next(run, "run")) {
        long long char_id = attr_l(run, "charPrIDRef", 0);
        int beside = run_has_object(run);
        object |= beside;
        hp_xml_node *c;
        for (c = hp_xml_child(run, NULL); c; c = hp_xml_next(c, NULL)) {
            if(is_object(c)) {
                object_count++;
                pos_advance(&pt, 8, 0);
                if(leading && hp_xml_is(c,"tbl") && attr_l(hp_xml_child(c,"pos"),"treatAsChar",0)) {
                    out->inline_prefix_width_hwp+=attr_u32(hp_xml_child(c,"sz"),"width",0);
                    prefix_count++;
                } else leading=0;
                continue;
            }
            if (add_run_child(out, c, st, char_id, 1, beside, &pt) != HP_OK) {
                paragraph_release(out);
                free(pt.v);
                return HP_ERR_NOMEM;
            }
            if(out->run_count && !is_blank(out->runs[out->run_count-1].text))leading=0;
        }
    }
    pos_apply(out->segs, out->seg_count, &pt);
    free(pt.v);
    if(!out->run_count) {
        /* an empty line is as tall as its paragraph mark's character shape */
        hp_run mark;
        memset(&mark,0,sizeof(mark));
        apply_char_style(&mark,st,attr_l(hp_xml_child(p,"run"),"charPrIDRef",0));
        out->mark_size_hwp=mark.font_size_hwp;
        free(mark.font_face);
    }
    if(object) {
        out->object_anchor_only=1;
        for(i=0;i<out->run_count;i++)if(!is_blank(out->runs[i].text))out->object_anchor_only=0;
        /* (the leading table's 8 saved units are mapped by pos_apply) */
        if(out->object_anchor_only || prefix_count!=1 || object_count!=1)out->inline_prefix_width_hwp=0;
    }
    return HP_OK;
}

/* Tables / pictures / drawings / equations anchored inside a cell paragraph. */
static hp_status cell_harvest_objects(const hp_xml_node *p, const hp_style_tables *st, hp_table_cell *cell) {
    hp_xml_node *n = hp_xml_find(p, NULL);
    while (n) {
        if (hp_xml_is(n, "tbl")) {
            if (cell_append_nested(cell, parse_table(n, st)) != 0) return HP_ERR_NOMEM;
        } else if (hp_xml_is(n, "pic")) {
            if (!cell->container && !cell->image) {
                cell->image = parse_pic(n, st);
            }
        } else if (hp_xml_is(n, "container")) {
            if (!cell->container) {
                cell->container = parse_container(n, st);
            }
        } else if (hp_xml_is(n, "equation")) {
            if (!cell->equation) {
                cell->equation = parse_equation(n);
            }
        } else {
            n = hp_xml_find_next(p, n, NULL);
            continue;
        }
        n = hp_xml_find_after(p, n, NULL);
    }
    return HP_OK;
}

static hp_status parse_cell_content(const hp_xml_node *tc, const hp_style_tables *st, hp_table_cell *cell) {
    hp_xml_node *n = hp_xml_find(tc, NULL);
    while (n) {
        if (hp_xml_is(n, "tbl")) {
            if (cell_append_nested(cell, parse_table(n, st)) != 0) {
                return HP_ERR_NOMEM;
            }
        } else if (hp_xml_is(n, "pic")) {
            if (!cell->image) {
                cell->image = parse_pic(n, st);
            }
        } else if (hp_xml_is(n, "p")) {
            hp_paragraph para;
            if (cell_harvest_objects(n, st, cell) != HP_OK || parse_cell_paragraph(n, st, &para) != HP_OK) {
                return HP_ERR_NOMEM;
            }
            para.source_p = p_index(st, n);
            if (para.run_count > 0 || para.seg_count > 0 || para.object_anchor_only) {
                hp_paragraph *arr =
                    (hp_paragraph *)realloc(cell->paragraphs, (cell->paragraph_count + 1) * sizeof(*arr));
                if (!arr) {
                    paragraph_release(&para);
                    return HP_ERR_NOMEM;
                }
                cell->paragraphs = arr;
                cell->paragraphs[cell->paragraph_count++] = para;
            } else {
                paragraph_release(&para);
            }
        } else {
            n = hp_xml_find_next(tc, n, NULL);
            continue;
        }
        n = hp_xml_find_after(tc, n, NULL);
    }
    return HP_OK;
}

/* A cell's saved line layout must have been made for that cell: Hancom's lines
 * end at the cell's inner width minus the paragraph indents. Some files (Hancom's
 * own HWPX -> HWP conversion among them) store placeholder lines instead — the
 * default 150 mm body width and every line at vertpos 0 — which would wrap at the
 * wrong width and draw lines on top of each other. Such lines are dropped and the
 * viewer lays the paragraph out itself. */
static void drop_implausible_cell_lines(hp_table_cell *cell) {
    size_t i, k;
    int64_t inner = (int64_t)cell->width_hwp - (int64_t)cell->pad_left_hwp - (int64_t)cell->pad_right_hwp;
    if (!cell->width_hwp || inner < 3000) {
        return; /* tiny cells: small overhangs are normal */
    }
    for (i = 0; i < cell->paragraph_count; i++) {
        hp_paragraph *para = &cell->paragraphs[i];
        int64_t expected = inner - para->indent_left_hwp - para->indent_right_hwp;
        int64_t tolerance = expected / 50 > 300 ? expected / 50 : 300;
        int bad = 0;
        if (para->object_anchor_only) {
            continue; /* the object's own line */
        }
        for (k = 0; k < para->seg_count && !bad; k++) {
            const hp_line_seg *g = &para->segs[k];
            int64_t end = (int64_t)hp_hwp_signed(g->horzpos_hwp) + (int64_t)hp_hwp_signed(g->horzsize_hwp);
            if (end > expected + tolerance || end < expected - tolerance) {
                bad = 1; /* laid out for another width */
            }
            if (k > 0 && g->vertpos_hwp <= para->segs[k - 1].vertpos_hwp) {
                bad = 1; /* lines on top of each other */
            }
        }
        if (bad) {
            free(para->segs);
            para->segs = NULL;
            para->seg_count = 0;
        }
    }
}

static hp_table *parse_table(const hp_xml_node *tbl, const hp_style_tables *st) {
    hp_table *table = (hp_table *)calloc(1, sizeof(*table));
    hp_xml_node *tr, *tc;
    if (!table) {
        return NULL;
    }
    table->row_count = (int)attr_l(tbl, "rowCnt", 0);
    table->col_count = (int)attr_l(tbl, "colCnt", 0);
    table->repeat_header = attr_l(tbl, "repeatHeader", 0) != 0;
    parse_size(tbl, &table->width_hwp, &table->height_hwp, 0);
    parse_float_pos(tbl, &table->pos, &table->has_pos);
    {
        const hp_xml_node *anchor=tbl->parent,*margin=hp_xml_child(tbl,"outMargin");
        table->outer_left_hwp=attr_u32(margin,"left",0);
        table->outer_right_hwp=attr_u32(margin,"right",0);
        while(anchor&&!hp_xml_is(anchor,"p"))anchor=anchor->parent;
        if(anchor&&table->pos.treat_as_char){
            hp_paragraph style={0};apply_para_style(&style,st,attr_l(anchor,"paraPrIDRef",0));
            table->has_anchor_align=1;table->anchor_align=style.align;
            table->anchor_left_hwp=style.indent_left_hwp;table->anchor_right_hwp=style.indent_right_hwp;
        }
    }
    for (tr = hp_xml_find(tbl, "tr"); tr; tr = hp_xml_find_after(tbl, tr, "tr")) {
        for (tc = hp_xml_find(tr, "tc"); tc; tc = hp_xml_find_after(tr, tc, "tc")) {
            hp_table_cell cell;
            hp_table_cell *arr;
            const hp_xml_node *addr, *span, *csz, *cm;
            memset(&cell, 0, sizeof(cell));
            cell.is_header = attr_l(tc, "header", 0) != 0;
            cell.fill_rgb = 0xFFFFFFFFu;
            /* the cell's own properties (direct children; nested tables have theirs) */
            addr = hp_xml_child(tc, "cellAddr");
            span = hp_xml_child(tc, "cellSpan");
            csz = hp_xml_child(tc, "cellSz");
            cm = hp_xml_child(tc, "cellMargin");
            if(!attr_l(tc,"hasMargin",1) && hp_xml_child(tbl,"inMargin"))cm=hp_xml_child(tbl,"inMargin");
            {
                const char *va=hp_xml_attr(hp_xml_child(tc,"subList"),"vertAlign");
                cell.vertical_align=va && strcmp(va,"CENTER")==0 ? 1 : va && strcmp(va,"BOTTOM")==0 ? 2 : 0;
            }
            if (addr) {
                cell.col = (int)attr_l(addr, "colAddr", 0);
                cell.row = (int)attr_l(addr, "rowAddr", 0);
            }
            cell.colspan = span ? (int)attr_l(span, "colSpan", 1) : 1;
            cell.rowspan = span ? (int)attr_l(span, "rowSpan", 1) : 1;
            if (csz) {
                cell.width_hwp = (uint32_t)attr_l(csz, "width", 0);
                cell.height_hwp = (uint32_t)attr_l(csz, "height", 0);
            }
            apply_border_style(&cell, st, attr_l(tc, "borderFillIDRef", -1));
            cell.pad_left_hwp = cm ? (uint32_t)attr_l(cm, "left", 140) : 140;
            cell.pad_right_hwp = cm ? (uint32_t)attr_l(cm, "right", 140) : 140;
            cell.pad_top_hwp = cm ? (uint32_t)attr_l(cm, "top", 140) : 140;
            cell.pad_bottom_hwp = cm ? (uint32_t)attr_l(cm, "bottom", 140) : 140;
            parse_cell_content(tc, st, &cell);
            drop_implausible_cell_lines(&cell);
            arr = (hp_table_cell *)realloc(table->cells, (table->cell_count + 1) * sizeof(*arr));
            if (!arr) {
                break; /* cell contents leak only on OOM; table stays consistent */
            }
            table->cells = arr;
            table->cells[table->cell_count++] = cell;
        }
    }
    return table;
}

/* ---------- section ---------- */

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

/* Deep copy of a paragraph into the flat sec->paragraphs list (used by the writer
 * for appended text and by hp_document_plain_text). */
static hp_status append_flat_para(hp_section *sec, const hp_paragraph *src) {
    hp_paragraph *paras, *dst;
    size_t i;
    if (src->run_count == 0) {
        return HP_OK;
    }
    paras = (hp_paragraph *)realloc(sec->paragraphs, (sec->paragraph_count + 1) * sizeof(*paras));
    if (!paras) {
        return HP_ERR_NOMEM;
    }
    sec->paragraphs = paras;
    dst = &sec->paragraphs[sec->paragraph_count];
    *dst = *src;
    dst->runs = NULL;
    dst->run_count = 0;
    dst->segs = NULL;
    dst->seg_count = 0;
    if (src->seg_count) {
        dst->segs = (hp_line_seg *)malloc(src->seg_count * sizeof(hp_line_seg));
        if (dst->segs) {
            memcpy(dst->segs, src->segs, src->seg_count * sizeof(hp_line_seg));
            dst->seg_count = src->seg_count;
        }
    }
    dst->runs = (hp_run *)calloc(src->run_count, sizeof(hp_run));
    if (!dst->runs) {
        free(dst->segs);
        return HP_ERR_NOMEM;
    }
    for (i = 0; i < src->run_count; i++) {
        dst->runs[i] = src->runs[i];
        dst->runs[i].text = hp_strdup(src->runs[i].text ? src->runs[i].text : "");
        dst->runs[i].font_face = src->runs[i].font_face ? hp_strdup(src->runs[i].font_face) : NULL;
    }
    dst->run_count = src->run_count;
    sec->paragraph_count++;
    return HP_OK;
}

/* Text gathered so far becomes a paragraph block (and a flat paragraph). */
static hp_status flush_pending(hp_section *sec, hp_paragraph *pending, const hp_style_tables *st, long long para_pr_id,
                               int *emitted) {
    hp_block blk;
    hp_status rc;
    if (pending->run_count == 0) {
        return HP_OK;
    }
    rc = append_flat_para(sec, pending);
    if (rc != HP_OK) {
        return rc;
    }
    memset(&blk, 0, sizeof(blk));
    blk.kind = HP_BLOCK_PARAGRAPH;
    blk.paragraph = *pending;
    rc = append_block(sec, &blk);
    if (rc != HP_OK) {
        return rc;
    }
    memset(pending, 0, sizeof(*pending));
    apply_para_style(pending, st, para_pr_id);
    *emitted = 1;
    return HP_OK;
}

static hp_status add_object_block(hp_section *sec, hp_block_kind kind, void *obj) {
    hp_block blk;
    if (!obj) {
        return HP_OK;
    }
    memset(&blk, 0, sizeof(blk));
    blk.kind = kind;
    switch (kind) {
    case HP_BLOCK_TABLE:
        blk.table = (hp_table *)obj;
        break;
    case HP_BLOCK_IMAGE:
        blk.image = (hp_image *)obj;
        break;
    case HP_BLOCK_EQUATION:
        blk.equation = (hp_equation *)obj;
        break;
    default:
        blk.container = (hp_container *)obj;
        break;
    }
    return append_block(sec, &blk); /* on OOM the object leaks; reading fails anyway */
}

static hp_status parse_section_paragraph(const hp_xml_node *p, hp_section *sec, const hp_style_tables *st) {
    long long para_pr_id = attr_l(p, "paraPrIDRef", 0);
    hp_paragraph pending;
    hp_xml_node *run;
    hp_status rc = HP_OK;
    int emitted = 0;
    hp_align align;
    pos_track pt = {0};
    hp_line_seg *segs;
    size_t seg_count;

    memset(&pending, 0, sizeof(pending));
    apply_para_style(&pending, st, para_pr_id);
    parse_linesegs(p, &pending);
    align = pending.align;
    /* the saved lines stay with the first piece (it owns them once flushed) */
    segs = pending.segs;
    seg_count = pending.seg_count;
    rc = add_heading_prefix(&pending, p, st, para_pr_id, &pt);

    /* Run children in document order: text accumulates into the pending paragraph;
     * a table / picture / drawing / equation ends it and becomes its own block, and
     * text after the object starts a new paragraph piece. */
    for (run = hp_xml_child(p, "run"); run && rc == HP_OK; run = hp_xml_next(run, "run")) {
        long long char_id = attr_l(run, "charPrIDRef", 0);
        int beside = run_has_object(run);
        hp_xml_node *c;
        for (c = hp_xml_child(run, NULL); c && rc == HP_OK; c = hp_xml_next(c, NULL)) {
            if (is_object(c)) {
                size_t ri;
                pos_advance(&pt, 8, 0);
                pending.object_anchor_only=pending.run_count>0;
                for(ri=0;ri<pending.run_count;ri++)if(!is_blank(pending.runs[ri].text))pending.object_anchor_only=0;
                rc = flush_pending(sec, &pending, st, para_pr_id, &emitted);
                if (rc == HP_OK) {
                    void *obj;
                    hp_block_kind kind;
                    if (hp_xml_is(c, "tbl")) {
                        obj = parse_table(c, st);
                        kind = HP_BLOCK_TABLE;
                    } else if (hp_xml_is(c, "pic")) {
                        obj = parse_pic(c, st);
                        kind = HP_BLOCK_IMAGE;
                    } else if (hp_xml_is(c, "container")) {
                        obj = parse_container(c, st);
                        kind = HP_BLOCK_CONTAINER;
                    } else {
                        obj = parse_equation(c);
                        kind = HP_BLOCK_EQUATION;
                    }
                    rc = add_object_block(sec, kind, obj);
                    emitted |= obj != NULL;
                }
            } else {
                rc = add_run_child(&pending, c, st, char_id, 0, beside, &pt);
            }
        }
    }
    if (rc == HP_OK) {
        pos_apply(segs, seg_count, &pt);
    }
    free(pt.v);
    if (rc != HP_OK) {
        paragraph_release(&pending);
        return rc;
    }
    if (pending.run_count > 0) {
        size_t ri;
        pending.object_anchor_only=emitted;
        for(ri=0;ri<pending.run_count;ri++)if(!is_blank(pending.runs[ri].text))pending.object_anchor_only=0;
        return flush_pending(sec, &pending, st, para_pr_id, &emitted);
    }
    if (!emitted) {
        hp_block blk;
        if(add_run(&pending,hp_strdup(""),st,attr_l(hp_xml_child(p,"run"),"charPrIDRef",0))!=HP_OK) {paragraph_release(&pending);return HP_ERR_NOMEM;}
        pending.align=align;
        memset(&blk,0,sizeof(blk));blk.kind=HP_BLOCK_PARAGRAPH;blk.paragraph=pending;
        return append_block(sec,&blk);
    }
    paragraph_release(&pending);

    return HP_OK;
}

static hp_status parse_section(const hp_xml_node *root, hp_section *sec, const hp_style_tables *st) {
    const hp_xml_node *pg, *col;
    hp_xml_node *n;
    hp_status rc = HP_OK;
    int have_saved_y = 0;
    long long previous_saved_y = 0;

    hp_page_default(&sec->page);
    sec->column_count = 1;
    sec->column_gap_hwp = 0;
    sec->page_number_position=-1;
    {
        const hp_xml_node *pr=hp_xml_find(root,"secPr");
        long long start=attr_l(hp_xml_child(pr,"startNum"),"page",0);
        sec->page_number_start=start>0 && start<=1000000 ? (int)start : 0;
        sec->hide_first_page_number=attr_l(hp_xml_child(pr,"visibility"),"hideFirstPageNum",0)!=0;
        /* footnote numbers and separator (0.12.0) */
        {
            const hp_xml_node *fn = hp_xml_child(pr, "footNotePr"), *fmt = hp_xml_child(fn, "autoNumFormat");
            const hp_xml_node *line = hp_xml_child(fn, "noteLine"), *gap = hp_xml_child(fn, "noteSpacing");
            const char *type = hp_xml_attr(line, "type");
            sec->footnote_shape.line_length_hwp = (int32_t)attr_l(line, "length", -1);
            sec->footnote_shape.line_type = !type || strcmp(type, "NONE") != 0;
            sec->footnote_shape.line_color = parse_color(hp_xml_attr(line, "color"));
            sec->footnote_shape.above_hwp = attr_u32(gap, "aboveLine", 850);
            sec->footnote_shape.below_hwp = attr_u32(gap, "belowLine", 567);
            sec->footnote_shape.between_hwp = attr_u32(gap, "betweenNotes", 283);
            if (st->state) {
                hp_note_format *f = &st->state->footnote;
                const char *pre = hp_xml_attr(fmt, "prefixChar"), *suf = hp_xml_attr(fmt, "suffixChar");
                st->state->sec = sec;
                st->state->outline_id = (int)attr_l(pr, "outlineShapeIDRef", 0);
                f->num_format = number_format_code(hp_xml_attr(fmt, "type"));
                snprintf(f->prefix, sizeof(f->prefix), "%s", pre ? pre : "");
                snprintf(f->suffix, sizeof(f->suffix), "%s", fmt ? (suf ? suf : "") : ")");
            }
        }
    }

    pg = hp_xml_find(root, "pagePr");
    if (pg) {
        const hp_xml_node *mg = hp_xml_find(pg, "margin");
        sec->page.width = (uint32_t)attr_l(pg, "width", (long long)sec->page.width);
        sec->page.height = (uint32_t)attr_l(pg, "height", (long long)sec->page.height);
        if (mg) {
            sec->page.margin_left = (uint32_t)attr_l(mg, "left", (long long)sec->page.margin_left);
            sec->page.margin_right = (uint32_t)attr_l(mg, "right", (long long)sec->page.margin_right);
            sec->page.margin_top = (uint32_t)attr_l(mg, "top", (long long)sec->page.margin_top);
            sec->page.header_height=attr_u32(mg,"header",0);
            sec->page.footer_height=attr_u32(mg,"footer",0);
            sec->page.margin_bottom = (uint32_t)attr_l(mg, "bottom", (long long)sec->page.margin_bottom);
        }
    }
    col = hp_xml_find(root, "colPr");
    if (col) {
        long long cc = attr_l(col, "colCount", 1);
        sec->column_count = cc < 1 ? 1 : (int)cc;
        sec->column_gap_hwp = (uint32_t)attr_l(col, "sameGap", 0);
    }

    /* Paragraphs (and any object placed directly in the section) in document order. */
    n = hp_xml_find(root, NULL);
    while (n && rc == HP_OK) {
        if (hp_xml_is(n, "p")) {
            size_t first = sec->block_count;
            const hp_xml_node *lines = hp_xml_child(n, "linesegarray");
            const hp_xml_node *line = hp_xml_child(lines, "lineseg");
            int saved_break = line && have_saved_y && attr_l(line, "vertpos", 0) < previous_saved_y;
            long long style_id = attr_l(n, "paraPrIDRef", 0);
            int style_break = style_id >= 0 && (size_t)style_id < st->para_count &&
                st->paras[style_id].valid && st->paras[style_id].page_break_before;
            rc = parse_section_paragraph(n, sec, st);
            {
                size_t bi;
                for (bi = first; rc == HP_OK && bi < sec->block_count; bi++) {
                    if (sec->blocks[bi].kind == HP_BLOCK_PARAGRAPH) {
                        sec->blocks[bi].paragraph.source_p = p_index(st, n);
                    }
                }
            }
            /* A floating table is anchored to the paragraph, not an inline
             * character. Text serialized after it still occupies the anchor
             * line above a table with a positive vertical paragraph offset. */
            if(line && sec->block_count==first+2 && sec->blocks[first].kind==HP_BLOCK_TABLE &&
               sec->blocks[first+1].kind==HP_BLOCK_PARAGRAPH) {
                hp_table *table=sec->blocks[first].table;
                if(table && table->has_pos && !table->pos.treat_as_char && table->pos.vert_rel_para &&
                   table->pos.vert_offset_hwp>0 && !sec->blocks[first+1].paragraph.object_anchor_only) {
                    hp_block object=sec->blocks[first];
                    sec->blocks[first]=sec->blocks[first+1];sec->blocks[first+1]=object;
                    sec->blocks[first].has_saved_y=1;sec->blocks[first].saved_y_hwp=attr_u32(line,"vertpos",0);
                    sec->blocks[first+1].has_saved_y=1;
                    sec->blocks[first+1].saved_y_hwp=attr_u32(line,"vertpos",0)+(uint32_t)table->pos.vert_offset_hwp;
                    table->pos.vert_offset_hwp=0; /* resolved into the block's page coordinate */
                }
            }
            if (sec->block_count > first) {
                sec->blocks[first].page_break_before = attr_l(n, "pageBreak", 0) != 0 || style_break;
                sec->blocks[first].column_break_before = attr_l(n, "columnBreak", 0) != 0;
                sec->blocks[first].saved_break_before = saved_break;
                {
                    const hp_xml_node *ctrl;
                    for(ctrl=hp_xml_find(n,"pageHiding");ctrl;ctrl=hp_xml_find_after(n,ctrl,"pageHiding"))
                        if(attr_l(ctrl,"hidePageNum",0))sec->blocks[first].hide_page_number=1;
                    for(ctrl=hp_xml_find(n,"newNum");ctrl;ctrl=hp_xml_find_after(n,ctrl,"newNum")) {
                        const char *type=hp_xml_attr(ctrl,"numType");long long value=attr_l(ctrl,"num",0);
                        if(type && strcmp(type,"PAGE")==0 && value>0 && value<=1000000)
                            sec->blocks[first].page_number_reset=(int)value;
                    }
                }
                /* A single rendered block can retain the paragraph's position.
                 * Mixed text/object paragraphs keep sequential flow. */
                if(line && sec->block_count==first+1) {
                    sec->blocks[first].has_saved_y=1;
                    sec->blocks[first].saved_y_hwp=attr_u32(line,"vertpos",0);
                }
            }
            for (; line; line = hp_xml_next(line, "lineseg")) {
                previous_saved_y = attr_l(line, "vertpos", 0);
                have_saved_y = 1;
            }
        } else if (hp_xml_is(n, "tbl")) {
            rc = add_object_block(sec, HP_BLOCK_TABLE, parse_table(n, st));
        } else if (hp_xml_is(n, "pic")) {
            rc = add_object_block(sec, HP_BLOCK_IMAGE, parse_pic(n, st));
        } else if (hp_xml_is(n, "container")) {
            rc = add_object_block(sec, HP_BLOCK_CONTAINER, parse_container(n, st));
        } else if (hp_xml_is(n, "equation")) {
            rc = add_object_block(sec, HP_BLOCK_EQUATION, parse_equation(n));
        } else {
            n = hp_xml_find_next(root, n, NULL);
            continue;
        }
        n = hp_xml_find_after(root, n, NULL);
    }
    {
        const hp_xml_node *pn=hp_xml_find(root,"pageNum");
        const char *pos=hp_xml_attr(pn,"pos"),*side=hp_xml_attr(pn,"sideChar");
        static const char *const positions[]={"NONE","TOP_LEFT","TOP_CENTER","TOP_RIGHT","BOTTOM_LEFT","BOTTOM_CENTER","BOTTOM_RIGHT",
            "TOP_OUTSIDE","BOTTOM_OUTSIDE","TOP_INSIDE","BOTTOM_INSIDE"};
        size_t i;
        if(pn) {
            sec->page_number_position=5;
            for(i=0;i<sizeof(positions)/sizeof(positions[0]);i++)if(pos && strcmp(pos,positions[i])==0)sec->page_number_position=(int)i;
            if(side)strncpy(sec->page_number_side,side,sizeof(sec->page_number_side)-1);
        }
    }
    return rc;
}

/* ---------- package ---------- */

static hp_status parse_part(const hp_package *pkg, const char *name, hp_xml_doc **out) {
    const hp_package_entry *e = hp_package_find(pkg, name);
    *out = NULL;
    if (!e || e->is_dir) {
        return HP_ERR_INVALID;
    }
    return hp_xml_parse((const char *)e->data, e->len, out, NULL, 0);
}

/* Section parts in reading order, from content.hpf: <opf:spine> itemrefs resolved
 * through <opf:manifest> (hrefs relative to Contents/ are accepted). Falls back to
 * Contents/section0.xml, section1.xml, ... when the package lists none. */
static hp_status list_sections(const hp_package *pkg, const hp_xml_node *hpf, char ***out, size_t *out_count) {
    char **names = NULL;
    size_t count = 0;
    const hp_xml_node *manifest = hpf ? hp_xml_find(hpf, "manifest") : NULL;
    const hp_xml_node *spine = hpf ? hp_xml_find(hpf, "spine") : NULL;
    hp_xml_node *ref;
    *out = NULL;
    *out_count = 0;
    for (ref = spine ? hp_xml_child(spine, "itemref") : NULL; ref; ref = hp_xml_next(ref, "itemref")) {
        const char *idref = hp_xml_attr(ref, "idref");
        hp_xml_node *item;
        const char *href = NULL, *base;
        char name[512];
        char **grown;
        for (item = manifest ? hp_xml_child(manifest, "item") : NULL; item && idref; item = hp_xml_next(item, "item")) {
            const char *id = hp_xml_attr(item, "id");
            if (id && strcmp(id, idref) == 0) {
                href = hp_xml_attr(item, "href");
                break;
            }
        }
        if (!href) {
            continue;
        }
        base = strrchr(href, '/');
        base = base ? base + 1 : href;
        if (strncmp(base, "section", 7) != 0 || strlen(base) < 5 || strcmp(base + strlen(base) - 4, ".xml") != 0) {
            continue; /* header, scripts, ... */
        }
        snprintf(name, sizeof(name), "%s%s", strchr(href, '/') ? "" : "Contents/", href);
        if (!hp_package_find(pkg, name)) {
            continue;
        }
        grown = (char **)realloc(names, (count + 1) * sizeof(char *));
        if (!grown || !(grown[count] = hp_strdup(name))) {
            size_t i;
            names = grown ? grown : names;
            for (i = 0; i < count; i++) {
                free(names[i]);
            }
            free(names);
            return HP_ERR_NOMEM;
        }
        names = grown;
        count++;
    }
    if (count == 0) {
        int i;
        for (i = 0;; i++) {
            char name[64];
            char **grown;
            snprintf(name, sizeof(name), "Contents/section%d.xml", i);
            if (!hp_package_find(pkg, name)) {
                break;
            }
            grown = (char **)realloc(names, (count + 1) * sizeof(char *));
            if (!grown || !(grown[count] = hp_strdup(name))) {
                size_t k;
                names = grown ? grown : names;
                for (k = 0; k < count; k++) {
                    free(names[k]);
                }
                free(names);
                return HP_ERR_NOMEM;
            }
            names = grown;
            count++;
        }
    }
    *out = names;
    *out_count = count;
    return HP_OK;
}

static char *first_text(const hp_xml_node *root, const char *name, const char *def) {
    const hp_xml_node *el = root ? hp_xml_find(root, name) : NULL;
    char *s;
    if (!el) {
        return hp_strdup(def);
    }
    s = element_text(el);
    return s;
}

/* Build the document from an in-memory package. Takes ownership of pkg: on
 * success it becomes doc->package, on failure it is freed. Used for .hwpx files
 * and for HWP 5.x files converted to OWPML parts in memory (hwp.c). */
hp_status hp_hwpx_read_package(hp_package *pkg, hp_document **out_doc) {
    hp_document *doc = NULL;
    hp_style_tables styles;
    hp_read_state state;
    hp_xml_doc *xml = NULL;
    const hp_package_entry *mt;
    hp_status st;
    char **sections = NULL;
    size_t section_total = 0, sec_i;

    memset(&styles, 0, sizeof(styles));
    memset(&state, 0, sizeof(state));
    if (!pkg || !out_doc) {
        hp_package_free(pkg);
        return HP_ERR_INVALID;
    }
    *out_doc = NULL;

    mt = hp_package_find(pkg, "mimetype");
    {
        size_t n = mt ? mt->len : 0;
        while (n > 0 && (mt->data[n - 1] == '\n' || mt->data[n - 1] == '\r' || mt->data[n - 1] == ' ')) {
            n--;
        }
        if (!mt || n != strlen(HP_HWPX_MIMETYPE) || memcmp(mt->data, HP_HWPX_MIMETYPE, n) != 0) {
            hp_package_free(pkg);
            return HP_ERR_NOT_HWPX;
        }
    }

    doc = hp_document_new();
    if (!doc) {
        hp_package_free(pkg);
        return HP_ERR_NOMEM;
    }
    /* sections are rebuilt from the file below */
    {
        size_t i;
        for (i = 0; i < doc->sections[0].paragraph_count; i++) {
            paragraph_release(&doc->sections[0].paragraphs[i]);
        }
        free(doc->sections[0].paragraphs);
        free(doc->sections[0].blocks);
        free(doc->sections);
        doc->sections = NULL;
        doc->section_count = 0;
    }

    /* metadata */
    if (hp_package_find(pkg, "Contents/content.hpf")) {
        st = parse_part(pkg, "Contents/content.hpf", &xml);
        if (st != HP_OK) {
            goto fail;
        }
    }
    free(doc->title);
    free(doc->language);
    doc->title = first_text(hp_xml_root(xml), "title", "");
    doc->language = first_text(hp_xml_root(xml), "language", "ko");
    st = list_sections(pkg, hp_xml_root(xml), &sections, &section_total);
    hp_xml_free(xml);
    xml = NULL;
    if (st != HP_OK || !doc->title || !doc->language) {
        st = HP_ERR_NOMEM;
        goto fail;
    }
    if (section_total == 0) {
        st = HP_ERR_INVALID; /* no body */
        goto fail;
    }
    if (doc->language[0] == '\0') {
        free(doc->language);
        doc->language = hp_strdup("ko");
    }

    /* styles */
    if (hp_package_find(pkg, "Contents/header.xml")) {
        st = parse_part(pkg, "Contents/header.xml", &xml);
        if (st != HP_OK) {
            goto fail;
        }
        parse_header_styles(hp_xml_root(xml), &styles);
        hp_xml_free(xml);
        xml = NULL;
    }
    st = load_bindata(pkg, &styles);
    if (st != HP_OK) {
        goto fail;
    }
    styles.state = &state;

    /* sections */
    for (sec_i = 0; sec_i < section_total; sec_i++) {
        const char *name = sections[sec_i];
        hp_section *secs;
        st = parse_part(pkg, name, &xml);
        if (st != HP_OK) {
            goto fail;
        }
        secs = (hp_section *)realloc(doc->sections, (doc->section_count + 1) * sizeof(hp_section));
        if (!secs) {
            st = HP_ERR_NOMEM;
            goto fail;
        }
        doc->sections = secs;
        memset(&doc->sections[doc->section_count], 0, sizeof(hp_section));
        doc->section_count++; /* counted before parsing so a failure is freed with doc */
        {
            /* document-order index of every <p> (the editor's paragraph order) */
            const hp_xml_node *root = hp_xml_root(xml), *pn;
            size_t cap = 0;
            styles.p_count = 0;
            for (pn = hp_xml_find(root, "p"); pn; pn = hp_xml_find_next(root, pn, "p")) {
                if (styles.p_count == cap) {
                    size_t *g = (size_t *)realloc(styles.p_starts, (cap ? cap * 2 : 256) * sizeof(*g));
                    if (!g) {
                        break;
                    }
                    styles.p_starts = g;
                    cap = cap ? cap * 2 : 256;
                }
                styles.p_starts[styles.p_count++] = pn->start;
            }
        }
        st = parse_section(hp_xml_root(xml), &doc->sections[doc->section_count - 1], &styles);
        hp_xml_free(xml);
        xml = NULL;
        if (st == HP_OK) {
            st = hp_package_add_section_part(pkg, name);
        }
        if (st != HP_OK) {
            goto fail;
        }
        doc->sections[doc->section_count - 1].loaded_paragraph_count =
            doc->sections[doc->section_count - 1].paragraph_count;
    }

    pkg->loaded_title = hp_strdup(doc->title);
    if (!pkg->loaded_title) {
        st = HP_ERR_NOMEM;
        goto fail;
    }
    style_tables_free(&styles); /* bins point into pkg: free before handing pkg over */
    free(state.counters);
    for (sec_i = 0; sec_i < section_total; sec_i++) {
        free(sections[sec_i]);
    }
    free(sections);
    doc->package = pkg;
    *out_doc = doc;
    return HP_OK;

fail:
    for (sec_i = 0; sec_i < section_total; sec_i++) {
        free(sections[sec_i]);
    }
    free(sections);
    hp_xml_free(xml);
    style_tables_free(&styles);
    free(state.counters);
    hp_document_free(doc);
    hp_package_free(pkg);
    return st;
}

hp_status hp_hwpx_read(const char *path, hp_document **out_doc) {
    zip_t *za;
    int zerr = 0;
    hp_package *pkg = NULL;
    hp_status st;
    if (!path || !out_doc) {
        return HP_ERR_INVALID;
    }
    *out_doc = NULL;
    za = zip_open(path, ZIP_RDONLY, &zerr);
    if (!za) {
        return HP_ERR_IO;
    }
    /* Keep every original part so a later save can be lossless; parse from memory. */
    st = hp_package_load_zip(za, &pkg);
    zip_close(za);
    if (st != HP_OK) {
        return st;
    }
    return hp_hwpx_read_package(pkg, out_doc);
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



