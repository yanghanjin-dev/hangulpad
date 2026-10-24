/*
 * HangulPad — minimal Windows GUI shell (Phase 1 preview).
 * Shows a window with editable text; Open/Save HWPX via the C engine.
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

#define ID_EDIT 1001
#define IDM_FILE_NEW 40001
#define IDM_FILE_OPEN 40002
#define IDM_FILE_SAVE 40003
#define IDM_FILE_SAVEAS 40004
#define IDM_FILE_EXIT 40005
#define IDM_HELP_ABOUT 40010

static HWND g_edit;
static HWND g_main;
static wchar_t g_path[MAX_PATH];
static int g_has_path;

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

static void set_edit_text_utf8(const char *utf8) {
    wchar_t *w = utf8_to_wide(utf8 ? utf8 : "");
    if (!w) {
        SetWindowTextW(g_edit, L"");
        return;
    }
    SetWindowTextW(g_edit, w);
    free(w);
}

static char *get_edit_text_utf8(void) {
    int n = GetWindowTextLengthW(g_edit);
    wchar_t *w;
    char *u;
    if (n < 0) {
        n = 0;
    }
    w = (wchar_t *)malloc(((size_t)n + 1) * sizeof(wchar_t));
    if (!w) {
        return NULL;
    }
    GetWindowTextW(g_edit, w, n + 1);
    u = wide_to_utf8(w);
    free(w);
    return u;
}

static void update_title(void) {
    wchar_t title[MAX_PATH + 64];
    if (g_has_path) {
        const wchar_t *name = wcsrchr(g_path, L'\\');
        name = name ? name + 1 : g_path;
        swprintf(title, MAX_PATH + 64, L"한글패드 — %ls", name);
    } else {
        wcscpy(title, L"한글패드");
    }
    SetWindowTextW(g_main, title);
}

static void doc_new(void) {
    set_edit_text_utf8("");
    g_has_path = 0;
    g_path[0] = 0;
    update_title();
}

static int load_hwpx(const wchar_t *wpath) {
    char *path;
    hp_document *doc = NULL;
    hp_status st;
    char *text;

    path = wide_to_utf8(wpath);
    if (!path) {
        return 0;
    }
    st = hp_hwpx_read(path, &doc);
    if (st != HP_OK || !doc) {
        wchar_t *st_w = utf8_to_wide(hp_status_string(st));
        wchar_t msg[512];
        swprintf(msg, 512, L"HWPX를 열 수 없습니다.\n(%ls)", st_w ? st_w : L"error");
        MessageBoxW(g_main, msg, L"한글패드", MB_ICONERROR | MB_OK);
        free(st_w);
        free(path);
        return 0;
    }
    text = hp_document_plain_text(doc);
    set_edit_text_utf8(text ? text : "");
    free(text);
    hp_document_free(doc);
    wcsncpy(g_path, wpath, MAX_PATH - 1);
    g_path[MAX_PATH - 1] = 0;
    g_has_path = 1;
    update_title();
    free(path);
    return 1;
}

static int save_hwpx(const wchar_t *wpath) {
    char *path;
    char *text;
    hp_document *doc;
    hp_status st;

    path = wide_to_utf8(wpath);
    text = get_edit_text_utf8();
    if (!path || !text) {
        free(path);
        free(text);
        MessageBoxW(g_main, L"메모리 부족", L"한글패드", MB_ICONERROR | MB_OK);
        return 0;
    }
    doc = hp_document_from_text(text);
    free(text);
    if (!doc) {
        free(path);
        MessageBoxW(g_main, L"문서를 만들 수 없습니다", L"한글패드", MB_ICONERROR | MB_OK);
        return 0;
    }
    if (g_has_path) {
        char *title_u = wide_to_utf8(wcsrchr(g_path, L'\\') ? wcsrchr(g_path, L'\\') + 1 : g_path);
        if (title_u) {
            hp_document_set_title(doc, title_u);
            free(title_u);
        }
    }
    st = hp_hwpx_write(doc, path);
    hp_document_free(doc);
    if (st != HP_OK) {
        MessageBoxW(g_main, L"HWPX 저장에 실패했습니다", L"한글패드", MB_ICONERROR | MB_OK);
        free(path);
        return 0;
    }
    wcsncpy(g_path, wpath, MAX_PATH - 1);
    g_path[MAX_PATH - 1] = 0;
    g_has_path = 1;
    update_title();
    free(path);
    return 1;
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
        load_hwpx(file);
    }
}

static void file_save_as(void) {
    OPENFILENAMEW ofn;
    wchar_t file[MAX_PATH];
    ZeroMemory(&ofn, sizeof(ofn));
    if (g_has_path) {
        wcsncpy(file, g_path, MAX_PATH - 1);
        file[MAX_PATH - 1] = 0;
    } else {
        wcscpy(file, L"제목없음.hwpx");
    }
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"HWPX 문서 (*.hwpx)\0*.hwpx\0모든 파일 (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_EXPLORER | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = L"hwpx";
    if (GetSaveFileNameW(&ofn)) {
        save_hwpx(file);
    }
}

static void file_save(void) {
    if (g_has_path) {
        save_hwpx(g_path);
    } else {
        file_save_as();
    }
}

static void about(void) {
    MessageBoxW(g_main,
                L"한글패드 — 경량 한글 워드 프로세서 (엔진 미리보기)\n\n"
                L"HWPX 열기/저장 + 단순 텍스트 편집\n"
                L"엔진: C11 IR / HWPX codec\n"
                L"본 제품은 한글과컴퓨터의 HWP 문서 파일(.hwp) 공개 문서를 참고하여 개발하였습니다.",
                L"한글패드 정보", MB_OK | MB_ICONINFORMATION);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HMENU menubar = CreateMenu();
        HMENU file = CreatePopupMenu();
        HMENU help = CreatePopupMenu();
        AppendMenuW(file, MF_STRING, IDM_FILE_NEW, L"새로 만들기\tCtrl+N");
        AppendMenuW(file, MF_STRING, IDM_FILE_OPEN, L"열기...\tCtrl+O");
        AppendMenuW(file, MF_STRING, IDM_FILE_SAVE, L"저장\tCtrl+S");
        AppendMenuW(file, MF_STRING, IDM_FILE_SAVEAS, L"다른 이름으로 저장...");
        AppendMenuW(file, MF_SEPARATOR, 0, NULL);
        AppendMenuW(file, MF_STRING, IDM_FILE_EXIT, L"종료");
        AppendMenuW(help, MF_STRING, IDM_HELP_ABOUT, L"한글패드 정보");
        AppendMenuW(menubar, MF_POPUP, (UINT_PTR)file, L"파일");
        AppendMenuW(menubar, MF_POPUP, (UINT_PTR)help, L"도움말");
        SetMenu(hwnd, menubar);

        g_edit = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_LEFT | ES_MULTILINE |
                ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_WANTRETURN,
            0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_EDIT, GetModuleHandleW(NULL), NULL);
        SendMessageW(g_edit, WM_SETFONT,
                     (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        set_edit_text_utf8(
            "한글패드에 오신 것을 환영합니다.\n\n"
            "파일 → 열기 로 HWPX를 불러오거나,\n"
            "여기에 글을 쓴 뒤 파일 → 저장 으로 HWPX를 만드세요.");
        return 0;
    }
    case WM_SIZE:
        if (g_edit) {
            MoveWindow(g_edit, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_FILE_NEW:
            doc_new();
            break;
        case IDM_FILE_OPEN:
            file_open();
            break;
        case IDM_FILE_SAVE:
            file_save();
            break;
        case IDM_FILE_SAVEAS:
            file_save_as();
            break;
        case IDM_FILE_EXIT:
            DestroyWindow(hwnd);
            break;
        case IDM_HELP_ABOUT:
            about();
            break;
        }
        return 0;
    case WM_DESTROY:
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
    wc.hCursor = LoadCursor(NULL, IDC_IBEAM);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    if (!RegisterClassW(&wc)) {
        return 1;
    }

    g_main = CreateWindowExW(
        0, L"HangulPadMain", L"한글패드",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 900, 640, NULL, NULL,
        hInst, NULL);
    if (!g_main) {
        return 1;
    }
    ShowWindow(g_main, show);
    UpdateWindow(g_main);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        /* Ctrl+N / O / S accelerators (simple) */
        if (msg.message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000)) {
            if (msg.wParam == 'N') {
                doc_new();
                continue;
            }
            if (msg.wParam == 'O') {
                file_open();
                continue;
            }
            if (msg.wParam == 'S') {
                file_save();
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
