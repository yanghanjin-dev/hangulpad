/*
 * HangulPad Windows GUI — PDF-like page viewer (JPG pages) + text fallback.
 * Uses WIC (windowscodecs) so MinGW C builds without GDI+.
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

#define ID_EDIT 1001
#define ID_BTN_PREV 1002
#define ID_BTN_NEXT 1003
#define ID_STATUS 1004
#define IDM_FILE_OPEN 40002
#define IDM_FILE_OPEN_SAMPLE 40003
#define IDM_FILE_EXIT 40005
#define IDM_VIEW_PAGE 40020
#define IDM_VIEW_TEXT 40021
#define IDM_HELP_ABOUT 40010

#define MAX_PAGES 64
#define TOOLBAR_H 40

static HWND g_main;
static HWND g_edit;
static HWND g_btn_prev;
static HWND g_btn_next;
static HWND g_status;
static int g_mode_page = 1; /* 1=page images, 0=text */

static wchar_t g_page_paths[MAX_PAGES][MAX_PATH];
static int g_page_count;
static int g_page_index;
static HBITMAP g_hbmp;
static int g_img_w;
static int g_img_h;
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

static void free_bitmap(void) {
    if (g_hbmp) {
        DeleteObject(g_hbmp);
        g_hbmp = NULL;
    }
    g_img_w = g_img_h = 0;
}

static void update_status(void) {
    wchar_t buf[160];
    if (g_mode_page && g_page_count > 0) {
        swprintf(buf, 160, L"페이지 %d / %d  —  PDF처럼 페이지 보기", g_page_index + 1,
                 g_page_count);
    } else if (!g_mode_page) {
        wcscpy(buf, L"텍스트 보기 (서식 없는 추출)");
    } else {
        wcscpy(buf, L"페이지 이미지 없음 — 파일→샘플 문서 열기");
    }
    SetWindowTextW(g_status, buf);
    EnableWindow(g_btn_prev, g_mode_page && g_page_index > 0);
    EnableWindow(g_btn_next, g_mode_page && g_page_index + 1 < g_page_count);
}

/* Decode JPG/PNG via WIC into a 32bpp HBITMAP. */
static int load_page_bitmap(int index) {
    HRESULT hr;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    UINT w = 0, h = 0;
    UINT stride, size;
    BYTE *bits = NULL;
    BITMAPINFO bmi;
    HDC hdc;
    void *dib_bits = NULL;
    HBITMAP hbmp = NULL;

    free_bitmap();
    if (!g_wic || index < 0 || index >= g_page_count) {
        return 0;
    }

    hr = g_wic->lpVtbl->CreateDecoderFromFilename(
        g_wic, g_page_paths[index], NULL, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr) || !decoder) {
        return 0;
    }
    hr = decoder->lpVtbl->GetFrame(decoder, 0, &frame);
    if (FAILED(hr) || !frame) {
        goto done;
    }
    hr = g_wic->lpVtbl->CreateFormatConverter(g_wic, &conv);
    if (FAILED(hr) || !conv) {
        goto done;
    }
    hr = conv->lpVtbl->Initialize(conv, (IWICBitmapSource *)frame,
                                  &GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, NULL, 0.0,
                                  WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        goto done;
    }
    hr = conv->lpVtbl->GetSize(conv, &w, &h);
    if (FAILED(hr) || w == 0 || h == 0) {
        goto done;
    }
    stride = w * 4;
    size = stride * h;
    bits = (BYTE *)malloc(size);
    if (!bits) {
        goto done;
    }
    hr = conv->lpVtbl->CopyPixels(conv, NULL, stride, size, bits);
    if (FAILED(hr)) {
        goto done;
    }

    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = (LONG)w;
    bmi.bmiHeader.biHeight = -(LONG)h; /* top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    hdc = GetDC(NULL);
    hbmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &dib_bits, NULL, 0);
    ReleaseDC(NULL, hdc);
    if (!hbmp || !dib_bits) {
        if (hbmp) {
            DeleteObject(hbmp);
        }
        hbmp = NULL;
        goto done;
    }
    memcpy(dib_bits, bits, size);
    g_hbmp = hbmp;
    g_img_w = (int)w;
    g_img_h = (int)h;
    g_page_index = index;
    update_status();
    InvalidateRect(g_main, NULL, TRUE);
    hbmp = NULL; /* ownership transferred to g_hbmp */

done:
    if (bits) {
        free(bits);
    }
    if (hbmp) {
        DeleteObject(hbmp);
    }
    if (conv) {
        conv->lpVtbl->Release(conv);
    }
    if (frame) {
        frame->lpVtbl->Release(frame);
    }
    if (decoder) {
        decoder->lpVtbl->Release(decoder);
    }
    return g_hbmp != NULL;
}

