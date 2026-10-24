#include "hp/hwpx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zip.h>

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

static hp_status zip_add_bytes(zip_t *za, const char *name, const void *data, size_t len, int stored) {
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
    return HP_OK;
}

static char *build_version_xml(void) {
    return hp_strdup(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\" ?>\n"
        "<hv:HCFVersion xmlns:hv=\"http://www.hancom.co.kr/hwpml/2011/version\" "
        "tagetApplication=\"WORDPROCESSOR\" major=\"5\" minor=\"1\" micro=\"0\" "
        "buildNumber=\"0\" os=\"0\" xmlVersion=\"1.5\" application=\"HangulPad\" "
        "appVersion=\"0.1.0\"/>\n");
}

static char *build_container_xml(void) {
    return hp_strdup(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\" ?>\n"
        "<ocf:container xmlns:ocf=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
        "  <ocf:rootfiles>\n"
        "    <ocf:rootfile full-path=\"Contents/content.hpf\" "
        "media-type=\"application/hwpml-package+xml\"/>\n"
        "  </ocf:rootfiles>\n"
        "</ocf:container>\n");
}

static char *build_content_hpf(const hp_document *doc) {
    char *title_esc;
    char *lang_esc;
    char *buf;
    size_t cap = 2048;
    size_t n;
    size_t section_count;
    size_t i;
    const char *title;
    const char *lang;

    title = (doc->title && doc->title[0]) ? doc->title : "Untitled";
    lang = (doc->language && doc->language[0]) ? doc->language : "ko";
    title_esc = xml_escape(title);
    lang_esc = xml_escape(lang);
    if (!title_esc || !lang_esc) {
        free(title_esc);
        free(lang_esc);
        return NULL;
    }

    section_count = doc->section_count ? doc->section_count : 1;
    buf = (char *)malloc(cap);
    if (!buf) {
        free(title_esc);
        free(lang_esc);
        return NULL;
    }

    n = (size_t)snprintf(
        buf, cap,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\" ?>\n"
        "<opf:package version=\"2.0\" unique-identifier=\"uid\" "
        "xmlns:opf=\"http://www.idpf.org/2007/opf/\" "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
        "xmlns:hpf=\"http://www.hancom.co.kr/schema/2011/hpf\">\n"
        "  <opf:metadata>\n"
        "    <dc:title>%s</dc:title>\n"
        "    <dc:language>%s</dc:language>\n"
        "    <dc:identifier id=\"uid\">hangulpad</dc:identifier>\n"
        "    <opf:meta name=\"creator\" content=\"HangulPad\"/>\n"
        "  </opf:metadata>\n"
        "  <opf:manifest>\n"
        "    <opf:item id=\"header\" href=\"header.xml\" media-type=\"application/xml\"/>\n",
        title_esc, lang_esc);
    free(title_esc);
    free(lang_esc);
    if (n >= cap) {
        free(buf);
        return NULL;
    }

    for (i = 0; i < section_count; i++) {
        char line[192];
        int m = snprintf(line, sizeof(line),
                         "    <opf:item id=\"section%zu\" href=\"section%zu.xml\" "
                         "media-type=\"application/xml\"/>\n",
                         i, i);
        if (m < 0 || n + (size_t)m + 1 > cap) {
            char *nbuf;
            cap *= 2;
            nbuf = (char *)realloc(buf, cap);
            if (!nbuf) {
                free(buf);
                return NULL;
            }
            buf = nbuf;
        }
        memcpy(buf + n, line, (size_t)m);
        n += (size_t)m;
        buf[n] = '\0';
    }

    {
        const char *mid = "  </opf:manifest>\n  <opf:spine>\n";
        size_t m = strlen(mid);
        while (n + m + 1 > cap) {
            char *nbuf;
            cap *= 2;
            nbuf = (char *)realloc(buf, cap);
            if (!nbuf) {
                free(buf);
                return NULL;
            }
            buf = nbuf;
        }
        memcpy(buf + n, mid, m);
        n += m;
        buf[n] = '\0';
    }

    for (i = 0; i < section_count; i++) {
        char line[96];
        int m = snprintf(line, sizeof(line), "    <opf:itemref idref=\"section%zu\"/>\n", i);
        while (n + (size_t)m + 64 > cap) {
            char *nbuf;
            cap *= 2;
            nbuf = (char *)realloc(buf, cap);
            if (!nbuf) {
                free(buf);
                return NULL;
            }
            buf = nbuf;
        }
        memcpy(buf + n, line, (size_t)m);
        n += (size_t)m;
        buf[n] = '\0';
    }

    {
        const char *end = "  </opf:spine>\n</opf:package>\n";
        size_t m = strlen(end);
        while (n + m + 1 > cap) {
            char *nbuf;
            cap *= 2;
            nbuf = (char *)realloc(buf, cap);
            if (!nbuf) {
                free(buf);
                return NULL;
            }
            buf = nbuf;
        }
        memcpy(buf + n, end, m + 1);
    }
    return buf;
}

