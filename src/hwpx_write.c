#include "hp/hwpx.h"

#include "hwpx_package.h"
#include "xml.h"
#include "hp/version.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zip.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

/* 1 if `path` does not exist yet or is an existing writable file. Checked before
 * writing so a read-only target fails cleanly (no stray libzip temp file). */
static int target_writable(const char *path) {
#ifdef _WIN32
    wchar_t wpath[1024];
    DWORD attr;
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, (int)(sizeof(wpath) / sizeof(wpath[0]))) == 0) {
        return 1; /* let libzip report the real error */
    }
    attr = GetFileAttributesW(wpath);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        return 1;
    }
    return (attr & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY)) == 0;
#else
    if (access(path, F_OK) != 0) {
        return 1;
    }
    return access(path, W_OK) == 0;
#endif
}

static char *hp_strdup(const char *s) {
    size_t n = strlen(s);
    char *out = (char *)malloc(n + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, s, n + 1);
    return out;
}

static char *xml_escape(const char *s) {
    size_t i, len = 0;
    char *out;
    const char *p;
    if (!s) {
        s = "";
    }
    for (p = s; *p; p++) {
        switch (*p) {
        case '&':
            len += 5;
            break;
        case '<':
        case '>':
            len += 4;
            break;
        case '"':
        case '\'':
            len += 6;
            break;
        default:
            len += 1;
            break;
        }
    }
    out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    i = 0;
    for (p = s; *p; p++) {
        switch (*p) {
        case '&':
            memcpy(out + i, "&amp;", 5);
            i += 5;
            break;
        case '<':
            memcpy(out + i, "&lt;", 4);
            i += 4;
            break;
        case '>':
            memcpy(out + i, "&gt;", 4);
            i += 4;
            break;
        case '"':
            memcpy(out + i, "&quot;", 6);
            i += 6;
            break;
        case '\'':
            memcpy(out + i, "&apos;", 6);
            i += 6;
            break;
        default:
            out[i++] = *p;
            break;
        }
    }
    out[i] = '\0';
    return out;
}

static hp_status zip_add_bytes_idx(zip_t *za, const char *name, const void *data, size_t len, int stored,
                                   zip_int64_t *out_idx);

static hp_status zip_add_bytes(zip_t *za, const char *name, const void *data, size_t len, int stored) {
    return zip_add_bytes_idx(za, name, data, len, stored, NULL);
}

static hp_status zip_add_bytes_idx(zip_t *za, const char *name, const void *data, size_t len, int stored,
                                   zip_int64_t *out_idx) {
    zip_source_t *src;
    zip_int64_t idx;

    src = zip_source_buffer(za, data, (zip_uint64_t)len, 0);
    if (!src) {
        return HP_ERR_IO;
    }
    idx = zip_file_add(za, name, src, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE);
    if (idx < 0) {
        zip_source_free(src);
        return HP_ERR_IO;
    }
    if (stored) {
        if (zip_set_file_compression(za, (zip_uint64_t)idx, ZIP_CM_STORE, 0) != 0) {
            return HP_ERR_IO;
        }
    } else {
        if (zip_set_file_compression(za, (zip_uint64_t)idx, ZIP_CM_DEFLATE, 6) != 0) {
            return HP_ERR_IO;
        }
    }
    if (out_idx) {
        *out_idx = idx;
    }
    return HP_OK;
}

static void append(char **buf, size_t *len, size_t *cap, const char *s) {
    size_t n = strlen(s);
    while (*len + n + 1 > *cap) {
        char *nb;
        *cap *= 2;
        nb = (char *)realloc(*buf, *cap);
        if (!nb) {
            free(*buf);
            *buf = NULL;
            *len = 0;
            *cap = 0;
            return;
        }
        *buf = nb;
    }
    memcpy(*buf + *len, s, n + 1);
    *len += n;
}

/* Append one plain-text <hp:p> (style/char refs 0 = the document's base style).
 * Returns 0 on allocation failure (buffer already freed by append()). */
static int append_paragraph_xml(char **buf, size_t *len, size_t *cap, const hp_paragraph *para, size_t id) {
    char tmp[256];
    size_t ri;
    snprintf(tmp, sizeof(tmp),
             "  <hp:p id=\"%zu\" paraPrIDRef=\"0\" styleIDRef=\"0\" pageBreak=\"0\" "
             "columnBreak=\"0\" merged=\"0\">\n"
             "    <hp:run charPrIDRef=\"0\">\n",
             id);
    append(buf, len, cap, tmp);
    if (!*buf) {
        return 0;
    }
    if (para->run_count == 0) {
        append(buf, len, cap, "      <hp:t></hp:t>\n");
    } else {
        for (ri = 0; ri < para->run_count; ri++) {
            char *esc = xml_escape(para->runs[ri].text);
            if (!esc) {
                free(*buf);
                *buf = NULL;
                return 0;
            }
            append(buf, len, cap, "      <hp:t>");
            append(buf, len, cap, esc);
            append(buf, len, cap, "</hp:t>\n");
            free(esc);
            if (!*buf) {
                return 0;
            }
        }
    }
    append(buf, len, cap, "    </hp:run>\n  </hp:p>\n");
    return *buf != NULL;
}

/* ---------- new-document writer (no source package) ----------
 * Emits a package shaped like one saved by Hancom Office Hangul: the same part
 * list/order, full namespace declarations, a header with all 7 font languages,
 * and a complete <hp:secPr>. (Hangul rejects packages missing these.) */