static int cmp_path(const void *a, const void *b) {
    return _wcsicmp(*(const wchar_t *const *)a, *(const wchar_t *const *)b);
}

static int load_pages_from_dir(const wchar_t *dir) {
    wchar_t pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    wchar_t *list[MAX_PAGES];
    int n = 0;
    int i;

    g_page_count = 0;
    free_bitmap();

    swprintf(pattern, MAX_PATH, L"%ls\\*", dir);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        const wchar_t *ext;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        ext = wcsrchr(fd.cFileName, L'.');
        if (!ext) {
            continue;
        }
        if (_wcsicmp(ext, L".jpg") && _wcsicmp(ext, L".jpeg") && _wcsicmp(ext, L".png")) {
            continue;
        }
        if (n >= MAX_PAGES) {
            break;
        }
        list[n] = (wchar_t *)malloc(MAX_PATH * sizeof(wchar_t));
        if (!list[n]) {
            break;
        }
        swprintf(list[n], MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    qsort(list, (size_t)n, sizeof(list[0]), cmp_path);
    for (i = 0; i < n; i++) {
        wcsncpy(g_page_paths[i], list[i], MAX_PATH - 1);
        g_page_paths[i][MAX_PATH - 1] = 0;
        free(list[i]);
    }
    g_page_count = n;
    if (g_page_count > 0) {
        g_mode_page = 1;
        ShowWindow(g_edit, SW_HIDE);
        load_page_bitmap(0);
    }
    return g_page_count;
}

static void set_edit_text_utf8(const char *utf8) {
    wchar_t *w = utf8_to_wide(utf8 ? utf8 : "");
    if (!w) {
        SetWindowTextW(g_edit, L"");
        return;
    }
    SetWindowTextW(g_edit, w);
    free(w);
}

static void clean_extracted_text(char *s) {
    char *src = s;
    char *dst = s;
    while (*src) {
        if (src[0] == '<' && (strncmp(src, "<hp:", 4) == 0 || strncmp(src, "</hp:", 5) == 0)) {
            char *gt = strchr(src, '>');
            if (gt) {
                src = gt + 1;
                continue;
            }
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

static int load_hwpx_text(const wchar_t *wpath) {
    char *path = wide_to_utf8(wpath);
    hp_document *doc = NULL;
    hp_status st;
    char *text;
    if (!path) {
        return 0;
    }
    st = hp_hwpx_read(path, &doc);
    free(path);
    if (st != HP_OK || !doc) {
        return 0;
    }
    text = hp_document_plain_text(doc);
    if (text) {
        clean_extracted_text(text);
        set_edit_text_utf8(text);
        free(text);
    }
    hp_document_free(doc);
    return 1;
}

static int try_load_pages_near(const wchar_t *base_dir) {
    wchar_t pages[MAX_PATH];
    swprintf(pages, MAX_PATH, L"%ls\\pages", base_dir);
    return load_pages_from_dir(pages);
}

static void try_sidecar_pages(const wchar_t *hwpx_path) {
    wchar_t dir[MAX_PATH];
    wchar_t pages[MAX_PATH];
    wchar_t exe_dir[MAX_PATH];
    wchar_t *slash;
    wchar_t *exe_slash;

    wcsncpy(dir, hwpx_path, MAX_PATH - 1);
    dir[MAX_PATH - 1] = 0;
    slash = wcsrchr(dir, L'\\');
    if (slash) {
        *slash = 0;
    }

    /* 1) <doc_dir>\pages  2) <doc_dir>\<stem>.pages  3) <exe>\pages */
    if (try_load_pages_near(dir) > 0) {
        return;
    }
    {
        wchar_t stem[MAX_PATH];
        const wchar_t *name = slash ? slash + 1 : hwpx_path;
        wcsncpy(stem, name, MAX_PATH - 1);
        stem[MAX_PATH - 1] = 0;
        {
            wchar_t *dot = wcsrchr(stem, L'.');
            if (dot) {
                *dot = 0;
            }
        }
        swprintf(pages, MAX_PATH, L"%ls\\%ls.pages", dir, stem);
        if (load_pages_from_dir(pages) > 0) {
            return;
        }
    }
    GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
    exe_slash = wcsrchr(exe_dir, L'\\');
    if (exe_slash) {
        *exe_slash = 0;
    }
    try_load_pages_near(exe_dir);
}

static void show_page_mode(void) {
    g_mode_page = 1;
    ShowWindow(g_edit, SW_HIDE);
    update_status();
    InvalidateRect(g_main, NULL, TRUE);
}

static void show_text_mode(void) {
    g_mode_page = 0;
    ShowWindow(g_edit, SW_SHOW);
    update_status();
    InvalidateRect(g_main, NULL, TRUE);
}

static void open_document(const wchar_t *path) {
    wchar_t title[MAX_PATH + 32];
    const wchar_t *name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    swprintf(title, MAX_PATH + 32, L"한글패드 — %ls", name);
    SetWindowTextW(g_main, title);

    try_sidecar_pages(path);
    load_hwpx_text(path);
    if (g_page_count > 0) {
        show_page_mode();
    } else {
        show_text_mode();
        MessageBoxW(g_main,
                    L"페이지 이미지가 없어 텍스트로 표시합니다.\n"
                    L"샘플: 파일 → 샘플 문서 열기",
                    L"한글패드", MB_OK | MB_ICONINFORMATION);
    }
}

static void file_open(void) {
    OPENFILENAMEW ofn;
    wchar_t file[MAX_PATH];
    ZeroMemory(&ofn, sizeof(ofn));
    file[0] = 0;
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"HWPX 문서 (*.hwpx)\0*.hwpx\0모든 파일 (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_EXPLORER;
    ofn.lpstrDefExt = L"hwpx";
    if (GetOpenFileNameW(&ofn)) {
        open_document(file);
    }
}

static void open_sample(void) {
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
            open_document(sample);
            return;
        }
    }
    MessageBoxW(g_main, L"샘플 문서를 찾을 수 없습니다.\n(documents 폴더)", L"한글패드",
                MB_ICONERROR);
}

static void about(void) {
    MessageBoxW(g_main,
                L"한글패드 — PDF처럼 페이지 보기\n\n"
                L"첨부 PDF와 동일한 페이지 이미지를 한 장씩 표시합니다.\n"
                L"이전/다음, 방향키, PageUp/PageDown, 스페이스로 넘깁니다.\n\n"
                L"본 제품은 한글과컴퓨터의 HWP 문서 파일(.hwp) 공개 문서를 "
                L"참고하여 개발하였습니다.",
                L"한글패드 정보", MB_OK | MB_ICONINFORMATION);
}

static void layout_children(int cx, int cy) {
    int y = 6;
    if (g_btn_prev) {
        MoveWindow(g_btn_prev, 8, y, 100, 28, TRUE);
    }
    if (g_btn_next) {
        MoveWindow(g_btn_next, 116, y, 100, 28, TRUE);
    }
    if (g_status) {
        MoveWindow(g_status, 230, y, cx - 240, 28, TRUE);
    }
    if (g_edit) {
        MoveWindow(g_edit, 0, TOOLBAR_H, cx, cy - TOOLBAR_H, TRUE);
    }
}

static void paint_page(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    HBRUSH bg;
    GetClientRect(hwnd, &rc);
    rc.top = TOOLBAR_H;

    /* PDF-reader style gray canvas behind the page */
    bg = CreateSolidBrush(RGB(82, 82, 82));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    if (g_mode_page && g_hbmp && g_img_w > 0 && g_img_h > 0) {
        HDC mem = CreateCompatibleDC(hdc);
        HGDIOBJ old;
        int avail_w = rc.right - rc.left - 24;
        int avail_h = rc.bottom - rc.top - 24;
        float scale;
        int dw, dh, dx, dy;
        RECT shadow;

        old = SelectObject(mem, g_hbmp);
        scale = (float)avail_w / (float)g_img_w;
        if ((float)g_img_h * scale > (float)avail_h) {
            scale = (float)avail_h / (float)g_img_h;
        }
        dw = (int)(g_img_w * scale);
        dh = (int)(g_img_h * scale);
        dx = rc.left + (avail_w - dw) / 2 + 12;
        dy = rc.top + (avail_h - dh) / 2 + 12;

        /* soft page shadow */
        shadow.left = dx + 4;
        shadow.top = dy + 4;
        shadow.right = dx + dw + 4;
        shadow.bottom = dy + dh + 4;
        FillRect(hdc, &shadow, (HBRUSH)GetStockObject(BLACK_BRUSH));

        SetStretchBltMode(hdc, HALFTONE);
        SetBrushOrgEx(hdc, 0, 0, NULL);
        StretchBlt(hdc, dx, dy, dw, dh, mem, 0, 0, g_img_w, g_img_h, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
    } else if (g_mode_page && g_page_count == 0) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(240, 240, 240));
        TextOutW(hdc, 24, TOOLBAR_H + 24,
                 L"페이지 이미지가 없습니다.  파일 → 샘플 문서 열기", 40);
    }
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HMENU menubar = CreateMenu();
        HMENU file = CreatePopupMenu();
        HMENU view = CreatePopupMenu();
        HMENU help = CreatePopupMenu();
        AppendMenuW(file, MF_STRING, IDM_FILE_OPEN_SAMPLE, L"샘플 문서 열기\tCtrl+1");
        AppendMenuW(file, MF_STRING, IDM_FILE_OPEN, L"HWPX 열기...\tCtrl+O");
        AppendMenuW(file, MF_SEPARATOR, 0, NULL);
        AppendMenuW(file, MF_STRING, IDM_FILE_EXIT, L"종료");
        AppendMenuW(view, MF_STRING, IDM_VIEW_PAGE, L"페이지 보기 (PDF처럼)");
        AppendMenuW(view, MF_STRING, IDM_VIEW_TEXT, L"텍스트 보기");
        AppendMenuW(help, MF_STRING, IDM_HELP_ABOUT, L"한글패드 정보");
        AppendMenuW(menubar, MF_POPUP, (UINT_PTR)file, L"파일");
        AppendMenuW(menubar, MF_POPUP, (UINT_PTR)view, L"보기");
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
        g_edit = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VSCROLL | WS_HSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL |
                ES_AUTOHSCROLL | ES_READONLY,
            0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_EDIT, GetModuleHandleW(NULL), NULL);
        SendMessageW(g_edit, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        ShowWindow(g_edit, SW_HIDE);
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
        paint_page(hwnd);
        return 0;
    case WM_MOUSEWHEEL:
        if (g_mode_page && g_page_count > 0) {
            if ((short)HIWORD(wParam) < 0) {
                SendMessageW(hwnd, WM_COMMAND, ID_BTN_NEXT, 0);
            } else {
                SendMessageW(hwnd, WM_COMMAND, ID_BTN_PREV, 0);
            }
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_FILE_OPEN:
            file_open();
            break;
        case IDM_FILE_OPEN_SAMPLE:
            open_sample();
            break;
        case IDM_FILE_EXIT:
            DestroyWindow(hwnd);
            break;
        case IDM_VIEW_PAGE:
            if (g_page_count > 0) {
                show_page_mode();
            } else {
                MessageBoxW(hwnd, L"표시할 페이지 이미지가 없습니다.", L"한글패드", MB_OK);
            }
            break;
        case IDM_VIEW_TEXT:
            show_text_mode();
            break;
        case ID_BTN_PREV:
            if (g_page_index > 0) {
                load_page_bitmap(g_page_index - 1);
            }
            break;
        case ID_BTN_NEXT:
            if (g_page_index + 1 < g_page_count) {
                load_page_bitmap(g_page_index + 1);
            }
            break;
        case IDM_HELP_ABOUT:
            about();
            break;
        }
        return 0;
    case WM_KEYDOWN:
        if (g_mode_page) {
            if (wParam == VK_PRIOR || wParam == VK_LEFT || wParam == VK_UP) {
                SendMessageW(hwnd, WM_COMMAND, ID_BTN_PREV, 0);
            } else if (wParam == VK_NEXT || wParam == VK_RIGHT || wParam == VK_DOWN ||
                       wParam == VK_SPACE) {
                SendMessageW(hwnd, WM_COMMAND, ID_BTN_NEXT, 0);
            }
        }
        return 0;
    case WM_DESTROY:
        free_bitmap();
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
        MessageBoxW(NULL, L"COM 초기화 실패", L"한글패드", MB_ICONERROR);
        return 1;
    }
    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWICImagingFactory, (void **)&g_wic);
    if (FAILED(hr) || !g_wic) {
        MessageBoxW(NULL, L"WIC(이미지) 초기화 실패", L"한글패드", MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"HangulPadMain";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    if (!RegisterClassW(&wc)) {
        g_wic->lpVtbl->Release(g_wic);
        CoUninitialize();
        return 1;
    }

    g_main = CreateWindowExW(0, L"HangulPadMain", L"한글패드",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VISIBLE, CW_USEDEFAULT,
                             CW_USEDEFAULT, 920, 1100, NULL, NULL, hInst, NULL);
    if (!g_main) {
        g_wic->lpVtbl->Release(g_wic);
        CoUninitialize();
        return 1;
    }
    ShowWindow(g_main, show);
    UpdateWindow(g_main);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000)) {
            if (msg.wParam == 'O') {
                file_open();
                continue;
            }
            if (msg.wParam == '1') {
                open_sample();
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
        g_wic = NULL;
    }
    CoUninitialize();
    return (int)msg.wParam;
}
