/* HWPML 2.x reader (0.12.1).
 *
 * HWPML is 한글's older XML document format (root element <HWPML>), still
 * published for example by 국가법령정보센터, often under a .hwp name. Its
 * elements mirror the HWP 5.x records (CHARSHAPE, PARASHAPE, SECDEF, TABLE,
 * PICTURE …), so it is converted the same way as an HWP file: into OWPML parts
 * (header.xml, one section part per <SECTION>, BinData from the Base64
 * <BINDATA> storage) that the HWPX reader then reads. Only what HangulPad shows
 * is converted; the document can be saved as HWPX.
 *
 * Values are as in HWP records: colors are COLORREF (0x00BBGGRR, 4294967295 =
 * none), paragraph margins and non-percent line spacing are doubled HWPUNITs
 * (the scale of OWPML's legacy <hp:default> branch). */
#include "hp/hwp.h"
#include "hwpx_ns.h"
#include "xml.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

typedef struct {
    char *s;
    size_t len, cap;
    int oom;
} ob;

static void out_n(ob *b, const char *s, size_t n) {
    if (b->oom) {
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

static void out(ob *b, const char *s) {
    out_n(b, s, strlen(s));
}

static void outf(ob *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) {
        out_n(b, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
    }
}

static void out_esc(ob *b, const char *s, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        switch (s[i]) {
        case '&': out(b, "&amp;"); break;
        case '<': out(b, "&lt;"); break;
        case '>': out(b, "&gt;"); break;
        case '"': out(b, "&quot;"); break;
        default: out_n(b, s + i, 1); break;
        }
    }
}

/* ---- attribute values ---- */

static const char *av(const hp_xml_node *n, const char *name, const char *def) {
    const char *v = n ? hp_xml_attr(n, name) : NULL;
    return v ? v : def;
}

static long long an(const hp_xml_node *n, const char *name, long long def) {
    return n ? hp_xml_attr_long(n, name, def) : def;
}

static int ab(const hp_xml_node *n, const char *name) {
    const char *v = av(n, name, "false");
    return strcmp(v, "true") == 0 || strcmp(v, "1") == 0;
}

/* "DashDotDot" -> DASH_DOT_DOT, "percent" -> PERCENT (OWPML enumerations) */
static void out_enum(ob *b, const char *v) {
    char tmp[64];
    size_t i, k = 0;
    for (i = 0; v[i] && k + 2 < sizeof(tmp); i++) {
        char c = v[i];
        if (c >= 'A' && c <= 'Z' && i > 0 && v[i - 1] >= 'a' && v[i - 1] <= 'z') {
            tmp[k++] = '_';
        }
        tmp[k++] = (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
    }
    tmp[k] = '\0';
    out(b, tmp);
}

static void attr_enum(ob *b, const char *name, const hp_xml_node *n, const char *attr, const char *def) {
    outf(b, " %s=\"", name);
    out_enum(b, av(n, attr, def));
    out(b, "\"");
}

/* COLORREF (decimal) as #RRGGBB, or none */
static void out_color(ob *b, const char *v) {
    unsigned long long c = v ? strtoull(v, NULL, 10) : 0;
    if (c > 0xFFFFFFull || (v && strcmp(v, "4294967295") == 0)) {
        out(b, "none");
        return;
    }
    outf(b, "#%02X%02X%02X", (unsigned)(c & 255), (unsigned)((c >> 8) & 255), (unsigned)((c >> 16) & 255));
}

static void attr_color(ob *b, const char *name, const hp_xml_node *n, const char *attr) {
    outf(b, " %s=\"", name);
    out_color(b, av(n, attr, "0"));
    out(b, "\"");
}

/* "0.12mm" -> "0.12 mm" */
static void attr_width(ob *b, const char *name, const hp_xml_node *n, const char *attr) {
    const char *v = av(n, attr, "0.1mm");
    size_t k = strspn(v, "0123456789.");
    outf(b, " %s=\"", name);
    out_n(b, v, k);
    out(b, " mm\"");
}

/* a length: plain HWPUNIT or with a cm/mm unit */
static long long length_hwp(const char *v) {
    char *end;
    double d = strtod(v ? v : "0", &end);
    if (end && (end[0] == 'c' || end[0] == 'C')) {
        d *= 7200.0 / 2.54;
    } else if (end && (end[0] == 'm' || end[0] == 'M')) {
        d *= 720.0 / 2.54;
    }
    return (long long)(d + (d < 0 ? -0.5 : 0.5));
}

static unsigned count_children(const hp_xml_node *n, const char *name) {
    unsigned c = 0;
    const hp_xml_node *k;
    for (k = hp_xml_child(n, name); k; k = hp_xml_next(k, name)) {
        c++;
    }
    return c;
}

/* ---- header.xml ---- */

static const char *const LANG_ATTRS[7] = {"Hangul", "Latin", "Hanja", "Japanese", "Other", "Symbol", "User"};

static void out_lang_values(ob *b, const char *element, const hp_xml_node *n, long long def) {
    int i;
    outf(b, "<hh:%s", element);
    for (i = 0; i < 7; i++) {
        char low[16];
        snprintf(low, sizeof(low), "%s", LANG_ATTRS[i]);
        low[0] = (char)(low[0] + 32);
        outf(b, " %s=\"%lld\"", low, an(n, LANG_ATTRS[i], def));
    }
    out(b, "/>");
}

static void out_border(ob *b, const char *element, const hp_xml_node *n) {
    outf(b, "<hh:%s", element);
    attr_enum(b, "type", n, "Type", "None");
    attr_width(b, "width", n, "Width");
    attr_color(b, "color", n, "Color");
    out(b, "/>");
}

static void out_char_shape(ob *b, const hp_xml_node *cs) {
    const hp_xml_node *u = hp_xml_child(cs, "UNDERLINE"), *s = hp_xml_child(cs, "STRIKEOUT");
    outf(b, "<hh:charPr id=\"%lld\" height=\"%lld\"", an(cs, "Id", 0), an(cs, "Height", 1000));
    attr_color(b, "textColor", cs, "TextColor");
    attr_color(b, "shadeColor", cs, "ShadeColor");
    outf(b, " useFontSpace=\"%d\" useKerning=\"%d\" symMark=\"NONE\" borderFillIDRef=\"%lld\">", ab(cs, "UseFontSpace"),
         ab(cs, "UseKerning"), an(cs, "BorderFillId", 0));
    out_lang_values(b, "fontRef", hp_xml_child(cs, "FONTID"), 0);
    out_lang_values(b, "ratio", hp_xml_child(cs, "RATIO"), 100);
    out_lang_values(b, "spacing", hp_xml_child(cs, "CHARSPACING"), 0);
    out_lang_values(b, "relSz", hp_xml_child(cs, "RELSIZE"), 100);
    out_lang_values(b, "offset", hp_xml_child(cs, "CHAROFFSET"), 0);
    if (hp_xml_child(cs, "ITALIC")) {
        out(b, "<hh:italic/>");
    }
    if (hp_xml_child(cs, "BOLD")) {
        out(b, "<hh:bold/>");
    }
    out(b, "<hh:underline");
    attr_enum(b, "type", u, "Type", u ? "Bottom" : "None");
    attr_enum(b, "shape", u, "Shape", "Solid");
    attr_color(b, "color", u, "Color");
    out(b, "/><hh:strikeout");
    attr_enum(b, "shape", s, "Shape", s ? "Solid" : "None");
    attr_color(b, "color", s, "Color");
    out(b, "/><hh:outline type=\"NONE\"/><hh:shadow type=\"NONE\" color=\"#B2B2B2\" offsetX=\"10\" offsetY=\"10\"/>");
    if (hp_xml_child(cs, "SUPERSCRIPT")) {
        out(b, "<hh:supscript/>");
    }
    if (hp_xml_child(cs, "SUBSCRIPT")) {
        out(b, "<hh:subscript/>");
    }
    out(b, "</hh:charPr>");
}

static void out_para_shape(ob *b, const hp_xml_node *ps) {
    const hp_xml_node *m = hp_xml_child(ps, "PARAMARGIN");
    const char *ls = av(m, "LineSpacingType", "Percent");
    long long spacing = an(m, "LineSpacing", 160);
    int percent = strcmp(ls, "Percent") == 0 || strcmp(ls, "percent") == 0;
    outf(b, "<hh:paraPr id=\"%lld\" tabPrIDRef=\"%lld\" condense=\"%lld\" fontLineHeight=\"%d\" snapToGrid=\"%d\" "
            "suppressLineNumbers=\"0\" checked=\"0\"><hh:align",
         an(ps, "Id", 0), an(ps, "TabDef", 0), an(ps, "Condense", 0), ab(ps, "FontLineHeight"), ab(ps, "SnapToGrid"));
    attr_enum(b, "horizontal", ps, "Align", "Justify");
    attr_enum(b, "vertical", ps, "VerAlign", "Baseline");
    out(b, "/><hh:heading");
    attr_enum(b, "type", ps, "HeadingType", "None");
    outf(b, " idRef=\"%lld\" level=\"%lld\"/><hh:breakSetting", an(ps, "Heading", 0), an(ps, "Level", 0));
    attr_enum(b, "breakLatinWord", ps, "BreakLatinWord", "KeepWord");
    /* a set BreakNonLatinWord is HWP's bit 7: 한글 broken by word */
    outf(b, " breakNonLatinWord=\"%s\" widowOrphan=\"%d\" keepWithNext=\"%d\" keepLines=\"%d\" pageBreakBefore=\"%d\" "
            "lineWrap=\"BREAK\"/>",
         ab(ps, "BreakNonLatinWord") ? "KEEP_WORD" : "BREAK_WORD", ab(ps, "WidowOrphan"), ab(ps, "KeepWithNext"),
         ab(ps, "KeepLines"), ab(ps, "PageBreakBefore"));
    /* doubled HWPUNITs; an odd value marks a character unit (approximated) */
    outf(b, "<hh:margin><hc:intent value=\"%lld\" unit=\"HWPUNIT\"/><hc:left value=\"%lld\" unit=\"HWPUNIT\"/>"
            "<hc:right value=\"%lld\" unit=\"HWPUNIT\"/><hc:prev value=\"%lld\" unit=\"HWPUNIT\"/>"
            "<hc:next value=\"%lld\" unit=\"HWPUNIT\"/></hh:margin>",
         an(m, "Indent", 0) / 2, an(m, "Left", 0) / 2, an(m, "Right", 0) / 2, an(m, "Prev", 0) / 2, an(m, "Next", 0) / 2);
    out(b, "<hh:lineSpacing");
    attr_enum(b, "type", m, "LineSpacingType", "Percent");
    outf(b, " value=\"%lld\" unit=\"HWPUNIT\"/>", percent ? spacing : spacing / 2);
    {
        const hp_xml_node *pb = hp_xml_child(ps, "PARABORDER");
        outf(b, "<hh:border borderFillIDRef=\"%lld\" offsetLeft=\"%lld\" offsetRight=\"%lld\" offsetTop=\"%lld\" "
                "offsetBottom=\"%lld\" connect=\"%d\" ignoreMargin=\"%d\"/>",
             an(pb, "BorderFill", 1), an(pb, "OffsetLeft", 0), an(pb, "OffsetRight", 0), an(pb, "OffsetTop", 0),
             an(pb, "OffsetBottom", 0), ab(pb, "Connect"), ab(pb, "IgnoreMargin"));
    }
    out(b, "</hh:paraPr>");
}

static void out_text_of(ob *b, const hp_xml_node *n) {
    const hp_xml_node *c;
    for (c = n->first_child; c; c = c->next) {
        if (c->type == HP_XML_TEXT) {
            out_esc(b, c->text, c->text_len);
        }
    }
}

static void out_header(ob *b, const hp_xml_node *root, unsigned sections) {
    const hp_xml_node *map = hp_xml_find(root, "MAPPINGTABLE"), *list, *n;
    out(b, HP_XML_DECL "<hh:head" HP_NS_ALL);
    outf(b, " version=\"1.4\" secCnt=\"%u\"><hh:beginNum page=\"1\" footnote=\"1\" endnote=\"1\" pic=\"1\" tbl=\"1\" "
            "equation=\"1\"/><hh:refList>",
         sections);
    list = hp_xml_child(map, "FACENAMELIST");
    outf(b, "<hh:fontfaces itemCnt=\"%u\">", count_children(list, "FONTFACE"));
    for (n = hp_xml_child(list, "FONTFACE"); n; n = hp_xml_next(n, "FONTFACE")) {
        const hp_xml_node *f;
        out(b, "<hh:fontface");
        attr_enum(b, "lang", n, "Lang", "Hangul");
        outf(b, " fontCnt=\"%u\">", count_children(n, "FONT"));
        for (f = hp_xml_child(n, "FONT"); f; f = hp_xml_next(f, "FONT")) {
            const char *face = av(f, "Name", "");
            outf(b, "<hh:font id=\"%lld\" face=\"", an(f, "Id", 0));
            out_esc(b, face, strlen(face));
            outf(b, "\" type=\"%s\" isEmbedded=\"0\"/>", strcmp(av(f, "Type", "ttf"), "hft") == 0 ? "HFT" : "TTF");
        }
        out(b, "</hh:fontface>");
    }
    out(b, "</hh:fontfaces>");
    list = hp_xml_child(map, "BORDERFILLLIST");
    outf(b, "<hh:borderFills itemCnt=\"%u\">", count_children(list, "BORDERFILL"));
    for (n = hp_xml_child(list, "BORDERFILL"); n; n = hp_xml_next(n, "BORDERFILL")) {
        const hp_xml_node *brush = hp_xml_find(n, "WINDOWBRUSH");
        outf(b, "<hh:borderFill id=\"%lld\" threeD=\"%d\" shadow=\"%d\" centerLine=\"NONE\" breakCellSeparateLine=\"0\">"
                "<hh:slash type=\"NONE\" Crooked=\"0\" isCounter=\"0\"/><hh:backSlash type=\"NONE\" Crooked=\"0\" "
                "isCounter=\"0\"/>",
             an(n, "Id", 0), ab(n, "ThreeD"), ab(n, "Shadow"));
        out_border(b, "leftBorder", hp_xml_child(n, "LEFTBORDER"));
        out_border(b, "rightBorder", hp_xml_child(n, "RIGHTBORDER"));
        out_border(b, "topBorder", hp_xml_child(n, "TOPBORDER"));
        out_border(b, "bottomBorder", hp_xml_child(n, "BOTTOMBORDER"));
        out_border(b, "diagonal", hp_xml_child(n, "DIAGONAL"));
        if (brush && strcmp(av(brush, "FaceColor", "4294967295"), "4294967295") != 0) {
            out(b, "<hc:fillBrush><hc:winBrush");
            attr_color(b, "faceColor", brush, "FaceColor");
            attr_color(b, "hatchColor", brush, "HatchColor");
            outf(b, " alpha=\"%lld\"/></hc:fillBrush>", an(brush, "Alpha", 0));
        }
        out(b, "</hh:borderFill>");
    }
    out(b, "</hh:borderFills>");
    list = hp_xml_child(map, "CHARSHAPELIST");
    outf(b, "<hh:charProperties itemCnt=\"%u\">", count_children(list, "CHARSHAPE"));
    for (n = hp_xml_child(list, "CHARSHAPE"); n; n = hp_xml_next(n, "CHARSHAPE")) {
        out_char_shape(b, n);
    }
    out(b, "</hh:charProperties>");
    list = hp_xml_child(map, "NUMBERINGLIST");
    if (list) {
        outf(b, "<hh:numberings itemCnt=\"%u\">", count_children(list, "NUMBERING"));
        for (n = hp_xml_child(list, "NUMBERING"); n; n = hp_xml_next(n, "NUMBERING")) {
            const hp_xml_node *h;
            outf(b, "<hh:numbering id=\"%lld\" start=\"%lld\">", an(n, "Id", 1), an(n, "Start", 0));
            for (h = hp_xml_child(n, "PARAHEAD"); h; h = hp_xml_next(h, "PARAHEAD")) {
                outf(b, "<hh:paraHead start=\"%lld\" level=\"%lld\"", an(h, "Start", 1), an(h, "Level", 1));
                attr_enum(b, "align", h, "Alignment", "Left");
                attr_enum(b, "numFormat", h, "NumFormat", "Digit");
                outf(b, " charPrIDRef=\"%lld\">", an(h, "CharShape", 4294967295ll));
                out_text_of(b, h);
                out(b, "</hh:paraHead>");
            }
            out(b, "</hh:numbering>");
        }
        out(b, "</hh:numberings>");
    }
    list = hp_xml_child(map, "BULLETLIST");
    if (list) {
        outf(b, "<hh:bullets itemCnt=\"%u\">", count_children(list, "BULLET"));
        for (n = hp_xml_child(list, "BULLET"); n; n = hp_xml_next(n, "BULLET")) {
            const char *ch = av(n, "Char", "");
            outf(b, "<hh:bullet id=\"%lld\" char=\"", an(n, "Id", 1));
            out_esc(b, ch, strlen(ch));
            out(b, "\" useImage=\"0\"/>");
        }
        out(b, "</hh:bullets>");
    }
    list = hp_xml_child(map, "PARASHAPELIST");
    outf(b, "<hh:paraProperties itemCnt=\"%u\">", count_children(list, "PARASHAPE"));
    for (n = hp_xml_child(list, "PARASHAPE"); n; n = hp_xml_next(n, "PARASHAPE")) {
        out_para_shape(b, n);
    }
    out(b, "</hh:paraProperties></hh:refList></hh:head>");
}

/* ---- sections ---- */

static void out_paragraphs(ob *b, const hp_xml_node *list, const hp_xml_node *bins);

static void out_sublist(ob *b, const hp_xml_node *paralist, const hp_xml_node *bins) {
    out(b, "<hp:subList id=\"\" textDirection=\"HORIZONTAL\" lineWrap=\"BREAK\"");
    attr_enum(b, "vertAlign", paralist, "VertAlign", "Top");
    out(b, ">");
    out_paragraphs(b, paralist, bins);
    out(b, "</hp:subList>");
}

static void out_note_shape(ob *b, const char *element, const hp_xml_node *shape) {
    const hp_xml_node *fmt = hp_xml_child(shape, "AUTONUMFORMAT"), *line = hp_xml_child(shape, "NOTELINE");
    const hp_xml_node *gap = hp_xml_child(shape, "NOTESPACING");
    const char *pre = av(fmt, "PrefixChar", ""), *suf = av(fmt, "SuffixChar", ")");
    if (!shape) {
        return;
    }
    outf(b, "<hp:%s><hp:autoNumFormat", element);
    attr_enum(b, "type", fmt, "Type", "Digit");
    out(b, " userChar=\"\" prefixChar=\"");
    out_esc(b, pre, strlen(pre));
    out(b, "\" suffixChar=\"");
    out_esc(b, suf, strlen(suf));
    outf(b, "\" supscript=\"%d\"/><hp:noteLine length=\"%lld\"", ab(fmt, "Superscript"), length_hwp(av(line, "Length", "-1")));
    attr_enum(b, "type", line, "Type", "Solid");
    attr_width(b, "width", line, "Width");
    attr_color(b, "color", line, "Color");
    outf(b, "/><hp:noteSpacing betweenNotes=\"%lld\" belowLine=\"%lld\" aboveLine=\"%lld\"/></hp:%s>",
         an(gap, "BetweenNotes", 283), an(gap, "BelowLine", 567), an(gap, "AboveLine", 850), element);
}

static void out_secdef(ob *b, const hp_xml_node *sd) {
    const hp_xml_node *start = hp_xml_child(sd, "STARTNUMBER"), *hide = hp_xml_child(sd, "HIDE");
    const hp_xml_node *page = hp_xml_child(sd, "PAGEDEF"), *m = hp_xml_child(page, "PAGEMARGIN");
    outf(b, "<hp:secPr id=\"\" textDirection=\"HORIZONTAL\" spaceColumns=\"%lld\" tabStop=\"%lld\" "
            "outlineShapeIDRef=\"%lld\" memoShapeIDRef=\"0\" textVerticalWidthHead=\"0\" masterPageCnt=\"0\">",
         an(sd, "SpaceColumns", 1134), an(sd, "TabStop", 8000), an(sd, "OutlineShape", 1));
    out(b, "<hp:startNum");
    attr_enum(b, "pageStartsOn", start, "PageStartsOn", "Both");
    outf(b, " page=\"%lld\" pic=\"%lld\" tbl=\"%lld\" equation=\"%lld\"/>", an(start, "Page", 0), an(start, "Figure", 0),
         an(start, "Table", 0), an(start, "Equation", 0));
    outf(b, "<hp:visibility hideFirstHeader=\"%d\" hideFirstFooter=\"%d\" hideFirstMasterPage=\"%d\" border=\"SHOW_ALL\" "
            "fill=\"SHOW_ALL\" hideFirstPageNum=\"%d\" hideFirstEmptyLine=\"%d\" showLineNumber=\"0\"/>",
         ab(hide, "Header"), ab(hide, "Footer"), ab(hide, "MasterPage"), ab(hide, "PageNumPos"), ab(hide, "EmptyLine"));
    outf(b, "<hp:pagePr landscape=\"%s\" width=\"%lld\" height=\"%lld\"", an(page, "Landscape", 0) ? "NARROWLY" : "WIDELY",
         an(page, "Width", 59528), an(page, "Height", 84188));
    attr_enum(b, "gutterType", page, "GutterType", "LeftOnly");
    outf(b, "><hp:margin header=\"%lld\" footer=\"%lld\" gutter=\"%lld\" left=\"%lld\" right=\"%lld\" top=\"%lld\" "
            "bottom=\"%lld\"/></hp:pagePr>",
         an(m, "Header", 4252), an(m, "Footer", 4252), an(m, "Gutter", 0), an(m, "Left", 8504), an(m, "Right", 8504),
         an(m, "Top", 5668), an(m, "Bottom", 4252));
    out_note_shape(b, "footNotePr", hp_xml_child(sd, "FOOTNOTESHAPE"));
    out_note_shape(b, "endNotePr", hp_xml_child(sd, "ENDNOTESHAPE"));
    out(b, "</hp:secPr>");
}

/* SHAPEOBJECT: object attributes go on the element, size and position inside */
static void out_object_attrs(ob *b, const hp_xml_node *so) {
    outf(b, " id=\"%lld\" zOrder=\"%lld\"", an(so, "InstId", 0), an(so, "ZOrder", 0));
    attr_enum(b, "numberingType", so, "NumberingType", "None");
    attr_enum(b, "textWrap", so, "TextWrap", "TopAndBottom");
    attr_enum(b, "textFlow", so, "TextFlow", "BothSides");
    out(b, " lock=\"0\" dropcapstyle=\"None\"");
}

static void out_object_placement(ob *b, const hp_xml_node *so) {
    const hp_xml_node *sz = hp_xml_child(so, "SIZE"), *pos = hp_xml_child(so, "POSITION");
    const hp_xml_node *om = hp_xml_child(so, "OUTSIDEMARGIN");
    outf(b, "<hp:sz width=\"%lld\"", an(sz, "Width", 0));
    attr_enum(b, "widthRelTo", sz, "WidthRelTo", "Absolute");
    outf(b, " height=\"%lld\"", an(sz, "Height", 0));
    attr_enum(b, "heightRelTo", sz, "HeightRelTo", "Absolute");
    outf(b, " protect=\"%d\"/><hp:pos treatAsChar=\"%d\" affectLSpacing=\"%d\" flowWithText=\"%d\" allowOverlap=\"%d\" "
            "holdAnchorAndSO=\"0\"",
         ab(sz, "Protect"), ab(pos, "TreatAsChar"), ab(pos, "AffectLSpacing"), ab(pos, "FlowWithText"),
         ab(pos, "AllowOverlap"));
    attr_enum(b, "vertRelTo", pos, "VertRelTo", "Para");
    attr_enum(b, "horzRelTo", pos, "HorzRelTo", "Para");
    attr_enum(b, "vertAlign", pos, "VertAlign", "Top");
    attr_enum(b, "horzAlign", pos, "HorzAlign", "Left");
    outf(b, " vertOffset=\"%lld\" horzOffset=\"%lld\"/>", an(pos, "VertOffset", 0), an(pos, "HorzOffset", 0));
    outf(b, "<hp:outMargin left=\"%lld\" right=\"%lld\" top=\"%lld\" bottom=\"%lld\"/>", an(om, "Left", 0),
         an(om, "Right", 0), an(om, "Top", 0), an(om, "Bottom", 0));
}

static void out_margin(ob *b, const char *element, const hp_xml_node *m) {
    outf(b, "<hp:%s left=\"%lld\" right=\"%lld\" top=\"%lld\" bottom=\"%lld\"/>", element, an(m, "Left", 0),
         an(m, "Right", 0), an(m, "Top", 0), an(m, "Bottom", 0));
}

static void out_table(ob *b, const hp_xml_node *t, const hp_xml_node *bins) {
    const hp_xml_node *so = hp_xml_child(t, "SHAPEOBJECT"), *row;
    out(b, "<hp:tbl");
    out_object_attrs(b, so);
    attr_enum(b, "pageBreak", t, "PageBreak", "Cell");
    outf(b, " repeatHeader=\"%d\" rowCnt=\"%lld\" colCnt=\"%lld\" cellSpacing=\"%lld\" borderFillIDRef=\"%lld\" "
            "noAdjust=\"0\">",
         ab(t, "RepeatHeader"), an(t, "RowCount", 1), an(t, "ColCount", 1), an(t, "CellSpacing", 0),
         an(t, "BorderFill", 1));
    out_object_placement(b, so);
    out_margin(b, "inMargin", hp_xml_child(t, "INSIDEMARGIN"));
    for (row = hp_xml_child(t, "ROW"); row; row = hp_xml_next(row, "ROW")) {
        const hp_xml_node *cell;
        out(b, "<hp:tr>");
        for (cell = hp_xml_child(row, "CELL"); cell; cell = hp_xml_next(cell, "CELL")) {
            outf(b, "<hp:tc name=\"\" header=\"%d\" hasMargin=\"%d\" protect=\"0\" editable=\"0\" dirty=\"0\" "
                    "borderFillIDRef=\"%lld\">",
                 ab(cell, "Header"), ab(cell, "HasMargin"), an(cell, "BorderFill", 1));
            out_sublist(b, hp_xml_child(cell, "PARALIST"), bins);
            outf(b, "<hp:cellAddr colAddr=\"%lld\" rowAddr=\"%lld\"/><hp:cellSpan colSpan=\"%lld\" rowSpan=\"%lld\"/>"
                    "<hp:cellSz width=\"%lld\" height=\"%lld\"/>",
                 an(cell, "ColAddr", 0), an(cell, "RowAddr", 0), an(cell, "ColSpan", 1), an(cell, "RowSpan", 1),
                 an(cell, "Width", 0), an(cell, "Height", 0));
            out_margin(b, "cellMargin", hp_xml_child(cell, "CELLMARGIN"));
            out(b, "</hp:tc>");
        }
        out(b, "</hp:tr>");
    }
    out(b, "</hp:tbl>");
}

/* BinItem (1-based BINITEM) -> the BINDATA id it names */
static long long bin_data_id(const hp_xml_node *bins, long long item) {
    const hp_xml_node *n;
    long long k = 1;
    for (n = hp_xml_child(bins, "BINITEM"); n; n = hp_xml_next(n, "BINITEM"), k++) {
        if (k == item) {
            return an(n, "BinData", item);
        }
    }
    return item;
}

static void out_picture(ob *b, const hp_xml_node *pic, const hp_xml_node *bins) {
    const hp_xml_node *so = hp_xml_child(pic, "SHAPEOBJECT"), *sc = hp_xml_child(pic, "SHAPECOMPONENT");
    const hp_xml_node *clip = hp_xml_child(pic, "IMAGECLIP"), *img = hp_xml_child(pic, "IMAGE");
    out(b, "<hp:pic");
    out_object_attrs(b, so);
    out(b, " reverse=\"0\">");
    outf(b, "<hp:offset x=\"0\" y=\"0\"/><hp:orgSz width=\"%lld\" height=\"%lld\"/><hp:curSz width=\"%lld\" "
            "height=\"%lld\"/>",
         an(sc, "OriWidth", 0), an(sc, "OriHeight", 0), an(sc, "CurWidth", 0), an(sc, "CurHeight", 0));
    outf(b, "<hp:imgClip left=\"%lld\" right=\"%lld\" top=\"%lld\" bottom=\"%lld\"/>", an(clip, "Left", 0),
         an(clip, "Right", 0), an(clip, "Top", 0), an(clip, "Bottom", 0));
    out_margin(b, "inMargin", hp_xml_child(pic, "INSIDEMARGIN"));
    outf(b, "<hc:img binaryItemIDRef=\"image%lld\" bright=\"%lld\" contrast=\"%lld\"",
         bin_data_id(bins, an(img, "BinItem", 0)), an(img, "Bright", 0), an(img, "Contrast", 0));
    attr_enum(b, "effect", img, "Effect", "RealPic");
    out(b, " alpha=\"0\"/>");
    out_object_placement(b, so);
    out(b, "</hp:pic>");
}

static void out_equation(ob *b, const hp_xml_node *eq) {
    const hp_xml_node *so = hp_xml_child(eq, "SHAPEOBJECT"), *script = hp_xml_child(eq, "SCRIPT");
    out(b, "<hp:equation");
    out_object_attrs(b, so);
    outf(b, " version=\"Equation Version 60\" baseLine=\"%lld\" textColor=\"#000000\" baseUnit=\"%lld\" "
            "lineMode=\"CHAR\" font=\"HYhwpEQ\">",
         an(eq, "BaseLine", 85), an(eq, "BaseUnit", 1000));
    out_object_placement(b, so);
    out(b, "<hp:script>");
    if (script) {
        out_text_of(b, script);
    }
    out(b, "</hp:script></hp:equation>");
}

/* <CHAR>: text and the inline characters inside it */
static void out_char(ob *b, const hp_xml_node *ch) {
    const hp_xml_node *c;
    if (!ch->first_child) {
        out(b, "<hp:t/>");
        return;
    }
    out(b, "<hp:t>");
    for (c = ch->first_child; c; c = c->next) {
        if (c->type == HP_XML_TEXT) {
            out_esc(b, c->text, c->text_len);
        } else if (hp_xml_is(c, "TAB")) {
            out(b, "<hp:tab/>");
        } else if (hp_xml_is(c, "LINEBREAK")) {
            out(b, "<hp:lineBreak/>");
        } else if (hp_xml_is(c, "NBSPACE")) {
            out(b, "<hp:nbSpace/>");
        } else if (hp_xml_is(c, "FWSPACE")) {
            out(b, "<hp:fwSpace/>");
        } else if (hp_xml_is(c, "HYPEN") || hp_xml_is(c, "HYPHEN")) {
            out(b, "<hp:hyphen/>");
        }
    }
    out(b, "</hp:t>");
}

static void out_text(ob *b, const hp_xml_node *text, const hp_xml_node *bins) {
    const hp_xml_node *c;
    outf(b, "<hp:run charPrIDRef=\"%lld\">", an(text, "CharShape", 0));
    for (c = hp_xml_child(text, NULL); c; c = hp_xml_next(c, NULL)) {
        if (hp_xml_is(c, "CHAR")) {
            out_char(b, c);
        } else if (hp_xml_is(c, "SECDEF")) {
            out_secdef(b, c);
        } else if (hp_xml_is(c, "COLDEF")) {
            outf(b, "<hp:ctrl><hp:colPr id=\"\" type=\"NEWSPAPER\" layout=\"LEFT\" colCount=\"%lld\" sameSz=\"1\" "
                    "sameGap=\"%lld\"/></hp:ctrl>",
                 an(c, "Count", 1), an(c, "SameGap", an(c, "Gap", 0)));
        } else if (hp_xml_is(c, "TABLE")) {
            out_table(b, c, bins);
        } else if (hp_xml_is(c, "PICTURE")) {
            out_picture(b, c, bins);
        } else if (hp_xml_is(c, "EQUATION")) {
            out_equation(b, c);
        } else if (hp_xml_is(c, "HEADER") || hp_xml_is(c, "FOOTER")) {
            const char *e = hp_xml_is(c, "HEADER") ? "header" : "footer";
            outf(b, "<hp:ctrl><hp:%s id=\"0\"", e);
            attr_enum(b, "applyPageType", c, "ApplyPageType", "Both");
            out(b, ">");
            out_sublist(b, hp_xml_child(c, "PARALIST"), bins);
            outf(b, "</hp:%s></hp:ctrl>", e);
        } else if (hp_xml_is(c, "FOOTNOTE") || hp_xml_is(c, "ENDNOTE")) {
            const char *e = hp_xml_is(c, "FOOTNOTE") ? "footNote" : "endNote";
            outf(b, "<hp:ctrl><hp:%s number=\"%lld\" instId=\"0\">", e, an(c, "Number", 0));
            out_sublist(b, hp_xml_child(c, "PARALIST"), bins);
            outf(b, "</hp:%s></hp:ctrl>", e);
        } else if (hp_xml_is(c, "AUTONUM") || hp_xml_is(c, "NEWNUM")) {
            outf(b, "<hp:ctrl><hp:%s num=\"%lld\"", hp_xml_is(c, "AUTONUM") ? "autoNum" : "newNum", an(c, "Number", 1));
            attr_enum(b, "numType", c, "NumberType", "Page");
            out(b, "/></hp:ctrl>");
        } else if (hp_xml_is(c, "PAGENUM")) {
            const char *side = av(c, "SideChar", "");
            out(b, "<hp:ctrl><hp:pageNum");
            attr_enum(b, "pos", c, "Pos", "BottomCenter");
            attr_enum(b, "formatType", c, "FormatType", "Digit");
            out(b, " sideChar=\"");
            out_esc(b, side, strlen(side));
            out(b, "\"/></hp:ctrl>");
        } else if (hp_xml_is(c, "PAGEHIDING")) {
            outf(b, "<hp:ctrl><hp:pageHiding hideHeader=\"%d\" hideFooter=\"%d\" hidePageNum=\"%d\"/></hp:ctrl>",
                 ab(c, "HideHeader"), ab(c, "HideFooter"), ab(c, "HidePageNum"));
        }
    }
    out(b, "</hp:run>");
}

static void out_paragraphs(ob *b, const hp_xml_node *list, const hp_xml_node *bins) {
    const hp_xml_node *p;
    for (p = hp_xml_child(list, "P"); p; p = hp_xml_next(p, "P")) {
        const hp_xml_node *t;
        outf(b, "<hp:p id=\"0\" paraPrIDRef=\"%lld\" styleIDRef=\"%lld\" pageBreak=\"%d\" columnBreak=\"%d\" merged=\"0\">",
             an(p, "ParaShape", 0), an(p, "Style", 0), ab(p, "PageBreak"), ab(p, "ColumnBreak"));
        for (t = hp_xml_child(p, "TEXT"); t; t = hp_xml_next(t, "TEXT")) {
            out_text(b, t, bins);
        }
        if (!hp_xml_child(p, "TEXT")) {
            out(b, "<hp:run charPrIDRef=\"0\"/>");
        }
        out(b, "</hp:p>");
    }
}

/* ---- pictures ---- */

static int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static unsigned char *base64_decode(const hp_xml_node *n, size_t *out_len) {
    const hp_xml_node *c;
    size_t total = 0, k = 0;
    unsigned char *data;
    unsigned acc = 0;
    int bits = 0;
    for (c = n->first_child; c; c = c->next) {
        if (c->type == HP_XML_TEXT) {
            total += c->text_len;
        }
    }
    data = (unsigned char *)malloc(total / 4 * 3 + 3);
    if (!data) {
        return NULL;
    }
    for (c = n->first_child; c; c = c->next) {
        size_t i;
        if (c->type != HP_XML_TEXT) {
            continue;
        }
        for (i = 0; i < c->text_len; i++) {
            int v = b64_value(c->text[i]);
            if (v < 0) {
                continue;
            }
            acc = (acc << 6) | (unsigned)v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                data[k++] = (unsigned char)((acc >> bits) & 255);
            }
        }
    }
    *out_len = k;
    return data;
}

/* raw deflate (Compress="true") */
static unsigned char *inflate_raw(const unsigned char *in, size_t len, size_t *out_len) {
    z_stream z;
    size_t cap = len * 4 + 1024;
    unsigned char *outb = (unsigned char *)malloc(cap);
    int rc;
    if (!outb) {
        return NULL;
    }
    memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, -15) != Z_OK) {
        free(outb);
        return NULL;
    }
    z.next_in = (Bytef *)in;
    z.avail_in = (uInt)len;
    do {
        unsigned char *g;
        if (z.total_out == cap) {
            if (cap > 256u * 1024 * 1024) {
                break;
            }
            g = (unsigned char *)realloc(outb, cap * 2);
            if (!g) {
                break;
            }
            outb = g;
            cap *= 2;
        }
        z.next_out = outb + z.total_out;
        z.avail_out = (uInt)(cap - z.total_out);
        rc = inflate(&z, Z_NO_FLUSH);
    } while (rc == Z_OK);
    *out_len = z.total_out;
    inflateEnd(&z);
    if (rc != Z_STREAM_END) {
        free(outb);
        return NULL;
    }
    return outb;
}

