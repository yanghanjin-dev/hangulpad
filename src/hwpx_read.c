#include "hp/hwpx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zip.h>

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
        /* trim trailing whitespace */
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
    /* Find <tag or <prefix:tag */
    const char *p = hay;
    size_t tlen = strlen(tag);
    while ((p = strchr(p, '<')) != NULL) {
        const char *q = p + 1;
        const char *colon;
        if (*q == '/') {
            p = q;
            continue;
        }
        colon = strchr(q, ':');
        if (colon && colon < strchr(q, '>') && colon < strchr(q, ' ')) {
            /* has prefix — compare local name */
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
    while (*p && *p != '>') {
        p++;
    }
    return *p ? p : NULL;
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

static char *unescape_xml(const char *s, size_t n) {
    char *out = (char *)malloc(n + 1);
    size_t i = 0, j = 0;
    if (!out) {
        return NULL;
    }
    while (i < n) {
        if (s[i] == '&') {
            if (i + 4 <= n && strncmp(s + i, "&amp;", 5) == 0) {
                out[j++] = '&';
                i += 5;
            } else if (i + 3 <= n && strncmp(s + i, "&lt;", 4) == 0) {
                out[j++] = '<';
                i += 4;
            } else if (i + 3 <= n && strncmp(s + i, "&gt;", 4) == 0) {
                out[j++] = '>';
                i += 4;
            } else if (i + 5 <= n && strncmp(s + i, "&quot;", 6) == 0) {
                out[j++] = '"';
                i += 6;
            } else if (i + 5 <= n && strncmp(s + i, "&apos;", 6) == 0) {
                out[j++] = '\'';
                i += 6;
            } else {
                out[j++] = s[i++];
            }
        } else {
            out[j++] = s[i++];
        }
    }
    out[j] = '\0';
    return out;
}

static hp_status parse_section_xml(const char *xml, hp_section *sec) {
    const char *p = xml;
    hp_page_default(&sec->page);
    sec->paragraphs = NULL;
    sec->paragraph_count = 0;

    while ((p = find_tag_open(p, "p")) != NULL) {
        const char *p_gt = find_close_gt(p);
        const char *p_end;
        const char *scan;
        hp_paragraph para;
        int has_text = 0;

        if (!p_gt) {
            break;
        }
        if (p_gt[-1] == '/') {
            p = p_gt + 1;
            continue;
        }
        scan = p_gt + 1;

        /* Find </…p> */
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
        /* Collect <t>…</t> inside paragraph */
        {
            const char *q = scan;
            while (q < p_end && (q = find_tag_open(q, "t")) != NULL && q < p_end) {
                const char *t_gt = find_close_gt(q);
                const char *t_end;
                char *text;
                hp_run *runs;

                if (!t_gt || t_gt >= p_end) {
                    break;
                }
                if (t_gt[-1] == '/') {
                    text = hp_strdup("");
                    q = t_gt + 1;
                } else {
                    t_end = strstr(t_gt + 1, "</");
                    while (t_end && t_end < p_end) {
                        const char *name = t_end + 2;
                        const char *colon = strchr(name, ':');
                        const char *cmp =
                            (colon && colon < strchr(name, '>')) ? colon + 1 : name;
                        if (strncmp(cmp, "t>", 2) == 0) {
                            break;
                        }
                        t_end = strstr(t_end + 2, "</");
                    }
                    if (!t_end || t_end > p_end) {
                        break;
                    }
                    text = unescape_xml(t_gt + 1, (size_t)(t_end - (t_gt + 1)));
                    q = t_end + 1;
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
                para.runs[para.run_count].text = text;
                para.run_count++;
                has_text = 1;
            }
        }

        if (has_text) {
            hp_paragraph *paras =
                (hp_paragraph *)realloc(sec->paragraphs, (sec->paragraph_count + 1) * sizeof(*paras));
            if (!paras) {
                size_t i;
                for (i = 0; i < para.run_count; i++) {
                    free(para.runs[i].text);
                }
                free(para.runs);
                return HP_ERR_NOMEM;
            }
            sec->paragraphs = paras;
            sec->paragraphs[sec->paragraph_count++] = para;
        } else {
            size_t i;
            for (i = 0; i < para.run_count; i++) {
                free(para.runs[i].text);
            }
            free(para.runs);
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
    char *section_xml = NULL;
    hp_document *doc = NULL;
    hp_status st = HP_OK;
    char *title = NULL;
    char *lang = NULL;

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
        while (n > 0 && (mimetype[n - 1] == '\n' || mimetype[n - 1] == '\r' || mimetype[n - 1] == ' ')) {
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

    section_xml = zip_read_entry(za, "Contents/section0.xml");
    if (!section_xml) {
        st = HP_ERR_INVALID;
        goto done;
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

    /* Replace default empty section contents */
    {
        size_t i;
        for (i = 0; i < doc->sections[0].paragraph_count; i++) {
            size_t r;
            for (r = 0; r < doc->sections[0].paragraphs[i].run_count; r++) {
                free(doc->sections[0].paragraphs[i].runs[r].text);
            }
            free(doc->sections[0].paragraphs[i].runs);
        }
        free(doc->sections[0].paragraphs);
        doc->sections[0].paragraphs = NULL;
        doc->sections[0].paragraph_count = 0;
    }

    st = parse_section_xml(section_xml, &doc->sections[0]);
    if (st != HP_OK) {
        hp_document_free(doc);
        doc = NULL;
        goto done;
    }

    *out_doc = doc;
    doc = NULL;

done:
    free(mimetype);
    free(hpf);
    free(section_xml);
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
