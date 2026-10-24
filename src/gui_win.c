/*
 * HangulPad Windows GUI — HWPX layout viewer.
 * Reads HWPX via the engine and paints styled text (font/size/align/color)
 * onto A4 pages so the document looks like the PDF reference — not a JPEG
 * slideshow and not a raw text dump.
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
#define MAX_LAYOUT_PAGES 256
#define MAX_FLAT_PARAS 4096

/* A4 at 96 DPI */
#define PAGE_W_PX 794
#define PAGE_H_PX 1123
#define MARGIN_X 64
#define MARGIN_Y 72

typedef struct {
    size_t sec;
    size_t para;
} flat_para_ref;

typedef struct {
    int first; /* index into g_flat */
    int count;
} layout_page;

static HWND g_main;
static HWND g_btn_prev;
static HWND g_btn_next;
static HWND g_status;

static hp_document *g_doc;
static flat_para_ref g_flat[MAX_FLAT_PARAS];
static int g_flat_count;
static layout_page g_pages[MAX_LAYOUT_PAGES];
static int g_page_count;
static int g_page_index;
static int g_scroll_y;

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

/* Map HWP face names to fonts typically available on Windows. */
static void map_face(const char *hw, wchar_t *out, size_t out_n) {
    if (!hw || !hw[0]) {
        wcsncpy(out, L"Malgun Gothic", out_n - 1);
        out[out_n - 1] = 0;
        return;
    }
    if (strstr(hw, "바탕") || strstr(hw, "명조") || strstr(hw, "Batang") || strstr(hw, "신명조")) {
        wcsncpy(out, L"Batang", out_n - 1);
    } else if (strstr(hw, "헤드라인") || strstr(hw, "Headline")) {
        wcsncpy(out, L"Malgun Gothic", out_n - 1);
    } else {
        /* 맑은 고딕 / 돋움 / 고딕 / etc. */
        wcsncpy(out, L"Malgun Gothic", out_n - 1);
    }
    out[out_n - 1] = 0;
}

static int hwp_to_px(uint32_t hwp_size, HDC hdc) {
    int pt = (int)((hwp_size + 50) / 100); /* round */
    int dpi;
    if (pt < 6) {
        pt = 6;
    }
    if (pt > 72) {
        pt = 72;
    }
    dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    if (dpi <= 0) {
        dpi = 96;
    }
    return -MulDiv(pt, dpi, 72);
}

static HFONT make_font(HDC hdc, const hp_run *run) {
    wchar_t face[64];
    LOGFONTW lf;
    map_face(run->font_face, face, 64);
    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight = hwp_to_px(run->font_size_hwp ? run->font_size_hwp : 1000, hdc);
    lf.lfWeight = run->bold ? FW_BOLD : FW_NORMAL;
    lf.lfCharSet = HANGUL_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    return CreateFontIndirectW(&lf);
}

static void update_status(void) {
    wchar_t buf[192];
    if (g_doc && g_page_count > 0) {
        swprintf(buf, 192, L"페이지 %d / %d  —  HWPX 엔진 렌더 (글꼴·크기 반영)",
                 g_page_index + 1, g_page_count);
    } else if (g_doc) {
        wcscpy(buf, L"문서 로드됨 — 레이아웃 중");
    } else {
        wcscpy(buf, L"파일 → 열기 또는 샘플 문서 열기");
    }
    SetWindowTextW(g_status, buf);
    EnableWindow(g_btn_prev, g_page_index > 0);
    EnableWindow(g_btn_next, g_page_index + 1 < g_page_count);
}

static void rebuild_flat(void) {
    size_t si, pi;
    g_flat_count = 0;
    if (!g_doc) {
        return;
    }
    for (si = 0; si < g_doc->section_count; si++) {
        const hp_section *sec = &g_doc->sections[si];
        for (pi = 0; pi < sec->paragraph_count; pi++) {
            if (g_flat_count >= MAX_FLAT_PARAS) {
                return;
            }
            g_flat[g_flat_count].sec = si;
            g_flat[g_flat_count].para = pi;
            g_flat_count++;
        }
    }
}