/* ---- entry points ---- */

int hp_looks_like_hwpml(const char *path) {
    FILE *f;
    char head[2048];
    size_t n;
    if (!path || !(f = hp_fopen(path, "rb"))) {
        return 0;
    }
    n = fread(head, 1, sizeof(head) - 1, f);
    fclose(f);
    head[n] = '\0';
    return strstr(head, "<HWPML") != NULL;
}

hp_status hp_hwpml_read_memory(const unsigned char *data, size_t len, hp_document **out_doc) {
    char *buf, *p;
    hp_xml_doc *xml = NULL;
    const hp_xml_node *root, *body, *sec, *bins, *storage, *n;
    ob header = {0}, *secs = NULL;
    char **parts = NULL;
    hp_owpml_bin *items = NULL;
    unsigned char **blobs = NULL;
    size_t sec_count = 0, item_count = 0, i;
    char *title = NULL;
    hp_status st;
    if (!data || !out_doc) {
        return HP_ERR_INVALID;
    }
    *out_doc = NULL;
    /* the DOCTYPE's one entity (&nbsp;) is not expanded by the parser: same length */
    buf = (char *)malloc(len + 1);
    if (!buf) {
        return HP_ERR_NOMEM;
    }
    memcpy(buf, data, len);
    buf[len] = '\0';
    for (p = strstr(buf, "&nbsp;"); p; p = strstr(p + 6, "&nbsp;")) {
        memcpy(p, "&#160;", 6);
    }
    st = hp_xml_parse(buf, len, &xml, NULL, 0);
    if (st != HP_OK) {
        free(buf);
        return st;
    }
    root = hp_xml_root(xml);
    if (!root || !hp_xml_is(root, "HWPML")) {
        hp_xml_free(xml);
        free(buf);
        return HP_ERR_INVALID;
    }
    body = hp_xml_child(root, "BODY");
    bins = hp_xml_find(root, "BINDATALIST");
    {
        const hp_xml_node *t = hp_xml_find(root, "TITLE"), *c;
        ob tb = {0};
        for (c = t ? t->first_child : NULL; c; c = c->next) {
            if (c->type == HP_XML_TEXT) {
                out_n(&tb, c->text, c->text_len); /* plain: the package escapes it */
            }
        }
        title = tb.s;
    }
    for (sec = hp_xml_child(body, "SECTION"); sec; sec = hp_xml_next(sec, "SECTION")) {
        sec_count++;
    }
    secs = (ob *)calloc(sec_count ? sec_count : 1, sizeof(*secs));
    parts = (char **)calloc(sec_count ? sec_count : 1, sizeof(*parts));
    st = secs && parts ? HP_OK : HP_ERR_NOMEM;
    out_header(&header, root, (unsigned)sec_count);
    for (sec = hp_xml_child(body, "SECTION"), i = 0; sec && st == HP_OK; sec = hp_xml_next(sec, "SECTION"), i++) {
        out(&secs[i], HP_XML_DECL "<hs:sec" HP_NS_ALL ">");
        out_paragraphs(&secs[i], sec, bins);
        out(&secs[i], "</hs:sec>");
        if (secs[i].oom) {
            st = HP_ERR_NOMEM;
        }
        parts[i] = secs[i].s;
    }
    if (header.oom) {
        st = HP_ERR_NOMEM;
    }
    /* pictures: <BINDATA Id> in the storage, extension from the BINITEM naming it */
    storage = hp_xml_find(root, "BINDATASTORAGE");
    for (n = hp_xml_child(storage, "BINDATA"); n && st == HP_OK; n = hp_xml_next(n, "BINDATA")) {
        long long id = an(n, "Id", 0);
        const hp_xml_node *it;
        const char *ext = "bin";
        size_t blen = 0;
        unsigned char *blob;
        hp_owpml_bin *gi;
        unsigned char **gb;
        for (it = hp_xml_child(bins, "BINITEM"); it; it = hp_xml_next(it, "BINITEM")) {
            if (an(it, "BinData", -1) == id) {
                ext = av(it, "Format", "bin");
            }
        }
        blob = base64_decode(n, &blen);
        if (blob && ab(n, "Compress")) {
            size_t raw_len = 0;
            unsigned char *raw = inflate_raw(blob, blen, &raw_len);
            if (raw) {
                free(blob);
                blob = raw;
                blen = raw_len;
            }
        }
        if (!blob) {
            st = HP_ERR_NOMEM;
            break;
        }
        gi = (hp_owpml_bin *)realloc(items, (item_count + 1) * sizeof(*gi));
        gb = gi ? (unsigned char **)realloc(blobs, (item_count + 1) * sizeof(*gb)) : NULL;
        if (gi) {
            items = gi;
        }
        if (!gi || !gb) {
            free(blob);
            st = HP_ERR_NOMEM;
            break;
        }
        blobs = gb;
        memset(&items[item_count], 0, sizeof(items[0]));
        items[item_count].id = (unsigned)id;
        snprintf(items[item_count].ext, sizeof(items[item_count].ext), "%s", ext);
        items[item_count].data = blob;
        items[item_count].len = blen;
        blobs[item_count++] = blob;
    }
    if (st == HP_OK) {
        st = hp_owpml_package(title, header.s ? header.s : "", (const char *const *)parts, sec_count, items, item_count,
                              out_doc);
    }
    for (i = 0; i < item_count; i++) {
        free(blobs[i]);
    }
    free(blobs);
    free(items);
    for (i = 0; secs && i < sec_count; i++) {
        free(secs[i].s);
    }
    free(secs);
    free(parts);
    free(header.s);
    free(title);
    hp_xml_free(xml);
    free(buf);
    return st;
}

hp_status hp_hwpml_read(const char *path, hp_document **out_doc) {
    FILE *f;
    long size;
    unsigned char *data;
    hp_status st;
    if (!path || !out_doc) {
        return HP_ERR_INVALID;
    }
    *out_doc = NULL;
    f = hp_fopen(path, "rb");
    if (!f) {
        return HP_ERR_IO;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0 ||
        size > 512L * 1024 * 1024) {
        fclose(f);
        return HP_ERR_IO;
    }
    data = (unsigned char *)malloc((size_t)size + 1);
    if (!data) {
        fclose(f);
        return HP_ERR_NOMEM;
    }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(data);
        return HP_ERR_IO;
    }
    fclose(f);
    st = hp_hwpml_read_memory(data, (size_t)size, out_doc);
    free(data);
    return st;
}
