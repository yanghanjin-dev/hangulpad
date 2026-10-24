/*
 * HangulPad Windows GUI — HWPX engine layout viewer.
 * Renders paragraphs (font/size/align), tables, and images from the IR.
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include "hp/hwpx.h"
#include "hp/ir.h"

#include <windows.h>
#include <commdlg.h>
#include <wincodec.h>
#include <objbase.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>

#define ID_BTN_PREV 1002
#define ID_BTN_NEXT 1003
#define ID_STATUS 1004
#define IDM_FILE_OPEN 40002
#define IDM_FILE_OPEN_SAMPLE 40003
#define IDM_FILE_EXIT 40005
#define IDM_HELP_ABOUT 40010

#define TOOLBAR_H 40
#define MAX_LAYOUT_PAGES 512
#define MAX_FLAT_BLOCKS 8192
#define PAGE_W_PX 794
#define PAGE_H_PX 1123
#define MARGIN_X 56
#define MARGIN_Y 64

typedef struct {
    size_t sec;
    size_t block;
} flat_ref;

typedef struct {
    int first;
    int count;
} layout_page;

static HWND g_main;
static HWND g_btn_prev;
static HWND g_btn_next;
static HWND g_status;
static hp_document *g_doc;
static flat_ref g_flat[MAX_FLAT_BLOCKS];
static int g_flat_count;
static layout_page g_pages[MAX_LAYOUT_PAGES];
static int g_page_count;
static int g_page_index;
static IWICImagingFactory *g_wic;

static wchar_t *utf8_to_wide(const char *utf8) {
    int n;
    wchar_t *w;
    if (!utf8) {
        utf8 = "";
    }
    n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (n <= 0) {
        return NULL;
    }
    w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) {
        return NULL;
    }
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n);
    return w;
}

static char *wide_to_utf8(const wchar_t *w) {
    int n;
    char *u;
    if (!w) {
        w = L"";
    }
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) {
        return NULL;
    }
    u = (char *)malloc((size_t)n);
    if (!u) {
        return NULL;
    }
    WideCharToMultiByte(CP_UTF8, 0, w, -1, u, n, NULL, NULL);
    return u;
}

static void map_face(const char *hw, wchar_t *out, size_t out_n) {
    if (!hw || !hw[0]) {
        wcsncpy(out, L"Malgun Gothic", out_n - 1);
    } else if (strstr(hw, "바탕") || strstr(hw, "명조") || strstr(hw, "신명조")) {
        wcsncpy(out, L"Batang", out_n - 1);
    } else {
        wcsncpy(out, L"Malgun Gothic", out_n - 1);
    }
    out[out_n - 1] = 0;
}

static HFONT make_font(HDC hdc, const hp_run *run) {
    wchar_t face[64];
    LOGFONTW lf;
    int pt = (int)((run->font_size_hwp ? run->font_size_hwp : 1000) + 50) / 100;
    int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    if (pt < 6) {
        pt = 6;
    }
    if (pt > 72) {
        pt = 72;
    }
    if (dpi <= 0) {
        dpi = 96;
    }
    map_face(run->font_face, face, 64);
    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight = -MulDiv(pt, dpi, 72);
    lf.lfWeight = run->bold ? FW_BOLD : FW_NORMAL;
    lf.lfCharSet = HANGUL_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    return CreateFontIndirectW(&lf);
}

static void update_status(void) {
    wchar_t buf[200];
    if (g_doc && g_page_count > 0) {
        swprintf(buf, 200, L"페이지 %d / %d  —  엔진 렌더 (글꼴·표·이미지)", g_page_index + 1,
                 g_page_count);
    } else {
        wcscpy(buf, L"파일 → 열기 또는 샘플 문서 열기");
    }
    SetWindowTextW(g_status, buf);
    EnableWindow(g_btn_prev, g_page_index > 0);
    EnableWindow(g_btn_next, g_page_index + 1 < g_page_count);
}

static void rebuild_flat(void) {
    size_t si, bi;
    g_flat_count = 0;
    if (!g_doc) {
        return;
    }
    for (si = 0; si < g_doc->section_count; si++) {
        const hp_section *sec = &g_doc->sections[si];
        for (bi = 0; bi < sec->block_count; bi++) {
            if (g_flat_count >= MAX_FLAT_BLOCKS) {
                return;
            }
            g_flat[g_flat_count].sec = si;
            g_flat[g_flat_count].block = bi;
            g_flat_count++;
        }
    }
}

static const hp_block *flat_block(int idx) {
    if (idx < 0 || idx >= g_flat_count || !g_doc) {
        return NULL;
    }
    return &g_doc->sections[g_flat[idx].sec].blocks[g_flat[idx].block];
}

static int para_is_blank(const hp_paragraph *para) {
    size_t i;
    if (!para || para->run_count == 0) {
        return 1;
    }
    for (i = 0; i < para->run_count; i++) {
        const char *t = para->runs[i].text;
        if (t) {
            while (*t) {
                if ((unsigned char)*t > 32) {
                    return 0;
                }
                t++;
            }
        }
    }
    return 1;
}

static int measure_para_height(HDC hdc, const hp_paragraph *para, int content_w) {
    size_t ri;
    int max_line = 14;
    int lines = 1;
    int x = 0;
    if (!para || para_is_blank(para)) {
        if (para && para->run_count && para->runs[0].font_size_hwp) {
            return (int)(para->runs[0].font_size_hwp / 100) + 4;
        }
        return 10;
    }
    for (ri = 0; ri < para->run_count; ri++) {
        const hp_run *run = &para->runs[ri];
        wchar_t *w;
        HFONT font, old;
        TEXTMETRICW tm;
        SIZE sz;
        int i, len;
        if (!run->text || !run->text[0]) {
            continue;
        }
        w = utf8_to_wide(run->text);
        if (!w) {
            continue;
        }
        font = make_font(hdc, run);
        old = (HFONT)SelectObject(hdc, font);
        GetTextMetricsW(hdc, &tm);
        if (tm.tmHeight + tm.tmExternalLeading > max_line) {
            max_line = tm.tmHeight + tm.tmExternalLeading;
        }
        len = (int)wcslen(w);
        for (i = 0; i < len; i++) {
            GetTextExtentPoint32W(hdc, &w[i], 1, &sz);
            if (x + sz.cx > content_w && x > 0) {
                lines++;
                x = 0;
            }
            x += sz.cx;
        }
        SelectObject(hdc, old);
        DeleteObject(font);
        free(w);
    }
    return lines * max_line + 4;
}

static HBITMAP decode_image_mem(const unsigned char *data, size_t len, int *out_w, int *out_h) {
    HRESULT hr;
    IStream *stream = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    HGLOBAL hg;
    void *ptr;
    UINT w = 0, h = 0, stride, size;
    BYTE *bits = NULL;
    BITMAPINFO bmi;
    HDC hdc;
    void *dib = NULL;
    HBITMAP hbmp = NULL;

    *out_w = *out_h = 0;
    if (!g_wic || !data || len == 0) {
        return NULL;
    }
    hg = GlobalAlloc(GMEM_MOVEABLE, len);
    if (!hg) {
        return NULL;
    }
    ptr = GlobalLock(hg);
    if (!ptr) {
        GlobalFree(hg);
        return NULL;
    }
    memcpy(ptr, data, len);
    GlobalUnlock(hg);
    hr = CreateStreamOnHGlobal(hg, TRUE, &stream);
    if (FAILED(hr) || !stream) {
        GlobalFree(hg);
        return NULL;
    }
    hr = g_wic->lpVtbl->CreateDecoderFromStream(g_wic, stream, NULL, WICDecodeMetadataCacheOnLoad, &dec);
    if (FAILED(hr) || !dec) {
        goto done;
    }
    hr = dec->lpVtbl->GetFrame(dec, 0, &frame);
    if (FAILED(hr) || !frame) {
        goto done;
    }
    hr = g_wic->lpVtbl->CreateFormatConverter(g_wic, &conv);
    if (FAILED(hr) || !conv) {
        goto done;
    }
    hr = conv->lpVtbl->Initialize(conv, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppBGR,
                                  WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        goto done;
    }
    conv->lpVtbl->GetSize(conv, &w, &h);
    if (w == 0 || h == 0) {
        goto done;
    }
    stride = w * 4;
    size = stride * h;
    bits = (BYTE *)malloc(size);
    if (!bits) {
        goto done;
    }
    if (FAILED(conv->lpVtbl->CopyPixels(conv, NULL, stride, size, bits))) {
        goto done;
    }
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = (LONG)w;
    bmi.bmiHeader.biHeight = -(LONG)h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    hdc = GetDC(NULL);
    hbmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &dib, NULL, 0);
    ReleaseDC(NULL, hdc);
    if (hbmp && dib) {
        memcpy(dib, bits, size);
        *out_w = (int)w;
        *out_h = (int)h;
    } else if (hbmp) {
        DeleteObject(hbmp);
        hbmp = NULL;
    }
done:
    free(bits);
    if (conv) {
        conv->lpVtbl->Release(conv);
    }
    if (frame) {
        frame->lpVtbl->Release(frame);
    }
    if (dec) {
        dec->lpVtbl->Release(dec);
    }
    if (stream) {
        stream->lpVtbl->Release(stream);
    }
    return hbmp;
}

static int measure_image_height(const hp_image *img, int content_w) {
    int w, h;
    if (!img) {
        return 8;
    }
    w = (int)hp_hwp_to_px(img->width_hwp);
    h = (int)hp_hwp_to_px(img->height_hwp);
    if (w <= 0) {
        w = content_w;
    }
    if (h <= 0) {
        h = 40;
    }
    if (w > content_w && w > 0) {
        h = h * content_w / w;
        w = content_w;
    }
    return h + 8;
}

static int measure_table_height(HDC hdc, const hp_table *t, int content_w) {
    size_t i;
    int *row_h;
    int rows;
    int total = 0;
    int r;
    if (!t || t->cell_count == 0) {
        return 20;
    }
    rows = t->row_count > 0 ? t->row_count : 1;
    row_h = (int *)calloc((size_t)rows, sizeof(int));
    if (!row_h) {
        return (int)hp_hwp_to_px(t->height_hwp) + 8;
    }
    for (i = 0; i < t->cell_count; i++) {
        const hp_table_cell *c = &t->cells[i];
        int cw = c->width_hwp ? (int)hp_hwp_to_px(c->width_hwp) : content_w / (t->col_count ? t->col_count : 1);
        int ch = c->height_hwp ? (int)hp_hwp_to_px(c->height_hwp) : 0;
        int text_h = 0;
        size_t p;
        if (c->row < 0 || c->row >= rows) {
            continue;
        }
        if (c->image) {
            int ih = measure_image_height(c->image, cw > 8 ? cw - 8 : cw);
            if (ih > ch) {
                ch = ih;
            }
        }
        for (p = 0; p < c->paragraph_count; p++) {
            text_h += measure_para_height(hdc, &c->paragraphs[p], cw > 8 ? cw - 8 : cw);
        }
        if (text_h + 6 > ch) {
            ch = text_h + 6;
        }
        if (ch < 18) {
            ch = 18;
        }
        if (ch > row_h[c->row]) {
            row_h[c->row] = ch;
        }
    }
    for (r = 0; r < rows; r++) {
        total += row_h[r] > 0 ? row_h[r] : 18;
    }
    free(row_h);
    return total + 10;
}

static int measure_block_height(HDC hdc, const hp_block *b, int content_w) {
    if (!b) {
        return 8;
    }
    if (b->kind == HP_BLOCK_PARAGRAPH) {
        return measure_para_height(hdc, &b->paragraph, content_w);
    }
    if (b->kind == HP_BLOCK_TABLE && b->table) {
        return measure_table_height(hdc, b->table, content_w);
    }
    if (b->kind == HP_BLOCK_IMAGE && b->image) {
        return measure_image_height(b->image, content_w);
    }
    return 8;
}

static void rebuild_pages(HWND hwnd) {
    HDC hdc;
    int content_w = PAGE_W_PX - 2 * MARGIN_X;
    int content_h = PAGE_H_PX - 2 * MARGIN_Y;
    int y = 0;
    int i;
    int page_first = 0;
    int cols = 1;

    g_page_count = 0;
    if (g_flat_count == 0) {
        return;
    }
    if (g_doc && g_doc->section_count > 0 && g_doc->sections[0].column_count > 1) {
        cols = g_doc->sections[0].column_count;
        content_w = (content_w - (cols - 1) * 16) / cols;
    }
    (void)cols;
    hdc = GetDC(hwnd ? hwnd : NULL);
    if (!hdc) {
        return;
    }
    for (i = 0; i < g_flat_count; i++) {
        const hp_block *b = flat_block(i);
        int h = measure_block_height(hdc, b, content_w);
        if (y > 0 && y + h > content_h) {
            if (g_page_count < MAX_LAYOUT_PAGES) {
                g_pages[g_page_count].first = page_first;
                g_pages[g_page_count].count = i - page_first;
                g_page_count++;
            }
            page_first = i;
            y = 0;
        }
        y += h;
    }
    if (page_first < g_flat_count && g_page_count < MAX_LAYOUT_PAGES) {
        g_pages[g_page_count].first = page_first;
        g_pages[g_page_count].count = g_flat_count - page_first;
        g_page_count++;
    }
    ReleaseDC(hwnd, hdc);
    if (g_page_index >= g_page_count) {
        g_page_index = g_page_count > 0 ? g_page_count - 1 : 0;
    }
}

static void draw_para(HDC hdc, const hp_paragraph *para, int left, int right, int *y) {
    size_t ri;
    int content_w = right - left;
    int x = left;
    int line_top = *y;
    int line_h = 14;
    int first_on_line = 1;
    hp_align align;
    SIZE total;

    if (!para) {
        return;
    }
    if (para_is_blank(para)) {
        *y += (para->run_count && para->runs[0].font_size_hwp) ? (int)(para->runs[0].font_size_hwp / 100) + 4 : 10;
        return;
    }
    align = para->align;
    if (align == HP_ALIGN_JUSTIFY && para->run_count > 0) {
        uint32_t sz = para->runs[0].font_size_hwp;
        size_t chars = 0, r;
        for (r = 0; r < para->run_count; r++) {
            if (para->runs[r].text) {
                chars += strlen(para->runs[r].text);
            }
            if (para->runs[r].font_size_hwp > sz) {
                sz = para->runs[r].font_size_hwp;
            }
        }
        if (sz >= 2000 && chars > 0 && chars < 40) {
            align = HP_ALIGN_CENTER;
        }
    }

    if (align == HP_ALIGN_CENTER || align == HP_ALIGN_RIGHT) {
        total.cx = 0;
        for (ri = 0; ri < para->run_count; ri++) {
            wchar_t *w;
            HFONT font, old;
            SIZE sz;
            if (!para->runs[ri].text || !para->runs[ri].text[0]) {
                continue;
            }
            w = utf8_to_wide(para->runs[ri].text);
            if (!w) {
                continue;
            }
            font = make_font(hdc, &para->runs[ri]);
            old = (HFONT)SelectObject(hdc, font);
            GetTextExtentPoint32W(hdc, w, (int)wcslen(w), &sz);
            total.cx += sz.cx;
            SelectObject(hdc, old);
            DeleteObject(font);
            free(w);
        }
        if (total.cx > 0 && total.cx <= content_w) {
            x = (align == HP_ALIGN_CENTER) ? left + (content_w - total.cx) / 2 : right - total.cx;
            for (ri = 0; ri < para->run_count; ri++) {
                wchar_t *w;
                HFONT font, old;
                SIZE sz;
                COLORREF col;
                if (!para->runs[ri].text || !para->runs[ri].text[0]) {
                    continue;
                }
                w = utf8_to_wide(para->runs[ri].text);
                if (!w) {
                    continue;
                }
                font = make_font(hdc, &para->runs[ri]);
                old = (HFONT)SelectObject(hdc, font);
                col = RGB((para->runs[ri].color_rgb >> 16) & 255, (para->runs[ri].color_rgb >> 8) & 255,
                          para->runs[ri].color_rgb & 255);
                SetTextColor(hdc, col);
                SetBkMode(hdc, TRANSPARENT);
                GetTextExtentPoint32W(hdc, w, (int)wcslen(w), &sz);
                TextOutW(hdc, x, line_top, w, (int)wcslen(w));
                x += sz.cx;
                if (sz.cy > line_h) {
                    line_h = sz.cy;
                }
                SelectObject(hdc, old);
                DeleteObject(font);
                free(w);
            }
            *y = line_top + line_h + 4;
            return;
        }
        x = left;
    }

    for (ri = 0; ri < para->run_count; ri++) {
        const hp_run *run = &para->runs[ri];
        wchar_t *w;
        HFONT font, old;
        TEXTMETRICW tm;
        COLORREF col;
        int i, len;
        if (!run->text || !run->text[0]) {
            continue;
        }
        w = utf8_to_wide(run->text);
        if (!w) {
            continue;
        }
        font = make_font(hdc, run);
        old = (HFONT)SelectObject(hdc, font);
        GetTextMetricsW(hdc, &tm);
        if (tm.tmHeight + tm.tmExternalLeading > line_h) {
            line_h = tm.tmHeight + tm.tmExternalLeading;
        }
        col = RGB((run->color_rgb >> 16) & 255, (run->color_rgb >> 8) & 255, run->color_rgb & 255);
        SetTextColor(hdc, col);
        SetBkMode(hdc, TRANSPARENT);
        len = (int)wcslen(w);
        for (i = 0; i < len; i++) {
            SIZE sz;
            GetTextExtentPoint32W(hdc, &w[i], 1, &sz);
            if (!first_on_line && x + sz.cx > right) {
                line_top += line_h;
                x = left;
                line_h = tm.tmHeight + tm.tmExternalLeading;
                first_on_line = 1;
            }
            TextOutW(hdc, x, line_top, &w[i], 1);
            x += sz.cx;
            first_on_line = 0;
        }
        SelectObject(hdc, old);
        DeleteObject(font);
        free(w);
    }
    *y = line_top + line_h + 4;
}

static void draw_image(HDC hdc, const hp_image *img, int left, int right, int *y) {
    int iw = 0, ih = 0;
    int dw, dh;
    int content_w = right - left;
    HBITMAP hbmp;
    HDC mem;
    HGDIOBJ old;
    if (!img || !img->data) {
        *y += 8;
        return;
    }
    hbmp = decode_image_mem(img->data, img->data_len, &iw, &ih);
    if (!hbmp) {
        *y += 8;
        return;
    }
    dw = (int)hp_hwp_to_px(img->width_hwp);
    dh = (int)hp_hwp_to_px(img->height_hwp);
    if (dw <= 0) {
        dw = iw;
    }
    if (dh <= 0) {
        dh = ih;
    }
    if (dw > content_w && dw > 0) {
        dh = dh * content_w / dw;
        dw = content_w;
    }
    mem = CreateCompatibleDC(hdc);
    old = SelectObject(mem, hbmp);
    SetStretchBltMode(hdc, HALFTONE);
    StretchBlt(hdc, left + (content_w - dw) / 2, *y, dw, dh, mem, 0, 0, iw, ih, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
    DeleteObject(hbmp);
    *y += dh + 8;
}

static void draw_table(HDC hdc, const hp_table *t, int left, int right, int *y) {
    int content_w = right - left;
    int cols = t->col_count > 0 ? t->col_count : 1;
    int rows = t->row_count > 0 ? t->row_count : 1;
    int *col_w;
    int *row_h;
    int r, c;
    size_t i;
    int table_w = t->width_hwp ? (int)hp_hwp_to_px(t->width_hwp) : content_w;
    int x0;

    if (!t) {
        return;
    }
    if (table_w > content_w) {
        table_w = content_w;
    }
    x0 = left + (content_w - table_w) / 2;
    col_w = (int *)calloc((size_t)cols, sizeof(int));
    row_h = (int *)calloc((size_t)rows, sizeof(int));
    if (!col_w || !row_h) {
        free(col_w);
        free(row_h);
        return;
    }
    /* column widths from cells */
    for (i = 0; i < t->cell_count; i++) {
        const hp_table_cell *cell = &t->cells[i];
        int cw;
        if (cell->col < 0 || cell->col >= cols) {
            continue;
        }
        cw = cell->width_hwp ? (int)hp_hwp_to_px(cell->width_hwp) : 0;
        if (cw > col_w[cell->col]) {
            col_w[cell->col] = cw;
        }
    }
    {
        int sum = 0, empty = 0;
        for (c = 0; c < cols; c++) {
            sum += col_w[c];
            if (col_w[c] == 0) {
                empty++;
            }
        }
        if (sum < table_w) {
            int rem = table_w - sum;
            int each = empty ? rem / empty : rem / cols;
            for (c = 0; c < cols; c++) {
                if (col_w[c] == 0) {
                    col_w[c] = each > 20 ? each : 20;
                }
            }
        } else if (sum > table_w && sum > 0) {
            for (c = 0; c < cols; c++) {
                col_w[c] = col_w[c] * table_w / sum;
            }
        }
    }
    for (i = 0; i < t->cell_count; i++) {
        const hp_table_cell *cell = &t->cells[i];
        int cw, ch = cell->height_hwp ? (int)hp_hwp_to_px(cell->height_hwp) : 0;
        size_t p;
        if (cell->row < 0 || cell->row >= rows || cell->col < 0 || cell->col >= cols) {
            continue;
        }
        cw = col_w[cell->col];
        if (cell->image) {
            int ih = measure_image_height(cell->image, cw > 8 ? cw - 8 : cw);
            if (ih > ch) {
                ch = ih;
            }
        }
        for (p = 0; p < cell->paragraph_count; p++) {
            int th = measure_para_height(hdc, &cell->paragraphs[p], cw > 8 ? cw - 8 : cw);
            ch += th;
        }
        if (ch < 20) {
            ch = 20;
        }
        if (ch > row_h[cell->row]) {
            row_h[cell->row] = ch;
        }
    }
    for (r = 0; r < rows; r++) {
        if (row_h[r] < 20) {
            row_h[r] = 20;
        }
    }

    {
        int yy = *y;
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(60, 60, 60));
        HGDIOBJ oldpen = SelectObject(hdc, pen);
        for (i = 0; i < t->cell_count; i++) {
            const hp_table_cell *cell = &t->cells[i];
            int cx, cy, cw, ch;
            int rr, cc;
            size_t p;
            if (cell->row < 0 || cell->row >= rows || cell->col < 0 || cell->col >= cols) {
                continue;
            }
            cx = x0;
            for (cc = 0; cc < cell->col; cc++) {
                cx += col_w[cc];
            }
            cy = yy;
            for (rr = 0; rr < cell->row; rr++) {
                cy += row_h[rr];
            }
            cw = 0;
            for (cc = 0; cc < cell->colspan && cell->col + cc < cols; cc++) {
                cw += col_w[cell->col + cc];
            }
            ch = 0;
            for (rr = 0; rr < cell->rowspan && cell->row + rr < rows; rr++) {
                ch += row_h[cell->row + rr];
            }
            if (cell->has_fill) {
                HBRUSH br = CreateSolidBrush(RGB((cell->fill_rgb >> 16) & 255, (cell->fill_rgb >> 8) & 255,
                                                 cell->fill_rgb & 255));
                RECT rc = {cx, cy, cx + cw, cy + ch};
                FillRect(hdc, &rc, br);
                DeleteObject(br);
            }
            Rectangle(hdc, cx, cy, cx + cw, cy + ch);
            {
                int ty = cy + 3;
                if (cell->image) {
                    draw_image(hdc, cell->image, cx + 4, cx + cw - 4, &ty);
                }
                for (p = 0; p < cell->paragraph_count; p++) {
                    draw_para(hdc, &cell->paragraphs[p], cx + 4, cx + cw - 4, &ty);
                }
            }
        }
        SelectObject(hdc, oldpen);
        DeleteObject(pen);
        for (r = 0; r < rows; r++) {
            yy += row_h[r];
        }
        *y = yy + 8;
    }
    free(col_w);
    free(row_h);
}