static const hp_paragraph *flat_para(int idx) {
    if (idx < 0 || idx >= g_flat_count || !g_doc) {
        return NULL;
    }
    return &g_doc->sections[g_flat[idx].sec].paragraphs[g_flat[idx].para];
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
    int max_line = 16;
    int lines = 1;
    int x = 0;
    int blank_h;

    if (!para) {
        return 8;
    }
    if (para_is_blank(para)) {
        /* empty line spacing from first run size or default */
        blank_h = 14;
        if (para->run_count > 0 && para->runs[0].font_size_hwp) {
            blank_h = (int)(para->runs[0].font_size_hwp / 100) + 4;
        }
        return blank_h;
    }

    for (ri = 0; ri < para->run_count; ri++) {
        const hp_run *run = &para->runs[ri];
        wchar_t *w;
        HFONT font, old;
        SIZE sz;
        int i, len;
        int fh;

        if (!run->text || !run->text[0]) {
            continue;
        }
        w = utf8_to_wide(run->text);
        if (!w) {
            continue;
        }
        font = make_font(hdc, run);
        old = (HFONT)SelectObject(hdc, font);
        {
            TEXTMETRICW tm;
            GetTextMetricsW(hdc, &tm);
            fh = tm.tmHeight + tm.tmExternalLeading;
            if (fh > max_line) {
                max_line = fh;
            }
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
    return lines * max_line + 6;
}

static void rebuild_pages(HWND hwnd) {
    HDC hdc;
    int content_w = PAGE_W_PX - 2 * MARGIN_X;
    int content_h = PAGE_H_PX - 2 * MARGIN_Y;
    int y = 0;
    int i;
    int page_first = 0;

    g_page_count = 0;
    if (g_flat_count == 0) {
        return;
    }
    hdc = GetDC(hwnd ? hwnd : NULL);
    if (!hdc) {
        /* fallback: fixed ~40 paras per page */
        int per = 35;
        for (i = 0; i < g_flat_count; i += per) {
            if (g_page_count >= MAX_LAYOUT_PAGES) {
                break;
            }
            g_pages[g_page_count].first = i;
            g_pages[g_page_count].count =
                (i + per <= g_flat_count) ? per : (g_flat_count - i);
            g_page_count++;
        }
        return;
    }

    for (i = 0; i < g_flat_count; i++) {
        const hp_paragraph *para = flat_para(i);
        int h = measure_para_height(hdc, para, content_w);
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
    int line_h = 16;
    int first_on_line = 1;
    SIZE total;
    hp_align align;

    if (!para) {
        return;
    }
    if (para_is_blank(para)) {
        int blank_h = 14;
        if (para->run_count > 0 && para->runs[0].font_size_hwp) {
            blank_h = (int)(para->runs[0].font_size_hwp / 100) + 4;
        }
        *y += blank_h;
        return;
    }

    align = para->align;
    /* Large short JUSTIFY lines on covers are visually centered in HWP. */
    if (align == HP_ALIGN_JUSTIFY && para->run_count > 0) {
        uint32_t sz = para->runs[0].font_size_hwp;
        size_t chars = 0;
        size_t r;
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

    /* Center: measure full paragraph width if it fits one line */
    if (align == HP_ALIGN_CENTER || align == HP_ALIGN_RIGHT) {
        total.cx = 0;
        total.cy = 0;
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
            if (sz.cy > total.cy) {
                total.cy = sz.cy;
            }
            SelectObject(hdc, old);
            DeleteObject(font);
            free(w);
        }
        if (total.cx > 0 && total.cx <= content_w) {
            if (align == HP_ALIGN_CENTER) {
                x = left + (content_w - total.cx) / 2;
            } else {
                x = right - total.cx;
            }
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
            *y = line_top + line_h + 6;
            return;
        }
        x = left; /* fall through to wrapped left layout */
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
    *y = line_top + line_h + 6;
}

static void paint_document(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    HBRUSH bg, paper, shadow;
    int avail_w, avail_h;
    float scale;
    int dw, dh, dx, dy;
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
        TextOutW(hdc, 24, TOOLBAR_H + 24, L"HWPX 문서를 열어 주세요.  파일 → 샘플 문서 열기", 40);
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

    /* Render page at 1:1 into memory DC, then stretch */
    mem = CreateCompatibleDC(hdc);
    bmp = CreateCompatibleBitmap(hdc, PAGE_W_PX, PAGE_H_PX);
    oldbmp = (HBITMAP)SelectObject(mem, bmp);
    paper = CreateSolidBrush(RGB(255, 255, 255));
    {
        RECT pr = {0, 0, PAGE_W_PX, PAGE_H_PX};
        FillRect(mem, &pr, paper);
    }
    DeleteObject(paper);

    {
        layout_page *pg = &g_pages[g_page_index];
        int y = MARGIN_Y;
        int i;
        for (i = 0; i < pg->count; i++) {
            const hp_paragraph *para = flat_para(pg->first + i);
            draw_para(mem, para, MARGIN_X, PAGE_W_PX - MARGIN_X, &y);
            if (y > PAGE_H_PX - MARGIN_Y / 2) {
                break;
            }
        }
        /* page number */
        {
            wchar_t pn[32];
            HFONT f, old;
            SIZE sz;
            swprintf(pn, 32, L"- %d -", g_page_index + 1);
            f = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, HANGUL_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                            L"Malgun Gothic");
            old = (HFONT)SelectObject(mem, f);
            SetTextColor(mem, RGB(80, 80, 80));
            SetBkMode(mem, TRANSPARENT);
            GetTextExtentPoint32W(mem, pn, (int)wcslen(pn), &sz);
            TextOutW(mem, (PAGE_W_PX - sz.cx) / 2, PAGE_H_PX - 40, pn, (int)wcslen(pn));
            SelectObject(mem, old);
            DeleteObject(f);
        }
    }

    shadow = CreateSolidBrush(RGB(0, 0, 0));
    {
        RECT sh = {dx + 4, dy + 4, dx + dw + 4, dy + dh + 4};
        FillRect(hdc, &sh, shadow);
    }
    DeleteObject(shadow);
    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, NULL);
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
    char *utf8;
    hp_document *doc = NULL;
    hp_status st;
    wchar_t title[MAX_PATH + 32];
    const wchar_t *name;

    utf8 = wide_to_utf8(path);
    if (!utf8) {
        return;
    }
    st = hp_hwpx_read(utf8, &doc);
    free(utf8);
    if (st != HP_OK || !doc) {
        MessageBoxW(hwnd, L"HWPX 문서를 읽지 못했습니다.", L"한글패드", MB_ICONERROR);
        return;
    }
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
    const wchar_t *candidates[] = {
        L"documents\\공공부문-인공지능-윤리기준.hwpx",
        L"documents\\sample.hwpx",
        L"공공부문-인공지능-윤리기준.hwpx",
        L"샘플.hwpx",
        NULL};
    int i;

    GetModuleFileNameW(NULL, exe, MAX_PATH);
    slash = wcsrchr(exe, L'\\');
    if (slash) {
        *slash = 0;
    }
    for (i = 0; candidates[i]; i++) {
        swprintf(sample, MAX_PATH, L"%ls\\%ls", exe, candidates[i]);
        if (GetFileAttributesW(sample) != INVALID_FILE_ATTRIBUTES) {
            open_document(hwnd, sample);
            return;
        }
    }
    MessageBoxW(hwnd, L"샘플 문서를 찾을 수 없습니다.\n(documents 폴더)", L"한글패드", MB_ICONERROR);
}

static void about(HWND hwnd) {
    MessageBoxW(hwnd,
                L"한글패드 — HWPX 엔진 렌더\n\n"
                L"HWPX를 읽어 글꼴·크기·정렬·색을 반영해\n"
                L"PDF처럼 페이지 단위로 표시합니다.\n"
                L"(페이지 이미지가 아닌 엔진 레이아웃)\n\n"
                L"본 제품은 한글과컴퓨터의 HWP 문서 파일(.hwp) 공개 문서를 "
                L"참고하여 개발하였습니다.",
                L"한글패드 정보", MB_OK | MB_ICONINFORMATION);
}

static void layout_children(int cx, int cy) {
    int y = 6;
    (void)cy;
    if (g_btn_prev) {
        MoveWindow(g_btn_prev, 8, y, 100, 28, TRUE);
    }
    if (g_btn_next) {
        MoveWindow(g_btn_next, 116, y, 100, 28, TRUE);
    }
    if (g_status) {
        MoveWindow(g_status, 230, y, cx - 240, 28, TRUE);
    }
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

        g_btn_prev = CreateWindowW(L"BUTTON", L"◀ 이전",
                                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                                   (HMENU)(INT_PTR)ID_BTN_PREV, GetModuleHandleW(NULL), NULL);
        g_btn_next = CreateWindowW(L"BUTTON", L"다음 ▶",
                                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
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
        if ((short)HIWORD(wParam) < 0) {
            goto_page(hwnd, g_page_index + 1);
        } else {
            goto_page(hwnd, g_page_index - 1);
        }
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
    (void)hPrev;
    (void)cmd;

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"HangulPadMain";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    if (!RegisterClassW(&wc)) {
        return 1;
    }

    g_main = CreateWindowExW(0, L"HangulPadMain", L"한글패드",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VISIBLE, CW_USEDEFAULT,
                             CW_USEDEFAULT, 920, 1100, NULL, NULL, hInst, NULL);
    if (!g_main) {
        return 1;
    }
    ShowWindow(g_main, show);
    UpdateWindow(g_main);

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
    return (int)msg.wParam;
}