#define HP_NS_ALL                                                                                  \
    " xmlns:ha=\"http://www.hancom.co.kr/hwpml/2011/app\""                                         \
    " xmlns:hp=\"http://www.hancom.co.kr/hwpml/2011/paragraph\""                                   \
    " xmlns:hp10=\"http://www.hancom.co.kr/hwpml/2016/paragraph\""                                 \
    " xmlns:hs=\"http://www.hancom.co.kr/hwpml/2011/section\""                                     \
    " xmlns:hc=\"http://www.hancom.co.kr/hwpml/2011/core\""                                        \
    " xmlns:hh=\"http://www.hancom.co.kr/hwpml/2011/head\""                                        \
    " xmlns:hhs=\"http://www.hancom.co.kr/hwpml/2011/history\""                                    \
    " xmlns:hm=\"http://www.hancom.co.kr/hwpml/2011/master-page\""                                 \
    " xmlns:hpf=\"http://www.hancom.co.kr/schema/2011/hpf\""                                       \
    " xmlns:dc=\"http://purl.org/dc/elements/1.1/\""                                               \
    " xmlns:opf=\"http://www.idpf.org/2007/opf/\""                                                 \
    " xmlns:ooxmlchart=\"http://www.hancom.co.kr/hwpml/2016/ooxmlchart\""                          \
    " xmlns:hwpunitchar=\"http://www.hancom.co.kr/hwpml/2016/HwpUnitChar\""                        \
    " xmlns:epub=\"http://www.idpf.org/2007/ops\""                                                 \
    " xmlns:config=\"urn:oasis:names:tc:opendocument:xmlns:config:1.0\""

#define HP_XML_DECL "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\" ?>"
#define HP_DEFAULT_FONT "맑은 고딕"

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} hp_sbuf;

static int sb_init(hp_sbuf *b, size_t cap) {
    b->buf = (char *)malloc(cap);
    b->len = 0;
    b->cap = cap;
    if (b->buf) {
        b->buf[0] = '\0';
    }
    return b->buf != NULL;
}

static void sb_add(hp_sbuf *b, const char *s) {
    if (b->buf) {
        append(&b->buf, &b->len, &b->cap, s);
    }
}

static void sb_addf(hp_sbuf *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        free(b->buf); /* formats here are short; overflow = programming error */
        b->buf = NULL;
        return;
    }
    sb_add(b, tmp);
}

static void sb_add_esc(hp_sbuf *b, const char *s) {
    char *e = xml_escape(s);
    if (!e) {
        free(b->buf);
        b->buf = NULL;
        return;
    }
    sb_add(b, e);
    free(e);
}

static size_t fresh_section_count(const hp_document *doc) {
    return doc->section_count ? doc->section_count : 1;
}

static char *build_version_xml(void) {
    return hp_strdup(HP_XML_DECL
                     "<hv:HCFVersion xmlns:hv=\"http://www.hancom.co.kr/hwpml/2011/version\" "
                     "tagetApplication=\"WORDPROCESSOR\" major=\"5\" minor=\"1\" micro=\"1\" "
                     "buildNumber=\"0\" os=\"1\" xmlVersion=\"1.5\" application=\"HangulPad\" "
                     "appVersion=\"" HP_VERSION_COMMAS "\"/>");
}

static char *build_container_xml(void) {
    return hp_strdup(HP_XML_DECL
                     "<ocf:container xmlns:ocf=\"urn:oasis:names:tc:opendocument:xmlns:container\" "
                     "xmlns:hpf=\"http://www.hancom.co.kr/schema/2011/hpf\"><ocf:rootfiles>"
                     "<ocf:rootfile full-path=\"Contents/content.hpf\" "
                     "media-type=\"application/hwpml-package+xml\"/>"
                     "<ocf:rootfile full-path=\"Preview/PrvText.txt\" media-type=\"text/plain\"/>"
                     "<ocf:rootfile full-path=\"META-INF/container.rdf\" media-type=\"application/rdf+xml\"/>"
                     "</ocf:rootfiles></ocf:container>");
}

static char *build_manifest_xml(void) {
    return hp_strdup(HP_XML_DECL
                     "<odf:manifest xmlns:odf=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\"/>");
}

static char *build_settings_xml(void) {
    return hp_strdup(HP_XML_DECL
                     "<ha:HWPApplicationSetting xmlns:ha=\"http://www.hancom.co.kr/hwpml/2011/app\" "
                     "xmlns:config=\"urn:oasis:names:tc:opendocument:xmlns:config:1.0\">"
                     "<ha:CaretPosition listIDRef=\"0\" paraIDRef=\"0\" pos=\"0\"/>"
                     "</ha:HWPApplicationSetting>");
}