static void draw_block(HDC hdc, const hp_block *b, int left, int right, int *y) {
    if (!b) {
        return;
    }
    if (b->kind == HP_BLOCK_PARAGRAPH) {
        draw_para(hdc, &b->paragraph, left, right, y);
    } else if (b->kind == HP_BLOCK_TABLE && b->table) {
        draw_table(hdc, b->table, left, right, y);
    } else if (b->kind == HP_BLOCK_IMAGE && b->image) {
        draw_image(hdc, b->image, left, right, y);
    }
}

static void paint_document(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    HBRUSH bg;
    int avail_w, avail_h, dw, dh, dx, dy;
    float scale;
    HDC mem;
    HBITMAP bmp, oldbmp;

    GetClientRect(hwnd, &rc);
    rc.top = TOOLBAR_H;
    bg = CreateSolidBrush(RGB(82, 82, 82));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    if (!g_doc || g_page_count == 0) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(240, 240, 240));
        TextOutW(hdc, 24, TOOLBAR_H + 24, L"HWPX 문서를 열어 주세요.", 20);
        EndPaint(hwnd, &ps);
        return;
    }

    avail_w = rc.right - rc.left - 24;
    avail_h = rc.bottom - rc.top - 24;
    scale = (float)avail_w / (float)PAGE_W_PX;
    if ((float)PAGE_H_PX * scale > (float)avail_h) {
        scale = (float)avail_h / (float)PAGE_H_PX;
    }
    if (scale < 0.2f) {
        scale = 0.2f;
    }
    dw = (int)(PAGE_W_PX * scale);
    dh = (int)(PAGE_H_PX * scale);
    dx = rc.left + (avail_w - dw) / 2 + 12;
    dy = rc.top + (avail_h - dh) / 2 + 12;

    mem = CreateCompatibleDC(hdc);
    bmp = CreateCompatibleBitmap(hdc, PAGE_W_PX, PAGE_H_PX);
    oldbmp = (HBITMAP)SelectObject(mem, bmp);
    {
        RECT pr = {0, 0, PAGE_W_PX, PAGE_H_PX};
        FillRect(mem, &pr, (HBRUSH)GetStockObject(WHITE_BRUSH));
    }
    {
        layout_page *pg = &g_pages[g_page_index];
        int y = MARGIN_Y;
        int i;
        int cols = 1;
        int gap = 16;
        int col_w = PAGE_W_PX - 2 * MARGIN_X;
        if (g_doc->section_count > 0 && g_doc->sections[0].column_count > 1) {
            cols = g_doc->sections[0].column_count;
            col_w = (PAGE_W_PX - 2 * MARGIN_X - (cols - 1) * gap) / cols;
        }
        if (cols <= 1) {
            for (i = 0; i < pg->count; i++) {
                draw_block(mem, flat_block(pg->first + i), MARGIN_X, PAGE_W_PX - MARGIN_X, &y);
                if (y > PAGE_H_PX - MARGIN_Y / 2) {
                    break;
                }
            }
        } else {
            /* Simple multi-column: fill column then next */
            int col = 0;
            int col_top = MARGIN_Y;
            y = col_top;
            for (i = 0; i < pg->count; i++) {
                int left = MARGIN_X + col * (col_w + gap);
                int right = left + col_w;
                int h = measure_block_height(mem, flat_block(pg->first + i), col_w);
                if (y > col_top && y + h > PAGE_H_PX - MARGIN_Y) {
                    col++;
                    if (col >= cols) {
                        break;
                    }
                    y = col_top;
                    left = MARGIN_X + col * (col_w + gap);
                    right = left + col_w;
                }
                draw_block(mem, flat_block(pg->first + i), left, right, &y);
            }
        }
        {
            wchar_t pn[32];
            HFONT f, old;
            SIZE sz;
            swprintf(pn, 32, L"- %d -", g_page_index + 1);
            f = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                            L"Malgun Gothic");
            old = (HFONT)SelectObject(mem, f);
            SetTextColor(mem, RGB(90, 90, 90));
            SetBkMode(mem, TRANSPARENT);
            GetTextExtentPoint32W(mem, pn, (int)wcslen(pn), &sz);
            TextOutW(mem, (PAGE_W_PX - sz.cx) / 2, PAGE_H_PX - 36, pn, (int)wcslen(pn));
            SelectObject(mem, old);
            DeleteObject(f);
        }
    }
    {
        RECT sh = {dx + 4, dy + 4, dx + dw + 4, dy + dh + 4};
        FillRect(hdc, &sh, (HBRUSH)GetStockObject(BLACK_BRUSH));
    }
    SetStretchBltMode(hdc, HALFTONE);
    StretchBlt(hdc, dx, dy, dw, dh, mem, 0, 0, PAGE_W_PX, PAGE_H_PX, SRCCOPY);
    SelectObject(mem, oldbmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

static void free_doc(void) {
    if (g_doc) {
        hp_document_free(g_doc);
        g_doc = NULL;
    }
    g_flat_count = 0;
    g_page_count = 0;
    g_page_index = 0;
}

static void open_document(HWND hwnd, const wchar_t *path) {
    char *utf8 = wide_to_utf8(path);
    hp_document *doc = NULL;
    wchar_t title[MAX_PATH + 32];
    const wchar_t *name;
    if (!utf8) {
        return;
    }
    if (hp_hwpx_read(utf8, &doc) != HP_OK || !doc) {
        free(utf8);
        MessageBoxW(hwnd, L"HWPX 문서를 읽지 못했습니다.", L"한글패드", MB_ICONERROR);
        return;
    }
    free(utf8);
    free_doc();
    g_doc = doc;
    rebuild_flat();
    rebuild_pages(hwnd);
    g_page_index = 0;
    update_status();
    name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    swprintf(title, MAX_PATH + 32, L"한글패드 — %ls", name);
    SetWindowTextW(hwnd, title);
    InvalidateRect(hwnd, NULL, TRUE);
}

static void file_open(HWND hwnd) {
    OPENFILENAMEW ofn;
    wchar_t file[MAX_PATH];
    ZeroMemory(&ofn, sizeof(ofn));
    file[0] = 0;
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"HWPX 문서 (*.hwpx)\0*.hwpx\0모든 파일 (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_EXPLORER;
    ofn.lpstrDefExt = L"hwpx";
    if (GetOpenFileNameW(&ofn)) {
        open_document(hwnd, file);
    }
}

static void open_sample(HWND hwnd) {
    wchar_t exe[MAX_PATH];
    wchar_t sample[MAX_PATH];
    wchar_t *slash;
    const wchar_t *cands[] = {L"documents\\공공부문-인공지능-윤리기준.hwpx", L"documents\\sample.hwpx",
                              L"샘플.hwpx", NULL};
    int i;
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    slash = wcsrchr(exe, L'\\');
    if (slash) {
        *slash = 0;
    }
    for (i = 0; cands[i]; i++) {
        swprintf(sample, MAX_PATH, L"%ls\\%ls", exe, cands[i]);
        if (GetFileAttributesW(sample) != INVALID_FILE_ATTRIBUTES) {
            open_document(hwnd, sample);
            return;
        }
    }
    MessageBoxW(hwnd, L"샘플 문서를 찾을 수 없습니다.", L"한글패드", MB_ICONERROR);
}

static void about(HWND hwnd) {
    MessageBoxW(hwnd,
                L"한글패드 — HWPX 엔진 레이아웃\n\n"
                L"글꼴·크기·정렬·표·이미지를 문서에서 읽어\n"
                L"PDF처럼 페이지로 표시합니다.\n\n"
                L"본 제품은 한글과컴퓨터의 HWP 문서 파일(.hwp) 공개 문서를 "
                L"참고하여 개발하였습니다.",
                L"한글패드 정보", MB_OK | MB_ICONINFORMATION);
}

static void layout_children(int cx, int cy) {
    (void)cy;
    MoveWindow(g_btn_prev, 8, 6, 100, 28, TRUE);
    MoveWindow(g_btn_next, 116, 6, 100, 28, TRUE);
    MoveWindow(g_status, 230, 6, cx - 240, 28, TRUE);
}

static void goto_page(HWND hwnd, int idx) {
    if (idx < 0 || idx >= g_page_count) {
        return;
    }
    g_page_index = idx;
    update_status();
    InvalidateRect(hwnd, NULL, TRUE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HMENU menubar = CreateMenu();
        HMENU file = CreatePopupMenu();
        HMENU help = CreatePopupMenu();
        AppendMenuW(file, MF_STRING, IDM_FILE_OPEN_SAMPLE, L"샘플 문서 열기\tCtrl+1");
        AppendMenuW(file, MF_STRING, IDM_FILE_OPEN, L"HWPX 열기...\tCtrl+O");
        AppendMenuW(file, MF_SEPARATOR, 0, NULL);
        AppendMenuW(file, MF_STRING, IDM_FILE_EXIT, L"종료");
        AppendMenuW(help, MF_STRING, IDM_HELP_ABOUT, L"한글패드 정보");
        AppendMenuW(menubar, MF_POPUP, (UINT_PTR)file, L"파일");
        AppendMenuW(menubar, MF_POPUP, (UINT_PTR)help, L"도움말");
        SetMenu(hwnd, menubar);
        g_btn_prev = CreateWindowW(L"BUTTON", L"◀ 이전", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0,
                                   hwnd, (HMENU)(INT_PTR)ID_BTN_PREV, GetModuleHandleW(NULL), NULL);
        g_btn_next = CreateWindowW(L"BUTTON", L"다음 ▶", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                                   (HMENU)(INT_PTR)ID_BTN_NEXT, GetModuleHandleW(NULL), NULL);
        g_status = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                                 (HMENU)(INT_PTR)ID_STATUS, GetModuleHandleW(NULL), NULL);
        update_status();
        PostMessageW(hwnd, WM_COMMAND, IDM_FILE_OPEN_SAMPLE, 0);
        return 0;
    }
    case WM_SIZE:
        layout_children(LOWORD(lParam), HIWORD(lParam));
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint_document(hwnd);
        return 0;
    case WM_MOUSEWHEEL:
        goto_page(hwnd, g_page_index + (((short)HIWORD(wParam) < 0) ? 1 : -1));
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_FILE_OPEN:
            file_open(hwnd);
            break;
        case IDM_FILE_OPEN_SAMPLE:
            open_sample(hwnd);
            break;
        case IDM_FILE_EXIT:
            DestroyWindow(hwnd);
            break;
        case ID_BTN_PREV:
            goto_page(hwnd, g_page_index - 1);
            break;
        case ID_BTN_NEXT:
            goto_page(hwnd, g_page_index + 1);
            break;
        case IDM_HELP_ABOUT:
            about(hwnd);
            break;
        }
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_PRIOR || wParam == VK_LEFT || wParam == VK_UP) {
            goto_page(hwnd, g_page_index - 1);
        } else if (wParam == VK_NEXT || wParam == VK_RIGHT || wParam == VK_DOWN || wParam == VK_SPACE) {
            goto_page(hwnd, g_page_index + 1);
        }
        return 0;
    case WM_DESTROY:
        free_doc();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmd, int show) {
    WNDCLASSW wc;
    MSG msg;
    HRESULT hr;
    (void)hPrev;
    (void)cmd;

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        return 1;
    }
    CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                     (void **)&g_wic);

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"HangulPadMain";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    if (!RegisterClassW(&wc)) {
        if (g_wic) {
            g_wic->lpVtbl->Release(g_wic);
        }
        CoUninitialize();
        return 1;
    }
    g_main = CreateWindowExW(0, L"HangulPadMain", L"한글패드",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                             920, 1100, NULL, NULL, hInst, NULL);
    if (!g_main) {
        if (g_wic) {
            g_wic->lpVtbl->Release(g_wic);
        }
        CoUninitialize();
        return 1;
    }
    ShowWindow(g_main, show);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000)) {
            if (msg.wParam == 'O') {
                file_open(g_main);
                continue;
            }
            if (msg.wParam == '1') {
                open_sample(g_main);
                continue;
            }
        }
        if (msg.message == WM_KEYDOWN) {
            SendMessageW(g_main, WM_KEYDOWN, msg.wParam, msg.lParam);
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_wic) {
        g_wic->lpVtbl->Release(g_wic);
    }
    CoUninitialize();
    return (int)msg.wParam;
}
