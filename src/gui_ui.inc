/* 0.10.0 window shell: classic 한글-style menus, two toolbars, status bar,
 * a scrolling/zooming page view, formatting, find/replace, print and dialogs.
 * Included by gui_win.c after gui_edit.inc. */

#include <commctrl.h>
#include <limits.h>
#include <math.h>

/* ---- command ids (menus, toolbar buttons, accelerators) ---- */
#define IDM_NEW_WINDOW 40100
#define IDM_FILE_CLOSE 40101
#define IDM_DOC_INFO 40102
#define IDM_PAGE_SETUP 40103
#define IDM_PRINT 40104
#define IDM_FIND 40110
#define IDM_REPLACE 40111
#define IDM_FIND_NEXT 40112
#define IDM_FIND_PREV 40113
#define IDM_GOTO 40114
#define IDM_ZOOM_PAGE 40120
#define IDM_ZOOM_WIDTH 40121
#define IDM_ZOOM_100 40122
#define IDM_ZOOM_IN 40123
#define IDM_ZOOM_OUT 40124
#define IDM_TWO_PAGES 40125
#define IDM_SHOW_FMTBAR 40126
#define IDM_SHOW_STATUS 40127
#define IDM_FIRST_PAGE 40128
#define IDM_LAST_PAGE 40129
#define IDM_CHARMAP 40130
#define IDM_INSERT_DATE 40131
#define IDM_CHAR_SHAPE 40140
#define IDM_PARA_SHAPE 40141
#define IDM_BOLD 40142
#define IDM_ITALIC 40143
#define IDM_UNDERLINE 40144
#define IDM_STRIKE 40145
#define IDM_COLOR 40146
#define IDM_SIZE_UP 40147
#define IDM_SIZE_DOWN 40148
#define IDM_ALIGN_JUSTIFY 40150
#define IDM_ALIGN_LEFT 40151
#define IDM_ALIGN_CENTER 40152
#define IDM_ALIGN_RIGHT 40153
#define IDM_ALIGN_DISTRIBUTE 40154
#define IDM_SPACING_UP 40155
#define IDM_SPACING_DOWN 40156
#define IDM_SHORTCUTS 40160
#define IDM_COLOR_BASE 40170 /* 8 colours */
#define IDM_RECENT_BASE 40500 /* 8 recent documents */
#define IDM_TODO 41000 /* not implemented yet: always greyed */
#define ID_VIEW 1102
#define ID_TB_MAIN 1103
#define ID_TB_FMT 1104
#define ID_CB_STYLE 1200
#define ID_CB_FONT 1201
#define ID_CB_SIZE 1202
#define ID_CB_SPACING 1203
#define ID_CB_ZOOM 1204
#define CBN_APPLY 0x7F00 /* Enter pressed in a toolbar combo */
#define TIMER_PREVIEW 1
#define WM_APP_CARET (WM_APP + 2)
#define RECENT_MAX 8


/* Common controls functions are looked up at run time so that they come from the
 * same comctl32 as the controls: version 6 once enable_visual_styles() has
 * activated the manifest (a static import would bind version 5 at start-up, and
 * a version 5 image list does not work with version 6 toolbars). */
typedef HIMAGELIST(WINAPI *il_create_fn)(int, int, UINT, int, int);
typedef int(WINAPI *il_add_fn)(HIMAGELIST, HBITMAP, HBITMAP);
typedef BOOL(WINAPI *il_destroy_fn)(HIMAGELIST);
typedef BOOL(WINAPI *icc_fn)(const INITCOMMONCONTROLSEX *);

static FARPROC comctl_proc(const char *name) {
    static HMODULE module;
    if (!module) {
        module = LoadLibraryW(L"comctl32.dll");
    }
    return module ? GetProcAddress(module, name) : NULL;
}

static HIMAGELIST cc_imagelist_create(int cx, int cy, UINT flags, int initial, int grow) {
    il_create_fn f = (il_create_fn)(void (*)(void))comctl_proc("ImageList_Create");
    return f ? f(cx, cy, flags, initial, grow) : NULL;
}

static void cc_imagelist_add(HIMAGELIST list, HBITMAP bmp) {
    il_add_fn f = (il_add_fn)(void (*)(void))comctl_proc("ImageList_Add");
    if (f && list) {
        f(list, bmp, NULL);
    }
}

static void cc_imagelist_destroy(HIMAGELIST list) {
    il_destroy_fn f = (il_destroy_fn)(void (*)(void))comctl_proc("ImageList_Destroy");
    if (f && list) {
        f(list);
    }
}

static void cc_init(void) {
    INITCOMMONCONTROLSEX icc;
    icc_fn f = (icc_fn)(void (*)(void))comctl_proc("InitCommonControlsEx");
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    if (f) {
        f(&icc);
    }
}

static HWND g_tb_main, g_tb_fmt, g_cb_style, g_cb_font, g_cb_size, g_cb_spacing, g_cb_zoom;
static HIMAGELIST g_images;
static HACCEL g_accel;
static HMENU g_recent_menu;
static int g_top; /* bottom of the toolbars */
static int g_show_fmtbar = 1, g_show_status = 1, g_two_pages;
enum { ZOOM_PERCENT, ZOOM_FIT_PAGE, ZOOM_FIT_WIDTH };
static int g_zoom_mode = ZOOM_FIT_WIDTH, g_zoom = 100;
static int g_scroll_x, g_scroll_y, g_content_w, g_content_h;
static RECT *g_page_rects;
static int g_page_rect_count;
static const hp_document *g_view_doc;
static int g_keep_scroll;
static int g_chord;
static int g_preview_pending;
static WNDPROC g_text_proc;
static wchar_t g_recent[RECENT_MAX][MAX_PATH];
static int g_recent_count;
static UINT g_find_msg;
static HWND g_find_dlg;
static FINDREPLACEW g_fr;
static wchar_t g_find_what[256], g_replace_with[256];
static wchar_t (*g_font_names)[LF_FACESIZE];
static int g_font_name_count, g_font_name_cap;

static const COLORREF CHORD_COLORS[8] = {RGB(0, 0, 0),     RGB(255, 0, 0),   RGB(0, 0, 255),   RGB(128, 0, 128),
                                         RGB(0, 128, 0),   RGB(255, 255, 0), RGB(0, 128, 128), RGB(255, 255, 255)};

static void format_sync(void);
static void view_sync(void);
static void status_refresh(void);
static void schedule_preview(int delay_ms);
static int preview_build(void);
static void page_caret(HDC dc, int page, const RECT *p, int pw, int ph);
static void view_click(int x, int y, int enter);
static void list_select_item(size_t item);

static void status_message(const wchar_t *text) {
    if (g_status) {
        SendMessageW(g_status, SB_SETTEXTW, 5, (LPARAM)(text ? text : L""));
    }
}

static void update_status(void) {
    view_sync();
    status_refresh();
}

/* ======================= recent documents ======================= */

static int recent_file(wchar_t *out, size_t n) {
    const wchar_t *appdata = _wgetenv(L"APPDATA");
    if (!appdata || !appdata[0]) {
        return 0;
    }
    swprintf(out, n, L"%ls\\HangulPad", appdata);
    CreateDirectoryW(out, NULL);
    wcsncat(out, L"\\recent.txt", n - wcslen(out) - 1);
    return 1;
}

static void recent_load(void) {
    wchar_t file[MAX_PATH];
    FILE *f;
    char line[MAX_PATH * 4];
    g_recent_count = 0;
    if (!recent_file(file, MAX_PATH) || !(f = _wfopen(file, L"rb"))) {
        return;
    }
    while (g_recent_count < RECENT_MAX && fgets(line, sizeof(line), f)) {
        wchar_t *w;
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || !(w = utf8_to_wide(line))) {
            continue;
        }
        if (wcslen(w) < MAX_PATH) {
            wcscpy(g_recent[g_recent_count++], w);
        }
        free(w);
    }
    fclose(f);
}

static void recent_add(const wchar_t *path) {
    wchar_t file[MAX_PATH];
    int i, j;
    FILE *f;
    if (!path || !path[0] || wcslen(path) >= MAX_PATH) {
        return;
    }
    for (i = 0; i < g_recent_count; i++) {
        if (_wcsicmp(g_recent[i], path) == 0) {
            break;
        }
    }
    if (i == g_recent_count && g_recent_count < RECENT_MAX) {
        g_recent_count++;
    }
    if (i >= RECENT_MAX) {
        i = RECENT_MAX - 1;
    }
    for (j = i; j > 0; j--) {
        wcscpy(g_recent[j], g_recent[j - 1]);
    }
    wcscpy(g_recent[0], path);
    if (!recent_file(file, MAX_PATH) || !(f = _wfopen(file, L"wb"))) {
        return;
    }
    for (i = 0; i < g_recent_count; i++) {
        char *u = wide_to_utf8(g_recent[i]);
        if (u) {
            fprintf(f, "%s\n", u);
            free(u);
        }
    }
    fclose(f);
}

static void recent_menu_fill(void) {
    int i;
    if (!g_recent_menu) {
        return;
    }
    while (GetMenuItemCount(g_recent_menu) > 0) {
        DeleteMenu(g_recent_menu, 0, MF_BYPOSITION);
    }
    for (i = 0; i < g_recent_count; i++) {
        wchar_t label[MAX_PATH + 8];
        const wchar_t *name = wcsrchr(g_recent[i], L'\\');
        swprintf(label, MAX_PATH + 8, L"&%d %ls", i + 1, name ? name + 1 : g_recent[i]);
        AppendMenuW(g_recent_menu, MF_STRING, IDM_RECENT_BASE + i, label);
    }
    if (!g_recent_count) {
        AppendMenuW(g_recent_menu, MF_STRING | MF_GRAYED, IDM_TODO, L"(최근 문서 없음)");
    }
}

static void open_and_remember(HWND hwnd, const wchar_t *path) {
    wchar_t copy[MAX_PATH];
    wcsncpy(copy, path, MAX_PATH - 1);
    copy[MAX_PATH - 1] = 0;
    open_document(hwnd, copy);
    if (g_doc && wcscmp(g_path, copy) == 0) {
        recent_add(copy);
    }
}

/* ======================= menus ======================= */

typedef struct {
    int menu; /* 0..8, or 100 + n for a sub-menu of the previous item */
    UINT id;  /* 0 = separator */
    const wchar_t *text;
} menu_item;

enum { M_FILE, M_EDIT, M_VIEW, M_INSERT, M_FORMAT, M_TOOLS, M_TABLE, M_WINDOW, M_HELP, M_COUNT };
static const wchar_t *const MENU_TITLES[M_COUNT] = {L"파일(&F)", L"편집(&E)", L"보기(&U)", L"입력(&D)", L"모양(&J)",
                                                    L"도구(&K)", L"표(&Q)",   L"창(&W)",   L"도움말(&H)"};
#define SUB_RECENT 1
#define SUB_ZOOM 2
#define SUB_TOOLBARS 3
#define SUB_DATE 4
#define SUB_ALIGN 5
#define SUB_COLOR 6

static const menu_item MENU_ITEMS[] = {
    {M_FILE, IDM_FILE_NEW, L"새 문서(&N)\tAlt+N"},
    {M_FILE, IDM_NEW_WINDOW, L"새 창(&W)"},
    {M_FILE, IDM_FILE_OPEN, L"불러오기(&O)...\tAlt+O"},
    {M_FILE, SUB_RECENT, L"최근 문서(&R)"},
    {M_FILE, IDM_FILE_OPEN_SAMPLE, L"샘플 문서 열기\tCtrl+1"},
    {M_FILE, IDM_TODO, L"문서 찾기(&F)..."},
    {M_FILE, IDM_TODO, L"문서 끼워 넣기(&I)..."},
    {M_FILE, 0, NULL},
    {M_FILE, IDM_FILE_SAVE, L"저장하기(&S)\tAlt+S"},
    {M_FILE, IDM_FILE_SAVE_AS, L"다른 이름으로 저장하기(&A)...\tAlt+V"},
    {M_FILE, IDM_TODO, L"PDF로 저장하기(&P)..."},
    {M_FILE, IDM_TODO, L"HWP 형식으로 저장하기"},
    {M_FILE, IDM_FILE_CLOSE, L"문서 닫기(&C)\tCtrl+F4"},
    {M_FILE, 0, NULL},
    {M_FILE, IDM_DOC_INFO, L"문서 정보(&I)...\tCtrl+Q,I"},
    {M_FILE, IDM_TODO, L"문서 암호(&E)..."},
    {M_FILE, IDM_TODO, L"보내기(&D)"},
    {M_FILE, 0, NULL},
    {M_FILE, IDM_PAGE_SETUP, L"편집 용지(&J)...\tF7"},
    {M_FILE, IDM_PREVIEW, L"미리 보기(&V)\tF5"},
    {M_FILE, IDM_PRINT, L"인쇄(&P)...\tAlt+P"},
    {M_FILE, 0, NULL},
    {M_FILE, IDM_FILE_EXIT, L"끝(&X)\tAlt+X"},

    {M_EDIT, IDM_UNDO, L"되돌리기(&U)\tCtrl+Z"},
    {M_EDIT, IDM_TODO, L"다시 실행(&R)\tCtrl+Shift+Z"},
    {M_EDIT, 0, NULL},
    {M_EDIT, IDM_CUT, L"오려 두기(&T)\tCtrl+X"},
    {M_EDIT, IDM_COPY, L"복사하기(&C)\tCtrl+C"},
    {M_EDIT, IDM_PASTE, L"붙이기(&P)\tCtrl+V"},
    {M_EDIT, IDM_TODO, L"골라 붙이기(&S)..."},
    {M_EDIT, IDM_TODO, L"모양 복사(&M)"},
    {M_EDIT, IDM_SELECT_ALL, L"모두 선택(&L)\tCtrl+A"},
    {M_EDIT, 0, NULL},
    {M_EDIT, IDM_EDIT_MODE, L"본문 편집(&B)\tF2"},
    {M_EDIT, IDM_ADD_PARA, L"문단 추가(&N)\tCtrl+Enter"},
    {M_EDIT, IDM_DELETE_PARA, L"문단 지우기(&D)"},
    {M_EDIT, 0, NULL},
    {M_EDIT, IDM_FIND, L"찾기(&F)...\tCtrl+F"},
    {M_EDIT, IDM_REPLACE, L"찾아 바꾸기(&E)...\tCtrl+H"},
    {M_EDIT, IDM_FIND_NEXT, L"다시 찾기(&X)\tCtrl+L"},
    {M_EDIT, IDM_FIND_PREV, L"거꾸로 찾기(&V)\tCtrl+Q,L"},
    {M_EDIT, IDM_GOTO, L"찾아가기(&G)...\tAlt+G"},
    {M_EDIT, 0, NULL},
    {M_EDIT, IDM_TODO, L"개체 속성(&O)..."},
    {M_EDIT, IDM_TODO, L"조판 부호 지우기(&Z)..."},

    {M_VIEW, IDM_TODO, L"쪽 윤곽(&L)\tCtrl+G,L"},
    {M_VIEW, IDM_TWO_PAGES, L"두 쪽(&2)"},
    {M_VIEW, SUB_ZOOM, L"화면 확대(&Z)"},
    {M_VIEW, 0, NULL},
    {M_VIEW, ID_BTN_PREV, L"이전 쪽(&P)\tPgUp"},
    {M_VIEW, ID_BTN_NEXT, L"다음 쪽(&N)\tPgDn"},
    {M_VIEW, IDM_FIRST_PAGE, L"첫 쪽(&F)\tCtrl+Home"},
    {M_VIEW, IDM_LAST_PAGE, L"마지막 쪽(&E)\tCtrl+End"},
    {M_VIEW, 0, NULL},
    {M_VIEW, SUB_TOOLBARS, L"도구 상자(&T)"},
    {M_VIEW, IDM_SHOW_STATUS, L"상황선(&S)"},
    {M_VIEW, 0, NULL},
    {M_VIEW, IDM_TODO, L"문단 부호(&G)\tCtrl+G,T"},
    {M_VIEW, IDM_TODO, L"조판 부호(&C)\tCtrl+G,C"},
    {M_VIEW, IDM_TODO, L"투명 선(&R)"},
    {M_VIEW, IDM_TODO, L"격자(&D)"},
    {M_VIEW, IDM_TODO, L"눈금자(&U)"},
    {M_VIEW, IDM_TODO, L"메모(&M)"},

    {M_INSERT, IDM_CHARMAP, L"문자표(&C)...\tCtrl+F10"},
    {M_INSERT, SUB_DATE, L"날짜/시간(&D)"},
    {M_INSERT, 0, NULL},
    {M_INSERT, IDM_TODO, L"그림(&G)..."},
    {M_INSERT, IDM_TODO, L"그리기 개체(&O)"},
    {M_INSERT, IDM_TODO, L"글상자(&B)"},
    {M_INSERT, IDM_TODO, L"수식(&E)..."},
    {M_INSERT, IDM_TODO, L"차트(&H)"},
    {M_INSERT, 0, NULL},
    {M_INSERT, IDM_TODO, L"주석(&N)"},
    {M_INSERT, IDM_TODO, L"메모(&M)"},
    {M_INSERT, IDM_TODO, L"책갈피(&K)...\tCtrl+K,B"},
    {M_INSERT, IDM_TODO, L"하이퍼링크(&H)...\tCtrl+K,H"},
    {M_INSERT, IDM_TODO, L"상용구(&A)"},
    {M_INSERT, IDM_TODO, L"덧말 넣기(&R)..."},
    {M_INSERT, IDM_TODO, L"글자 겹치기(&V)..."},
    {M_INSERT, IDM_TODO, L"필드 입력(&F)...\tCtrl+K,E"},

    {M_FORMAT, IDM_CHAR_SHAPE, L"글자 모양(&L)...\tAlt+L"},
    {M_FORMAT, IDM_PARA_SHAPE, L"문단 모양(&T)...\tAlt+T"},
    {M_FORMAT, IDM_TODO, L"스타일(&S)...\tF6"},
    {M_FORMAT, 0, NULL},
    {M_FORMAT, IDM_BOLD, L"진하게(&B)\tCtrl+B"},
    {M_FORMAT, IDM_ITALIC, L"기울임(&I)\tCtrl+I"},
    {M_FORMAT, IDM_UNDERLINE, L"밑줄(&U)\tCtrl+U"},
    {M_FORMAT, IDM_STRIKE, L"취소선(&K)"},
    {M_FORMAT, IDM_COLOR, L"글자 색(&C)..."},
    {M_FORMAT, SUB_COLOR, L"글자 색 바로 바꾸기"},
    {M_FORMAT, IDM_SIZE_UP, L"글자 크게(&G)\tCtrl+]"},
    {M_FORMAT, IDM_SIZE_DOWN, L"글자 작게(&M)\tCtrl+["},
    {M_FORMAT, 0, NULL},
    {M_FORMAT, SUB_ALIGN, L"정렬(&A)"},
    {M_FORMAT, IDM_SPACING_UP, L"줄 간격 넓게(&W)\tCtrl+Shift+W"},
    {M_FORMAT, IDM_SPACING_DOWN, L"줄 간격 좁게(&Q)\tCtrl+Shift+Q"},
    {M_FORMAT, 0, NULL},
    {M_FORMAT, IDM_TODO, L"문단 번호 모양(&N)...\tCtrl+K,N"},
    {M_FORMAT, IDM_TODO, L"글머리표 모양(&H)..."},
    {M_FORMAT, IDM_TODO, L"개요 번호 모양(&O)...\tCtrl+K,O"},
    {M_FORMAT, IDM_TODO, L"다단(&D)..."},
    {M_FORMAT, IDM_TODO, L"쪽 테두리/배경(&P)..."},
    {M_FORMAT, IDM_TODO, L"머리말/꼬리말(&E)..."},
    {M_FORMAT, IDM_TODO, L"쪽 번호 매기기(&R)..."},
    {M_FORMAT, IDM_TODO, L"구역 설정(&J)..."},

    {M_TOOLS, IDM_TODO, L"맞춤법(&S)...\tF8"},
    {M_TOOLS, IDM_TODO, L"한자 단어 등록(&H)..."},
    {M_TOOLS, IDM_TODO, L"사전(&D)..."},
    {M_TOOLS, IDM_TODO, L"차례/색인(&T)"},
    {M_TOOLS, IDM_TODO, L"메일 머지(&M)"},
    {M_TOOLS, IDM_TODO, L"매크로(&R)"},
    {M_TOOLS, 0, NULL},
    {M_TOOLS, IDM_FONT_REPORT, L"대체 글꼴 내역(&F)..."},
    {M_TOOLS, IDM_TODO, L"환경 설정(&O)..."},

    {M_TABLE, IDM_TODO, L"표 만들기(&T)...\tCtrl+N,T"},
    {M_TABLE, 0, NULL},
    {M_TABLE, IDM_TODO, L"셀 나누기(&S)...\tS"},
    {M_TABLE, IDM_TODO, L"셀 합치기(&M)\tM"},
    {M_TABLE, IDM_TODO, L"줄/칸 추가하기(&I)...\tAlt+Insert"},
    {M_TABLE, IDM_TODO, L"줄/칸 지우기(&D)...\tAlt+Delete"},
    {M_TABLE, IDM_TODO, L"셀 높이를 같게(&H)\tH"},
    {M_TABLE, IDM_TODO, L"셀 너비를 같게(&W)\tW"},
    {M_TABLE, 0, NULL},
    {M_TABLE, IDM_TODO, L"셀 테두리/배경(&B)..."},
    {M_TABLE, IDM_TODO, L"표 속성(&P)...\tP"},
    {M_TABLE, IDM_TODO, L"계산식(&F)...\tCtrl+N,F"},

    {M_WINDOW, IDM_NEW_WINDOW, L"새 창(&N)"},
    {M_WINDOW, IDM_TODO, L"모두 정렬(&A)"},
    {M_WINDOW, IDM_TODO, L"계단식 배열(&C)"},
    {M_WINDOW, IDM_TODO, L"창 나누기(&S)"},

    {M_HELP, IDM_SHORTCUTS, L"단축키 일람(&K)..."},
    {M_HELP, IDM_FONT_REPORT, L"대체 글꼴 내역(&F)..."},
    {M_HELP, 0, NULL},
    {M_HELP, IDM_HELP_ABOUT, L"한글패드 정보(&A)..."},
};