static char *build_container_rdf(const hp_document *doc) {
    hp_sbuf b;
    size_t i, n = fresh_section_count(doc);
    if (!sb_init(&b, 2048)) {
        return NULL;
    }
    sb_add(&b, HP_XML_DECL "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
                           "<rdf:Description rdf:about=\"\"><ns0:hasPart "
                           "xmlns:ns0=\"http://www.hancom.co.kr/hwpml/2016/meta/pkg#\" "
                           "rdf:resource=\"Contents/header.xml\"/></rdf:Description>"
                           "<rdf:Description rdf:about=\"Contents/header.xml\"><rdf:type "
                           "rdf:resource=\"http://www.hancom.co.kr/hwpml/2016/meta/pkg#HeaderFile\"/>"
                           "</rdf:Description>");
    for (i = 0; i < n; i++) {
        sb_addf(&b,
                "<rdf:Description rdf:about=\"\"><ns0:hasPart "
                "xmlns:ns0=\"http://www.hancom.co.kr/hwpml/2016/meta/pkg#\" "
                "rdf:resource=\"Contents/section%zu.xml\"/></rdf:Description>"
                "<rdf:Description rdf:about=\"Contents/section%zu.xml\"><rdf:type "
                "rdf:resource=\"http://www.hancom.co.kr/hwpml/2016/meta/pkg#SectionFile\"/>"
                "</rdf:Description>",
                i, i);
    }
    sb_add(&b, "<rdf:Description rdf:about=\"\"><rdf:type "
               "rdf:resource=\"http://www.hancom.co.kr/hwpml/2016/meta/pkg#Document\"/>"
               "</rdf:Description></rdf:RDF>");
    return b.buf;
}

static char *build_content_hpf(const hp_document *doc) {
    hp_sbuf b;
    size_t i, n = fresh_section_count(doc);
    char now[32];
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    if (!tm || strftime(now, sizeof(now), "%Y-%m-%dT%H:%M:%SZ", tm) == 0) {
        strcpy(now, "1970-01-01T00:00:00Z");
    }
    if (!sb_init(&b, 4096)) {
        return NULL;
    }
    sb_add(&b, HP_XML_DECL "<opf:package" HP_NS_ALL " version=\"\" unique-identifier=\"\" id=\"\">"
                           "<opf:metadata><opf:title>");
    sb_add_esc(&b, (doc->title && doc->title[0]) ? doc->title : "");
    sb_add(&b, "</opf:title><opf:language>");
    sb_add_esc(&b, (doc->language && doc->language[0]) ? doc->language : "ko");
    sb_add(&b, "</opf:language>"
               "<opf:meta name=\"creator\" content=\"text\">HangulPad</opf:meta>"
               "<opf:meta name=\"subject\" content=\"text\"/>"
               "<opf:meta name=\"description\" content=\"text\"/>"
               "<opf:meta name=\"lastsaveby\" content=\"text\">HangulPad</opf:meta>");
    sb_addf(&b,
            "<opf:meta name=\"CreatedDate\" content=\"text\">%s</opf:meta>"
            "<opf:meta name=\"ModifiedDate\" content=\"text\">%s</opf:meta>",
            now, now);
    sb_add(&b, "<opf:meta name=\"keyword\" content=\"text\"/></opf:metadata><opf:manifest>"
               "<opf:item id=\"header\" href=\"Contents/header.xml\" media-type=\"application/xml\"/>");
    for (i = 0; i < n; i++) {
        sb_addf(&b,
                "<opf:item id=\"section%zu\" href=\"Contents/section%zu.xml\" "
                "media-type=\"application/xml\"/>",
                i, i);
    }
    sb_add(&b, "<opf:item id=\"settings\" href=\"settings.xml\" media-type=\"application/xml\"/>"
               "</opf:manifest><opf:spine><opf:itemref idref=\"header\" linear=\"yes\"/>");
    for (i = 0; i < n; i++) {
        sb_addf(&b, "<opf:itemref idref=\"section%zu\" linear=\"yes\"/>", i);
    }
    sb_add(&b, "</opf:spine></opf:package>");
    return b.buf;
}

