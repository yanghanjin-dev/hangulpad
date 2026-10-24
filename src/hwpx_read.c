#include "hp/hwpx.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zip.h>

#define HP_MAX_STYLE 512
#define HP_MAX_FONTS 64

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
    char *faces[HP_MAX_FONTS]; /* hangul face by id */
    hp_char_style chars[HP_MAX_STYLE];
    hp_para_style paras[HP_MAX_STYLE];
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
    size_t i;
    memset(st, 0, sizeof(*st));
    for (i = 0; i < HP_MAX_FONTS; i++) {
        st->faces[i] = NULL;
    }
}

static void style_tables_free(hp_style_tables *st) {
    size_t i;
    if (!st) {
        return;
    }
    for (i = 0; i < HP_MAX_FONTS; i++) {
        free(st->faces[i]);
        st->faces[i] = NULL;
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

static const char *find_tag_open(const char *hay, const char *tag) {
    const char *p = hay;
    size_t tlen = strlen(tag);
    while ((p = strchr(p, '<')) != NULL) {
        const char *q = p + 1;
        const char *colon;
        if (*q == '/' || *q == '!' || *q == '?') {
            p = q;
            continue;
        }
        colon = strchr(q, ':');
        if (colon && colon < strchr(q, '>') && (!strchr(q, ' ') || colon < strchr(q, ' '))) {
            const char *local = colon + 1;
            if (strncmp(local, tag, tlen) == 0 &&
                (local[tlen] == ' ' || local[tlen] == '>' || local[tlen] == '/' ||
                 local[tlen] == '\t' || local[tlen] == '\n' || local[tlen] == '\r')) {
                return p;
            }
        } else if (strncmp(q, tag, tlen) == 0 &&
                   (q[tlen] == ' ' || q[tlen] == '>' || q[tlen] == '/' || q[tlen] == '\t' ||
                    q[tlen] == '\n' || q[tlen] == '\r')) {
            return p;
        }
        p = q;
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

static int attr_int(const char *tag_open, const char *name, int def) {
    char key[64];
    const char *p;
    size_t n = strlen(name);
    if (n + 3 >= sizeof(key)) {
        return def;
    }
    snprintf(key, sizeof(key), "%s=\"", name);
    p = strstr(tag_open, key);
    if (!p) {
        return def;
    }
    p += n + 2;
    return (int)strtol(p, NULL, 10);
}

static int attr_str(const char *tag_open, const char *name, char *out, size_t out_sz) {
    char key[64];
    const char *p;
    const char *end;
    size_t n = strlen(name);
    size_t len;
    if (!out || out_sz == 0 || n + 3 >= sizeof(key)) {
        return 0;
    }
    snprintf(key, sizeof(key), "%s=\"", name);
    p = strstr(tag_open, key);
    if (!p) {
        out[0] = '\0';
        return 0;
    }
    p += n + 2;
    end = strchr(p, '"');
    if (!end) {
        out[0] = '\0';
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
    if (!s || s[0] == '\0' || strcmp(s, "none") == 0) {
        return 0;
    }
    if (s[0] == '#') {
        if (strlen(s) >= 7) {
            sscanf(s + 1, "%02x%02x%02x", &r, &g, &b);
        }
    }
    return (r << 16) | (g << 8) | b;
}

static char *extract_dc_field(const char *xml, const char *local) {
    const char *start;
    const char *gt;
    const char *end;
    size_t n;
    char *out;

    start = find_tag_open(xml, local);
    if (!start) {
        return hp_strdup("");
    }
    gt = find_close_gt(start);
    if (!gt || gt[0] != '>' || (gt > start && gt[-1] == '/')) {
        return hp_strdup("");
    }
    start = gt + 1;
    end = strstr(start, "</");
    while (end) {
        const char *name = end + 2;
        const char *gt2 = strchr(name, '>');
        const char *colon = strchr(name, ':');
        const char *cmp = (colon && gt2 && colon < gt2) ? colon + 1 : name;
        size_t llen = strlen(local);
        if (gt2 && strncmp(cmp, local, llen) == 0 && cmp[llen] == '>') {
            break;
        }
        end = strstr(end + 2, "</");
    }
    if (!end) {
        return hp_strdup("");
    }
    n = (size_t)(end - start);
    out = (char *)malloc(n + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, start, n);
    out[n] = '\0';
    return out;
}

/* Unescape XML entities and strip any nested markup (e.g. <hp:tab .../>). */
static char *clean_text_xml(const char *s, size_t n) {
    char *tmp = (char *)malloc(n + 1);
    char *out;
    size_t i = 0, j = 0;
    if (!tmp) {
        return NULL;
    }
    while (i < n) {
        if (s[i] == '<') {
            /* treat inline breaks/tabs as space */
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
            if (i + 4 <= n && strncmp(s + i, "&amp;", 5) == 0) {
                tmp[j++] = '&';
                i += 5;
            } else if (i + 3 <= n && strncmp(s + i, "&lt;", 4) == 0) {
                tmp[j++] = '<';
                i += 4;
            } else if (i + 3 <= n && strncmp(s + i, "&gt;", 4) == 0) {
                tmp[j++] = '>';
                i += 4;
            } else if (i + 5 <= n && strncmp(s + i, "&quot;", 6) == 0) {
                tmp[j++] = '"';
                i += 6;
            } else if (i + 5 <= n && strncmp(s + i, "&apos;", 6) == 0) {
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

    /* Hangul font faces: first <hh:fontface lang="HANGUL" ...> block */
    p = xml;
    while ((p = find_tag_open(p, "fontface")) != NULL) {
        char lang[32];
        const char *gt = find_close_gt(p);
        const char *end;
        if (!gt) {
            break;
        }
        attr_str(p, "lang", lang, sizeof(lang));
        if (gt[-1] == '/') {
            p = gt + 1;
            continue;
        }
        end = strstr(gt + 1, "</");
        while (end) {
            const char *name = end + 2;
            const char *colon = strchr(name, ':');
            const char *cmp = (colon && colon < strchr(name, '>')) ? colon + 1 : name;
            if (strncmp(cmp, "fontface>", 9) == 0) {
                break;
            }
            end = strstr(end + 2, "</");
        }
        if (!end) {
            break;
        }
        if (strcmp(lang, "HANGUL") == 0) {
            const char *q = gt + 1;
            while (q < end && (q = find_tag_open(q, "font")) != NULL && q < end) {
                const char *fgt = find_close_gt(q);
                int id;
                char face[128];
                if (!fgt || fgt >= end) {
                    break;
                }
                id = attr_int(q, "id", -1);
                if (id >= 0 && id < HP_MAX_FONTS && attr_str(q, "face", face, sizeof(face))) {
                    free(st->faces[id]);
                    st->faces[id] = hp_strdup(face);
                }
                q = fgt + 1;
            }
            break; /* only first HANGUL face table */
        }
        p = end + 1;
    }

    /* charPr */
    p = xml;
    while ((p = find_tag_open(p, "charPr")) != NULL) {
        const char *gt = find_close_gt(p);
        const char *end;
        int id;
        char color[32];
        if (!gt) {
            break;
        }
        id = attr_int(p, "id", -1);
        if (gt[-1] == '/') {
            if (id >= 0 && id < HP_MAX_STYLE) {
                st->chars[id].font_size_hwp = (uint32_t)attr_int(p, "height", 1000);
                attr_str(p, "textColor", color, sizeof(color));
                st->chars[id].color_rgb = parse_color(color);
                st->chars[id].bold = 0;
                st->chars[id].hangul_font_id = 0;
                st->chars[id].valid = 1;
            }
            p = gt + 1;
            continue;
        }
        end = strstr(gt + 1, "</");
        while (end) {
            const char *name = end + 2;
            const char *colon = strchr(name, ':');
            const char *cmp = (colon && colon < strchr(name, '>')) ? colon + 1 : name;
            if (strncmp(cmp, "charPr>", 7) == 0) {
                break;
            }
            end = strstr(end + 2, "</");
        }
        if (!end) {
            break;
        }
        if (id >= 0 && id < HP_MAX_STYLE) {
            const char *fontref = find_tag_open(gt + 1, "fontRef");
            st->chars[id].font_size_hwp = (uint32_t)attr_int(p, "height", 1000);
            attr_str(p, "textColor", color, sizeof(color));
            st->chars[id].color_rgb = parse_color(color);
            st->chars[id].bold = (strstr(gt + 1, "<hh:bold") != NULL || strstr(gt + 1, "<bold") != NULL) &&
                                  (end > gt);
            if (st->chars[id].bold) {
                /* ensure bold tag is before end */
                const char *b = strstr(gt + 1, "<hh:bold");
                if (!b) {
                    b = strstr(gt + 1, "<bold");
                }
                st->chars[id].bold = (b && b < end);
            }
            st->chars[id].hangul_font_id = 0;
            if (fontref && fontref < end) {
                st->chars[id].hangul_font_id = attr_int(fontref, "hangul", 0);
            }
            st->chars[id].valid = 1;
        }
        p = end + 1;
    }

    /* paraPr */
    p = xml;
    while ((p = find_tag_open(p, "paraPr")) != NULL) {
        const char *gt = find_close_gt(p);
        const char *end;
        int id;
        if (!gt) {
            break;
        }
        id = attr_int(p, "id", -1);
        if (gt[-1] == '/') {
            p = gt + 1;
            continue;
        }
        end = strstr(gt + 1, "</");
        while (end) {
            const char *name = end + 2;
            const char *colon = strchr(name, ':');
            const char *cmp = (colon && colon < strchr(name, '>')) ? colon + 1 : name;
            if (strncmp(cmp, "paraPr>", 7) == 0) {
                break;
            }
            end = strstr(end + 2, "</");
        }
        if (!end) {
            break;
        }
        if (id >= 0 && id < HP_MAX_STYLE) {
            const char *al = find_tag_open(gt + 1, "align");
            char horiz[32];
            st->paras[id].align = HP_ALIGN_JUSTIFY;
            if (al && al < end && attr_str(al, "horizontal", horiz, sizeof(horiz))) {
                st->paras[id].align = parse_align_name(horiz);
            }
            st->paras[id].valid = 1;
        }
        p = end + 1;
    }
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

static hp_status parse_section_xml(const char *xml, hp_section *sec, const hp_style_tables *styles) {
    const char *p = xml;
    hp_page_default(&sec->page);
    sec->paragraphs = NULL;
    sec->paragraph_count = 0;

    while ((p = find_tag_open(p, "p")) != NULL) {
        const char *p_gt = find_close_gt(p);
        const char *p_end;
        const char *scan;
        hp_paragraph para;
        int para_pr_id;
        int has_content = 0;

        if (!p_gt) {
            break;
        }
        if (p_gt[-1] == '/') {
            p = p_gt + 1;
            continue;
        }
        para_pr_id = attr_int(p, "paraPrIDRef", 0);
        scan = p_gt + 1;

        p_end = strstr(scan, "</");
        while (p_end) {
            const char *name = p_end + 2;
            const char *colon = strchr(name, ':');
            const char *cmp = (colon && colon < strchr(name, '>')) ? colon + 1 : name;
            if (strncmp(cmp, "p>", 2) == 0) {
                break;
            }
            p_end = strstr(p_end + 2, "</");
        }
        if (!p_end) {
            break;
        }

        memset(&para, 0, sizeof(para));
        para.align = HP_ALIGN_JUSTIFY;
        if (styles && para_pr_id >= 0 && para_pr_id < HP_MAX_STYLE && styles->paras[para_pr_id].valid) {
            para.align = styles->paras[para_pr_id].align;
        }

        /* Walk <hp:run>… then nested <hp:t> */
        {
            const char *q = scan;
            while (q < p_end && (q = find_tag_open(q, "run")) != NULL && q < p_end) {
                const char *run_gt = find_close_gt(q);
                const char *run_end;
                int char_id;
                const char *tq;

                if (!run_gt || run_gt >= p_end) {
                    break;
                }
                char_id = attr_int(q, "charPrIDRef", 0);
                if (run_gt[-1] == '/') {
                    q = run_gt + 1;
                    continue;
                }
                run_end = strstr(run_gt + 1, "</");
                while (run_end && run_end < p_end) {
                    const char *name = run_end + 2;
                    const char *colon = strchr(name, ':');
                    const char *cmp = (colon && colon < strchr(name, '>')) ? colon + 1 : name;
                    if (strncmp(cmp, "run>", 4) == 0) {
                        break;
                    }
                    run_end = strstr(run_end + 2, "</");
                }
                if (!run_end || run_end > p_end) {
                    break;
                }

                tq = run_gt + 1;
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
                        t_end = strstr(t_gt + 1, "</");
                        while (t_end && t_end < run_end) {
                            const char *name = t_end + 2;
                            const char *colon = strchr(name, ':');
                            const char *cmp =
                                (colon && colon < strchr(name, '>')) ? colon + 1 : name;
                            if (strncmp(cmp, "t>", 2) == 0) {
                                break;
                            }
                            t_end = strstr(t_end + 2, "</");
                        }
                        if (!t_end || t_end > run_end) {
                            break;
                        }
                        text = clean_text_xml(t_gt + 1, (size_t)(t_end - (t_gt + 1)));
                        tq = t_end + 1;
                    }
                    if (!text) {
                        return HP_ERR_NOMEM;
                    }
                    runs = (hp_run *)realloc(para.runs, (para.run_count + 1) * sizeof(hp_run));
                    if (!runs) {
                        free(text);
                        return HP_ERR_NOMEM;
                    }
                    para.runs = runs;
                    memset(&para.runs[para.run_count], 0, sizeof(hp_run));
                    para.runs[para.run_count].text = text;
                    apply_char_style(&para.runs[para.run_count], styles, char_id);
                    para.run_count++;
                    has_content = 1;
                }
                q = run_end + 1;
            }
        }

        /* Keep empty paragraphs as vertical spacing (blank lines). */
        if (!has_content) {
            hp_run *runs = (hp_run *)calloc(1, sizeof(hp_run));
            if (runs) {
                runs[0].text = hp_strdup("");
                runs[0].font_size_hwp = 1000;
                para.runs = runs;
                para.run_count = 1;
                has_content = 1;
            }
        }

        if (has_content) {
            hp_paragraph *paras =
                (hp_paragraph *)realloc(sec->paragraphs, (sec->paragraph_count + 1) * sizeof(*paras));
            if (!paras) {
                size_t i;
                for (i = 0; i < para.run_count; i++) {
                    free(para.runs[i].text);
                    free(para.runs[i].font_face);
                }
                free(para.runs);
                return HP_ERR_NOMEM;
            }
            sec->paragraphs = paras;
            sec->paragraphs[sec->paragraph_count++] = para;
        }

        p = p_end + 1;
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
        if (!lang) {
            st = HP_ERR_NOMEM;
            goto done;
        }
    }

    header_xml = zip_read_entry(za, "Contents/header.xml");
    if (header_xml) {
        parse_header_styles(header_xml, &styles);
    }

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

    /* Clear default empty section; we'll rebuild from all sectionN.xml */
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