static HMENU build_submenu(int which) {
    HMENU m = CreatePopupMenu();
    int i;
    switch (which) {
    case SUB_RECENT:
        g_recent_menu = m;
        recent_menu_fill();
        break;
    case SUB_ZOOM:
        AppendMenuW(m, MF_STRING, IDM_ZOOM_PAGE, L"쪽 맞춤(&P)\tCtrl+G,P");
        AppendMenuW(m, MF_STRING, IDM_ZOOM_WIDTH, L"폭 맞춤(&W)\tCtrl+G,W");
        AppendMenuW(m, MF_STRING, IDM_ZOOM_100, L"100%(&Q)\tCtrl+G,Q");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, IDM_ZOOM_IN, L"확대(&I)\tCtrl+휠 위");
        AppendMenuW(m, MF_STRING, IDM_ZOOM_OUT, L"축소(&O)\tCtrl+휠 아래");
        break;
    case SUB_TOOLBARS:
        AppendMenuW(m, MF_STRING | MF_CHECKED | MF_GRAYED, IDM_TODO, L"기본(&B)");
        AppendMenuW(m, MF_STRING, IDM_SHOW_FMTBAR, L"서식(&F)");
        break;
    case SUB_DATE:
        AppendMenuW(m, MF_STRING, IDM_INSERT_DATE, L"날짜/시간 문자열(&S)\tCtrl+K,D");
        AppendMenuW(m, MF_STRING | MF_GRAYED, IDM_TODO, L"날짜/시간 코드(&C)\tCtrl+K,C");
        AppendMenuW(m, MF_STRING | MF_GRAYED, IDM_TODO, L"날짜/시간 형식(&F)...\tCtrl+K,F");
        break;
    case SUB_ALIGN:
        AppendMenuW(m, MF_STRING, IDM_ALIGN_JUSTIFY, L"양쪽 정렬(&J)\tCtrl+Shift+M");
        AppendMenuW(m, MF_STRING, IDM_ALIGN_LEFT, L"왼쪽 정렬(&L)\tCtrl+Shift+L");
        AppendMenuW(m, MF_STRING, IDM_ALIGN_CENTER, L"가운데 정렬(&C)\tCtrl+Shift+C");
        AppendMenuW(m, MF_STRING, IDM_ALIGN_RIGHT, L"오른쪽 정렬(&R)\tCtrl+Shift+R");
        AppendMenuW(m, MF_STRING, IDM_ALIGN_DISTRIBUTE, L"배분 정렬(&D)\tCtrl+Shift+T");
        break;
    case SUB_COLOR: {
        static const wchar_t *const names[8] = {L"검정\tCtrl+M,K", L"빨강\tCtrl+M,R", L"파랑\tCtrl+M,B",
                                                L"자주\tCtrl+M,D", L"초록\tCtrl+M,G", L"노랑\tCtrl+M,Y",
                                                L"청록\tCtrl+M,C", L"흰색\tCtrl+M,W"};
        for (i = 0; i < 8; i++) {
            AppendMenuW(m, MF_STRING, IDM_COLOR_BASE + i, names[i]);
        }
        break;
    }
    }
    return m;
}

static HMENU build_menus(void) {
    HMENU bar = CreateMenu(), pop[M_COUNT];
    size_t i;
    int k;
    for (k = 0; k < M_COUNT; k++) {
        pop[k] = CreatePopupMenu();
    }
    for (i = 0; i < sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]); i++) {
        const menu_item *it = &MENU_ITEMS[i];
        if (!it->text) {
            AppendMenuW(pop[it->menu], MF_SEPARATOR, 0, NULL);
        } else if (it->id < 100) {
            AppendMenuW(pop[it->menu], MF_POPUP, (UINT_PTR)build_submenu((int)it->id), it->text);
        } else {
            AppendMenuW(pop[it->menu], MF_STRING | (it->id == IDM_TODO ? MF_GRAYED : 0), it->id, it->text);
        }
    }
    for (k = 0; k < M_COUNT; k++) {
        AppendMenuW(bar, MF_POPUP, (UINT_PTR)pop[k], MENU_TITLES[k]);
    }
    return bar;
}

/* ======================= toolbar icons ======================= */

enum {
    IC_NEW, IC_OPEN, IC_SAVE, IC_PRINT, IC_PREVIEW, IC_CUT, IC_COPY, IC_PASTE, IC_UNDO, IC_FIND, IC_CHAR, IC_PARA,
    IC_PAGESETUP, IC_FITPAGE, IC_FITWIDTH, IC_EDIT, IC_PREV, IC_NEXT, IC_BOLD, IC_ITALIC, IC_UNDERLINE, IC_STRIKE,
    IC_COLOR, IC_JUSTIFY, IC_LEFT, IC_CENTER, IC_RIGHT, IC_DISTRIBUTE, IC_TWOPAGE, IC_CHARMAP, IC_COUNT
};

/* glyph in Segoe Fluent Icons / Segoe MDL2 Assets (0 = drawn here), a fallback label, colour */
static const struct {
    wchar_t glyph;
    const wchar_t *fallback;
    COLORREF color;
} ICONS[IC_COUNT] = {
    {0xE8A5, L"새", RGB(60, 60, 60)},   {0xE8E5, L"열", RGB(196, 140, 20)}, {0xE74E, L"저", RGB(30, 90, 170)},
    {0xE749, L"인", RGB(60, 60, 60)},   {0xE8FF, L"보", RGB(30, 90, 170)},  {0xE8C6, L"잘", RGB(60, 60, 60)},
    {0xE8C8, L"복", RGB(60, 60, 60)},   {0xE77F, L"붙", RGB(150, 100, 30)}, {0xE7A7, L"↶", RGB(30, 90, 170)},
    {0xE721, L"찾", RGB(60, 60, 60)},   {0, L"가", RGB(30, 90, 170)},       {0, L"¶", RGB(30, 90, 170)},
    {0, NULL, RGB(60, 60, 60)},         {0, NULL, RGB(60, 60, 60)},         {0, NULL, RGB(60, 60, 60)},
    {0xE70F, L"편", RGB(30, 120, 60)},  {0xE76B, L"◀", RGB(60, 60, 60)},    {0xE76C, L"▶", RGB(60, 60, 60)},
    {0, L"가", RGB(20, 20, 20)},        {0, L"가", RGB(20, 20, 20)},        {0, L"가", RGB(20, 20, 20)},
    {0, L"가", RGB(20, 20, 20)},        {0, L"가", RGB(20, 20, 20)},        {0, NULL, RGB(50, 50, 50)},
    {0, NULL, RGB(50, 50, 50)},         {0, NULL, RGB(50, 50, 50)},         {0, NULL, RGB(50, 50, 50)},
    {0, NULL, RGB(50, 50, 50)},         {0xE89A, L"2", RGB(60, 60, 60)},    {0, L"Ω", RGB(60, 60, 60)},
};

static int CALLBACK font_exists_cb(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM out) {
    (void)lf;
    (void)tm;
    (void)type;
    *(int *)out = 1;
    return 0;
}

static int font_installed(const wchar_t *face) {
    LOGFONTW lf;
    HDC dc = GetDC(NULL);
    int found = 0;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    if (dc) {
        EnumFontFamiliesExW(dc, &lf, font_exists_cb, (LPARAM)&found, 0);
        ReleaseDC(NULL, dc);
    }
    return found;
}

static void icon_bar(HDC dc, int x, int y, int w, int h) {
    RECT r = {x, y, x + w, y + h};
    FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
}

static void draw_icon(HDC dc, int k, int s, const wchar_t *symbol_face) {
    RECT r = {0, 0, s, s};
    int t = s >= 20 ? 2 : 1, i;
    FillRect(dc, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0, 0, 0));
    if (k >= IC_JUSTIFY && k <= IC_DISTRIBUTE) {
        /* alignment: four lines */
        int left = s / 8, full = s - 2 * (s / 8);
        for (i = 0; i < 4; i++) {
            int y = s / 6 + i * (s * 2 / 9), w = full, x = left;
            int short_line = i % 2 == 1;
            if (k == IC_LEFT && short_line) {
                w = full * 6 / 10;
            } else if (k == IC_RIGHT && short_line) {
                w = full * 6 / 10;
                x = left + full - w;
            } else if (k == IC_CENTER && short_line) {
                w = full * 6 / 10;
                x = left + (full - w) / 2;
            } else if (k == IC_JUSTIFY && i == 3) {
                w = full * 6 / 10;
            }
            if (k == IC_DISTRIBUTE) {
                int seg = (full - 3 * t) / 4, j;
                for (j = 0; j < 4; j++) {
                    icon_bar(dc, x + j * (seg + t), y, seg, t);
                }
            } else {
                icon_bar(dc, x, y, w, t);
            }
        }
        return;
    }
    if (k == IC_PAGESETUP || k == IC_FITPAGE || k == IC_FITWIDTH) {
        HPEN pen = CreatePen(PS_SOLID, t, RGB(0, 0, 0)), old = (HPEN)SelectObject(dc, pen);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        if (k == IC_FITWIDTH) {
            int y = s / 2;
            Rectangle(dc, 1, s / 5, s - 1, s - s / 5);
            MoveToEx(dc, 3, y, NULL);
            LineTo(dc, s - 3, y);
            MoveToEx(dc, 3 + s / 6, y - s / 6, NULL);
            LineTo(dc, 3, y);
            LineTo(dc, 3 + s / 6, y + s / 6);
            MoveToEx(dc, s - 3 - s / 6, y - s / 6, NULL);
            LineTo(dc, s - 3, y);
            LineTo(dc, s - 3 - s / 6, y + s / 6);
        } else {
            Rectangle(dc, s / 5, 1, s - s / 5, s - 1);
            if (k == IC_PAGESETUP) {
                HPEN dot = CreatePen(PS_DOT, 1, RGB(0, 0, 0));
                SelectObject(dc, dot);
                Rectangle(dc, s / 5 + s / 7, 1 + s / 6, s - s / 5 - s / 7, s - 1 - s / 6);
                SelectObject(dc, pen);
                DeleteObject(dot);
            } else {
                int x = s / 2;
                MoveToEx(dc, x, 3, NULL);
                LineTo(dc, x, s - 3);
                MoveToEx(dc, x - s / 6, 3 + s / 6, NULL);
                LineTo(dc, x, 3);
                LineTo(dc, x + s / 6 + 1, 3 + s / 6 + 1);
                MoveToEx(dc, x - s / 6, s - 3 - s / 6, NULL);
                LineTo(dc, x, s - 3);
                LineTo(dc, x + s / 6 + 1, s - 3 - s / 6 - 1);
            }
        }
        SelectObject(dc, ob);
        SelectObject(dc, old);
        DeleteObject(pen);
        return;
    }
    {
        int text_only = ICONS[k].glyph == 0 || !symbol_face;
        const wchar_t *face = text_only ? L"Malgun Gothic" : symbol_face;
        int weight = k == IC_BOLD ? FW_BOLD : k == IC_CHAR ? FW_SEMIBOLD : FW_NORMAL;
        int h = text_only ? -(s * 9 / 10) : -s;
        RECT box = r;
        HFONT f, old;
        wchar_t one[2] = {ICONS[k].glyph, 0};
        const wchar_t *label = text_only ? ICONS[k].fallback : one;
        if (k == IC_COLOR) {
            box.bottom = s * 3 / 4;
            h = -(s * 7 / 10);
        }
        f = CreateFontW(h, 0, 0, 0, weight, k == IC_ITALIC, k == IC_UNDERLINE, k == IC_STRIKE, DEFAULT_CHARSET, 0, 0,
                        ANTIALIASED_QUALITY, 0, face);
        old = (HFONT)SelectObject(dc, f);
        DrawTextW(dc, label ? label : L"?", -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old);
        DeleteObject(f);
    }
}

static HIMAGELIST build_icons(int s) {
    HIMAGELIST list = cc_imagelist_create(s, s, ILC_COLOR32, IC_COUNT, 0);
    const wchar_t *symbols = font_installed(L"Segoe Fluent Icons")  ? L"Segoe Fluent Icons"
                             : font_installed(L"Segoe MDL2 Assets") ? L"Segoe MDL2 Assets"
                                                                    : NULL;
    HDC dc = CreateCompatibleDC(NULL);
    int k;
    if (!list || !dc) {
        if (dc) {
            DeleteDC(dc);
        }
        return list;
    }
    for (k = 0; k < IC_COUNT; k++) {
        BITMAPINFO bi;
        void *bits = NULL;
        HBITMAP bmp, old;
        ZeroMemory(&bi, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = s;
        bi.bmiHeader.biHeight = -s;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (!bmp || !bits) {
            continue;
        }
        old = (HBITMAP)SelectObject(dc, bmp);
        draw_icon(dc, k, s, symbols);
        GdiFlush();
        {
            /* black-on-white coverage -> premultiplied colour with alpha */
            uint32_t *px = (uint32_t *)bits;
            int i, n = s * s;
            COLORREF c = ICONS[k].color;
            for (i = 0; i < n; i++) {
                unsigned b = px[i] & 255, g = (px[i] >> 8) & 255, rr = (px[i] >> 16) & 255;
                unsigned a = 255 - (rr + g + b) / 3;
                px[i] = (a << 24) | ((GetRValue(c) * a / 255) << 16) | ((GetGValue(c) * a / 255) << 8) |
                        (GetBValue(c) * a / 255);
            }
            if (k == IC_COLOR) {
                int y, x;
                for (y = s * 4 / 5; y < s; y++) {
                    for (x = 1; x < s - 1; x++) {
                        px[y * s + x] = 0xFFE00000u; /* red bar */
                    }
                }
            }
        }
        SelectObject(dc, old);
        cc_imagelist_add(list, bmp);
        DeleteObject(bmp);
    }
    DeleteDC(dc);
    return list;
}

/* ======================= toolbars ======================= */

typedef struct {
    int icon; /* -1 = separator; -2 = combo slot (id = combo id, width in 96-dpi px) */
    int id;
    const wchar_t *tip;
    int check;
    int width;
} tb_def;

static const tb_def TB_MAIN[] = {
    {IC_NEW, IDM_FILE_NEW, L"새 문서 (Alt+N)", 0, 0},
    {IC_OPEN, IDM_FILE_OPEN, L"불러오기 (Alt+O)", 0, 0},
    {IC_SAVE, IDM_FILE_SAVE, L"저장하기 (Alt+S)", 0, 0},
    {-1, 0, NULL, 0, 0},
    {IC_PRINT, IDM_PRINT, L"인쇄 (Alt+P)", 0, 0},
    {IC_PREVIEW, IDM_PREVIEW, L"미리 보기 (F5)", 0, 0},
    {-1, 0, NULL, 0, 0},
    {IC_CUT, IDM_CUT, L"오려 두기 (Ctrl+X)", 0, 0},
    {IC_COPY, IDM_COPY, L"복사하기 (Ctrl+C)", 0, 0},
    {IC_PASTE, IDM_PASTE, L"붙이기 (Ctrl+V)", 0, 0},
    {IC_UNDO, IDM_UNDO, L"되돌리기 (Ctrl+Z)", 0, 0},
    {-1, 0, NULL, 0, 0},
    {IC_FIND, IDM_FIND, L"찾기 (Ctrl+F)", 0, 0},
    {IC_CHAR, IDM_CHAR_SHAPE, L"글자 모양 (Alt+L)", 0, 0},
    {IC_PARA, IDM_PARA_SHAPE, L"문단 모양 (Alt+T)", 0, 0},
    {IC_PAGESETUP, IDM_PAGE_SETUP, L"편집 용지 (F7)", 0, 0},
    {IC_CHARMAP, IDM_CHARMAP, L"문자표 (Ctrl+F10)", 0, 0},
    {-1, 0, NULL, 0, 0},
    {IC_EDIT, IDM_EDIT_MODE, L"본문 편집 (F2)", 1, 0},
    {-1, 0, NULL, 0, 0},
    {IC_PREV, ID_BTN_PREV, L"이전 쪽 (PgUp)", 0, 0},
    {IC_NEXT, ID_BTN_NEXT, L"다음 쪽 (PgDn)", 0, 0},
    {-1, 0, NULL, 0, 0},
    {IC_FITPAGE, IDM_ZOOM_PAGE, L"쪽 맞춤 (Ctrl+G,P)", 1, 0},
    {IC_FITWIDTH, IDM_ZOOM_WIDTH, L"폭 맞춤 (Ctrl+G,W)", 1, 0},
    {IC_TWOPAGE, IDM_TWO_PAGES, L"두 쪽", 1, 0},
    {-2, ID_CB_ZOOM, NULL, 0, 84},
};

static const tb_def TB_FMT[] = {
    {-2, ID_CB_STYLE, NULL, 0, 96},
    {-2, ID_CB_FONT, NULL, 0, 160},
    {-2, ID_CB_SIZE, NULL, 0, 56},
    {-1, 0, NULL, 0, 0},
    {IC_BOLD, IDM_BOLD, L"진하게 (Ctrl+B)", 1, 0},
    {IC_ITALIC, IDM_ITALIC, L"기울임 (Ctrl+I)", 1, 0},
    {IC_UNDERLINE, IDM_UNDERLINE, L"밑줄 (Ctrl+U)", 1, 0},
    {IC_STRIKE, IDM_STRIKE, L"취소선", 1, 0},
    {IC_COLOR, IDM_COLOR, L"글자 색", 0, 0},
    {-1, 0, NULL, 0, 0},
    {IC_JUSTIFY, IDM_ALIGN_JUSTIFY, L"양쪽 정렬 (Ctrl+Shift+M)", 1, 0},
    {IC_LEFT, IDM_ALIGN_LEFT, L"왼쪽 정렬 (Ctrl+Shift+L)", 1, 0},
    {IC_CENTER, IDM_ALIGN_CENTER, L"가운데 정렬 (Ctrl+Shift+C)", 1, 0},
    {IC_RIGHT, IDM_ALIGN_RIGHT, L"오른쪽 정렬 (Ctrl+Shift+R)", 1, 0},
    {IC_DISTRIBUTE, IDM_ALIGN_DISTRIBUTE, L"배분 정렬 (Ctrl+Shift+T)", 1, 0},
    {-1, 0, NULL, 0, 0},
    {-2, ID_CB_SPACING, NULL, 0, 70},
};

static HWND combo_for(int id) {
    switch (id) {
    case ID_CB_STYLE: return g_cb_style;
    case ID_CB_FONT: return g_cb_font;
    case ID_CB_SIZE: return g_cb_size;
    case ID_CB_SPACING: return g_cb_spacing;
    case ID_CB_ZOOM: return g_cb_zoom;
    }
    return NULL;
}

static void toolbar_fill(HWND tb, const tb_def *defs, size_t n) {
    TBBUTTON *b = (TBBUTTON *)calloc(n, sizeof(TBBUTTON));
    size_t i;
    if (!b) {
        return;
    }
    while (SendMessageW(tb, TB_BUTTONCOUNT, 0, 0) > 0) {
        SendMessageW(tb, TB_DELETEBUTTON, 0, 0);
    }
    for (i = 0; i < n; i++) {
        if (defs[i].icon == -1) {
            b[i].fsStyle = BTNS_SEP;
        } else if (defs[i].icon == -2) {
            b[i].fsStyle = BTNS_SEP;
            b[i].iBitmap = S(defs[i].width) + S(4);
        } else {
            b[i].iBitmap = defs[i].icon;
            b[i].idCommand = defs[i].id;
            b[i].fsState = TBSTATE_ENABLED;
            b[i].fsStyle = (BYTE)(BTNS_BUTTON | (defs[i].check ? BTNS_CHECK : 0));
            b[i].iString = (INT_PTR)defs[i].tip;
        }
    }
    SendMessageW(tb, TB_ADDBUTTONSW, (WPARAM)n, (LPARAM)b);
    free(b);
}

static HWND toolbar_create(HWND parent, int id) {
    HWND tb = CreateWindowExW(0, TOOLBARCLASSNAMEW, NULL,
                              WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS |
                                  CCS_NORESIZE | CCS_NOPARENTALIGN | CCS_NODIVIDER,
                              0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(tb, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    SendMessageW(tb, TB_SETMAXTEXTROWS, 0, 0); /* button text becomes the tooltip */
    return tb;
}

static void toolbars_rebuild(void) {
    HIMAGELIST old = g_images;
    g_images = build_icons(S(16));
    SendMessageW(g_tb_main, TB_SETIMAGELIST, 0, (LPARAM)g_images);
    SendMessageW(g_tb_fmt, TB_SETIMAGELIST, 0, (LPARAM)g_images);
    SendMessageW(g_tb_main, TB_SETBUTTONSIZE, 0, MAKELPARAM(S(24), S(24)));
    SendMessageW(g_tb_fmt, TB_SETBUTTONSIZE, 0, MAKELPARAM(S(24), S(24)));
    toolbar_fill(g_tb_main, TB_MAIN, sizeof(TB_MAIN) / sizeof(TB_MAIN[0]));
    toolbar_fill(g_tb_fmt, TB_FMT, sizeof(TB_FMT) / sizeof(TB_FMT[0]));
    if (old) {
        cc_imagelist_destroy(old);
    }
}

static int toolbar_height(void) {
    return S(30);
}

/* put the combos over their separator slots */
static void place_combos(HWND tb, const tb_def *defs, size_t n, int y) {
    size_t i;
    for (i = 0; i < n; i++) {
        RECT r;
        HWND cb;
        if (defs[i].icon != -2 || !(cb = combo_for(defs[i].id))) {
            continue;
        }
        if (!SendMessageW(tb, TB_GETITEMRECT, (WPARAM)i, (LPARAM)&r)) {
            continue;
        }
        {
            RECT cr;
            int h;
            GetWindowRect(cb, &cr);
            h = cr.bottom - cr.top;
            SetWindowPos(cb, HWND_TOP, r.left + S(2), y + (toolbar_height() - h) / 2, S(defs[i].width), S(300),
                         SWP_NOACTIVATE);
            ShowWindow(cb, IsWindowVisible(tb) ? SW_SHOW : SW_HIDE);
        }
    }
}

/* Enter / Esc in a toolbar combo's edit field (window subclass) */
static LRESULT CALLBACK combo_edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    WNDPROC old = (WNDPROC)GetPropW(h, L"HangulPad.old");
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        PostMessageW(g_main, WM_COMMAND, MAKEWPARAM((UINT)(UINT_PTR)GetPropW(h, L"HangulPad.id"), CBN_APPLY), 0);
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        format_sync();
        SetFocus(g_editing ? g_text : g_view);
        return 0;
    }
    if (msg == WM_CHAR && (wp == '\r' || wp == 27)) {
        return 0;
    }
    if (msg == WM_NCDESTROY) {
        RemovePropW(h, L"HangulPad.old");
        RemovePropW(h, L"HangulPad.id");
        SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)old);
    }
    return old ? CallWindowProcW(old, h, msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

static HWND combo_create(HWND parent, int id, int editable) {
    HWND cb = CreateWindowExW(0, WC_COMBOBOXW, L"",
                              WS_CHILD | WS_VSCROLL | WS_TABSTOP | (editable ? CBS_DROPDOWN : CBS_DROPDOWNLIST) |
                                  CBS_AUTOHSCROLL,
                              0, 0, 100, 300, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    if (cb && editable) {
        COMBOBOXINFO info;
        ZeroMemory(&info, sizeof(info));
        info.cbSize = sizeof(info);
        if (GetComboBoxInfo(cb, &info) && info.hwndItem) {
            SetPropW(info.hwndItem, L"HangulPad.id", (HANDLE)(UINT_PTR)id);
            SetPropW(info.hwndItem, L"HangulPad.old",
                     (HANDLE)SetWindowLongPtrW(info.hwndItem, GWLP_WNDPROC, (LONG_PTR)combo_edit_proc));
        }
    }
    return cb;
}

static int CALLBACK font_name_cb(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM unused) {
    int i;
    (void)tm;
    (void)type;
    (void)unused;
    if (lf->lfFaceName[0] == L'@') {
        return 1; /* vertical variants */
    }
    for (i = 0; i < g_font_name_count; i++) {
        if (wcscmp(g_font_names[i], lf->lfFaceName) == 0) {
            return 1;
        }
    }
    if (g_font_name_count == g_font_name_cap) {
        int cap = g_font_name_cap ? g_font_name_cap * 2 : 256;
        void *next = realloc(g_font_names, (size_t)cap * sizeof(*g_font_names));
        if (!next) {
            return 0;
        }
        g_font_names = next;
        g_font_name_cap = cap;
    }
    wcsncpy(g_font_names[g_font_name_count], lf->lfFaceName, LF_FACESIZE - 1);
    g_font_names[g_font_name_count++][LF_FACESIZE - 1] = 0;
    return 1;
}

static void fill_font_combo(HWND cb) {
    int i;
    if (!g_font_name_count) {
        LOGFONTW lf;
        HDC dc = GetDC(NULL);
        ZeroMemory(&lf, sizeof(lf));
        lf.lfCharSet = DEFAULT_CHARSET;
        if (dc) {
            EnumFontFamiliesExW(dc, &lf, font_name_cb, 0, 0);
            ReleaseDC(NULL, dc);
        }
    }
    SendMessageW(cb, WM_SETREDRAW, FALSE, 0);
    for (i = 0; i < g_font_name_count; i++) {
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)g_font_names[i]);
    }
    SendMessageW(cb, WM_SETREDRAW, TRUE, 0);
}