static char *build_header_xml(const hp_document *doc) {
    static const char *const langs[7] = {"HANGUL", "LATIN", "HANJA", "JAPANESE", "OTHER", "SYMBOL", "USER"};
    hp_sbuf b;
    int i;
    if (!sb_init(&b, 8192)) {
        return NULL;
    }
    sb_addf(&b, HP_XML_DECL "<hh:head" HP_NS_ALL " version=\"1.5\" secCnt=\"%zu\">",
            fresh_section_count(doc));
    sb_add(&b, "<hh:beginNum page=\"1\" footnote=\"1\" endnote=\"1\" pic=\"1\" tbl=\"1\" equation=\"1\"/>"
               "<hh:refList><hh:fontfaces itemCnt=\"7\">");
    for (i = 0; i < 7; i++) {
        sb_addf(&b,
                "<hh:fontface lang=\"%s\" fontCnt=\"1\"><hh:font id=\"0\" face=\"" HP_DEFAULT_FONT
                "\" type=\"TTF\" isEmbedded=\"0\"><hh:typeInfo familyType=\"FCAT_GOTHIC\" weight=\"5\" "
                "proportion=\"3\" contrast=\"2\" strokeVariation=\"0\" armStyle=\"0\" letterform=\"2\" "
                "midline=\"0\" xHeight=\"4\"/></hh:font></hh:fontface>",
                langs[i]);
    }
    sb_add(&b,
           "</hh:fontfaces><hh:borderFills itemCnt=\"2\">"
           "<hh:borderFill id=\"1\" threeD=\"0\" shadow=\"0\" centerLine=\"NONE\" breakCellSeparateLine=\"0\">"
           "<hh:slash type=\"NONE\" Crooked=\"0\" isCounter=\"0\"/>"
           "<hh:backSlash type=\"NONE\" Crooked=\"0\" isCounter=\"0\"/>"
           "<hh:leftBorder type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/>"
           "<hh:rightBorder type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/>"
           "<hh:topBorder type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/>"
           "<hh:bottomBorder type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/>"
           "<hh:diagonal type=\"SOLID\" width=\"0.1 mm\" color=\"#000000\"/></hh:borderFill>"
           "<hh:borderFill id=\"2\" threeD=\"0\" shadow=\"0\" centerLine=\"NONE\" breakCellSeparateLine=\"0\">"
           "<hh:slash type=\"NONE\" Crooked=\"0\" isCounter=\"0\"/>"
           "<hh:backSlash type=\"NONE\" Crooked=\"0\" isCounter=\"0\"/>"
           "<hh:leftBorder type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/>"
           "<hh:rightBorder type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/>"
           "<hh:topBorder type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/>"
           "<hh:bottomBorder type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/>"
           "<hh:diagonal type=\"SOLID\" width=\"0.1 mm\" color=\"#000000\"/>"
           "<hc:fillBrush><hc:winBrush faceColor=\"none\" hatchColor=\"#999999\" alpha=\"0\"/></hc:fillBrush>"
           "</hh:borderFill></hh:borderFills>"
           "<hh:charProperties itemCnt=\"1\">"
           "<hh:charPr id=\"0\" height=\"1000\" textColor=\"#000000\" shadeColor=\"none\" useFontSpace=\"0\" "
           "useKerning=\"0\" symMark=\"NONE\" borderFillIDRef=\"2\">"
           "<hh:fontRef hangul=\"0\" latin=\"0\" hanja=\"0\" japanese=\"0\" other=\"0\" symbol=\"0\" user=\"0\"/>"
           "<hh:ratio hangul=\"100\" latin=\"100\" hanja=\"100\" japanese=\"100\" other=\"100\" symbol=\"100\" "
           "user=\"100\"/>"
           "<hh:spacing hangul=\"0\" latin=\"0\" hanja=\"0\" japanese=\"0\" other=\"0\" symbol=\"0\" user=\"0\"/>"
           "<hh:relSz hangul=\"100\" latin=\"100\" hanja=\"100\" japanese=\"100\" other=\"100\" symbol=\"100\" "
           "user=\"100\"/>"
           "<hh:offset hangul=\"0\" latin=\"0\" hanja=\"0\" japanese=\"0\" other=\"0\" symbol=\"0\" user=\"0\"/>"
           "<hh:underline type=\"NONE\" shape=\"SOLID\" color=\"#000000\"/>"
           "<hh:strikeout shape=\"NONE\" color=\"#000000\"/><hh:outline type=\"NONE\"/>"
           "<hh:shadow type=\"NONE\" color=\"#C0C0C0\" offsetX=\"10\" offsetY=\"10\"/></hh:charPr>"
           "</hh:charProperties>"
           "<hh:tabProperties itemCnt=\"1\"><hh:tabPr id=\"0\" autoTabLeft=\"0\" autoTabRight=\"0\"/>"
           "</hh:tabProperties>"
           "<hh:numberings itemCnt=\"1\"><hh:numbering id=\"1\" start=\"1\">");
    {
        static const char *const fmt[7] = {"^1.", "^1.^2.", "^1.^2.^3.", "^1.^2.^3.^4.", "^1.^2.^3.^4.^5.",
                                           "^1.^2.^3.^4.^5.^6.", "^1.^2.^3.^4.^5.^6.^7."};
        for (i = 1; i <= 10; i++) {
            sb_addf(&b,
                    "<hh:paraHead start=\"1\" level=\"%d\" align=\"LEFT\" useInstWidth=\"%d\" autoIndent=\"1\" "
                    "widthAdjust=\"0\" textOffsetType=\"PERCENT\" textOffset=\"50\" numFormat=\"DIGIT\" "
                    "charPrIDRef=\"4294967295\" checkable=\"0\"",
                    i, i <= 7 ? 1 : 0);
            if (i <= 7) {
                sb_addf(&b, ">%s</hh:paraHead>", fmt[i - 1]);
            } else {
                sb_add(&b, "/>");
            }
        }
    }
    sb_add(&b,
           "</hh:numbering></hh:numberings>"
           "<hh:paraProperties itemCnt=\"1\">"
           "<hh:paraPr id=\"0\" tabPrIDRef=\"0\" condense=\"0\" fontLineHeight=\"0\" snapToGrid=\"1\" "
           "suppressLineNumbers=\"0\" checked=\"0\" textDir=\"LTR\">"
           "<hh:align horizontal=\"JUSTIFY\" vertical=\"BASELINE\"/>"
           "<hh:heading type=\"NONE\" idRef=\"0\" level=\"0\"/>"
           "<hh:breakSetting breakLatinWord=\"KEEP_WORD\" breakNonLatinWord=\"KEEP_WORD\" widowOrphan=\"0\" "
           "keepWithNext=\"0\" keepLines=\"0\" pageBreakBefore=\"0\" lineWrap=\"BREAK\"/>"
           "<hh:autoSpacing eAsianEng=\"0\" eAsianNum=\"0\"/>"
           "<hp:switch><hp:case hp:required-namespace=\"http://www.hancom.co.kr/hwpml/2016/HwpUnitChar\">"
           "<hh:margin><hc:intent value=\"0\" unit=\"HWPUNIT\"/><hc:left value=\"0\" unit=\"HWPUNIT\"/>"
           "<hc:right value=\"0\" unit=\"HWPUNIT\"/><hc:prev value=\"0\" unit=\"HWPUNIT\"/>"
           "<hc:next value=\"0\" unit=\"HWPUNIT\"/></hh:margin>"
           "<hh:lineSpacing type=\"PERCENT\" value=\"160\" unit=\"HWPUNIT\"/></hp:case>"
           "<hp:default><hh:margin><hc:intent value=\"0\" unit=\"HWPUNIT\"/><hc:left value=\"0\" unit=\"HWPUNIT\"/>"
           "<hc:right value=\"0\" unit=\"HWPUNIT\"/><hc:prev value=\"0\" unit=\"HWPUNIT\"/>"
           "<hc:next value=\"0\" unit=\"HWPUNIT\"/></hh:margin>"
           "<hh:lineSpacing type=\"PERCENT\" value=\"160\" unit=\"HWPUNIT\"/></hp:default></hp:switch>"
           "<hh:border borderFillIDRef=\"2\" offsetLeft=\"0\" offsetRight=\"0\" offsetTop=\"0\" "
           "offsetBottom=\"0\" connect=\"0\" ignoreMargin=\"0\"/></hh:paraPr></hh:paraProperties>"
           "<hh:styles itemCnt=\"1\"><hh:style id=\"0\" type=\"PARA\" name=\"바탕글\" engName=\"Normal\" "
           "paraPrIDRef=\"0\" charPrIDRef=\"0\" nextStyleIDRef=\"0\" langID=\"1042\" lockForm=\"0\"/>"
           "</hh:styles></hh:refList>"
           "<hh:compatibleDocument targetProgram=\"HWP201X\"><hh:layoutCompatibility/></hh:compatibleDocument>"
           "<hh:docOption><hh:linkinfo path=\"\" pageInherit=\"0\" footnoteInherit=\"0\"/></hh:docOption>"
           "<hh:trackchageConfig flags=\"56\"><config:config-item-set name=\"TrackChangePasswordInfo\">"
           "<config:config-item name=\"algorithm-name\" type=\"string\">SHA1</config:config-item>"
           "</config:config-item-set></hh:trackchageConfig></hh:head>");
    return b.buf;
}