static char *build_header_xml(const hp_document *doc) {
    size_t sec_cnt = doc->section_count ? doc->section_count : 1;
    char *buf;
    int n;

    buf = (char *)malloc(4096);
    if (!buf) {
        return NULL;
    }
    n = snprintf(
        buf, 4096,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\" ?>\n"
        "<hh:head version=\"1.5\" secCnt=\"%zu\" "
        "xmlns:hh=\"http://www.hancom.co.kr/hwpml/2011/head\" "
        "xmlns:hp=\"http://www.hancom.co.kr/hwpml/2011/paragraph\" "
        "xmlns:hc=\"http://www.hancom.co.kr/hwpml/2011/core\">\n"
        "  <hh:beginNum page=\"1\" footnote=\"1\" endnote=\"1\" pic=\"1\" tbl=\"1\" equation=\"1\"/>\n"
        "  <hh:refList>\n"
        "    <hh:fontfaces itemCnt=\"1\">\n"
        "      <hh:fontface lang=\"HANGUL\" fontCnt=\"1\">\n"
        "        <hh:font id=\"0\" face=\"맑은 고딕\" type=\"TTF\" isEmbedded=\"0\"/>\n"
        "      </hh:fontface>\n"
        "    </hh:fontfaces>\n"
        "    <hh:borderFills itemCnt=\"1\">\n"
        "      <hh:borderFill id=\"0\" threeD=\"0\" shadow=\"0\" centerLine=\"NONE\" "
        "breakCellSeparateLine=\"0\">\n"
        "        <hh:leftBorder type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/>\n"
        "        <hh:rightBorder type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/>\n"
        "        <hh:topBorder type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/>\n"
        "        <hh:bottomBorder type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/>\n"
        "        <hh:diagonal type=\"NONE\" width=\"0.1 mm\" color=\"#000000\"/>\n"
        "      </hh:borderFill>\n"
        "    </hh:borderFills>\n"
        "    <hh:charProperties itemCnt=\"1\">\n"
        "      <hh:charPr id=\"0\" height=\"1000\" textColor=\"#000000\" shadeColor=\"none\" "
        "useFontSpace=\"0\" useKerning=\"0\" symMark=\"NONE\" borderFillIDRef=\"0\">\n"
        "        <hh:fontRef hangul=\"0\" latin=\"0\" hanja=\"0\" japanese=\"0\" other=\"0\" "
        "symbol=\"0\" user=\"0\"/>\n"
        "      </hh:charPr>\n"
        "    </hh:charProperties>\n"
        "    <hh:tabProperties itemCnt=\"1\">\n"
        "      <hh:tabPr id=\"0\" autoTabLeft=\"0\" autoTabRight=\"0\"/>\n"
        "    </hh:tabProperties>\n"
        "    <hh:paraProperties itemCnt=\"1\">\n"
        "      <hh:paraPr id=\"0\" tabPrIDRef=\"0\" condense=\"0\" fontLineHeight=\"0\" "
        "snapToGrid=\"1\" suppressLineNumbers=\"0\" checked=\"0\">\n"
        "        <hh:align horizontal=\"JUSTIFY\" vertical=\"BASELINE\"/>\n"
        "        <hh:margin left=\"0\" right=\"0\" prev=\"0\" next=\"0\"/>\n"
        "        <hh:lineSpacing type=\"PERCENT\" value=\"160\" unit=\"HWPUNIT\"/>\n"
        "      </hh:paraPr>\n"
        "    </hh:paraProperties>\n"
        "    <hh:styles itemCnt=\"1\">\n"
        "      <hh:style id=\"0\" type=\"PARA\" name=\"바탕글\" engName=\"Normal\" "
        "paraPrIDRef=\"0\" charPrIDRef=\"0\"/>\n"
        "    </hh:styles>\n"
        "  </hh:refList>\n"
        "</hh:head>\n",
        sec_cnt);
    if (n < 0 || (size_t)n >= 4096) {
        free(buf);
        return NULL;
    }
    return buf;
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

static char *build_section_xml(const hp_section *sec) {
    size_t cap = 2048;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    char tmp[768];
    size_t pi, ri;
    const hp_page *page;

    if (!buf) {
        return NULL;
    }
    buf[0] = '\0';
    page = &sec->page;

    append(&buf, &len, &cap,
           "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\" ?>\n"
           "<hs:sec xmlns:hs=\"http://www.hancom.co.kr/hwpml/2011/section\" "
           "xmlns:hp=\"http://www.hancom.co.kr/hwpml/2011/paragraph\" "
           "xmlns:hc=\"http://www.hancom.co.kr/hwpml/2011/core\">\n");
    if (!buf) {
        return NULL;
    }

    snprintf(tmp, sizeof(tmp),
             "  <hp:p id=\"2147483648\" paraPrIDRef=\"0\" styleIDRef=\"0\" pageBreak=\"0\" "
             "columnBreak=\"0\" merged=\"0\">\n"
             "    <hp:run charPrIDRef=\"0\">\n"
             "      <hp:secPr id=\"\" textDirection=\"HORIZONTAL\" spaceColumns=\"1134\" "
             "tabStop=\"8000\" outlineShapeIDRef=\"1\" memoShapeIDRef=\"0\" "
             "textVerticalWidthHead=\"0\" masterPageCnt=\"0\">\n"
             "        <hp:pagePr landscape=\"0\" width=\"%u\" height=\"%u\" gutterType=\"LEFT_RIGHT\">\n"
             "          <hp:margin header=\"%u\" footer=\"%u\" gutter=\"0\" left=\"%u\" "
             "right=\"%u\" top=\"%u\" bottom=\"%u\"/>\n"
             "        </hp:pagePr>\n"
             "      </hp:secPr>\n"
             "    </hp:run>\n"
             "  </hp:p>\n",
             page->width, page->height, page->margin_top, page->margin_bottom, page->margin_left,
             page->margin_right, page->margin_top, page->margin_bottom);
    append(&buf, &len, &cap, tmp);
    if (!buf) {
        return NULL;
    }

    if (sec->paragraph_count == 0) {
        append(&buf, &len, &cap,
               "  <hp:p id=\"0\" paraPrIDRef=\"0\" styleIDRef=\"0\" pageBreak=\"0\" "
               "columnBreak=\"0\" merged=\"0\">\n"
               "    <hp:run charPrIDRef=\"0\">\n"
               "      <hp:t></hp:t>\n"
               "    </hp:run>\n"
               "  </hp:p>\n");
    } else {
        for (pi = 0; pi < sec->paragraph_count; pi++) {
            const hp_paragraph *para = &sec->paragraphs[pi];
            snprintf(tmp, sizeof(tmp),
                     "  <hp:p id=\"%zu\" paraPrIDRef=\"0\" styleIDRef=\"0\" pageBreak=\"0\" "
                     "columnBreak=\"0\" merged=\"0\">\n"
                     "    <hp:run charPrIDRef=\"0\">\n",
                     pi);
            append(&buf, &len, &cap, tmp);
            if (!buf) {
                return NULL;
            }
            if (para->run_count == 0) {
                append(&buf, &len, &cap, "      <hp:t></hp:t>\n");
            } else {
                for (ri = 0; ri < para->run_count; ri++) {
                    char *esc = xml_escape(para->runs[ri].text);
                    if (!esc) {
                        free(buf);
                        return NULL;
                    }
                    append(&buf, &len, &cap, "      <hp:t>");
                    append(&buf, &len, &cap, esc);
                    append(&buf, &len, &cap, "</hp:t>\n");
                    free(esc);
                    if (!buf) {
                        return NULL;
                    }
                }
            }
            append(&buf, &len, &cap, "    </hp:run>\n  </hp:p>\n");
            if (!buf) {
                return NULL;
            }
        }
    }

    append(&buf, &len, &cap, "</hs:sec>\n");
    return buf;
}

hp_status hp_hwpx_write(const hp_document *doc, const char *path) {
    zip_t *za;
    int zerr = 0;
    hp_status st = HP_OK;
    char *version_xml = NULL;
    char *container_xml = NULL;
    char *content_hpf = NULL;
    char *header_xml = NULL;
    char **section_xmls = NULL;
    size_t section_count;
    size_t i;
    const char *mimetype = HP_HWPX_MIMETYPE;

    if (!doc || !path) {
        return HP_ERR_INVALID;
    }

    section_count = doc->section_count ? doc->section_count : 1;
    section_xmls = (char **)calloc(section_count, sizeof(char *));
    if (!section_xmls) {
        return HP_ERR_NOMEM;
    }

    version_xml = build_version_xml();
    container_xml = build_container_xml();
    content_hpf = build_content_hpf(doc);
    header_xml = build_header_xml(doc);
    if (!version_xml || !container_xml || !content_hpf || !header_xml) {
        st = HP_ERR_NOMEM;
        goto cleanup;
    }

    for (i = 0; i < section_count; i++) {
        hp_section empty;
        const hp_section *sec;
        if (doc->section_count > 0) {
            sec = &doc->sections[i];
        } else {
            memset(&empty, 0, sizeof(empty));
            hp_page_default(&empty.page);
            sec = &empty;
        }
        section_xmls[i] = build_section_xml(sec);
        if (!section_xmls[i]) {
            st = HP_ERR_NOMEM;
            goto cleanup;
        }
    }

    /* Remove existing file so zip_open create is clean. */
    remove(path);
    za = zip_open(path, ZIP_CREATE | ZIP_TRUNCATE, &zerr);
    if (!za) {
        st = HP_ERR_IO;
        goto cleanup;
    }

    /* mimetype first + STORED */
    st = zip_add_bytes(za, "mimetype", mimetype, strlen(mimetype), 1);
    if (st != HP_OK) {
        zip_discard(za);
        goto cleanup;
    }
    st = zip_add_bytes(za, "version.xml", version_xml, strlen(version_xml), 0);
    if (st != HP_OK) {
        zip_discard(za);
        goto cleanup;
    }
    st = zip_add_bytes(za, "META-INF/container.xml", container_xml, strlen(container_xml), 0);
    if (st != HP_OK) {
        zip_discard(za);
        goto cleanup;
    }
    st = zip_add_bytes(za, "Contents/content.hpf", content_hpf, strlen(content_hpf), 0);
    if (st != HP_OK) {
        zip_discard(za);
        goto cleanup;
    }
    st = zip_add_bytes(za, "Contents/header.xml", header_xml, strlen(header_xml), 0);
    if (st != HP_OK) {
        zip_discard(za);
        goto cleanup;
    }
    for (i = 0; i < section_count; i++) {
        char name[64];
        snprintf(name, sizeof(name), "Contents/section%zu.xml", i);
        st = zip_add_bytes(za, name, section_xmls[i], strlen(section_xmls[i]), 0);
        if (st != HP_OK) {
            zip_discard(za);
            goto cleanup;
        }
    }

    if (zip_close(za) != 0) {
        st = HP_ERR_IO;
        goto cleanup;
    }

cleanup:
    free(version_xml);
    free(container_xml);
    free(content_hpf);
    free(header_xml);
    if (section_xmls) {
        for (i = 0; i < section_count; i++) {
            free(section_xmls[i]);
        }
        free(section_xmls);
    }
    return st;
}