static void combos_create(HWND hwnd) {
    static const wchar_t *const sizes[] = {L"8", L"9", L"10", L"11", L"12", L"13", L"14", L"16", L"18",
                                           L"20", L"22", L"24", L"28", L"32", L"36", L"48", L"72"};
    static const wchar_t *const spacing[] = {L"100 %", L"130 %", L"150 %", L"160 %", L"180 %",
                                             L"200 %", L"250 %", L"300 %"};
    static const wchar_t *const zooms[] = {L"쪽 맞춤", L"폭 맞춤", L"50%",  L"75%",  L"100%",
                                           L"125%",    L"150%",    L"200%", L"300%", L"400%"};
    size_t i;
    g_cb_style = combo_create(hwnd, ID_CB_STYLE, 0);
    g_cb_font = combo_create(hwnd, ID_CB_FONT, 1);
    g_cb_size = combo_create(hwnd, ID_CB_SIZE, 1);
    g_cb_spacing = combo_create(hwnd, ID_CB_SPACING, 1);
    g_cb_zoom = combo_create(hwnd, ID_CB_ZOOM, 1);
    SendMessageW(g_cb_style, CB_ADDSTRING, 0, (LPARAM)L"바탕글");
    SendMessageW(g_cb_style, CB_SETCURSEL, 0, 0);
    EnableWindow(g_cb_style, FALSE); /* styles: later version */
    SendMessageW(g_cb_font, CB_SETEXTENDEDUI, TRUE, 0);
    fill_font_combo(g_cb_font);
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        SendMessageW(g_cb_size, CB_ADDSTRING, 0, (LPARAM)sizes[i]);
    }
    for (i = 0; i < sizeof(spacing) / sizeof(spacing[0]); i++) {
        SendMessageW(g_cb_spacing, CB_ADDSTRING, 0, (LPARAM)spacing[i]);
    }
    for (i = 0; i < sizeof(zooms) / sizeof(zooms[0]); i++) {
        SendMessageW(g_cb_zoom, CB_ADDSTRING, 0, (LPARAM)zooms[i]);
    }
}

/* ======================= layout ======================= */

static void layout_children(int cx, int cy) {
    int y = 0, sb = 0, area;
    if (g_tb_main) {
        MoveWindow(g_tb_main, 0, y, cx, toolbar_height(), TRUE);
        place_combos(g_tb_main, TB_MAIN, sizeof(TB_MAIN) / sizeof(TB_MAIN[0]), y);
        y += toolbar_height();
    }
    if (g_tb_fmt) {
        ShowWindow(g_tb_fmt, g_show_fmtbar ? SW_SHOW : SW_HIDE);
        if (g_show_fmtbar) {
            MoveWindow(g_tb_fmt, 0, y, cx, toolbar_height(), TRUE);
            y += toolbar_height();
        }
        place_combos(g_tb_fmt, TB_FMT, sizeof(TB_FMT) / sizeof(TB_FMT[0]), y - toolbar_height());
    }
    if (g_status) {
        ShowWindow(g_status, g_show_status ? SW_SHOW : SW_HIDE);
        if (g_show_status) {
            RECT r;
            int parts[6], w = cx;
            SendMessageW(g_status, WM_SIZE, 0, 0);
            GetWindowRect(g_status, &r);
            sb = r.bottom - r.top;
            parts[0] = S(110);
            parts[1] = parts[0] + S(80);
            parts[2] = parts[1] + S(110);
            parts[3] = parts[2] + S(64);
            parts[4] = parts[3] + S(150);
            parts[5] = -1;
            (void)w;
            SendMessageW(g_status, SB_SETPARTS, 6, (LPARAM)parts);
        }
    }
    g_top = y;
    area = cy - y - sb;
    if (area < 0) {
        area = 0;
    }
    if (g_editing) {
        int lw = cx / 3, th;
        if (lw > S(340)) {
            lw = S(340);
        }
        th = area * 2 / 5;
        if (th < S(90)) {
            th = S(90);
        }
        MoveWindow(g_paralist, S(4), y + S(4), lw - S(8), area - S(8), TRUE);
        MoveWindow(g_text, lw, y + S(4), cx - lw - S(4), th, TRUE);
        if (g_view) {
            MoveWindow(g_view, lw, y + S(8) + th, cx - lw - S(4), area - th - S(12) > 0 ? area - th - S(12) : 0, TRUE);
        }
    } else if (g_view) {
        MoveWindow(g_view, 0, y, cx, area, TRUE);
    }
}