static void add_sec_pr(hp_sbuf *b, const hp_section *sec) {
    const hp_page *pg = &sec->page;
    int cols = sec->column_count > 1 ? sec->column_count : 1;
    sb_add(b, "<hp:secPr id=\"\" textDirection=\"HORIZONTAL\" spaceColumns=\"1134\" tabStop=\"8000\" "
              "tabStopVal=\"4000\" tabStopUnit=\"HWPUNIT\" outlineShapeIDRef=\"1\" memoShapeIDRef=\"0\" "
              "textVerticalWidthHead=\"0\" masterPageCnt=\"0\">"
              "<hp:grid lineGrid=\"0\" charGrid=\"0\" wonggojiFormat=\"0\"/>"
              "<hp:startNum pageStartsOn=\"BOTH\" page=\"0\" pic=\"0\" tbl=\"0\" equation=\"0\"/>"
              "<hp:visibility hideFirstHeader=\"0\" hideFirstFooter=\"0\" hideFirstMasterPage=\"0\" "
              "border=\"SHOW_ALL\" fill=\"SHOW_ALL\" hideFirstPageNum=\"0\" hideFirstEmptyLine=\"0\" "
              "showLineNumber=\"0\"/>"
              "<hp:lineNumberShape restartType=\"0\" countBy=\"0\" distance=\"0\" startNumber=\"0\"/>");
    /* landscape="WIDELY" is Hangul's value for portrait orientation */
    sb_addf(b,
            "<hp:pagePr landscape=\"WIDELY\" width=\"%u\" height=\"%u\" gutterType=\"LEFT_ONLY\">"
            "<hp:margin header=\"4252\" footer=\"4252\" gutter=\"0\" left=\"%u\" right=\"%u\" top=\"%u\" "
            "bottom=\"%u\"/></hp:pagePr>",
            pg->width, pg->height, pg->margin_left, pg->margin_right, pg->margin_top, pg->margin_bottom);
    sb_add(b, "<hp:footNotePr><hp:autoNumFormat type=\"DIGIT\" userChar=\"\" prefixChar=\"\" suffixChar=\")\" "
              "supscript=\"0\"/><hp:noteLine length=\"-1\" type=\"SOLID\" width=\"0.12 mm\" color=\"#000000\"/>"
              "<hp:noteSpacing betweenNotes=\"283\" belowLine=\"567\" aboveLine=\"850\"/>"
              "<hp:numbering type=\"CONTINUOUS\" newNum=\"1\"/>"
              "<hp:placement place=\"EACH_COLUMN\" beneathText=\"0\"/></hp:footNotePr>"
              "<hp:endNotePr><hp:autoNumFormat type=\"DIGIT\" userChar=\"\" prefixChar=\"\" suffixChar=\")\" "
              "supscript=\"0\"/><hp:noteLine length=\"14692344\" type=\"SOLID\" width=\"0.12 mm\" "
              "color=\"#000000\"/><hp:noteSpacing betweenNotes=\"0\" belowLine=\"567\" aboveLine=\"850\"/>"
              "<hp:numbering type=\"CONTINUOUS\" newNum=\"1\"/>"
              "<hp:placement place=\"END_OF_DOCUMENT\" beneathText=\"0\"/></hp:endNotePr>"
              "<hp:pageBorderFill type=\"BOTH\" borderFillIDRef=\"1\" textBorder=\"PAPER\" headerInside=\"0\" "
              "footerInside=\"0\" fillArea=\"PAPER\"><hp:offset left=\"1417\" right=\"1417\" top=\"1417\" "
              "bottom=\"1417\"/></hp:pageBorderFill>"
              "<hp:pageBorderFill type=\"EVEN\" borderFillIDRef=\"1\" textBorder=\"PAPER\" headerInside=\"0\" "
              "footerInside=\"0\" fillArea=\"PAPER\"><hp:offset left=\"1417\" right=\"1417\" top=\"1417\" "
              "bottom=\"1417\"/></hp:pageBorderFill>"
              "<hp:pageBorderFill type=\"ODD\" borderFillIDRef=\"1\" textBorder=\"PAPER\" headerInside=\"0\" "
              "footerInside=\"0\" fillArea=\"PAPER\"><hp:offset left=\"1417\" right=\"1417\" top=\"1417\" "
              "bottom=\"1417\"/></hp:pageBorderFill></hp:secPr>");
    sb_addf(b,
            "<hp:ctrl><hp:colPr id=\"\" type=\"NEWSPAPER\" layout=\"LEFT\" colCount=\"%d\" sameSz=\"1\" "
            "sameGap=\"%u\"/></hp:ctrl>",
            cols, cols > 1 ? sec->column_gap_hwp : 0u);
}

/* Section XML for a new document. The first paragraph carries <hp:secPr> plus the
 * first paragraph's text, as Hangul writes it. */
static char *build_section_xml(const hp_section *sec) {
    hp_sbuf b;
    size_t pi, ri;
    if (!sb_init(&b, 4096)) {
        return NULL;
    }
    sb_add(&b, HP_XML_DECL "<hs:sec" HP_NS_ALL ">"
                           "<hp:p id=\"0\" paraPrIDRef=\"0\" styleIDRef=\"0\" pageBreak=\"0\" "
                           "columnBreak=\"0\" merged=\"0\"><hp:run charPrIDRef=\"0\">");
    add_sec_pr(&b, sec);
    sb_add(&b, "</hp:run><hp:run charPrIDRef=\"0\">");
    if (sec->paragraph_count > 0) {
        const hp_paragraph *first = &sec->paragraphs[0];
        for (ri = 0; ri < first->run_count; ri++) {
            sb_add(&b, "<hp:t>");
            sb_add_esc(&b, first->runs[ri].text ? first->runs[ri].text : "");
            sb_add(&b, "</hp:t>");
        }
    }
    sb_add(&b, "</hp:run></hp:p>");
    for (pi = 1; b.buf && pi < sec->paragraph_count; pi++) {
        if (!append_paragraph_xml(&b.buf, &b.len, &b.cap, &sec->paragraphs[pi], 0)) {
            return NULL;
        }
    }
    sb_add(&b, "</hs:sec>");
    return b.buf;
}

/* Preview/PrvText.txt: plain text, CRLF, capped at ~1 KB on a UTF-8 boundary. */
static char *build_preview_text(const hp_document *doc) {
    char *plain = hp_document_plain_text(doc);
    hp_sbuf b;
    size_t i;
    if (!sb_init(&b, 1200)) {
        free(plain);
        return NULL;
    }
    for (i = 0; plain && plain[i] && b.buf && b.len < 1024; i++) {
        char one[2] = {plain[i], '\0'};
        if (plain[i] == '\n') {
            sb_add(&b, "\r\n");
        } else {
            sb_add(&b, one);
        }
    }
    if (b.buf && plain && plain[i]) {
        /* Stopped early: drop a trailing UTF-8 sequence that was cut in half. */
        size_t lead = b.len;
        size_t need;
        unsigned char c;
        while (lead > 0 && ((unsigned char)b.buf[lead - 1] & 0xC0) == 0x80) {
            lead--;
        }
        if (lead > 0) {
            c = (unsigned char)b.buf[lead - 1];
            need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            if (b.len - (lead - 1) < need) {
                b.len = lead - 1;
            }
        }
        b.buf[b.len] = '\0';
    }
    free(plain);
    return b.buf;
}