static void apply_ui_fonts(void) {
    HWND controls[] = {g_status, g_paralist, g_cb_style, g_cb_font, g_cb_size, g_cb_spacing, g_cb_zoom};
    HFONT ui, edit;
    size_t i;
    ui = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                     CLEARTYPE_QUALITY, 0, L"Malgun Gothic");
    edit = CreateFontW(-MulDiv(11, g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                       CLEARTYPE_QUALITY, 0, L"Malgun Gothic");
    for (i = 0; i < sizeof(controls) / sizeof(controls[0]); i++) {
        if (controls[i]) {
            SendMessageW(controls[i], WM_SETFONT, (WPARAM)(ui ? (HGDIOBJ)ui : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        }
    }
    if (g_text) {
        SendMessageW(g_text, WM_SETFONT, (WPARAM)(edit ? (HGDIOBJ)edit : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    }
    if (g_ui_font) {
        DeleteObject(g_ui_font);
    }
    if (g_edit_font) {
        DeleteObject(g_edit_font);
    }
    g_ui_font = ui;
    g_edit_font = edit;
}

/* ======================= text positions (0.11.0) =======================
 * Edit control: UTF-16 with CRLF. Engine: UTF-8 with LF. Page hit map:
 * UTF-16 index of the LF text. */

/* edit control index -> UTF-16 index of the LF text */
static long edit_to_lf(const wchar_t *w, long at) {
    long i, k = 0;
    for (i = 0; i < at && w[i]; i++) {
        if (w[i] != L'\r') {
            k++;
        }
    }
    return k;
}

/* UTF-16 index of the LF text -> UTF-8 byte offset */
static size_t lf_to_utf8(const wchar_t *w, long at) {
    long i;
    size_t bytes = 0;
    for (i = 0; i < at && w[i]; i++) {
        if (w[i] == L'\r') {
            continue;
        }
        bytes += w[i] < 0x80 ? 1 : w[i] < 0x800 ? 2 : (w[i] >= 0xD800 && w[i] <= 0xDFFF) ? 2 : 3;
    }
    return bytes;
}

static wchar_t *text_box_text(void) {
    int n = g_text ? GetWindowTextLengthW(g_text) : 0;
    wchar_t *w = (wchar_t *)malloc(((size_t)n + 1) * sizeof(wchar_t));
    if (w) {
        if (g_text) {
            GetWindowTextW(g_text, w, n + 1);
        } else {
            w[0] = 0;
        }
    }
    return w;
}

/* selection of the text box in UTF-8 bytes of the current paragraph's text;
 * returns 1 when there is a non-empty selection */
static int text_selection_bytes(size_t *start, size_t *end) {
    DWORD a = 0, b = 0;
    wchar_t *w;
    if (!g_editing || !g_text || g_edit_index == (size_t)-1) {
        return 0;
    }
    SendMessageW(g_text, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
    if (b <= a || !(w = text_box_text())) {
        return 0;
    }
    *start = lf_to_utf8(w, (long)a);
    *end = lf_to_utf8(w, (long)b);
    free(w);
    return *end > *start;
}

static size_t caret_bytes(void) {
    DWORD a = 0, b = 0;
    wchar_t *w;
    size_t r;
    if (!g_editing || !g_text) {
        return 0;
    }
    SendMessageW(g_text, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
    if (!(w = text_box_text())) {
        return 0;
    }
    r = lf_to_utf8(w, (long)a);
    free(w);
    return r;
}

/* the character format the toolbar shows / toggles start from */
static int current_char_format(hp_char_format *cf) {
    if (!g_editor || g_edit_index == (size_t)-1) {
        return 0;
    }
    if (g_editing) {
        return hp_editor_get_format_at(g_editor, g_edit_index, caret_bytes(), cf) == HP_OK;
    }
    return hp_editor_get_format(g_editor, g_edit_index, cf, NULL) == HP_OK;
}

/* editor index of the paragraph with source index source_p (1-based) in a
 * section of the displayed document, and back */
static size_t item_for_source(size_t section, uint32_t source_p) {
    size_t i, k = 0;
    for (i = 0; g_editor && i < hp_editor_count(g_editor); i++) {
        if (hp_editor_section(g_editor, i) == section && !hp_editor_is_deleted(g_editor, i) && ++k == source_p) {
            return i;
        }
    }
    return (size_t)-1;
}

static uint32_t source_of_item(size_t item) {
    size_t i;
    uint32_t k = 0;
    for (i = 0; g_editor && i <= item && i < hp_editor_count(g_editor); i++) {
        if (hp_editor_section(g_editor, i) == hp_editor_section(g_editor, item) && !hp_editor_is_deleted(g_editor, i)) {
            k++;
        }
    }
    return k;
}


/* ======================= page view ======================= */

static void view_page_size(int i, int *w, int *h) {
    const hp_page *pg = &g_doc->sections[g_pages[i].section].page;
    *w = pg->width ? hp_line_px(pg->width) : 794;
    *h = pg->height ? hp_line_px(pg->height) : 1123;
    if (*w < 100) {
        *w = 100;
    }
    if (*h < 100) {
        *h = 100;
    }
}

/* screen pixels per page pixel */
static double view_scale(void) {
    RECT r;
    int cw, ch, pw = 794, ph = 1123, cols = g_two_pages ? 2 : 1, gap = S(16);
    double sc;
    if (!g_view || !g_doc || g_page_count <= 0) {
        return g_zoom / 100.0 * g_dpi / 96.0;
    }
    if (g_zoom_mode == ZOOM_PERCENT) {
        return g_zoom / 100.0 * g_dpi / 96.0;
    }
    GetClientRect(g_view, &r);
    cw = r.right - gap * (cols + 1);
    ch = r.bottom - gap * 2;
    view_page_size(g_page_index < g_page_count ? g_page_index : 0, &pw, &ph);
    sc = (double)cw / (pw * cols);
    if (g_zoom_mode == ZOOM_FIT_PAGE && (double)ch / ph < sc) {
        sc = (double)ch / ph;
    }
    if (sc < 0.05) {
        sc = 0.05;
    }
    return sc;
}

static int view_zoom_percent(void) {
    return (int)(view_scale() * 96.0 / g_dpi * 100.0 + 0.5);
}

static void view_relayout(void) {
    RECT r;
    int i, cols = g_two_pages ? 2 : 1, gap = S(16), y = gap, maxw = 0, cw;
    double sc = view_scale();
    free(g_page_rects);
    g_page_rects = NULL;
    g_page_rect_count = 0;
    g_content_w = g_content_h = 0;
    if (!g_view || !g_doc || g_page_count <= 0) {
        return;
    }
    g_page_rects = (RECT *)calloc((size_t)g_page_count, sizeof(RECT));
    if (!g_page_rects) {
        return;
    }
    g_page_rect_count = g_page_count;
    GetClientRect(g_view, &r);
    cw = r.right;
    for (i = 0; i < g_page_count; i += cols) {
        int k, roww = 0, rowh = 0, x;
        for (k = i; k < i + cols && k < g_page_count; k++) {
            int pw, ph;
            view_page_size(k, &pw, &ph);
            g_page_rects[k].right = (LONG)(pw * sc + 0.5);
            g_page_rects[k].bottom = (LONG)(ph * sc + 0.5);
            roww += g_page_rects[k].right + (k > i ? gap : 0);
            if (g_page_rects[k].bottom > rowh) {
                rowh = g_page_rects[k].bottom;
            }
        }
        if (roww > maxw) {
            maxw = roww;
        }
        x = 0;
        for (k = i; k < i + cols && k < g_page_count; k++) {
            int w = g_page_rects[k].right, h = g_page_rects[k].bottom;
            g_page_rects[k].left = x;
            g_page_rects[k].top = y;
            g_page_rects[k].right = x + w;
            g_page_rects[k].bottom = y + h;
            x += w + gap;
        }
        y += rowh + gap;
    }
    g_content_w = maxw + 2 * gap;
    g_content_h = y;
    /* horizontal centring */
    for (i = 0; i < g_page_count; i += cols) {
        int k, last = i + cols - 1 < g_page_count - 1 ? i + cols - 1 : g_page_count - 1;
        int roww = g_page_rects[last].right - g_page_rects[i].left;
        int total = cw > g_content_w ? cw : g_content_w, off = (total - roww) / 2;
        for (k = i; k <= last; k++) {
            OffsetRect(&g_page_rects[k], off, 0);
        }
    }
}

static void view_set_scrollbars(void) {
    SCROLLINFO si;
    RECT r;
    if (!g_view) {
        return;
    }
    GetClientRect(g_view, &r);
    if (g_scroll_y > g_content_h - r.bottom) {
        g_scroll_y = g_content_h - r.bottom;
    }
    if (g_scroll_y < 0) {
        g_scroll_y = 0;
    }
    if (g_scroll_x > g_content_w - r.right) {
        g_scroll_x = g_content_w - r.right;
    }
    if (g_scroll_x < 0) {
        g_scroll_x = 0;
    }
    ZeroMemory(&si, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL | SIF_DISABLENOSCROLL;
    si.nMax = g_content_h > 0 ? g_content_h - 1 : 0;
    si.nPage = (UINT)r.bottom;
    si.nPos = g_scroll_y;
    SetScrollInfo(g_view, SB_VERT, &si, TRUE);
    si.nMax = g_content_w > 0 ? g_content_w - 1 : 0;
    si.nPage = (UINT)r.right;
    si.nPos = g_scroll_x;
    SetScrollInfo(g_view, SB_HORZ, &si, TRUE);
}

/* the page at the top third of the view becomes the current page */
static void view_track_page(void) {
    RECT r;
    int i, mark;
    if (!g_view || !g_page_rect_count) {
        return;
    }
    GetClientRect(g_view, &r);
    mark = g_scroll_y + r.bottom / 3;
    for (i = 0; i < g_page_rect_count; i++) {
        if (g_page_rects[i].bottom + S(8) > mark) {
            break;
        }
    }
    if (i >= g_page_rect_count) {
        i = g_page_rect_count - 1;
    }
    if (i != g_page_index) {
        g_page_index = i;
        status_refresh();
    }
}

static void view_scroll_to(int x, int y) {
    g_scroll_x = x;
    g_scroll_y = y;
    view_set_scrollbars();
    if (g_view) {
        InvalidateRect(g_view, NULL, FALSE);
    }
    view_track_page();
}

static void view_show_page(int i) {
    if (i < 0 || i >= g_page_rect_count) {
        return;
    }
    g_page_index = i;
    g_scroll_y = g_page_rects[i].top - S(16);
    view_set_scrollbars();
    if (g_view) {
        InvalidateRect(g_view, NULL, FALSE);
    }
    status_refresh();
}

static void view_sync(void) {
    int page = g_page_index;
    if (!g_view) {
        return;
    }
    view_relayout();
    if (g_doc != g_view_doc && !g_keep_scroll) {
        g_scroll_x = 0;
        if (page >= 0 && page < g_page_rect_count) {
            g_scroll_y = g_page_rects[page].top - S(16);
        } else {
            g_scroll_y = 0;
        }
    }
    g_view_doc = g_doc;
    view_set_scrollbars();
    InvalidateRect(g_view, NULL, FALSE);
}

static void set_zoom(int mode, int percent) {
    int page = g_page_index;
    double rel = 0;
    if (g_page_rect_count && page < g_page_rect_count) {
        int h = g_page_rects[page].bottom - g_page_rects[page].top;
        rel = h > 0 ? (double)(g_scroll_y + S(16) - g_page_rects[page].top) / h : 0;
    }
    if (percent < 10) {
        percent = 10;
    }
    if (percent > 500) {
        percent = 500;
    }
    g_zoom_mode = mode;
    g_zoom = percent;
    view_relayout();
    if (page < g_page_rect_count) {
        g_scroll_y = g_page_rects[page].top - S(16) +
                     (int)(rel * (g_page_rects[page].bottom - g_page_rects[page].top));
    }
    view_set_scrollbars();
    if (g_view) {
        InvalidateRect(g_view, NULL, FALSE);
    }
    status_refresh();
}

static void view_paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps), mem;
    RECT rc;
    HBITMAP bmp, oldbmp;
    int i;
    GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) {
        EndPaint(hwnd, &ps);
        return;
    }
    mem = CreateCompatibleDC(hdc);
    bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    oldbmp = (HBITMAP)SelectObject(mem, bmp);
    {
        HBRUSH bg = CreateSolidBrush(RGB(128, 128, 128));
        FillRect(mem, &rc, bg);
        DeleteObject(bg);
    }
    if (!g_doc || g_page_count == 0 || !g_page_rect_count) {
        HGDIOBJ old_font = SelectObject(mem, g_ui_font ? (HGDIOBJ)g_ui_font : GetStockObject(DEFAULT_GUI_FONT));
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(245, 245, 245));
        TextOutW(mem, S(24), S(24), L"HWP 또는 HWPX 문서를 열어 주세요.",
                 (int)wcslen(L"HWP 또는 HWPX 문서를 열어 주세요."));
        SelectObject(mem, old_font);
    } else {
        for (i = 0; i < g_page_rect_count; i++) {
            RECT p = g_page_rects[i], hit;
            OffsetRect(&p, -g_scroll_x, -g_scroll_y);
            if (!IntersectRect(&hit, &p, &ps.rcPaint)) {
                continue;
            }
            {
                RECT sh = p;
                OffsetRect(&sh, S(3), S(3));
                FillRect(mem, &sh, (HBRUSH)GetStockObject(BLACK_BRUSH));
                FillRect(mem, &p, (HBRUSH)GetStockObject(WHITE_BRUSH));
            }
            {
                int pw, ph, saved = SaveDC(mem);
                view_page_size(i, &pw, &ph);
                IntersectClipRect(mem, p.left, p.top, p.right, p.bottom);
                SetMapMode(mem, MM_ANISOTROPIC);
                SetWindowExtEx(mem, pw, ph, NULL);
                SetViewportExtEx(mem, p.right - p.left, p.bottom - p.top, NULL);
                SetViewportOrgEx(mem, p.left, p.top, NULL);
                {
                    hp_hit_map hits = {0};
                    int caret = g_editing && g_editor && g_edit_index != (size_t)-1;
                    if (caret) {
                        g_hit = &hits;
                    }
                    paint_document_page(mem, i);
                    g_hit = NULL;
                    SetGraphicsMode(mem, GM_ADVANCED);
                    ModifyWorldTransform(mem, NULL, MWT_IDENTITY);
                    SetGraphicsMode(mem, GM_COMPATIBLE);
                    RestoreDC(mem, saved);
                    if (caret) {
                        g_hit = &hits;
                        page_caret(mem, i, &p, pw, ph);
                        g_hit = NULL;
                    }
                    free(hits.v);
                }
            }
        }
    }
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldbmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

static void goto_page(HWND hwnd, int idx) {
    (void)hwnd;
    if (idx < 0 || idx >= g_page_count) {
        return;
    }
    view_show_page(idx);
}

static void view_scroll_cmd(int bar, int code) {
    RECT r;
    SCROLLINFO si;
    int pos = bar == SB_VERT ? g_scroll_y : g_scroll_x, page, line = S(40);
    GetClientRect(g_view, &r);
    page = bar == SB_VERT ? r.bottom : r.right;
    ZeroMemory(&si, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_TRACKPOS;
    GetScrollInfo(g_view, bar, &si);
    switch (code) {
    case SB_LINEUP: pos -= line; break;
    case SB_LINEDOWN: pos += line; break;
    case SB_PAGEUP: pos -= page * 9 / 10; break;
    case SB_PAGEDOWN: pos += page * 9 / 10; break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: pos = si.nTrackPos; break;
    case SB_TOP: pos = 0; break;
    case SB_BOTTOM: pos = 1 << 30; break;
    default: return;
    }
    if (bar == SB_VERT) {
        view_scroll_to(g_scroll_x, pos);
    } else {
        view_scroll_to(pos, g_scroll_y);
    }
}

static void quick_menu(HWND hwnd, int x, int y) {
    HMENU m = CreatePopupMenu();
    UINT ro = g_editor ? 0 : MF_GRAYED;
    AppendMenuW(m, MF_STRING | ro, IDM_EDIT_MODE, L"본문 편집(&B)\tF2");
    AppendMenuW(m, MF_STRING | ro, IDM_CHAR_SHAPE, L"글자 모양(&L)...\tAlt+L");
    AppendMenuW(m, MF_STRING | ro, IDM_PARA_SHAPE, L"문단 모양(&T)...\tAlt+T");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_ZOOM_PAGE, L"쪽 맞춤(&P)");
    AppendMenuW(m, MF_STRING, IDM_ZOOM_WIDTH, L"폭 맞춤(&W)");
    AppendMenuW(m, MF_STRING, IDM_ZOOM_100, L"100%(&Q)");
    AppendMenuW(m, MF_STRING | (g_two_pages ? MF_CHECKED : 0), IDM_TWO_PAGES, L"두 쪽(&2)");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_GOTO, L"찾아가기(&G)...\tAlt+G");
    AppendMenuW(m, MF_STRING, IDM_PRINT, L"인쇄(&P)...\tAlt+P");
    TrackPopupMenu(m, TPM_RIGHTBUTTON, x, y, 0, hwnd, NULL);
    DestroyMenu(m);
}

/* ======================= caret on the page (0.11.0) ======================= */

/* the current paragraph's caret/selection as UTF-16 indices of its LF text */
static int caret_range(long *a, long *b) {
    DWORD s0 = 0, s1 = 0;
    wchar_t *w;
    if (!g_text || !(w = text_box_text())) {
        return 0;
    }
    SendMessageW(g_text, EM_GETSEL, (WPARAM)&s0, (LPARAM)&s1);
    *a = edit_to_lf(w, (long)s0);
    *b = edit_to_lf(w, (long)s1);
    free(w);
    return 1;
}

static void page_caret(HDC dc, int page, const RECT *p, int pw, int ph) {
    size_t section = hp_editor_section(g_editor, g_edit_index), k;
    uint32_t src = source_of_item(g_edit_index);
    long a, b;
    double sx = (double)(p->right - p->left) / pw, sy = (double)(p->bottom - p->top) / ph;
    int drawn = 0;
    (void)page;
    if (!caret_range(&a, &b)) {
        return;
    }
    for (k = 0; k < g_hit->n; k++) {
        const hp_hit *h = &g_hit->v[k];
        RECT r;
        if (h->section != section || h->source_p != src) {
            continue;
        }
        r.top = p->top + (int)(h->y0 * sy);
        r.bottom = p->top + (int)(h->y1 * sy + 0.5);
        if (b > a) {
            if ((long)h->ci >= a && (long)h->ci < b && h->len) {
                r.left = p->left + (int)(h->x0 * sx);
                r.right = p->left + (int)(h->x1 * sx + 0.5);
                InvertRect(dc, &r);
            }
        } else if (!drawn && ((long)h->ci == a || ((long)(h->ci + h->len) == a && h->len))) {
            /* caret before this glyph, or after it at the end of a line */
            int x = p->left + (int)(((long)h->ci == a ? h->x0 : h->x1) * sx);
            if ((long)h->ci != a && k + 1 < g_hit->n && (long)g_hit->v[k + 1].ci == a &&
                g_hit->v[k + 1].source_p == src && g_hit->v[k + 1].y0 == h->y0) {
                continue; /* the next glyph on this line starts there */
            }
            r.left = x;
            r.right = x + (S(2) > 1 ? S(2) : 2);
            PatBlt(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, DSTINVERT);
            drawn = 1;
        }
    }
}

/* page coordinates -> paragraph and character (UTF-16 index of its LF text) */
static int hit_test(int page, double x, double y, size_t *section, uint32_t *src, long *ci) {
    hp_hit_map hits = {0};
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bmp, old;
    int pw, ph, found = 0;
    size_t k, best = 0;
    double best_d = 1e18;
    if (!dc) {
        return 0;
    }
    view_page_size(page, &pw, &ph);
    bmp = CreateBitmap(pw, ph, 1, 32, NULL);
    old = (HBITMAP)SelectObject(dc, bmp);
    g_hit = &hits;
    paint_document_page(dc, page);
    g_hit = NULL;
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    for (k = 0; k < hits.n; k++) {
        const hp_hit *h = &hits.v[k];
        double dy = y < h->y0 ? h->y0 - y : y >= h->y1 ? y - h->y1 + 1 : 0;
        double dx = x < h->x0 ? h->x0 - x : x > h->x1 ? x - h->x1 : 0;
        double d = dy * 1000 + dx; /* same line first, then nearest glyph */
        if (d < best_d) {
            best_d = d;
            best = k;
            found = 1;
        }
    }
    if (found) {
        const hp_hit *h = &hits.v[best];
        *section = h->section;
        *src = h->source_p;
        *ci = (long)h->ci + (h->len && x > (h->x0 + h->x1) / 2 ? (long)h->len : 0);
    }
    free(hits.v);
    return found;
}

/* click on the page: open that paragraph in the editor with the caret there */
static void view_click(int x, int y, int enter) {
    int i;
    if (!g_editor || !g_doc) {
        return;
    }
    if (g_editing && g_preview_pending) {
        preview_build(); /* the page must show the editor's current paragraphs */
    }
    for (i = 0; i < g_page_rect_count; i++) {
        RECT p = g_page_rects[i];
        OffsetRect(&p, -g_scroll_x, -g_scroll_y);
        if (x >= p.left && x < p.right && y >= p.top && y < p.bottom) {
            int pw, ph;
            size_t section, item;
            uint32_t src;
            long ci;
            view_page_size(i, &pw, &ph);
            if (!hit_test(i, (double)(x - p.left) * pw / (p.right - p.left), (double)(y - p.top) * ph / (p.bottom - p.top),
                          &section, &src, &ci)) {
                return;
            }
            item = item_for_source(section, src);
            if (item == (size_t)-1) {
                return;
            }
            if (!g_editing) {
                if (!enter) {
                    return;
                }
                editor_mode(1);
            }
            if (!editor_commit()) {
                return;
            }
            if (item != g_edit_index) {
                list_select_item(item);
            }
            {
                /* LF index -> edit control index (each LF is CRLF there) */
                wchar_t *w = text_box_text();
                long k = 0, lf = 0;
                if (w) {
                    while (w[k] && lf < ci) {
                        if (w[k] != L'\r') {
                            lf++;
                        }
                        k++;
                    }
                    free(w);
                }
                SendMessageW(g_text, EM_SETSEL, (WPARAM)k, (LPARAM)k);
                SendMessageW(g_text, EM_SCROLLCARET, 0, 0);
            }
            g_page_index = i;
            SetFocus(g_text);
            format_sync();
            status_refresh();
            InvalidateRect(g_view, NULL, FALSE);
            return;
        }
    }
}

/* caret moves in the text box: refresh toolbar state and the page caret */
static LRESULT CALLBACK TextProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT r = CallWindowProcW(g_text_proc, h, msg, wp, lp);
    if (msg == WM_KEYUP || msg == WM_LBUTTONUP || msg == WM_CHAR || (msg == WM_MOUSEMOVE && (wp & MK_LBUTTON))) {
        PostMessageW(g_main, WM_APP_CARET, 0, 0);
    }
    return r;
}

static LRESULT CALLBACK ViewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        view_relayout();
        view_set_scrollbars();
        InvalidateRect(hwnd, NULL, FALSE);
        status_refresh();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        view_paint(hwnd);
        return 0;
    case WM_VSCROLL:
        view_scroll_cmd(SB_VERT, LOWORD(wp));
        return 0;
    case WM_HSCROLL:
        view_scroll_cmd(SB_HORZ, LOWORD(wp));
        return 0;
    case WM_MOUSEWHEEL: {
        int delta = (short)HIWORD(wp);
        if (LOWORD(wp) & MK_CONTROL) {
            int z = view_zoom_percent();
            set_zoom(ZOOM_PERCENT, delta > 0 ? z + 10 : z - 10);
        } else {
            view_scroll_to(g_scroll_x, g_scroll_y - delta * S(120) / WHEEL_DELTA);
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        view_click((short)LOWORD(lp), (short)HIWORD(lp), 0); /* while editing: move there */
        return 0;
    case WM_LBUTTONDBLCLK:
        view_click((short)LOWORD(lp), (short)HIWORD(lp), 1); /* in page view: start editing there */
        return 0;
    case WM_RBUTTONDOWN:
        SetFocus(hwnd);
        return 0;
    case WM_CONTEXTMENU: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        if (x == -1 && y == -1) {
            POINT pt = {S(20), S(20)};
            ClientToScreen(hwnd, &pt);
            x = pt.x;
            y = pt.y;
        }
        quick_menu(g_main, x, y);
        return 0;
    }
    case WM_KEYDOWN: {
        int ctrl = GetKeyState(VK_CONTROL) & 0x8000;
        switch (wp) {
        case VK_PRIOR: goto_page(g_main, g_page_index - 1); break;
        case VK_NEXT:
        case VK_SPACE: goto_page(g_main, g_page_index + 1); break;
        case VK_UP: view_scroll_cmd(SB_VERT, SB_LINEUP); break;
        case VK_DOWN: view_scroll_cmd(SB_VERT, SB_LINEDOWN); break;
        case VK_LEFT: view_scroll_cmd(SB_HORZ, SB_LINEUP); break;
        case VK_RIGHT: view_scroll_cmd(SB_HORZ, SB_LINEDOWN); break;
        case VK_HOME:
            if (ctrl) {
                goto_page(g_main, 0);
            }
            break;
        case VK_END:
            if (ctrl) {
                goto_page(g_main, g_page_count - 1);
            }
            break;
        }
        return 0;
    }
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ======================= status / toolbar state ======================= */

static void status_refresh(void) {
    wchar_t buf[160];
    if (!g_status) {
        return;
    }
    if (g_doc && g_page_count > 0) {
        int at = g_page_index < g_page_count ? g_page_index : g_page_count - 1;
        swprintf(buf, 160, L"%d / %d 쪽", at + 1, g_page_count);
        SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)buf);
        swprintf(buf, 160, L"%zu / %zu 구역", g_pages[at].section + 1, g_doc->section_count);
        SendMessageW(g_status, SB_SETTEXTW, 1, (LPARAM)buf);
    } else {
        SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)L"문서 없음");
        SendMessageW(g_status, SB_SETTEXTW, 1, (LPARAM)L"");
    }
    if (g_editing && g_editor && g_edit_index != (size_t)-1) {
        size_t i, n = 0, at = 0;
        for (i = 0; i < hp_editor_count(g_editor); i++) {
            if (!hp_editor_is_deleted(g_editor, i)) {
                n++;
                if (i == g_edit_index) {
                    at = n;
                }
            }
        }
        swprintf(buf, 160, L"문단 %zu / %zu", at, n);
    } else {
        buf[0] = 0;
    }
    SendMessageW(g_status, SB_SETTEXTW, 2, (LPARAM)buf);
    swprintf(buf, 160, L"%d%%", view_zoom_percent());
    SendMessageW(g_status, SB_SETTEXTW, 3, (LPARAM)buf);
    SendMessageW(g_status, SB_SETTEXTW, 4,
                 (LPARAM)(!g_doc         ? L""
                          : g_read_only  ? L"HWP 배포용 · 읽기 전용"
                          : g_source_hwp ? L"HWP → HWPX로 저장"
                          : g_path[0]    ? L"HWPX"
                                         : L"새 문서 (HWPX)"));
    if (g_cb_zoom && GetFocus() != g_cb_zoom) {
        HWND edit = NULL;
        COMBOBOXINFO info;
        ZeroMemory(&info, sizeof(info));
        info.cbSize = sizeof(info);
        if (GetComboBoxInfo(g_cb_zoom, &info)) {
            edit = info.hwndItem;
        }
        if (!edit || GetFocus() != edit) {
            if (g_zoom_mode == ZOOM_FIT_PAGE) {
                wcscpy(buf, L"쪽 맞춤");
            } else if (g_zoom_mode == ZOOM_FIT_WIDTH) {
                wcscpy(buf, L"폭 맞춤");
            } else {
                swprintf(buf, 160, L"%d%%", g_zoom);
            }
            SetWindowTextW(g_cb_zoom, buf);
        }
    }
    if (g_tb_main) {
        SendMessageW(g_tb_main, TB_ENABLEBUTTON, ID_BTN_PREV, g_page_index > 0);
        SendMessageW(g_tb_main, TB_ENABLEBUTTON, ID_BTN_NEXT, g_page_index + 1 < g_page_count);
        SendMessageW(g_tb_main, TB_CHECKBUTTON, IDM_ZOOM_PAGE, g_zoom_mode == ZOOM_FIT_PAGE);
        SendMessageW(g_tb_main, TB_CHECKBUTTON, IDM_ZOOM_WIDTH, g_zoom_mode == ZOOM_FIT_WIDTH);
        SendMessageW(g_tb_main, TB_CHECKBUTTON, IDM_TWO_PAGES, g_two_pages);
        SendMessageW(g_tb_main, TB_CHECKBUTTON, IDM_EDIT_MODE, g_editing);
        SendMessageW(g_tb_main, TB_ENABLEBUTTON, IDM_EDIT_MODE, g_editor != NULL);
        SendMessageW(g_tb_main, TB_ENABLEBUTTON, IDM_FILE_SAVE, g_editor != NULL);
        SendMessageW(g_tb_main, TB_ENABLEBUTTON, IDM_PRINT, g_doc && g_page_count > 0);
        SendMessageW(g_tb_main, TB_ENABLEBUTTON, IDM_CHAR_SHAPE, g_editor != NULL);
        SendMessageW(g_tb_main, TB_ENABLEBUTTON, IDM_PARA_SHAPE, g_editor != NULL);
        SendMessageW(g_tb_main, TB_ENABLEBUTTON, IDM_FIND, g_editor != NULL);
        SendMessageW(g_tb_main, TB_ENABLEBUTTON, IDM_PREVIEW, g_editor != NULL);
        SendMessageW(g_tb_main, TB_ENABLEBUTTON, IDM_CUT, g_editing);
        SendMessageW(g_tb_main, TB_ENABLEBUTTON, IDM_PASTE, g_editing);
        SendMessageW(g_tb_main, TB_ENABLEBUTTON, IDM_UNDO, g_editing);
        InvalidateRect(g_tb_main, NULL, FALSE);
    }
}

static wchar_t *trim_number(wchar_t *s) {
    size_t n = wcslen(s);
    while (n > 0 && wcschr(s, L'.') && s[n - 1] == L'0') {
        s[--n] = 0;
    }
    if (n > 0 && s[n - 1] == L'.') {
        s[--n] = 0;
    }
    return s;
}

static void format_sync(void) {
    hp_char_format cf;
    hp_para_format pf;
    int ok = g_editor && g_edit_index != (size_t)-1 &&
             hp_editor_get_format(g_editor, g_edit_index, &cf, &pf) == HP_OK && current_char_format(&cf);
    static const int align_ids[5] = {IDM_ALIGN_LEFT, IDM_ALIGN_CENTER, IDM_ALIGN_RIGHT, IDM_ALIGN_JUSTIFY,
                                     IDM_ALIGN_DISTRIBUTE};
    static const int fmt_ids[] = {IDM_BOLD, IDM_ITALIC, IDM_UNDERLINE, IDM_STRIKE, IDM_COLOR,
                                  IDM_ALIGN_JUSTIFY, IDM_ALIGN_LEFT, IDM_ALIGN_CENTER, IDM_ALIGN_RIGHT,
                                  IDM_ALIGN_DISTRIBUTE};
    size_t i;
    wchar_t buf[64];
    if (!g_tb_fmt) {
        return;
    }
    for (i = 0; i < sizeof(fmt_ids) / sizeof(fmt_ids[0]); i++) {
        SendMessageW(g_tb_fmt, TB_ENABLEBUTTON, fmt_ids[i], g_editor != NULL);
    }
    EnableWindow(g_cb_font, g_editor != NULL);
    EnableWindow(g_cb_size, g_editor != NULL);
    EnableWindow(g_cb_spacing, g_editor != NULL);
    if (!ok) {
        SetWindowTextW(g_cb_font, L"");
        SetWindowTextW(g_cb_size, L"");
        SetWindowTextW(g_cb_spacing, L"");
        for (i = 0; i < sizeof(fmt_ids) / sizeof(fmt_ids[0]); i++) {
            SendMessageW(g_tb_fmt, TB_CHECKBUTTON, fmt_ids[i], FALSE);
        }
        return;
    }
    {
        wchar_t *face = utf8_to_wide(cf.face);
        SetWindowTextW(g_cb_font, face ? face : L"");
        free(face);
    }
    swprintf(buf, 64, L"%.1f", cf.size_hwp / 100.0);
    SetWindowTextW(g_cb_size, trim_number(buf));
    if (pf.line_spacing_percent > 0) {
        swprintf(buf, 64, L"%d %%", pf.line_spacing_percent);
    } else {
        wcscpy(buf, L"고정값");
    }
    SetWindowTextW(g_cb_spacing, buf);
    SendMessageW(g_tb_fmt, TB_CHECKBUTTON, IDM_BOLD, cf.bold);
    SendMessageW(g_tb_fmt, TB_CHECKBUTTON, IDM_ITALIC, cf.italic);
    SendMessageW(g_tb_fmt, TB_CHECKBUTTON, IDM_UNDERLINE, cf.underline);
    SendMessageW(g_tb_fmt, TB_CHECKBUTTON, IDM_STRIKE, cf.strikeout);
    for (i = 0; i < 5; i++) {
        SendMessageW(g_tb_fmt, TB_CHECKBUTTON, align_ids[i], pf.align == (int)i);
    }
    InvalidateRect(g_tb_fmt, NULL, FALSE);
}