static hp_status write_fresh(const hp_document *doc, const char *path) {
    enum { P_VERSION, P_HEADER, P_PREVIEW, P_SETTINGS, P_RDF, P_HPF, P_CONTAINER, P_MANIFEST, P_COUNT };
    char *parts[P_COUNT] = {0};
    char **sections;
    size_t section_count = fresh_section_count(doc);
    size_t i;
    zip_t *za;
    int zerr = 0;
    hp_status st = HP_OK;
    const char *mimetype = HP_HWPX_MIMETYPE;

    sections = (char **)calloc(section_count, sizeof(char *));
    if (!sections) {
        return HP_ERR_NOMEM;
    }
    parts[P_VERSION] = build_version_xml();
    parts[P_HEADER] = build_header_xml(doc);
    parts[P_PREVIEW] = build_preview_text(doc);
    parts[P_SETTINGS] = build_settings_xml();
    parts[P_RDF] = build_container_rdf(doc);
    parts[P_HPF] = build_content_hpf(doc);
    parts[P_CONTAINER] = build_container_xml();
    parts[P_MANIFEST] = build_manifest_xml();
    for (i = 0; i < P_COUNT; i++) {
        if (!parts[i]) {
            st = HP_ERR_NOMEM;
        }
    }
    for (i = 0; st == HP_OK && i < section_count; i++) {
        hp_section empty;
        const hp_section *sec;
        if (doc->section_count > 0) {
            sec = &doc->sections[i];
        } else {
            memset(&empty, 0, sizeof(empty));
            hp_page_default(&empty.page);
            sec = &empty;
        }
        sections[i] = build_section_xml(sec);
        if (!sections[i]) {
            st = HP_ERR_NOMEM;
        }
    }
    if (st != HP_OK) {
        goto cleanup;
    }

    /* No remove(path) first: libzip writes a temp file and renames it over the
     * target only on a successful zip_close(). */
    za = zip_open(path, ZIP_CREATE | ZIP_TRUNCATE, &zerr);
    if (!za) {
        st = HP_ERR_IO;
        goto cleanup;
    }
    /* Same part order and STORED entries as Hangul: mimetype, version.xml first. */
    st = zip_add_bytes(za, "mimetype", mimetype, strlen(mimetype), 1);
    if (st == HP_OK) {
        st = zip_add_bytes(za, "version.xml", parts[P_VERSION], strlen(parts[P_VERSION]), 1);
    }
    if (st == HP_OK) {
        st = zip_add_bytes(za, "Contents/header.xml", parts[P_HEADER], strlen(parts[P_HEADER]), 0);
    }
    for (i = 0; st == HP_OK && i < section_count; i++) {
        char name[64];
        snprintf(name, sizeof(name), "Contents/section%zu.xml", i);
        st = zip_add_bytes(za, name, sections[i], strlen(sections[i]), 0);
    }
    if (st == HP_OK) {
        st = zip_add_bytes(za, "Preview/PrvText.txt", parts[P_PREVIEW], strlen(parts[P_PREVIEW]), 0);
    }
    if (st == HP_OK) {
        st = zip_add_bytes(za, "settings.xml", parts[P_SETTINGS], strlen(parts[P_SETTINGS]), 0);
    }
    if (st == HP_OK) {
        st = zip_add_bytes(za, "META-INF/container.rdf", parts[P_RDF], strlen(parts[P_RDF]), 0);
    }
    if (st == HP_OK) {
        st = zip_add_bytes(za, "Contents/content.hpf", parts[P_HPF], strlen(parts[P_HPF]), 0);
    }
    if (st == HP_OK) {
        st = zip_add_bytes(za, "META-INF/container.xml", parts[P_CONTAINER], strlen(parts[P_CONTAINER]), 0);
    }
    if (st == HP_OK) {
        st = zip_add_bytes(za, "META-INF/manifest.xml", parts[P_MANIFEST], strlen(parts[P_MANIFEST]), 0);
    }
    if (st != HP_OK) {
        zip_discard(za);
    } else if (zip_close(za) != 0) {
        zip_discard(za);
        st = HP_ERR_IO;
    }

cleanup:
    for (i = 0; i < P_COUNT; i++) {
        free(parts[i]);
    }
    for (i = 0; i < section_count; i++) {
        free(sections[i]);
    }
    free(sections);
    return st;
}

/* ---------- lossless save (document read from an HWPX file) ---------- */

/* Parse a package part with the DOM parser (NULL if it is not well-formed XML). */
static hp_xml_doc *parse_entry(const hp_package_entry *e) {
    hp_xml_doc *d = NULL;
    if (hp_xml_parse((const char *)e->data, e->len, &d, NULL, 0) != HP_OK) {
        return NULL;
    }
    return d;
}

/* buf = data[a..b) appended */
static void append_range(char **buf, size_t *len, size_t *cap, const unsigned char *data, size_t a, size_t b) {
    char *tmp;
    if (!*buf || b <= a) {
        return;
    }
    tmp = (char *)malloc(b - a + 1);
    if (!tmp) {
        free(*buf);
        *buf = NULL;
        return;
    }
    memcpy(tmp, data + a, b - a);
    tmp[b - a] = '\0';
    append(buf, len, cap, tmp);
    free(tmp);
}

/* Original section XML + paragraphs added after reading, inserted right before the
 * root's end tag (</hs:sec>, located by the parser). Everything already in the file
 * (tables, pictures, styles) stays byte-for-byte. NULL if the part is not
 * well-formed or the root has no end tag. */
static char *build_section_appended(const hp_package_entry *orig, const hp_section *sec) {
    hp_xml_doc *d = parse_entry(orig);
    const hp_xml_node *root = hp_xml_root(d);
    char *buf;
    size_t len = 0, cap;
    size_t pi;
    if (!root || root->inner_start == root->end) {
        hp_xml_free(d); /* <hs:sec/>: nothing to insert into */
        return NULL;
    }
    cap = orig->len + 1024;
    buf = (char *)malloc(cap);
    if (buf) {
        buf[0] = '\0';
    }
    append_range(&buf, &len, &cap, orig->data, 0, root->inner_end);
    for (pi = sec->loaded_paragraph_count; buf && pi < sec->paragraph_count; pi++) {
        /* id 0: Hangul treats paragraph ids as non-unique hints. */
        if (!append_paragraph_xml(&buf, &len, &cap, &sec->paragraphs[pi], 0)) {
            break;
        }
    }
    append_range(&buf, &len, &cap, orig->data, root->inner_end, orig->len);
    hp_xml_free(d);
    return buf;
}

/* content.hpf with only the text of its first <...:title> element replaced.
 * NULL if there is no title element (the part is then kept unchanged). */