/* ======================= paragraph selection ======================= */

static void list_select_item(size_t item) {
    int rows = (int)SendMessageW(g_paralist, LB_GETCOUNT, 0, 0), r;
    for (r = 0; r < rows; r++) {
        if ((size_t)SendMessageW(g_paralist, LB_GETITEMDATA, r, 0) == item) {
            SendMessageW(g_paralist, LB_SETSEL, FALSE, -1);
            SendMessageW(g_paralist, LB_SETSEL, TRUE, r);
            SendMessageW(g_paralist, LB_SETCARETINDEX, r, FALSE);
            editor_select(item);
            format_sync();
            status_refresh();
            return;
        }
    }
}

/* the selected paragraphs (list selection, else the current one); caller frees */
static size_t selected_items(size_t **out) {
    int n = g_paralist ? (int)SendMessageW(g_paralist, LB_GETSELCOUNT, 0, 0) : 0, i;
    size_t *v;
    *out = NULL;
    if (n <= 0) {
        if (g_edit_index == (size_t)-1) {
            return 0;
        }
        v = (size_t *)malloc(sizeof(size_t));
        if (!v) {
            return 0;
        }
        v[0] = g_edit_index;
        *out = v;
        return 1;
    }
    {
        int *rows = (int *)malloc((size_t)n * sizeof(int));
        v = (size_t *)malloc((size_t)n * sizeof(size_t));
        if (!rows || !v) {
            free(rows);
            free(v);
            return 0;
        }
        n = (int)SendMessageW(g_paralist, LB_GETSELITEMS, (WPARAM)n, (LPARAM)rows);
        for (i = 0; i < n; i++) {
            v[i] = (size_t)SendMessageW(g_paralist, LB_GETITEMDATA, rows[i], 0);
        }
        free(rows);
    }
    *out = v;
    return (size_t)(n > 0 ? n : 0);
}

/* ======================= live preview ======================= */

static int preview_build(void) {
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    char *s;
    hp_document *doc = NULL;
    int ok = 0;
    if (!g_editor || !editor_commit()) {
        return 0;
    }
    g_preview_pending = 0;
    if (!GetTempPathW(MAX_PATH, dir) || !GetTempFileNameW(dir, L"hpe", 0, path)) {
        return 0;
    }
    s = wide_to_utf8(path);
    if (s && hp_editor_save(g_editor, s) == HP_OK && hp_hwpx_read(s, &doc) == HP_OK) {
        int page = g_page_index;
        free_doc();
        g_doc = doc;
        rebuild_flat();
        rebuild_pages(g_main);
        g_page_index = page < g_page_count ? page : g_page_count - 1;
        if (g_page_index < 0) {
            g_page_index = 0;
        }
        g_keep_scroll = 1;
        update_status();
        g_keep_scroll = 0;
        ok = 1;
    }
    free(s);
    DeleteFileW(path);
    return ok;
}

static void schedule_preview(int delay_ms) {
    g_preview_pending = 1;
    if (g_main && g_editing) {
        SetTimer(g_main, TIMER_PREVIEW, (UINT)delay_ms, NULL);
    }
}

/* ======================= formatting commands ======================= */

static int format_target_ready(void) {
    if (!g_editor) {
        MessageBoxW(g_main, L"배포용 문서는 모양을 바꿀 수 없습니다.", L"한글패드", MB_ICONINFORMATION);
        return 0;
    }
    if (!g_editing) {
        editor_mode(1);
    }
    return editor_commit();
}

static void format_done(size_t applied, size_t failed) {
    if (applied) {
        g_dirty = 1;
        editor_title();
        preview_build();
    }
    format_sync();
    if (failed) {
        status_message(L"일부 문단의 모양을 바꾸지 못했습니다.");
    }
}

static void apply_formats(const hp_char_format *cf, const hp_para_format *pf) {
    size_t *items, n, i, ok = 0, bad = 0, a = 0, z = 0;
    int range;
    if (!format_target_ready()) {
        return;
    }
    n = selected_items(&items);
    /* a selection inside the text box formats just those characters */
    range = cf && cf->fields && n == 1 && items[0] == g_edit_index && text_selection_bytes(&a, &z);
    for (i = 0; i < n; i++) {
        int good = 1;
        if (cf && cf->fields) {
            hp_status st = range ? hp_editor_apply_char_format_range(g_editor, items[i], a, z, cf)
                                 : hp_editor_apply_char_format(g_editor, items[i], cf);
            if (st == HP_ERR_UNSUPPORTED && range) {
                status_message(L"표·그림 등 개체가 있는 문단은 문단 전체에만 글자 모양을 줄 수 있습니다.");
                good = 0;
            } else if (st != HP_OK) {
                good = 0;
            }
        }
        if (pf && pf->fields && hp_editor_apply_para_format(g_editor, items[i], pf) != HP_OK) {
            good = 0;
        }
        if (good) {
            ok++;
        } else {
            bad++;
        }
    }
    free(items);
    format_done(ok, bad);
    if (range && g_text) {
        SetFocus(g_text);
    }
}

/* toggles follow the current paragraph */
static void toggle_char(unsigned field) {
    hp_char_format cur, cf;
    if (!format_target_ready() || g_edit_index == (size_t)-1 || !current_char_format(&cur)) {
        return;
    }
    ZeroMemory(&cf, sizeof(cf));
    cf.fields = field;
    cf.bold = !cur.bold;
    cf.italic = !cur.italic;
    cf.underline = !cur.underline;
    cf.strikeout = !cur.strikeout;
    apply_formats(&cf, NULL);
}

static void set_align(int align) {
    hp_para_format pf;
    ZeroMemory(&pf, sizeof(pf));
    pf.fields = HP_PF_ALIGN;
    pf.align = align;
    apply_formats(NULL, &pf);
}

static void set_color(COLORREF c) {
    hp_char_format cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.fields = HP_CF_COLOR;
    cf.color_rgb = ((uint32_t)GetRValue(c) << 16) | ((uint32_t)GetGValue(c) << 8) | GetBValue(c);
    apply_formats(&cf, NULL);
}

/* relative steps: each paragraph from its own value */
static void step_format(int size_step, int spacing_step) {
    size_t *items, n, i, ok = 0, bad = 0, a = 0, z = 0;
    if (!format_target_ready()) {
        return;
    }
    if (size_step && text_selection_bytes(&a, &z)) {
        hp_char_format cur, cf;
        if (hp_editor_get_format_at(g_editor, g_edit_index, a, &cur) == HP_OK) {
            long v = (long)cur.size_hwp + size_step * 100;
            ZeroMemory(&cf, sizeof(cf));
            cf.fields = HP_CF_SIZE;
            cf.size_hwp = (uint32_t)(v < 100 ? 100 : v > 409600 ? 409600 : v);
            apply_formats(&cf, NULL);
        }
        return;
    }
    n = selected_items(&items);
    for (i = 0; i < n; i++) {
        hp_char_format cur, cf;
        hp_para_format pcur, pf;
        hp_status st = HP_ERR_INVALID;
        if (hp_editor_get_format(g_editor, items[i], &cur, &pcur) != HP_OK) {
            bad++;
            continue;
        }
        ZeroMemory(&cf, sizeof(cf));
        ZeroMemory(&pf, sizeof(pf));
        if (size_step) {
            long v = (long)cur.size_hwp + size_step * 100;
            cf.fields = HP_CF_SIZE;
            cf.size_hwp = (uint32_t)(v < 100 ? 100 : v > 409600 ? 409600 : v);
            st = hp_editor_apply_char_format(g_editor, items[i], &cf);
        } else {
            int v = (pcur.line_spacing_percent > 0 ? pcur.line_spacing_percent : 160) + spacing_step;
            pf.fields = HP_PF_LINE_SPACING;
            pf.line_spacing_percent = v < 50 ? 50 : v > 500 ? 500 : v;
            st = hp_editor_apply_para_format(g_editor, items[i], &pf);
        }
        if (st == HP_OK) {
            ok++;
        } else {
            bad++;
        }
    }
    free(items);
    format_done(ok, bad);
}

static void pick_color(void) {
    static COLORREF custom[16];
    CHOOSECOLORW cc;
    hp_char_format cur;
    ZeroMemory(&cc, sizeof(cc));
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = g_main;
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (current_char_format(&cur)) {
        cc.rgbResult = RGB((cur.color_rgb >> 16) & 255, (cur.color_rgb >> 8) & 255, cur.color_rgb & 255);
    }
    if (ChooseColorW(&cc)) {
        set_color(cc.rgbResult);
    }
}

/* combo text applied (Enter or list pick) */
static void combo_apply(int id, int from_list) {
    HWND cb = combo_for(id);
    wchar_t text[LF_FACESIZE + 8];
    int sel;
    if (!cb) {
        return;
    }
    sel = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
    if (from_list && sel != CB_ERR && SendMessageW(cb, CB_GETLBTEXTLEN, (WPARAM)sel, 0) < LF_FACESIZE + 8) {
        SendMessageW(cb, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)text);
    } else {
        GetWindowTextW(cb, text, LF_FACESIZE + 8);
    }
    if (id == ID_CB_ZOOM) {
        if (wcsstr(text, L"쪽")) {
            set_zoom(ZOOM_FIT_PAGE, g_zoom);
        } else if (wcsstr(text, L"폭")) {
            set_zoom(ZOOM_FIT_WIDTH, g_zoom);
        } else {
            int z = _wtoi(text);
            if (z > 0) {
                set_zoom(ZOOM_PERCENT, z);
            }
        }
        SetFocus(g_view);
        return;
    }
    if (id == ID_CB_FONT && text[0]) {
        hp_char_format cf;
        char *u = wide_to_utf8(text);
        ZeroMemory(&cf, sizeof(cf));
        cf.fields = HP_CF_FACE;
        if (u && strlen(u) < sizeof(cf.face)) {
            strcpy(cf.face, u);
            apply_formats(&cf, NULL);
        }
        free(u);
    } else if (id == ID_CB_SIZE) {
        double pt = _wtof(text);
        if (pt >= 1 && pt <= 4096) {
            hp_char_format cf;
            ZeroMemory(&cf, sizeof(cf));
            cf.fields = HP_CF_SIZE;
            cf.size_hwp = (uint32_t)(pt * 100 + 0.5);
            apply_formats(&cf, NULL);
        } else {
            format_sync();
        }
    } else if (id == ID_CB_SPACING) {
        int v = _wtoi(text);
        if (v >= 10 && v <= 1000) {
            hp_para_format pf;
            ZeroMemory(&pf, sizeof(pf));
            pf.fields = HP_PF_LINE_SPACING;
            pf.line_spacing_percent = v;
            apply_formats(NULL, &pf);
        } else {
            format_sync();
        }
    }
    if (g_editing) {
        SetFocus(g_text);
    }
}

/* ======================= in-memory dialog templates ======================= */

typedef struct {
    WORD *p;
    size_t n, cap;
    size_t count_at;
    int failed;
} dlg_t;

static void dw(dlg_t *d, WORD w) {
    if (d->n == d->cap) {
        size_t cap = d->cap ? d->cap * 2 : 512;
        WORD *next = (WORD *)realloc(d->p, cap * sizeof(WORD));
        if (!next) {
            d->failed = 1;
            return;
        }
        d->p = next;
        d->cap = cap;
    }
    d->p[d->n++] = w;
}

static void dw_dword(dlg_t *d, DWORD v) {
    dw(d, LOWORD(v));
    dw(d, HIWORD(v));
}

static void dw_str(dlg_t *d, const wchar_t *s) {
    do {
        dw(d, (WORD)*s);
    } while (*s++);
}

static void dlg_begin(dlg_t *d, const wchar_t *title, short cx, short cy) {
    ZeroMemory(d, sizeof(*d));
    dw_dword(d, DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    dw_dword(d, 0);
    d->count_at = d->n;
    dw(d, 0);
    dw(d, 0);
    dw(d, 0);
    dw(d, (WORD)cx);
    dw(d, (WORD)cy);
    dw(d, 0); /* menu */
    dw(d, 0); /* class */
    dw_str(d, title);
    dw(d, 9);
    dw_str(d, L"Malgun Gothic");
}

/* cls: 0x80 button, 0x81 edit, 0x82 static, 0x85 combobox */
static void dlg_item(dlg_t *d, WORD cls, const wchar_t *text, DWORD style, short x, short y, short cx, short cy,
                     WORD id) {
    if (d->n & 1) {
        dw(d, 0);
    }
    dw_dword(d, style | WS_CHILD | WS_VISIBLE);
    dw_dword(d, 0);
    dw(d, (WORD)x);
    dw(d, (WORD)y);
    dw(d, (WORD)cx);
    dw(d, (WORD)cy);
    dw(d, id);
    dw(d, 0xFFFF);
    dw(d, cls);
    dw_str(d, text);
    dw(d, 0);
    if (!d->failed) {
        d->p[d->count_at]++;
    }
}

static INT_PTR dlg_run(dlg_t *d, DLGPROC proc, LPARAM param) {
    INT_PTR r = -1;
    if (!d->failed) {
        r = DialogBoxIndirectParamW(GetModuleHandleW(NULL), (LPCDLGTEMPLATEW)d->p, g_main, proc, param);
    }
    free(d->p);
    return r;
}

#define DLG_BUTTON 0x80
#define DLG_EDIT 0x81
#define DLG_STATIC 0x82
#define DLG_COMBO 0x85

static void dlg_set_number(HWND dlg, int id, double v) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%.1f", v);
    SetDlgItemTextW(dlg, id, trim_number(buf));
}

static int dlg_get_number(HWND dlg, int id, double lo, double hi, double *out) {
    wchar_t buf[32], *end;
    double v;
    GetDlgItemTextW(dlg, id, buf, 32);
    v = wcstod(buf, &end);
    while (*end == L' ') {
        end++;
    }
    if (end == buf || (*end && wcscmp(end, L"pt") != 0 && wcscmp(end, L"%") != 0) || v < lo || v > hi) {
        wchar_t msg[128];
        swprintf(msg, 128, L"%g에서 %g 사이의 값을 입력하세요.", lo, hi);
        MessageBoxW(dlg, msg, L"한글패드", MB_ICONWARNING);
        SetFocus(GetDlgItem(dlg, id));
        SendDlgItemMessageW(dlg, id, EM_SETSEL, 0, -1);
        return 0;
    }
    *out = v;
    return 1;
}

/* ---- 글자 모양 ---- */

#define IDC_CF_FACE 101
#define IDC_CF_SIZE 102
#define IDC_CF_BOLD 103
#define IDC_CF_ITALIC 104
#define IDC_CF_UNDERLINE 105
#define IDC_CF_STRIKE 106
#define IDC_CF_COLOR 107
#define IDC_CF_SWATCH 108
#define IDC_CF_SAMPLE 109

typedef struct {
    hp_char_format start, result;
    COLORREF color;
    HBRUSH swatch;
} char_dlg;

static void char_dlg_read(HWND dlg, char_dlg *s, hp_char_format *f) {
    wchar_t face[LF_FACESIZE + 8], size[32];
    char *u;
    *f = s->start;
    GetDlgItemTextW(dlg, IDC_CF_FACE, face, LF_FACESIZE + 8);
    u = wide_to_utf8(face);
    if (u && strlen(u) < sizeof(f->face)) {
        strcpy(f->face, u);
    }
    free(u);
    GetDlgItemTextW(dlg, IDC_CF_SIZE, size, 32);
    if (_wtof(size) >= 1 && _wtof(size) <= 4096) {
        f->size_hwp = (uint32_t)(_wtof(size) * 100 + 0.5);
    }
    f->bold = IsDlgButtonChecked(dlg, IDC_CF_BOLD) == BST_CHECKED;
    f->italic = IsDlgButtonChecked(dlg, IDC_CF_ITALIC) == BST_CHECKED;
    f->underline = IsDlgButtonChecked(dlg, IDC_CF_UNDERLINE) == BST_CHECKED;
    f->strikeout = IsDlgButtonChecked(dlg, IDC_CF_STRIKE) == BST_CHECKED;
    f->color_rgb = ((uint32_t)GetRValue(s->color) << 16) | ((uint32_t)GetGValue(s->color) << 8) | GetBValue(s->color);
}