static char *patch_hpf_title(const hp_package_entry *orig, const char *title) {
    hp_xml_doc *d = parse_entry(orig);
    const hp_xml_node *el = hp_xml_root(d) ? hp_xml_find(hp_xml_root(d), "title") : NULL;
    char *esc;
    char *buf;
    size_t len = 0, cap;
    if (!el) {
        hp_xml_free(d);
        return NULL;
    }
    esc = xml_escape(title);
    cap = orig->len + (esc ? strlen(esc) : 0) + 128;
    buf = esc ? (char *)malloc(cap) : NULL;
    if (!buf) {
        free(esc);
        hp_xml_free(d);
        return NULL;
    }
    buf[0] = '\0';
    if (el->inner_start == el->end) {
        /* <opf:title/> -> <opf:title>TEXT</opf:title> */
        append_range(&buf, &len, &cap, orig->data, 0, el->end - 2);
        append(&buf, &len, &cap, ">");
        if (buf) {
            append(&buf, &len, &cap, esc);
        }
        if (buf) {
            append(&buf, &len, &cap, "</");
        }
        if (buf) {
            append(&buf, &len, &cap, el->qname);
        }
        if (buf) {
            append(&buf, &len, &cap, ">");
        }
        append_range(&buf, &len, &cap, orig->data, el->end, orig->len);
    } else {
        append_range(&buf, &len, &cap, orig->data, 0, el->inner_start);
        if (buf) {
            append(&buf, &len, &cap, esc);
        }
        append_range(&buf, &len, &cap, orig->data, el->inner_end, orig->len);
    }
    free(esc);
    hp_xml_free(d);
    return buf;
}

static hp_status write_preserving(const hp_document *doc, const char *path) {
    const hp_package *pkg = doc->package;
    char **generated;
    size_t gen_count = 0;
    zip_t *za;
    int zerr = 0;
    hp_status st = HP_OK;
    size_t pass, i;
    int title_changed;

    if (doc->section_count != pkg->section_part_count) {
        /* Sections were added/removed in the IR; no lossless mapping exists. */
        return HP_ERR_UNSUPPORTED;
    }
    /* Never regenerate an opened section through the text-only writer. Check
     * before opening the destination, including saves over the source file. */
    for (i = 0; i < doc->section_count; i++) {
        const hp_section *sec = &doc->sections[i];
        if (sec->needs_rewrite || sec->paragraph_count < sec->loaded_paragraph_count ||
            !hp_package_find(pkg, pkg->section_parts[i])) {
            return HP_ERR_UNSUPPORTED;
        }
    }
    title_changed = strcmp(doc->title ? doc->title : "", pkg->loaded_title ? pkg->loaded_title : "") != 0;

    generated = (char **)calloc(pkg->entry_count + 1, sizeof(char *));
    if (!generated) {
        return HP_ERR_NOMEM;
    }
    za = zip_open(path, ZIP_CREATE | ZIP_TRUNCATE, &zerr);
    if (!za) {
        free(generated);
        return HP_ERR_IO;
    }

    /* pass 0: "mimetype" (must be first, STORED); pass 1: everything else in original order */
    for (pass = 0; pass < 2 && st == HP_OK; pass++) {
        for (i = 0; i < pkg->entry_count && st == HP_OK; i++) {
            const hp_package_entry *e = &pkg->entries[i];
            int is_mime = strcmp(e->name, "mimetype") == 0;
            const void *data = e->data;
            size_t len = e->len;
            char *gen = NULL;
            size_t si;
            zip_int64_t idx = -1;

            if ((pass == 0) != is_mime) {
                continue;
            }
            if (e->is_dir) {
                if (zip_dir_add(za, e->name, ZIP_FL_ENC_UTF_8) < 0) {
                    st = HP_ERR_IO;
                }
                continue;
            }
            for (si = 0; si < pkg->section_part_count; si++) {
                const hp_section *sec;
                if (strcmp(e->name, pkg->section_parts[si]) != 0) {
                    continue;
                }
                sec = &doc->sections[si];
                if (sec->paragraph_count > sec->loaded_paragraph_count) {
                    gen = build_section_appended(e, sec);
                } else {
                    break; /* unchanged */
                }
                if (!gen) {
                    /* Malformed source XML or allocation failure: abort the
                     * transaction instead of falling back to a lossy rewrite. */
                    st = HP_ERR_UNSUPPORTED;
                }
                break;
            }
            if (st == HP_OK && !gen && title_changed && strcmp(e->name, "Contents/content.hpf") == 0) {
                gen = patch_hpf_title(e, doc->title ? doc->title : "");
                /* no <title> element → keep the part unchanged */
            }
            if (st != HP_OK) {
                break;
            }
            if (gen) {
                generated[gen_count++] = gen;
                data = gen;
                len = strlen(gen);
            }
            st = zip_add_bytes_idx(za, e->name, data, len, is_mime || e->stored, &idx);
            if (st == HP_OK && !gen && e->has_mtime && idx >= 0) {
                zip_file_set_mtime(za, (zip_uint64_t)idx, e->mtime, 0);
            }
        }
    }

    if (st != HP_OK) {
        zip_discard(za);
    } else if (zip_close(za) != 0) {
        zip_discard(za);
        st = HP_ERR_IO;
    }
    for (i = 0; i < gen_count; i++) {
        free(generated[i]);
    }
    free(generated);
    return st;
}

hp_status hp_hwpx_write(const hp_document *doc, const char *path) {
    if (!doc || !path) {
        return HP_ERR_INVALID;
    }
    if (!target_writable(path)) {
        return HP_ERR_IO;
    }
    if (doc->package) {
        return write_preserving(doc, path);
    }
    return write_fresh(doc, path);
}