static INT_PTR CALLBACK CharDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    char_dlg *s = (char_dlg *)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg) {
    case WM_INITDIALOG: {
        wchar_t *face;
        s = (char_dlg *)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)s);
        fill_font_combo(GetDlgItem(dlg, IDC_CF_FACE));
        face = utf8_to_wide(s->start.face);
        SetDlgItemTextW(dlg, IDC_CF_FACE, face ? face : L"");
        free(face);
        dlg_set_number(dlg, IDC_CF_SIZE, s->start.size_hwp / 100.0);
        CheckDlgButton(dlg, IDC_CF_BOLD, s->start.bold ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_CF_ITALIC, s->start.italic ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_CF_UNDERLINE, s->start.underline ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_CF_STRIKE, s->start.strikeout ? BST_CHECKED : BST_UNCHECKED);
        s->color = RGB((s->start.color_rgb >> 16) & 255, (s->start.color_rgb >> 8) & 255, s->start.color_rgb & 255);
        s->swatch = CreateSolidBrush(s->color);
        return TRUE;
    }
    case WM_CTLCOLORSTATIC:
        if (s && (HWND)lp == GetDlgItem(dlg, IDC_CF_SWATCH)) {
            return (INT_PTR)s->swatch;
        }
        break;
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *di = (const DRAWITEMSTRUCT *)lp;
        if (s && di->CtlID == IDC_CF_SAMPLE) {
            hp_char_format f;
            wchar_t *face;
            HFONT font, old;
            double pt;
            RECT r = di->rcItem;
            char_dlg_read(dlg, s, &f);
            {
                wchar_t mapped[LF_FACESIZE];
                face = utf8_to_wide(f.face);
                if (face && !font_installed(face)) {
                    /* same fallback idea as the page: an installed face that has the glyphs */
                    free(face);
                    face = map_face(di->hDC, f.face, mapped, LF_FACESIZE) && font_installed(mapped) ? _wcsdup(mapped)
                                                                                                  : _wcsdup(L"Malgun Gothic");
                }
            }
            pt = f.size_hwp / 100.0;
            if (pt > 28) {
                pt = 28;
            }
            FillRect(di->hDC, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
            FrameRect(di->hDC, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
            font = CreateFontW(-(int)(pt * GetDeviceCaps(di->hDC, LOGPIXELSY) / 72.0 + 0.5), 0, 0, 0,
                               f.bold ? FW_BOLD : FW_NORMAL, f.italic, f.underline, f.strikeout, DEFAULT_CHARSET, 0, 0,
                               CLEARTYPE_QUALITY, 0, face ? face : L"Malgun Gothic");
            old = (HFONT)SelectObject(di->hDC, font);
            SetBkMode(di->hDC, TRANSPARENT);
            SetTextColor(di->hDC, s->color);
            DrawTextW(di->hDC, L"한글패드 가나다 AaBb 123", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(di->hDC, old);
            DeleteObject(font);
            free(face);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_CF_COLOR: {
            static COLORREF custom[16];
            CHOOSECOLORW cc;
            ZeroMemory(&cc, sizeof(cc));
            cc.lStructSize = sizeof(cc);
            cc.hwndOwner = dlg;
            cc.lpCustColors = custom;
            cc.rgbResult = s->color;
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;
            if (ChooseColorW(&cc)) {
                s->color = cc.rgbResult;
                DeleteObject(s->swatch);
                s->swatch = CreateSolidBrush(s->color);
                InvalidateRect(GetDlgItem(dlg, IDC_CF_SWATCH), NULL, TRUE);
                InvalidateRect(GetDlgItem(dlg, IDC_CF_SAMPLE), NULL, TRUE);
            }
            return TRUE;
        }
        case IDC_CF_FACE:
        case IDC_CF_SIZE:
        case IDC_CF_BOLD:
        case IDC_CF_ITALIC:
        case IDC_CF_UNDERLINE:
        case IDC_CF_STRIKE:
            if (s) {
                InvalidateRect(GetDlgItem(dlg, IDC_CF_SAMPLE), NULL, TRUE);
            }
            break;
        case IDOK: {
            double pt;
            wchar_t face[LF_FACESIZE + 8];
            GetDlgItemTextW(dlg, IDC_CF_FACE, face, LF_FACESIZE + 8);
            if (!face[0] || wcspbrk(face, L"<>&\"")) {
                MessageBoxW(dlg, L"글꼴 이름을 확인하세요.", L"한글패드", MB_ICONWARNING);
                return TRUE;
            }
            if (!dlg_get_number(dlg, IDC_CF_SIZE, 1, 4096, &pt)) {
                return TRUE;
            }
            char_dlg_read(dlg, s, &s->result);
            s->result.size_hwp = (uint32_t)(pt * 100 + 0.5);
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_DESTROY:
        if (s && s->swatch) {
            DeleteObject(s->swatch);
            s->swatch = NULL;
        }
        break;
    }
    return FALSE;
}

static void char_shape_dialog(void) {
    dlg_t d;
    char_dlg s;
    hp_char_format change;
    if (!format_target_ready() || g_edit_index == (size_t)-1) {
        return;
    }
    ZeroMemory(&s, sizeof(s));
    {
        size_t a = 0, z = 0;
        hp_status st = text_selection_bytes(&a, &z) ? hp_editor_get_format_at(g_editor, g_edit_index, a, &s.start)
                                                    : hp_editor_get_format(g_editor, g_edit_index, &s.start, NULL);
        if (st != HP_OK) {
            return;
        }
    }
    dlg_begin(&d, L"글자 모양", 270, 196);
    dlg_item(&d, DLG_STATIC, L"글꼴(&F):", SS_LEFT, 10, 12, 44, 10, (WORD)-1);
    dlg_item(&d, DLG_COMBO, L"", CBS_DROPDOWN | CBS_SORT | WS_VSCROLL | WS_TABSTOP | CBS_AUTOHSCROLL, 58, 10, 140,
             160, IDC_CF_FACE);
    dlg_item(&d, DLG_STATIC, L"크기(&Z):", SS_LEFT, 10, 32, 44, 10, (WORD)-1);
    dlg_item(&d, DLG_EDIT, L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 58, 30, 40, 13, IDC_CF_SIZE);
    dlg_item(&d, DLG_STATIC, L"pt", SS_LEFT, 102, 32, 20, 10, (WORD)-1);
    dlg_item(&d, DLG_BUTTON, L"속성", BS_GROUPBOX, 8, 50, 254, 48, (WORD)-1);
    dlg_item(&d, DLG_BUTTON, L"진하게(&B)", BS_AUTOCHECKBOX | WS_TABSTOP, 16, 63, 56, 12, IDC_CF_BOLD);
    dlg_item(&d, DLG_BUTTON, L"기울임(&I)", BS_AUTOCHECKBOX | WS_TABSTOP, 76, 63, 56, 12, IDC_CF_ITALIC);
    dlg_item(&d, DLG_BUTTON, L"밑줄(&U)", BS_AUTOCHECKBOX | WS_TABSTOP, 136, 63, 52, 12, IDC_CF_UNDERLINE);
    dlg_item(&d, DLG_BUTTON, L"취소선(&K)", BS_AUTOCHECKBOX | WS_TABSTOP, 192, 63, 60, 12, IDC_CF_STRIKE);
    dlg_item(&d, DLG_STATIC, L"글자 색:", SS_LEFT, 16, 81, 36, 10, (WORD)-1);
    dlg_item(&d, DLG_STATIC, L"", SS_LEFT | WS_BORDER, 56, 79, 30, 12, IDC_CF_SWATCH);
    dlg_item(&d, DLG_BUTTON, L"색 고르기(&C)...", BS_PUSHBUTTON | WS_TABSTOP, 92, 78, 64, 14, IDC_CF_COLOR);
    dlg_item(&d, DLG_BUTTON, L"미리 보기", BS_GROUPBOX, 8, 102, 254, 64, (WORD)-1);
    dlg_item(&d, DLG_STATIC, L"", SS_OWNERDRAW, 16, 114, 238, 44, IDC_CF_SAMPLE);
    dlg_item(&d, DLG_BUTTON, L"설정(&D)", BS_DEFPUSHBUTTON | WS_TABSTOP, 150, 174, 54, 15, IDOK);
    dlg_item(&d, DLG_BUTTON, L"취소", BS_PUSHBUTTON | WS_TABSTOP, 208, 174, 54, 15, IDCANCEL);
    if (dlg_run(&d, CharDlgProc, (LPARAM)&s) != IDOK) {
        return;
    }
    /* only what the user changed (keeps mixed values across a selection) */
    ZeroMemory(&change, sizeof(change));
    change = s.result;
    change.fields = 0;
    if (strcmp(s.result.face, s.start.face) != 0) {
        change.fields |= HP_CF_FACE;
    }
    if (s.result.size_hwp != s.start.size_hwp) {
        change.fields |= HP_CF_SIZE;
    }
    if (s.result.bold != s.start.bold) {
        change.fields |= HP_CF_BOLD;
    }
    if (s.result.italic != s.start.italic) {
        change.fields |= HP_CF_ITALIC;
    }
    if (s.result.underline != s.start.underline) {
        change.fields |= HP_CF_UNDERLINE;
    }
    if (s.result.strikeout != s.start.strikeout) {
        change.fields |= HP_CF_STRIKEOUT;
    }
    if (s.result.color_rgb != s.start.color_rgb) {
        change.fields |= HP_CF_COLOR;
    }
    if (change.fields) {
        apply_formats(&change, NULL);
    }
}

/* ---- 문단 모양 ---- */

#define IDC_PF_ALIGN0 201 /* 201..205: justify, left, center, right, distribute */
#define IDC_PF_LEFT 206
#define IDC_PF_RIGHT 207
#define IDC_PF_FIRST_NORMAL 208
#define IDC_PF_FIRST_INDENT 209
#define IDC_PF_FIRST_HANG 210
#define IDC_PF_FIRST_VALUE 211
#define IDC_PF_SPACING 212
#define IDC_PF_BEFORE 213
#define IDC_PF_AFTER 214

static const int DLG_ALIGNS[5] = {HP_ALIGN_JUSTIFY, HP_ALIGN_LEFT, HP_ALIGN_CENTER, HP_ALIGN_RIGHT, HP_ALIGN_DISTRIBUTE};

typedef struct {
    hp_para_format start, result;
} para_dlg;

static INT_PTR CALLBACK ParaDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    para_dlg *s = (para_dlg *)GetWindowLongPtrW(dlg, DWLP_USER);
    int i;
    switch (msg) {
    case WM_INITDIALOG:
        s = (para_dlg *)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)s);
        for (i = 0; i < 5; i++) {
            CheckDlgButton(dlg, IDC_PF_ALIGN0 + i, s->start.align == DLG_ALIGNS[i] ? BST_CHECKED : BST_UNCHECKED);
        }
        dlg_set_number(dlg, IDC_PF_LEFT, s->start.left_hwp / 100.0);
        dlg_set_number(dlg, IDC_PF_RIGHT, s->start.right_hwp / 100.0);
        CheckDlgButton(dlg, s->start.indent_hwp > 0   ? IDC_PF_FIRST_INDENT
                            : s->start.indent_hwp < 0 ? IDC_PF_FIRST_HANG
                                                      : IDC_PF_FIRST_NORMAL,
                       BST_CHECKED);
        dlg_set_number(dlg, IDC_PF_FIRST_VALUE, (s->start.indent_hwp < 0 ? -s->start.indent_hwp : s->start.indent_hwp) / 100.0);
        EnableWindow(GetDlgItem(dlg, IDC_PF_FIRST_VALUE), s->start.indent_hwp != 0);
        if (s->start.line_spacing_percent > 0) {
            SetDlgItemInt(dlg, IDC_PF_SPACING, (UINT)s->start.line_spacing_percent, FALSE);
        } else {
            SetDlgItemTextW(dlg, IDC_PF_SPACING, L"");
        }
        dlg_set_number(dlg, IDC_PF_BEFORE, s->start.before_hwp / 100.0);
        dlg_set_number(dlg, IDC_PF_AFTER, s->start.after_hwp / 100.0);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_PF_FIRST_NORMAL:
        case IDC_PF_FIRST_INDENT:
        case IDC_PF_FIRST_HANG:
            EnableWindow(GetDlgItem(dlg, IDC_PF_FIRST_VALUE), LOWORD(wp) != IDC_PF_FIRST_NORMAL);
            return TRUE;
        case IDOK: {
            double left, right, first = 0, before, after, spacing = 0;
            wchar_t sp[16];
            if (!dlg_get_number(dlg, IDC_PF_LEFT, -1000, 1000, &left) ||
                !dlg_get_number(dlg, IDC_PF_RIGHT, -1000, 1000, &right) ||
                (IsDlgButtonChecked(dlg, IDC_PF_FIRST_NORMAL) != BST_CHECKED &&
                 !dlg_get_number(dlg, IDC_PF_FIRST_VALUE, 0, 1000, &first)) ||
                !dlg_get_number(dlg, IDC_PF_BEFORE, 0, 1000, &before) ||
                !dlg_get_number(dlg, IDC_PF_AFTER, 0, 1000, &after)) {
                return TRUE;
            }
            GetDlgItemTextW(dlg, IDC_PF_SPACING, sp, 16);
            if (sp[0] && !dlg_get_number(dlg, IDC_PF_SPACING, 10, 1000, &spacing)) {
                return TRUE;
            }
            s->result = s->start;
            for (i = 0; i < 5; i++) {
                if (IsDlgButtonChecked(dlg, IDC_PF_ALIGN0 + i) == BST_CHECKED) {
                    s->result.align = DLG_ALIGNS[i];
                }
            }
            s->result.left_hwp = (int32_t)(left * 100 + (left < 0 ? -0.5 : 0.5));
            s->result.right_hwp = (int32_t)(right * 100 + (right < 0 ? -0.5 : 0.5));
            s->result.indent_hwp = (int32_t)(first * 100 + 0.5);
            if (IsDlgButtonChecked(dlg, IDC_PF_FIRST_HANG) == BST_CHECKED) {
                s->result.indent_hwp = -s->result.indent_hwp;
            } else if (IsDlgButtonChecked(dlg, IDC_PF_FIRST_NORMAL) == BST_CHECKED) {
                s->result.indent_hwp = 0;
            }
            s->result.before_hwp = (int32_t)(before * 100 + 0.5);
            s->result.after_hwp = (int32_t)(after * 100 + 0.5);
            s->result.line_spacing_percent = sp[0] ? (int)(spacing + 0.5) : s->start.line_spacing_percent;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void para_shape_dialog(void) {
    dlg_t d;
    para_dlg s;
    hp_para_format change;
    static const wchar_t *const align_names[5] = {L"양쪽(&J)", L"왼쪽(&L)", L"가운데(&C)", L"오른쪽(&R)", L"배분(&D)"};
    int i;
    if (!format_target_ready() || g_edit_index == (size_t)-1) {
        return;
    }
    ZeroMemory(&s, sizeof(s));
    if (hp_editor_get_format(g_editor, g_edit_index, NULL, &s.start) != HP_OK) {
        return;
    }
    dlg_begin(&d, L"문단 모양", 270, 198);
    dlg_item(&d, DLG_BUTTON, L"정렬 방식", BS_GROUPBOX, 8, 6, 254, 32, (WORD)-1);
    for (i = 0; i < 5; i++) {
        dlg_item(&d, DLG_BUTTON, align_names[i], BS_AUTORADIOBUTTON | (i == 0 ? WS_GROUP | WS_TABSTOP : 0),
                 (short)(16 + i * 49), 19, 47, 12, (WORD)(IDC_PF_ALIGN0 + i));
    }
    dlg_item(&d, DLG_BUTTON, L"여백", BS_GROUPBOX | WS_GROUP, 8, 42, 124, 50, (WORD)-1);
    dlg_item(&d, DLG_STATIC, L"왼쪽(&A):", SS_LEFT, 16, 57, 40, 10, (WORD)-1);
    dlg_item(&d, DLG_EDIT, L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 60, 55, 44, 13, IDC_PF_LEFT);
    dlg_item(&d, DLG_STATIC, L"pt", SS_LEFT, 108, 57, 16, 10, (WORD)-1);
    dlg_item(&d, DLG_STATIC, L"오른쪽(&B):", SS_LEFT, 16, 75, 40, 10, (WORD)-1);
    dlg_item(&d, DLG_EDIT, L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 60, 73, 44, 13, IDC_PF_RIGHT);
    dlg_item(&d, DLG_STATIC, L"pt", SS_LEFT, 108, 75, 16, 10, (WORD)-1);
    dlg_item(&d, DLG_BUTTON, L"첫 줄", BS_GROUPBOX, 138, 42, 124, 50, (WORD)-1);
    dlg_item(&d, DLG_BUTTON, L"보통(&N)", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 146, 55, 36, 12,
             IDC_PF_FIRST_NORMAL);
    dlg_item(&d, DLG_BUTTON, L"들여쓰기(&I)", BS_AUTORADIOBUTTON, 184, 55, 50, 12, IDC_PF_FIRST_INDENT);
    dlg_item(&d, DLG_BUTTON, L"내어쓰기(&O)", BS_AUTORADIOBUTTON, 146, 73, 52, 12, IDC_PF_FIRST_HANG);
    dlg_item(&d, DLG_EDIT, L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP | WS_GROUP, 202, 72, 36, 13,
             IDC_PF_FIRST_VALUE);
    dlg_item(&d, DLG_STATIC, L"pt", SS_LEFT, 242, 74, 16, 10, (WORD)-1);
    dlg_item(&d, DLG_BUTTON, L"간격", BS_GROUPBOX, 8, 96, 254, 70, (WORD)-1);
    dlg_item(&d, DLG_STATIC, L"줄 간격(&S):", SS_LEFT, 16, 111, 48, 10, (WORD)-1);
    dlg_item(&d, DLG_EDIT, L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 66, 109, 44, 13, IDC_PF_SPACING);
    dlg_item(&d, DLG_STATIC, L"% (글자에 따라)", SS_LEFT, 114, 111, 80, 10, (WORD)-1);
    dlg_item(&d, DLG_STATIC, L"문단 위(&U):", SS_LEFT, 16, 129, 48, 10, (WORD)-1);
    dlg_item(&d, DLG_EDIT, L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 66, 127, 44, 13, IDC_PF_BEFORE);
    dlg_item(&d, DLG_STATIC, L"pt", SS_LEFT, 114, 129, 16, 10, (WORD)-1);
    dlg_item(&d, DLG_STATIC, L"문단 아래(&W):", SS_LEFT, 16, 147, 48, 10, (WORD)-1);
    dlg_item(&d, DLG_EDIT, L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 66, 145, 44, 13, IDC_PF_AFTER);
    dlg_item(&d, DLG_STATIC, L"pt", SS_LEFT, 114, 147, 16, 10, (WORD)-1);
    dlg_item(&d, DLG_BUTTON, L"설정(&D)", BS_DEFPUSHBUTTON | WS_TABSTOP, 150, 176, 54, 15, IDOK);
    dlg_item(&d, DLG_BUTTON, L"취소", BS_PUSHBUTTON | WS_TABSTOP, 208, 176, 54, 15, IDCANCEL);
    if (dlg_run(&d, ParaDlgProc, (LPARAM)&s) != IDOK) {
        return;
    }
    change = s.result;
    change.fields = 0;
    if (s.result.align != s.start.align) {
        change.fields |= HP_PF_ALIGN;
    }
    if (s.result.left_hwp != s.start.left_hwp) {
        change.fields |= HP_PF_LEFT;
    }
    if (s.result.right_hwp != s.start.right_hwp) {
        change.fields |= HP_PF_RIGHT;
    }
    if (s.result.indent_hwp != s.start.indent_hwp) {
        change.fields |= HP_PF_INDENT;
    }
    if (s.result.before_hwp != s.start.before_hwp) {
        change.fields |= HP_PF_BEFORE;
    }
    if (s.result.after_hwp != s.start.after_hwp) {
        change.fields |= HP_PF_AFTER;
    }
    if (s.result.line_spacing_percent != s.start.line_spacing_percent && s.result.line_spacing_percent > 0) {
        change.fields |= HP_PF_LINE_SPACING;
    }
    if (change.fields) {
        apply_formats(NULL, &change);
    }
}

/* ---- 찾아가기 ---- */

#define IDC_GOTO_PAGE 301

static INT_PTR CALLBACK GotoDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        wchar_t label[64];
        swprintf(label, 64, L"쪽 번호 (1~%d)(&P):", g_page_count);
        SetDlgItemTextW(dlg, IDC_GOTO_PAGE - 1, label);
        SetDlgItemInt(dlg, IDC_GOTO_PAGE, (UINT)(g_page_index + 1), FALSE);
        SendDlgItemMessageW(dlg, IDC_GOTO_PAGE, EM_SETSEL, 0, -1);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            BOOL ok;
            UINT v = GetDlgItemInt(dlg, IDC_GOTO_PAGE, &ok, FALSE);
            if (!ok || v < 1 || (int)v > g_page_count) {
                MessageBoxW(dlg, L"쪽 번호를 확인하세요.", L"한글패드", MB_ICONWARNING);
                return TRUE;
            }
            EndDialog(dlg, (INT_PTR)v);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void goto_dialog(void) {
    dlg_t d;
    INT_PTR page;
    if (!g_doc || g_page_count <= 0) {
        return;
    }
    dlg_begin(&d, L"찾아가기", 180, 64);
    dlg_item(&d, DLG_STATIC, L"쪽 번호:", SS_LEFT, 10, 14, 90, 10, IDC_GOTO_PAGE - 1);
    dlg_item(&d, DLG_EDIT, L"", ES_NUMBER | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 104, 12, 66, 13, IDC_GOTO_PAGE);
    dlg_item(&d, DLG_BUTTON, L"가기(&D)", BS_DEFPUSHBUTTON | WS_TABSTOP, 62, 40, 52, 15, IDOK);
    dlg_item(&d, DLG_BUTTON, L"취소", BS_PUSHBUTTON | WS_TABSTOP, 118, 40, 52, 15, IDCANCEL);
    page = dlg_run(&d, GotoDlgProc, 0);
    if (page > 0) {
        if (g_editing) {
            editor_mode(0);
        }
        goto_page(g_main, (int)page - 1);
        SetFocus(g_view);
    }
}

/* ---- 편집 용지 (보기 전용) ---- */

static INT_PTR CALLBACK InfoDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    if (msg == WM_INITDIALOG) {
        return TRUE;
    }
    if (msg == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
        EndDialog(dlg, IDOK);
        return TRUE;
    }
    return FALSE;
}

static double hwp_mm(uint32_t v) {
    return v / 7200.0 * 25.4;
}

static void page_setup_dialog(void) {
    dlg_t d;
    const hp_section *sec;
    const hp_page *pg;
    wchar_t buf[128];
    const wchar_t *kind = L"사용자 정의";
    double w, h;
    int portrait;
    if (!g_doc || !g_doc->section_count) {
        return;
    }
    sec = &g_doc->sections[g_page_count > 0 ? g_pages[g_page_index].section : 0];
    pg = &sec->page;
    w = hwp_mm(pg->width);
    h = hwp_mm(pg->height);
    portrait = h >= w;
    {
        double a = portrait ? w : h, b = portrait ? h : w;
        if (fabs(a - 210) < 1.5 && fabs(b - 297) < 1.5) {
            kind = L"A4 (국배판)";
        } else if (fabs(a - 297) < 1.5 && fabs(b - 420) < 1.5) {
            kind = L"A3";
        } else if (fabs(a - 182) < 1.5 && fabs(b - 257) < 1.5) {
            kind = L"B5 (46배판)";
        } else if (fabs(a - 257) < 1.5 && fabs(b - 364) < 1.5) {
            kind = L"B4";
        } else if (fabs(a - 215.9) < 1.5 && fabs(b - 279.4) < 1.5) {
            kind = L"Letter";
        }
    }
    dlg_begin(&d, L"편집 용지", 236, 178);
    dlg_item(&d, DLG_BUTTON, L"용지 종류", BS_GROUPBOX, 8, 6, 220, 46, (WORD)-1);
    swprintf(buf, 128, L"종류: %ls    방향: %ls", kind, portrait ? L"세로" : L"가로");
    dlg_item(&d, DLG_STATIC, buf, SS_LEFT, 16, 19, 204, 10, (WORD)-1);
    swprintf(buf, 128, L"폭: %.1f mm    길이: %.1f mm", w, h);
    dlg_item(&d, DLG_STATIC, buf, SS_LEFT, 16, 34, 204, 10, (WORD)-1);
    dlg_item(&d, DLG_BUTTON, L"용지 여백", BS_GROUPBOX, 8, 56, 220, 70, (WORD)-1);
    swprintf(buf, 128, L"위쪽: %.1f mm    아래쪽: %.1f mm", hwp_mm(pg->margin_top), hwp_mm(pg->margin_bottom));
    dlg_item(&d, DLG_STATIC, buf, SS_LEFT, 16, 69, 204, 10, (WORD)-1);
    swprintf(buf, 128, L"왼쪽: %.1f mm    오른쪽: %.1f mm", hwp_mm(pg->margin_left), hwp_mm(pg->margin_right));
    dlg_item(&d, DLG_STATIC, buf, SS_LEFT, 16, 84, 204, 10, (WORD)-1);
    swprintf(buf, 128, L"머리말: %.1f mm    꼬리말: %.1f mm", hwp_mm(pg->header_height), hwp_mm(pg->footer_height));
    dlg_item(&d, DLG_STATIC, buf, SS_LEFT, 16, 99, 204, 10, (WORD)-1);
    swprintf(buf, 128, L"구역 %zu / %zu", (size_t)(sec - g_doc->sections) + 1, g_doc->section_count);
    dlg_item(&d, DLG_STATIC, buf, SS_LEFT, 16, 112, 204, 10, (WORD)-1);
    dlg_item(&d, DLG_STATIC, L"이 버전에서는 용지 설정을 볼 수만 있습니다.", SS_LEFT, 8, 134, 220, 10, (WORD)-1);
    dlg_item(&d, DLG_BUTTON, L"닫기", BS_DEFPUSHBUTTON | WS_TABSTOP, 174, 154, 54, 15, IDOK);
    dlg_run(&d, InfoDlgProc, 0);
}

/* ---- 문서 정보 ---- */

static void count_blocks(const hp_block *b, size_t n, size_t *tables, size_t *images) {
    size_t i;
    for (i = 0; i < n; i++) {
        if (b[i].kind == HP_BLOCK_TABLE) {
            (*tables)++;
        } else if (b[i].kind == HP_BLOCK_IMAGE) {
            (*images)++;
        }
    }
}

static void document_info(void) {
    wchar_t msg[2048], size[64] = L"-";
    size_t i, paragraphs = 0, chars = 0, tables = 0, images = 0;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    const wchar_t *name;
    wchar_t *title = NULL;
    if (!g_doc) {
        return;
    }
    if (g_editor) {
        editor_commit();
        for (i = 0; i < hp_editor_count(g_editor); i++) {
            wchar_t *w;
            if (hp_editor_is_deleted(g_editor, i)) {
                continue;
            }
            paragraphs++;
            w = utf8_to_wide(hp_editor_text(g_editor, i));
            if (w) {
                size_t k;
                for (k = 0; w[k]; k++) {
                    if (w[k] != L'\n' && (w[k] < 0xDC00 || w[k] > 0xDFFF)) {
                        chars++;
                    }
                }
                free(w);
            }
        }
    }
    for (i = 0; i < g_doc->section_count; i++) {
        count_blocks(g_doc->sections[i].blocks, g_doc->sections[i].block_count, &tables, &images);
    }
    if (g_path[0] && GetFileAttributesExW(g_path, GetFileExInfoStandard, &fa)) {
        unsigned long long bytes = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        swprintf(size, 64, L"%.1f KB (%llu 바이트)", bytes / 1024.0, bytes);
    }
    name = g_path[0] ? g_path : L"(저장하지 않은 새 문서)";
    if (g_doc->title && g_doc->title[0]) {
        title = utf8_to_wide(g_doc->title);
    }
    swprintf(msg, 2048,
             L"파일: %ls\n크기: %ls\n형식: %ls\n제목: %ls\n\n"
             L"구역: %zu\n쪽: %d\n문단: %zu\n글자 (공백 포함): %zu\n표(본문): %zu\n그림(본문): %zu",
             name, size,
             g_read_only ? L"HWP 배포용 문서 (읽기 전용)" : g_source_hwp ? L"HWP (저장은 HWPX)" : L"HWPX",
             title ? title : L"-", g_doc->section_count, g_page_count, paragraphs, chars, tables, images);
    free(title);
    MessageBoxW(g_main, msg, L"문서 정보", MB_OK | MB_ICONINFORMATION);
}

static void shortcut_list(void) {
    MessageBoxW(g_main,
                L"파일\n"
                L"  새 문서 Alt+N / Ctrl+N    불러오기 Alt+O / Ctrl+O\n"
                L"  저장하기 Alt+S / Ctrl+S    다른 이름으로 저장 Alt+V\n"
                L"  인쇄 Alt+P / Ctrl+P    편집 용지 F7    문서 정보 Ctrl+Q,I\n"
                L"  미리 보기 F5    문서 닫기 Ctrl+F4    끝 Alt+X\n\n"
                L"편집\n"
                L"  본문 편집 F2    문단 추가 Ctrl+Enter\n"
                L"  쪽 보기에서 더블클릭: 그 자리에서 편집, 편집 중 쪽 클릭: 커서 이동\n"
                L"  입력창에서 글자를 고른 뒤 서식: 고른 글자에만 적용\n"
                L"  찾기 Ctrl+F / Ctrl+Q,F    찾아 바꾸기 Ctrl+H / Ctrl+F2 / Ctrl+Q,A\n"
                L"  다시 찾기 Ctrl+L    거꾸로 찾기 Ctrl+Q,L    찾아가기 Alt+G\n\n"
                L"보기\n"
                L"  쪽 맞춤 Ctrl+G,P    폭 맞춤 Ctrl+G,W    100% Ctrl+G,Q\n"
                L"  확대/축소 Ctrl+마우스 휠    이전/다음 쪽 PgUp / PgDn\n\n"
                L"입력\n"
                L"  문자표 Ctrl+F10    날짜/시간 문자열 Ctrl+K,D\n\n"
                L"모양\n"
                L"  글자 모양 Alt+L    문단 모양 Alt+T\n"
                L"  진하게 Ctrl+B    기울임 Ctrl+I    밑줄 Ctrl+U\n"
                L"  글자 크게 Ctrl+]    글자 작게 Ctrl+[\n"
                L"  양쪽/왼쪽/가운데/오른쪽/배분 정렬 Ctrl+Shift+M/L/C/R/T\n"
                L"  줄 간격 넓게/좁게 Ctrl+Shift+W/Q\n"
                L"  글자 색 Ctrl+M,K(검정) R(빨강) B(파랑) D(자주) G(초록) Y(노랑) C(청록) W(흰색)",
                L"단축키 일람", MB_OK | MB_ICONINFORMATION);
}

/* ======================= find / replace ======================= */

static void find_open(int replace) {
    if (!g_editor) {
        MessageBoxW(g_main, L"배포용 문서에서는 찾기를 쓸 수 없습니다.", L"한글패드", MB_ICONINFORMATION);
        return;
    }
    if (g_find_dlg) {
        DestroyWindow(g_find_dlg);
        g_find_dlg = NULL;
    }
    if (g_editing && g_text) {
        DWORD a = 0, b = 0;
        SendMessageW(g_text, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
        if (b > a && b - a < 255) {
            int n = GetWindowTextLengthW(g_text);
            wchar_t *w = (wchar_t *)malloc(((size_t)n + 1) * sizeof(wchar_t));
            if (w) {
                GetWindowTextW(g_text, w, n + 1);
                if ((int)b <= n && !wmemchr(w + a, L'\n', b - a)) {
                    wmemcpy(g_find_what, w + a, b - a);
                    g_find_what[b - a] = 0;
                }
                free(w);
            }
        }
    }
    ZeroMemory(&g_fr, sizeof(g_fr));
    g_fr.lStructSize = sizeof(g_fr);
    g_fr.hwndOwner = g_main;
    g_fr.lpstrFindWhat = g_find_what;
    g_fr.wFindWhatLen = 256;
    g_fr.lpstrReplaceWith = g_replace_with;
    g_fr.wReplaceWithLen = 256;
    g_fr.Flags = FR_DOWN | FR_HIDEWHOLEWORD;
    g_find_dlg = replace ? ReplaceTextW(&g_fr) : FindTextW(&g_fr);
}

static void lower_copy(wchar_t *s) {
    if (s && *s) {
        CharLowerBuffW(s, (DWORD)wcslen(s));
    }
}

/* index of the next/previous match in w (from `from`), or -1 */
static long find_in(const wchar_t *w, const wchar_t *what, long from, int down, int match_case) {
    wchar_t *hay = _wcsdup(w), *nee = _wcsdup(what);
    long found = -1, n, m, i;
    if (!hay || !nee) {
        free(hay);
        free(nee);
        return -1;
    }
    if (!match_case) {
        lower_copy(hay);
        lower_copy(nee);
    }
    n = (long)wcslen(hay);
    m = (long)wcslen(nee);
    if (m > 0 && m <= n) {
        if (down) {
            for (i = from < 0 ? 0 : from; i + m <= n; i++) {
                if (wcsncmp(hay + i, nee, (size_t)m) == 0) {
                    found = i;
                    break;
                }
            }
        } else {
            for (i = (from > n ? n : from) - m; i >= 0; i--) {
                if (wcsncmp(hay + i, nee, (size_t)m) == 0) {
                    found = i;
                    break;
                }
            }
        }
    }
    free(hay);
    free(nee);
    return found;
}

/* LF offset -> edit control (CRLF) offset */
static long crlf_offset(const wchar_t *w, long at) {
    long i, extra = 0;
    for (i = 0; i < at && w[i]; i++) {
        if (w[i] == L'\n') {
            extra++;
        }
    }
    return at + extra;
}

static long lf_offset(long at) {
    int n = GetWindowTextLengthW(g_text);
    wchar_t *w = (wchar_t *)malloc(((size_t)n + 1) * sizeof(wchar_t));
    long i, lf = 0;
    if (!w) {
        return at;
    }
    GetWindowTextW(g_text, w, n + 1);
    for (i = 0; i < at && i < n; i++) {
        if (w[i] != L'\r') {
            lf++;
        }
    }
    free(w);
    return lf;
}

static int find_next(int down, int match_case, int quiet) {
    size_t count, i, start, step;
    long from = down ? 0 : LONG_MAX;
    int wrapped = 0;
    if (!g_editor || !g_find_what[0]) {
        if (!g_find_what[0]) {
            find_open(0);
        }
        return 0;
    }
    if (!editor_commit()) {
        return 0;
    }
    count = hp_editor_count(g_editor);
    if (!count) {
        return 0;
    }
    start = g_edit_index != (size_t)-1 ? g_edit_index : 0;
    if (g_editing && g_edit_index != (size_t)-1) {
        DWORD a = 0, b = 0;
        SendMessageW(g_text, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
        from = lf_offset(down ? (long)b : (long)a);
    }
    for (step = 0; step <= count; step++) {
        size_t k = down ? (start + step) % count : (start + count - step % count) % count;
        wchar_t *w;
        long at;
        if (step > 0 && ((down && k == 0) || (!down && k == count - 1))) {
            wrapped = 1;
        }
        i = k;
        if (hp_editor_is_deleted(g_editor, i)) {
            continue;
        }
        w = utf8_to_wide(hp_editor_text(g_editor, i));
        if (!w) {
            continue;
        }
        at = find_in(w, g_find_what, step == 0 ? from : (down ? 0 : LONG_MAX), down, match_case);
        if (at >= 0) {
            long a = crlf_offset(w, at), b = crlf_offset(w, at + (long)wcslen(g_find_what));
            if (!g_editing) {
                editor_mode(1);
            }
            if (i != g_edit_index) {
                list_select_item(i);
            }
            SendMessageW(g_text, EM_SETSEL, (WPARAM)a, (LPARAM)b);
            SendMessageW(g_text, EM_SCROLLCARET, 0, 0);
            status_message(wrapped ? L"문서 끝에서 처음으로 이어서 찾았습니다." : L"");
            free(w);
            return 1;
        }
        free(w);
    }
    if (!quiet) {
        MessageBoxW(g_find_dlg ? g_find_dlg : g_main, L"찾는 문자열이 없습니다.", L"한글패드", MB_ICONINFORMATION);
    }
    return 0;
}

static int selection_matches(int match_case) {
    DWORD a = 0, b = 0;
    int n, ok = 0;
    wchar_t *w;
    if (!g_editing || g_edit_index == (size_t)-1) {
        return 0;
    }
    SendMessageW(g_text, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
    if (b <= a || b - a != wcslen(g_find_what)) {
        return 0;
    }
    n = GetWindowTextLengthW(g_text);
    w = (wchar_t *)malloc(((size_t)n + 1) * sizeof(wchar_t));
    if (!w) {
        return 0;
    }
    GetWindowTextW(g_text, w, n + 1);
    if ((int)b <= n) {
        ok = match_case ? wcsncmp(w + a, g_find_what, b - a) == 0 : _wcsnicmp(w + a, g_find_what, b - a) == 0;
    }
    free(w);
    return ok;
}

static size_t replace_all(int match_case) {
    size_t i, total = 0;
    if (!g_editor || !g_find_what[0] || !editor_commit()) {
        return 0;
    }
    for (i = 0; i < hp_editor_count(g_editor); i++) {
        wchar_t *w, *out;
        size_t cap, n = 0, hits = 0, mlen = wcslen(g_find_what), rlen = wcslen(g_replace_with);
        long pos = 0, at;
        if (hp_editor_is_deleted(g_editor, i)) {
            continue;
        }
        w = utf8_to_wide(hp_editor_text(g_editor, i));
        if (!w) {
            continue;
        }
        cap = wcslen(w) + 1;
        out = (wchar_t *)malloc(cap * sizeof(wchar_t));
        while (out && (at = find_in(w, g_find_what, pos, 1, match_case)) >= 0) {
            size_t need = n + (size_t)(at - pos) + rlen + wcslen(w + at + mlen) + 1;
            if (need > cap) {
                wchar_t *next = (wchar_t *)realloc(out, need * 2 * sizeof(wchar_t));
                if (!next) {
                    free(out);
                    out = NULL;
                    break;
                }
                out = next;
                cap = need * 2;
            }
            wmemcpy(out + n, w + pos, (size_t)(at - pos));
            n += (size_t)(at - pos);
            wmemcpy(out + n, g_replace_with, rlen);
            n += rlen;
            pos = at + (long)mlen;
            hits++;
        }
        if (out && hits) {
            char *u;
            size_t rest = wcslen(w + pos);
            if (n + rest + 1 > cap) {
                wchar_t *next = (wchar_t *)realloc(out, (n + rest + 1) * sizeof(wchar_t));
                if (!next) {
                    free(out);
                    free(w);
                    continue;
                }
                out = next;
            }
            wmemcpy(out + n, w + pos, rest + 1);
            u = wide_to_utf8(out);
            if (u && hp_editor_set(g_editor, i, u) == HP_OK) {
                total += hits;
            }
            free(u);
        }
        free(out);
        free(w);
    }
    if (total) {
        size_t keep = g_edit_index;
        g_dirty = 1;
        editor_title();
        editor_list(keep != (size_t)-1 ? keep : 0);
        preview_build();
    }
    return total;
}

static void find_message(void) {
    int down = (g_fr.Flags & FR_DOWN) != 0, match_case = (g_fr.Flags & FR_MATCHCASE) != 0;
    if (g_fr.Flags & FR_DIALOGTERM) {
        g_find_dlg = NULL;
        return;
    }
    if (g_fr.Flags & FR_FINDNEXT) {
        find_next(down, match_case, 0);
    } else if (g_fr.Flags & FR_REPLACE) {
        if (selection_matches(match_case)) {
            SendMessageW(g_text, EM_REPLACESEL, TRUE, (LPARAM)g_replace_with);
            g_dirty = 1;
            editor_title();
        }
        find_next(1, match_case, 0);
    } else if (g_fr.Flags & FR_REPLACEALL) {
        wchar_t msg[96];
        size_t n = replace_all(match_case);
        swprintf(msg, 96, n ? L"%zu개를 바꾸었습니다." : L"찾는 문자열이 없습니다.", n);
        MessageBoxW(g_find_dlg ? g_find_dlg : g_main, msg, L"찾아 바꾸기", MB_ICONINFORMATION);
    }
}

/* ======================= printing ======================= */

static void print_document(void) {
    PRINTDLGW pd;
    DOCINFOW di;
    int from, to, i, ok = 1;
    const wchar_t *name;
    if (!g_doc || g_page_count <= 0) {
        return;
    }
    if (g_editing) {
        preview_build();
    }
    ZeroMemory(&pd, sizeof(pd));
    pd.lStructSize = sizeof(pd);
    pd.hwndOwner = g_main;
    pd.Flags = PD_RETURNDC | PD_NOSELECTION | PD_USEDEVMODECOPIESANDCOLLATE;
    pd.nMinPage = 1;
    pd.nMaxPage = (WORD)(g_page_count > 65535 ? 65535 : g_page_count);
    pd.nFromPage = 1;
    pd.nToPage = pd.nMaxPage;
    pd.nCopies = 1;
    if (!PrintDlgW(&pd)) {
        return;
    }
    from = 1;
    to = g_page_count;
    if (pd.Flags & PD_PAGENUMS) {
        from = pd.nFromPage;
        to = pd.nToPage;
    }
    if (from < 1) {
        from = 1;
    }
    if (to > g_page_count) {
        to = g_page_count;
    }
    name = wcsrchr(g_path, L'\\');
    ZeroMemory(&di, sizeof(di));
    di.cbSize = sizeof(di);
    di.lpszDocName = g_path[0] ? (name ? name + 1 : g_path) : L"한글패드 문서";
    if (StartDocW(pd.hDC, &di) > 0) {
        int dpx = GetDeviceCaps(pd.hDC, LOGPIXELSX), dpy = GetDeviceCaps(pd.hDC, LOGPIXELSY);
        int offx = GetDeviceCaps(pd.hDC, PHYSICALOFFSETX), offy = GetDeviceCaps(pd.hDC, PHYSICALOFFSETY);
        status_message(L"인쇄하는 중...");
        for (i = from - 1; i < to && ok; i++) {
            int pw, ph, saved;
            view_page_size(i, &pw, &ph);
            if (StartPage(pd.hDC) <= 0) {
                ok = 0;
                break;
            }
            saved = SaveDC(pd.hDC);
            SetMapMode(pd.hDC, MM_ANISOTROPIC);
            SetWindowExtEx(pd.hDC, pw, ph, NULL);
            SetViewportExtEx(pd.hDC, MulDiv(pw, dpx, 96), MulDiv(ph, dpy, 96), NULL);
            SetViewportOrgEx(pd.hDC, -offx, -offy, NULL);
            paint_document_page(pd.hDC, i);
            SetGraphicsMode(pd.hDC, GM_ADVANCED);
            ModifyWorldTransform(pd.hDC, NULL, MWT_IDENTITY);
            SetGraphicsMode(pd.hDC, GM_COMPATIBLE);
            RestoreDC(pd.hDC, saved);
            if (EndPage(pd.hDC) <= 0) {
                ok = 0;
            }
        }
        if (ok) {
            EndDoc(pd.hDC);
        } else {
            AbortDoc(pd.hDC);
        }
    } else {
        ok = 0;
    }
    DeleteDC(pd.hDC);
    if (pd.hDevMode) {
        GlobalFree(pd.hDevMode);
    }
    if (pd.hDevNames) {
        GlobalFree(pd.hDevNames);
    }
    status_message(ok ? L"인쇄를 보냈습니다." : L"");
    if (!ok) {
        MessageBoxW(g_main, L"인쇄하지 못했습니다. 프린터 상태를 확인하세요.", L"한글패드", MB_ICONERROR);
    }
}

/* ======================= misc commands ======================= */

static void new_window(void) {
    wchar_t exe[MAX_PATH];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (GetModuleFileNameW(NULL, exe, MAX_PATH) &&
        CreateProcessW(exe, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

static void insert_date(void) {
    SYSTEMTIME t;
    wchar_t buf[64];
    if (!g_editor) {
        return;
    }
    if (!g_editing) {
        editor_mode(1);
    }
    GetLocalTime(&t);
    swprintf(buf, 64, L"%d년 %d월 %d일", t.wYear, t.wMonth, t.wDay);
    SendMessageW(g_text, EM_REPLACESEL, TRUE, (LPARAM)buf);
    SetFocus(g_text);
}

static HWND focused_edit(void) {
    HWND f = GetFocus();
    wchar_t cls[16];
    if (f && GetClassNameW(f, cls, 16) && _wcsicmp(cls, L"Edit") == 0) {
        return f;
    }
    return g_editing ? g_text : NULL;
}

static void update_menu_state(HMENU m) {
    int has_doc = g_doc != NULL, ed = g_editor != NULL;
    struct {
        UINT id;
        int on;
    } en[] = {{IDM_FILE_SAVE, ed},          {IDM_FILE_SAVE_AS, ed},      {IDM_DOC_INFO, has_doc},
              {IDM_PAGE_SETUP, has_doc},    {IDM_PRINT, has_doc && g_page_count > 0},
              {IDM_PREVIEW, ed},            {IDM_EDIT_MODE, ed},         {IDM_ADD_PARA, ed && g_editing},
              {IDM_DELETE_PARA, ed && g_editing},
              {IDM_UNDO, g_editing},        {IDM_CUT, g_editing},        {IDM_PASTE, g_editing},
              {IDM_SELECT_ALL, g_editing},  {IDM_FIND, ed},              {IDM_REPLACE, ed},
              {IDM_FIND_NEXT, ed},          {IDM_FIND_PREV, ed},         {IDM_GOTO, has_doc},
              {ID_BTN_PREV, g_page_index > 0},                           {ID_BTN_NEXT, g_page_index + 1 < g_page_count},
              {IDM_FIRST_PAGE, has_doc},    {IDM_LAST_PAGE, has_doc},    {IDM_INSERT_DATE, ed},
              {IDM_CHAR_SHAPE, ed},         {IDM_PARA_SHAPE, ed},        {IDM_BOLD, ed},
              {IDM_ITALIC, ed},             {IDM_UNDERLINE, ed},         {IDM_STRIKE, ed},
              {IDM_COLOR, ed},              {IDM_SIZE_UP, ed},           {IDM_SIZE_DOWN, ed},
              {IDM_ALIGN_JUSTIFY, ed},      {IDM_ALIGN_LEFT, ed},        {IDM_ALIGN_CENTER, ed},
              {IDM_ALIGN_RIGHT, ed},        {IDM_ALIGN_DISTRIBUTE, ed},  {IDM_SPACING_UP, ed},
              {IDM_SPACING_DOWN, ed}};
    size_t i;
    hp_char_format cf;
    hp_para_format pf;
    int fmt = ed && g_edit_index != (size_t)-1 && hp_editor_get_format(g_editor, g_edit_index, &cf, &pf) == HP_OK;
    for (i = 0; i < sizeof(en) / sizeof(en[0]); i++) {
        EnableMenuItem(m, en[i].id, MF_BYCOMMAND | (en[i].on ? MF_ENABLED : MF_GRAYED));
    }
    for (i = 0; i < 8; i++) {
        EnableMenuItem(m, IDM_COLOR_BASE + (UINT)i, MF_BYCOMMAND | (ed ? MF_ENABLED : MF_GRAYED));
    }
    CheckMenuItem(m, IDM_EDIT_MODE, MF_BYCOMMAND | (g_editing ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_TWO_PAGES, MF_BYCOMMAND | (g_two_pages ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_SHOW_FMTBAR, MF_BYCOMMAND | (g_show_fmtbar ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_SHOW_STATUS, MF_BYCOMMAND | (g_show_status ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_ZOOM_PAGE, MF_BYCOMMAND | (g_zoom_mode == ZOOM_FIT_PAGE ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_ZOOM_WIDTH, MF_BYCOMMAND | (g_zoom_mode == ZOOM_FIT_WIDTH ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_ZOOM_100,
                  MF_BYCOMMAND | (g_zoom_mode == ZOOM_PERCENT && g_zoom == 100 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_BOLD, MF_BYCOMMAND | (fmt && cf.bold ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_ITALIC, MF_BYCOMMAND | (fmt && cf.italic ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_UNDERLINE, MF_BYCOMMAND | (fmt && cf.underline ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_STRIKE, MF_BYCOMMAND | (fmt && cf.strikeout ? MF_CHECKED : MF_UNCHECKED));
    {
        static const UINT ids[5] = {IDM_ALIGN_LEFT, IDM_ALIGN_CENTER, IDM_ALIGN_RIGHT, IDM_ALIGN_JUSTIFY,
                                    IDM_ALIGN_DISTRIBUTE};
        for (i = 0; i < 5; i++) {
            CheckMenuItem(m, ids[i], MF_BYCOMMAND | (fmt && pf.align == (int)i ? MF_CHECKED : MF_UNCHECKED));
        }
    }
}

/* ======================= keyboard ======================= */

static HACCEL build_accelerators(void) {
    ACCEL a[] = {
        {FVIRTKEY | FCONTROL, 'N', IDM_FILE_NEW},
        {FVIRTKEY | FALT, 'N', IDM_FILE_NEW},
        {FVIRTKEY | FCONTROL, 'O', IDM_FILE_OPEN},
        {FVIRTKEY | FALT, 'O', IDM_FILE_OPEN},
        {FVIRTKEY | FCONTROL, 'S', IDM_FILE_SAVE},
        {FVIRTKEY | FALT, 'S', IDM_FILE_SAVE},
        {FVIRTKEY | FCONTROL | FSHIFT, 'S', IDM_FILE_SAVE_AS},
        {FVIRTKEY | FALT, 'V', IDM_FILE_SAVE_AS},
        {FVIRTKEY | FCONTROL, 'P', IDM_PRINT},
        {FVIRTKEY | FALT, 'P', IDM_PRINT},
        {FVIRTKEY | FALT, 'X', IDM_FILE_EXIT},
        {FVIRTKEY | FCONTROL, VK_F4, IDM_FILE_CLOSE},
        {FVIRTKEY | FCONTROL, '1', IDM_FILE_OPEN_SAMPLE},
        {FVIRTKEY, VK_F2, IDM_EDIT_MODE},
        {FVIRTKEY, VK_F5, IDM_PREVIEW},
        {FVIRTKEY, VK_F7, IDM_PAGE_SETUP},
        {FVIRTKEY | FCONTROL, 'Z', IDM_UNDO},
        {FVIRTKEY | FCONTROL, 'X', IDM_CUT},
        {FVIRTKEY | FCONTROL, 'C', IDM_COPY},
        {FVIRTKEY | FCONTROL, 'V', IDM_PASTE},
        {FVIRTKEY | FCONTROL, 'A', IDM_SELECT_ALL},
        {FVIRTKEY | FCONTROL, VK_RETURN, IDM_ADD_PARA},
        {FVIRTKEY | FCONTROL, 'F', IDM_FIND},
        {FVIRTKEY | FCONTROL, 'H', IDM_REPLACE},
        {FVIRTKEY | FCONTROL, VK_F2, IDM_REPLACE},
        {FVIRTKEY | FCONTROL, 'L', IDM_FIND_NEXT},
        {FVIRTKEY | FALT, 'G', IDM_GOTO},
        {FVIRTKEY | FALT, 'L', IDM_CHAR_SHAPE},
        {FVIRTKEY | FALT, 'T', IDM_PARA_SHAPE},
        {FVIRTKEY | FCONTROL, 'B', IDM_BOLD},
        {FVIRTKEY | FCONTROL, 'I', IDM_ITALIC},
        {FVIRTKEY | FCONTROL, 'U', IDM_UNDERLINE},
        {FVIRTKEY | FCONTROL, VK_OEM_6, IDM_SIZE_UP},
        {FVIRTKEY | FCONTROL, VK_OEM_4, IDM_SIZE_DOWN},
        {FVIRTKEY | FCONTROL | FSHIFT, 'M', IDM_ALIGN_JUSTIFY},
        {FVIRTKEY | FCONTROL | FSHIFT, 'L', IDM_ALIGN_LEFT},
        {FVIRTKEY | FCONTROL | FSHIFT, 'C', IDM_ALIGN_CENTER},
        {FVIRTKEY | FCONTROL | FSHIFT, 'R', IDM_ALIGN_RIGHT},
        {FVIRTKEY | FCONTROL | FSHIFT, 'T', IDM_ALIGN_DISTRIBUTE},
        {FVIRTKEY | FCONTROL | FSHIFT, 'W', IDM_SPACING_UP},
        {FVIRTKEY | FCONTROL | FSHIFT, 'Q', IDM_SPACING_DOWN},
        {FVIRTKEY | FCONTROL, VK_F10, IDM_CHARMAP},
    };
    return CreateAcceleratorTableW(a, (int)(sizeof(a) / sizeof(a[0])));
}

/* Ctrl+Q / Ctrl+G / Ctrl+K / Ctrl+M two-key shortcuts; returns 1 when the key was used */
static int chord_key(const MSG *m) {
    UINT k;
    if (m->message != WM_KEYDOWN && m->message != WM_SYSKEYDOWN) {
        return 0;
    }
    k = (UINT)m->wParam;
    if (k == VK_CONTROL || k == VK_SHIFT || k == VK_MENU || k == VK_LCONTROL || k == VK_RCONTROL) {
        return 0;
    }
    if (g_chord) {
        int prefix = g_chord, id = 0;
        static const char colors[] = "KRBDGYCW";
        const char *c;
        g_chord = 0;
        status_message(L"");
        switch (prefix) {
        case 'Q':
            id = k == 'I' || k == 'D' ? IDM_DOC_INFO : k == 'F' ? IDM_FIND : k == 'A' ? IDM_REPLACE
                 : k == 'L'                                    ? IDM_FIND_PREV
                                                               : 0;
            break;
        case 'G':
            id = k == 'P' ? IDM_ZOOM_PAGE : k == 'W' ? IDM_ZOOM_WIDTH : k == 'Q' ? IDM_ZOOM_100 : 0;
            break;
        case 'K':
            id = k == 'D' ? IDM_INSERT_DATE : 0;
            break;
        case 'M':
            c = k < 128 ? strchr(colors, (int)k) : NULL;
            id = c && *c ? IDM_COLOR_BASE + (int)(c - colors) : 0;
            break;
        }
        if (id) {
            SendMessageW(g_main, WM_COMMAND, (WPARAM)id, 0);
        }
        return 1;
    }
    if (m->message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_SHIFT) & 0x8000) &&
        !(GetKeyState(VK_MENU) & 0x8000) && (k == 'Q' || k == 'G' || k == 'K' || k == 'M')) {
        wchar_t msg[64];
        g_chord = (int)k;
        swprintf(msg, 64, L"Ctrl+%c, 다음 글쇠를 누르세요...", (wchar_t)k);
        status_message(msg);
        return 1;
    }
    return 0;
}

static void ui_message_loop(void) {
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (g_find_dlg && IsDialogMessageW(g_find_dlg, &msg)) {
            continue;
        }
        if (chord_key(&msg)) {
            continue;
        }
        if (g_accel && TranslateAcceleratorW(g_main, g_accel, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

/* ======================= main window ======================= */

static void ui_create(HWND hwnd) {
    static int registered;
    cc_init();
    if (!registered) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc = ViewProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"HangulPadView";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.style = CS_DBLCLKS;
        RegisterClassW(&wc);
        g_find_msg = RegisterWindowMessageW(FINDMSGSTRINGW);
        registered = 1;
    }
    g_dpi = window_dpi(hwnd);
    recent_load();
    SetMenu(hwnd, build_menus());
    g_tb_main = toolbar_create(hwnd, ID_TB_MAIN);
    g_tb_fmt = toolbar_create(hwnd, ID_TB_FMT);
    toolbars_rebuild();
    combos_create(hwnd);
    g_status = CreateWindowExW(0, STATUSCLASSNAMEW, NULL, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, hwnd,
                               (HMENU)(INT_PTR)ID_STATUS, GetModuleHandleW(NULL), NULL);
    g_paralist = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                 WS_CHILD | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
                                     LBS_EXTENDEDSEL,
                                 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_PARALIST, GetModuleHandleW(NULL), NULL);
    g_text = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VSCROLL | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN |
                                 ES_NOHIDESEL, /* no horizontal scroll: long paragraphs wrap */
                             0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_TEXT, GetModuleHandleW(NULL), NULL);
    SendMessageW(g_text, EM_SETLIMITTEXT, 16 * 1024 * 1024, 0);
    g_text_proc = (WNDPROC)SetWindowLongPtrW(g_text, GWLP_WNDPROC, (LONG_PTR)TextProc);
    g_view = CreateWindowExW(0, L"HangulPadView", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP,
                             0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_VIEW, GetModuleHandleW(NULL), NULL);
    if (!g_accel) {
        g_accel = build_accelerators();
    }
    apply_ui_fonts();
    {
        RECT r;
        GetClientRect(hwnd, &r);
        layout_children(r.right, r.bottom);
    }
    update_status();
    format_sync();
}

static void zoom_step(int up) {
    static const int steps[] = {25, 50, 75, 100, 125, 150, 200, 300, 400, 500};
    int z = view_zoom_percent(), i, next = up ? 500 : 25;
    for (i = 0; i < (int)(sizeof(steps) / sizeof(steps[0])); i++) {
        if (up && steps[i] > z + 2) {
            next = steps[i];
            break;
        }
        if (!up && steps[i] < z - 2) {
            next = steps[i];
        }
    }
    set_zoom(ZOOM_PERCENT, next);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_find_msg && g_find_msg) {
        find_message();
        return 0;
    }
    switch (msg) {
    case WM_CREATE:
        g_main = hwnd;
        ui_create(hwnd);
        /* queued so the window is fully shown first; a command-line document
         * replaces the empty new document */
        if (g_startup_path[0]) {
            PostMessageW(hwnd, WM_APP_OPEN_STARTUP, 0, 0);
        } else {
            PostMessageW(hwnd, WM_COMMAND, IDM_FILE_NEW, 0);
        }
        return 0;
    case WM_APP_OPEN_STARTUP:
        open_and_remember(hwnd, g_startup_path);
        if (!g_doc) {
            PostMessageW(hwnd, WM_COMMAND, IDM_FILE_NEW, 0); /* could not open: start with a new document */
        }
        return 0;
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lParam;
        RECT client;
        g_dpi = HIWORD(wParam) >= 48 ? HIWORD(wParam) : 96;
        apply_ui_fonts();
        toolbars_rebuild();
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        GetClientRect(hwnd, &client);
        layout_children(client.right, client.bottom);
        update_status();
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    case WM_SIZE:
        layout_children(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_ERASEBKGND: {
        /* also the background of the transparent (flat) toolbars */
        RECT r;
        GetClientRect(hwnd, &r);
        FillRect((HDC)wParam, &r, GetSysColorBrush(COLOR_BTNFACE));
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        FillRect(dc, &ps.rcPaint, GetSysColorBrush(COLOR_BTNFACE));
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SETFOCUS:
        if (g_editing ? g_text != NULL : g_view != NULL) {
            SetFocus(g_editing ? g_text : g_view);
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (g_view) {
            return SendMessageW(g_view, msg, wParam, lParam);
        }
        return 0;
    case WM_APP_CARET:
        format_sync();
        if (g_view && g_editing) {
            InvalidateRect(g_view, NULL, FALSE);
        }
        return 0;
    case WM_TIMER:
        if (wParam == TIMER_PREVIEW) {
            KillTimer(hwnd, TIMER_PREVIEW);
            if (g_editing) {
                preview_build();
            }
        }
        return 0;
    case WM_INITMENUPOPUP:
        if ((HMENU)wParam == g_recent_menu) {
            recent_menu_fill();
        }
        update_menu_state((HMENU)wParam);
        return 0;
    case WM_MENUSELECT:
        if (LOWORD(wParam) == IDM_TODO && !(HIWORD(wParam) & MF_POPUP)) {
            status_message(L"다음 버전에서 지원할 기능입니다.");
        } else if (HIWORD(wParam) == 0xFFFF && lParam == 0) {
            status_message(L"");
        }
        return 0;
    case WM_NOTIFY: {
        const NMHDR *nm = (const NMHDR *)lParam;
        if (nm->code == TTN_GETDISPINFOW) {
            return 0;
        }
        break;
    }
    case WM_CONTEXTMENU:
        if ((HWND)wParam == g_view) {
            return SendMessageW(g_view, msg, wParam, lParam);
        }
        break;
    case WM_COMMAND: {
        UINT id = LOWORD(wParam), code = HIWORD(wParam);
        if (id == ID_TEXT && code == EN_CHANGE && !g_loading) {
            g_dirty = 1;
            editor_title();
            schedule_preview(900);
            return 0;
        }
        if (id == ID_PARALIST && code == LBN_SELCHANGE) {
            int row = (int)SendMessageW(g_paralist, LB_GETCARETINDEX, 0, 0);
            if (editor_commit() && row != LB_ERR) {
                size_t item = (size_t)SendMessageW(g_paralist, LB_GETITEMDATA, row, 0);
                int changed = SendMessageW(g_paralist, LB_GETSEL, row, 0) > 0 && item != g_edit_index;
                if (changed) {
                    editor_select(item);
                    schedule_preview(250);
                }
            }
            format_sync();
            status_refresh();
            return 0;
        }
        if (id >= ID_CB_STYLE && id <= ID_CB_ZOOM) {
            if (code == CBN_SELENDOK || code == CBN_APPLY) {
                combo_apply((int)id, code == CBN_SELENDOK);
            }
            return 0;
        }
        if (id >= IDM_RECENT_BASE && id < IDM_RECENT_BASE + RECENT_MAX) {
            int k = (int)(id - IDM_RECENT_BASE);
            if (k < g_recent_count) {
                open_and_remember(hwnd, g_recent[k]);
            }
            return 0;
        }
        if (id >= IDM_COLOR_BASE && id < IDM_COLOR_BASE + 8) {
            set_color(CHORD_COLORS[id - IDM_COLOR_BASE]);
            return 0;
        }
        switch (id) {
        case IDM_FILE_NEW: editor_new(); break;
        case IDM_NEW_WINDOW: new_window(); break;
        case IDM_FILE_CLOSE:
            if (editor_discard()) {
                g_dirty = 0;
                editor_new();
            }
            break;
        case IDM_FILE_SAVE:
        case IDM_FILE_SAVE_AS:
        case IDM_EDIT_MODE:
            if (g_read_only) {
                MessageBoxW(hwnd, L"배포용 문서입니다. 읽을 수는 있지만 편집하거나 다른 형식으로 저장할 수 없습니다.",
                            L"한글패드", MB_ICONINFORMATION);
                break;
            }
            if (id == IDM_EDIT_MODE) {
                if (g_editing) {
                    editor_preview(); /* back to the page view with the edits applied */
                } else {
                    editor_mode(1);
                }
            } else {
                editor_save(id == IDM_FILE_SAVE_AS);
            }
            break;
        case IDM_PREVIEW: editor_preview(); break;
        case IDM_ADD_PARA:
            if (g_editing) {
                editor_add();
            }
            break;
        case IDM_DELETE_PARA: editor_remove(); break;
        case IDM_UNDO:
        case IDM_CUT:
        case IDM_COPY:
        case IDM_PASTE:
        case IDM_SELECT_ALL: {
            HWND e = focused_edit();
            if (e) {
                if (id == IDM_UNDO) {
                    SendMessageW(e, EM_UNDO, 0, 0);
                } else if (id == IDM_SELECT_ALL) {
                    SendMessageW(e, EM_SETSEL, 0, -1);
                } else {
                    SendMessageW(e, id == IDM_CUT ? WM_CUT : id == IDM_COPY ? WM_COPY : WM_PASTE, 0, 0);
                }
            }
            break;
        }
        case IDM_FILE_OPEN: file_open(hwnd); break;
        case IDM_FILE_OPEN_SAMPLE: open_sample(hwnd); break;
        case IDM_FILE_EXIT: SendMessageW(hwnd, WM_CLOSE, 0, 0); break;
        case IDM_DOC_INFO: document_info(); break;
        case IDM_PAGE_SETUP: page_setup_dialog(); break;
        case IDM_PRINT: print_document(); break;
        case IDM_FIND: find_open(0); break;
        case IDM_REPLACE: find_open(1); break;
        case IDM_FIND_NEXT: find_next((g_fr.Flags & FR_DOWN) || !g_fr.lStructSize, (g_fr.Flags & FR_MATCHCASE) != 0, 0); break;
        case IDM_FIND_PREV: find_next(0, (g_fr.Flags & FR_MATCHCASE) != 0, 0); break;
        case IDM_GOTO: goto_dialog(); break;
        case ID_BTN_PREV: goto_page(hwnd, g_page_index - 1); break;
        case ID_BTN_NEXT: goto_page(hwnd, g_page_index + 1); break;
        case IDM_FIRST_PAGE: goto_page(hwnd, 0); break;
        case IDM_LAST_PAGE: goto_page(hwnd, g_page_count - 1); break;
        case IDM_ZOOM_PAGE: set_zoom(ZOOM_FIT_PAGE, g_zoom); break;
        case IDM_ZOOM_WIDTH: set_zoom(ZOOM_FIT_WIDTH, g_zoom); break;
        case IDM_ZOOM_100: set_zoom(ZOOM_PERCENT, 100); break;
        case IDM_ZOOM_IN: zoom_step(1); break;
        case IDM_ZOOM_OUT: zoom_step(0); break;
        case IDM_TWO_PAGES:
            g_two_pages = !g_two_pages;
            set_zoom(g_zoom_mode, g_zoom);
            break;
        case IDM_SHOW_FMTBAR:
        case IDM_SHOW_STATUS: {
            RECT r;
            if (id == IDM_SHOW_FMTBAR) {
                g_show_fmtbar = !g_show_fmtbar;
            } else {
                g_show_status = !g_show_status;
            }
            GetClientRect(hwnd, &r);
            layout_children(r.right, r.bottom);
            InvalidateRect(hwnd, NULL, TRUE);
            break;
        }
        case IDM_CHARMAP: ShellExecuteW(hwnd, L"open", L"charmap.exe", NULL, NULL, SW_SHOWNORMAL); break;
        case IDM_INSERT_DATE: insert_date(); break;
        case IDM_CHAR_SHAPE: char_shape_dialog(); break;
        case IDM_PARA_SHAPE: para_shape_dialog(); break;
        case IDM_BOLD: toggle_char(HP_CF_BOLD); break;
        case IDM_ITALIC: toggle_char(HP_CF_ITALIC); break;
        case IDM_UNDERLINE: toggle_char(HP_CF_UNDERLINE); break;
        case IDM_STRIKE: toggle_char(HP_CF_STRIKEOUT); break;
        case IDM_COLOR: pick_color(); break;
        case IDM_SIZE_UP: step_format(1, 0); break;
        case IDM_SIZE_DOWN: step_format(-1, 0); break;
        case IDM_SPACING_UP: step_format(0, 10); break;
        case IDM_SPACING_DOWN: step_format(0, -10); break;
        case IDM_ALIGN_JUSTIFY: set_align(HP_ALIGN_JUSTIFY); break;
        case IDM_ALIGN_LEFT: set_align(HP_ALIGN_LEFT); break;
        case IDM_ALIGN_CENTER: set_align(HP_ALIGN_CENTER); break;
        case IDM_ALIGN_RIGHT: set_align(HP_ALIGN_RIGHT); break;
        case IDM_ALIGN_DISTRIBUTE: set_align(HP_ALIGN_DISTRIBUTE); break;
        case IDM_SHORTCUTS: shortcut_list(); break;
        case IDM_HELP_ABOUT: about(hwnd); break;
        case IDM_FONT_REPORT: font_report(hwnd); break;
        }
        return 0;
    }
    case WM_CLOSE:
        if (editor_discard()) {
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_PREVIEW);
        if (g_find_dlg) {
            DestroyWindow(g_find_dlg);
            g_find_dlg = NULL;
        }
        hp_editor_free(g_editor);
        g_editor = NULL;
        free_doc();
        free(g_page_rects);
        g_page_rects = NULL;
        g_page_rect_count = 0;
        g_view = NULL;
        g_tb_main = g_tb_fmt = NULL;
        g_status = NULL;
        if (g_images) {
            cc_imagelist_destroy(g_images);
            g_images = NULL;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
