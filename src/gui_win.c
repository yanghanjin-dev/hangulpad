/*
 * HangulPad Windows GUI — HWP/HWPX engine layout viewer and paragraph editor.
 * Renders paragraphs, tables (incl. nested), images, floats, containers, equations.
 *
 * High DPI (0.9.1): the process is per-monitor DPI aware. Toolbar, controls and
 * UI fonts are scaled with S(); the document is laid out in 96-dpi page pixels
 * and painted straight into a bitmap of the on-screen size through a GDI mapping
 * mode, so text and lines stay sharp at any display scale or window size.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include "hp/hwpx.h"
#include "hp/hwp.h"
#include "hp/edit.h"
#include "hp/ir.h"
#include "hp/version.h"
#include "line_layout.h"
#include "pagination.h"
#include "table_pagination.h"

#include <windows.h>
#include <shellapi.h>
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
#define IDM_FILE_NEW 40020
#define IDM_FILE_SAVE 40021
#define IDM_FILE_SAVE_AS 40022
#define IDM_EDIT_MODE 40023
#define IDM_PREVIEW 40024
#define IDM_ADD_PARA 40025
#define IDM_DELETE_PARA 40026
#define IDM_UNDO 40027
#define IDM_CUT 40028
#define IDM_COPY 40029
#define IDM_PASTE 40030
#define IDM_SELECT_ALL 40031
#define ID_PARALIST 1100
#define ID_TEXT 1101
#define IDM_FILE_OPEN 40002
#define IDM_FILE_OPEN_SAMPLE 40003
#define IDM_FILE_EXIT 40005
#define IDM_HELP_ABOUT 40010
#define IDM_FONT_REPORT 40011

/* Display DPI of the main window (96 = 100%). Page geometry stays in 96-dpi
 * pixels; only the UI and the final page bitmap follow the display. */
static int g_dpi = 96;
static HFONT g_ui_font, g_edit_font;
static int S(int v) {
    return MulDiv(v, g_dpi, 96);
}
#define FONT_SCALE 8
static int g_paper_width = 794, g_paper_height = 1123;
static size_t g_layout_section;
/* Glyph boxes recorded while a page is drawn (0.11.0): page clicks find the
 * source paragraph and character, and the caret/selection is drawn on them. */
typedef struct { size_t section; uint32_t source_p; size_t ci, len; double x0, x1; int y0, y1; } hp_hit;
typedef struct { hp_hit *v; size_t n, cap; } hp_hit_map;
static hp_hit_map *g_hit;
static void hit_add(const hp_paragraph *para, size_t ci, size_t len, double x0, double x1, int y0, int y1) {
    hp_hit *h;
    if (!g_hit || !para->source_p) {
        return;
    }
    /* the paragraph number is not part of the edited text (0.12.0) */
    if (ci < para->generated_prefix) {
        if (len) {
            return;
        }
        ci = para->generated_prefix;
    }
    ci -= para->generated_prefix;
    if (g_hit->n == g_hit->cap) {
        size_t cap = g_hit->cap ? g_hit->cap * 2 : 1024;
        h = (hp_hit *)realloc(g_hit->v, cap * sizeof(*h));
        if (!h) {
            return;
        }
        g_hit->v = h;
        g_hit->cap = cap;
    }
    h = &g_hit->v[g_hit->n++];
    h->section = g_layout_section;
    h->source_p = para->source_p;
    h->ci = ci;
    h->len = len;
    h->x0 = x0;
    h->x1 = x1;
    h->y0 = y0;
    h->y1 = y1;
}
#define PAGE_W_PX g_paper_width
#define PAGE_H_PX g_paper_height
#define MARGIN_X 56
#define MARGIN_Y 64

typedef struct {
    size_t sec;
    size_t block;
} flat_ref;

typedef struct {
    int first;
    int count;
    size_t section;
    int printed_number,number_hidden,number_position;
    char number_side[16];
} layout_page;
typedef struct { int block, column, y, offset, height, header_height; } layout_piece;
static int g_unsafe_table_cuts, g_suppressed_headers;

static HWND g_main;
static HWND g_status;
static hp_document *g_doc;
static hp_editor *g_editor;
static HWND g_paralist,g_text,g_view;
static size_t g_edit_index=(size_t)-1;
static wchar_t g_path[MAX_PATH];
/* The open file is HWP 5.x: saving writes HWPX to a new .hwpx file, never over the .hwp. */
static int g_source_hwp;
/* Document named on the command line; opened once the window exists. */
static wchar_t g_startup_path[MAX_PATH];
#define WM_APP_OPEN_STARTUP (WM_APP + 1)
/* Distribution-only (배포용) HWP: shown, but editing and saving are disabled. */
static int g_read_only;
static int g_dirty,g_loading,g_editing;
static int editor_discard(void);
static void editor_list(size_t selected);
static void editor_title(void);
static void editor_mode(int editing);
static void open_document(HWND hwnd,const wchar_t *path);
/* gui_ui.inc */
static void update_status(void);
static void status_refresh(void);
static void status_message(const wchar_t *text);
static void format_sync(void);
static int preview_build(void);
static void schedule_preview(int delay_ms);
static void recent_add(const wchar_t *path);
static void open_and_remember(HWND hwnd, const wchar_t *path);
static flat_ref *g_flat;
static int g_flat_capacity;
static int g_flat_count;
static layout_page *g_pages;
static int g_page_capacity;
static layout_piece *g_pieces;
static int g_piece_count, g_piece_capacity;
static int g_page_count;
/* Footnotes placed by rebuild_pages (0.12.0): page and height of each, the
 * space each page keeps for them, and each block's first page (headers and
 * footers apply from the page of their control). */
typedef struct { const hp_subtext *sub; size_t section; int page, height; } note_ref;
static note_ref *g_notes;
static int g_note_count, g_note_capacity;
static int *g_page_reserve, g_reserve_capacity;
static int *g_block_page, g_block_page_capacity;
static int g_page_index;
static IWICImagingFactory *g_wic;
static struct { LOGFONTW lf; HFONT font; } g_regular_fonts[64];
static int g_regular_font_count;
/* Unique substitutions, collected by both measurement and painting. */
static struct { wchar_t source[LF_FACESIZE], target[LF_FACESIZE]; unsigned code; } g_font_changes[256];
static int g_font_change_count, g_font_changes_truncated;

static void font_report(HWND hwnd) {
    wchar_t message[8192]; int groups[256],counts[256]={0};
    int total=0,i,j,page;
    for(i=0;i<g_font_change_count;i++) {
        for(j=0;j<total;j++)if(wcscmp(g_font_changes[groups[j]].source,g_font_changes[i].source)==0 &&
            wcscmp(g_font_changes[groups[j]].target,g_font_changes[i].target)==0)break;
        if(j==total)groups[total++]=i;
        counts[j]++;
    }
    for(page=0;page==0 || page<total;page+=8) {
        size_t used=(size_t)swprintf(message,8192,L"원본 글꼴 → 실제 사용한 대체 글꼴\n\n");
        int end=page+8<total ? page+8 : total;
        for(j=page;j<end;j++) {
            int examples=0;
            i=groups[j];
            used+=(size_t)swprintf(message+used,8192-used,L"%ls → %ls\n  서로 다른 문자 %d개 · 예: ",
                g_font_changes[i].source,g_font_changes[i].target,counts[j]);
            for(i=0;i<g_font_change_count && examples<3;i++) {
                if(wcscmp(g_font_changes[i].source,g_font_changes[groups[j]].source)!=0 ||
                    wcscmp(g_font_changes[i].target,g_font_changes[groups[j]].target)!=0)continue;
                used+=(size_t)swprintf(message+used,8192-used,L"%lsU+%04X",examples ? L", " : L"",g_font_changes[i].code);
                examples++;
            }
            used+=(size_t)swprintf(message+used,8192-used,L"\n\n");
        }
        if(!total)wcscat(message,L"확인된 대체 글꼴이 없습니다.\n\n");
        if(g_font_changes_truncated)wcscat(message,L"처음 256개 문자 내역을 집계했습니다.\n");
        wcscat(message,L"현재까지 레이아웃·표시한 문자의 내역입니다.\n원본 글꼴이 설치되지 않은 경우 해당 글꼴의 문자 전체가 대체됩니다.");
        if(end<total)wcscat(message,L"\n\n확인: 다음 내역 / 취소: 닫기");
        if(MessageBoxW(hwnd,message,L"대체 글꼴 내역",end<total ? MB_OKCANCEL : MB_OK)==IDCANCEL || end==total)break;
    }
}

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

static int CALLBACK font_found(const LOGFONTW *font, const TEXTMETRICW *metrics, DWORD type, LPARAM value) {
    (void)font; (void)metrics; (void)type;
    *(int *)value = 1;
    return 0;
}
static int map_face(HDC hdc, const char *name, wchar_t *out, size_t count) {
    wchar_t *native = utf8_to_wide(name ? name : "");
    LOGFONTW query = {0};
    static wchar_t families[128][LF_FACESIZE];
    static int present[128], cached;
    int found = 0, cached_index = -1, fi;
    query.lfCharSet = DEFAULT_CHARSET;
    if (native && native[0]) {
        wcsncpy(query.lfFaceName, native, LF_FACESIZE - 1);
        for(fi=0;fi<cached;fi++)if(wcscmp(families[fi],query.lfFaceName)==0) {cached_index=fi;break;}
        if(cached_index>=0)found=present[cached_index];
        else {
            EnumFontFamiliesExW(hdc, &query, font_found, (LPARAM)&found, 0);
            if(cached<128) {wcscpy(families[cached],query.lfFaceName);present[cached++]=found;}
        }
    }
    wcsncpy(out, found ? native : L"Malgun Gothic", count - 1);
    out[count - 1] = 0;
    free(native);
    return found;
}

static int hwp_px_i(int32_t hwp) {
    if (hwp >= 0) {
        return (int)hp_hwp_to_px((uint32_t)hwp);
    }
    return -(int)hp_hwp_to_px((uint32_t)(-hwp));
}

static HFONT make_font(HDC hdc, const hp_run *run) {
    wchar_t face[64];
    LOGFONTW lf;
    int size_hwp = (int)(run->font_size_hwp ? run->font_size_hwp : 1000);
    int dpi = 96; /* The page bitmap and HWPUNIT geometry always use 96 dpi. */
    if(size_hwp<600)size_hwp=600;
    if(size_hwp>7200)size_hwp=7200;
    if (dpi <= 0) {
        dpi = 96;
    }
    map_face(hdc, run->font_face, face, 64);
    ZeroMemory(&lf, sizeof(lf));
    /* Measure at subpixel precision; rounding every glyph to an integer pixel
     * accumulates enough error to wrap saved lines and expand table rows. */
    lf.lfHeight = -MulDiv(size_hwp, dpi*FONT_SCALE, 7200);
    lf.lfWeight = run->bold ? FW_BOLD : FW_NORMAL;
    lf.lfItalic=(BYTE)run->italic;lf.lfUnderline=FALSE; /* underline follows full glyph advances, including spaces */
    lf.lfStrikeOut=(BYTE)run->strikeout;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = ANTIALIASED_QUALITY; /* neutral page pixels, without LCD color fringes */
    wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    return CreateFontIndirectW(&lf);
}

static int selected_font_has_text(HDC dc,const wchar_t *text,int length) {
    WORD indices[2]={0xffff,0xffff}; int i;
    if(length<1 || length>2 || GetGlyphIndicesW(dc,text,length,indices,GGI_MARK_NONEXISTING_GLYPHS)==GDI_ERROR)return 0;
    for(i=0;i<length;i++)if(indices[i]==0xffff || indices[i]==0)return 0;
    return 1;
}

static int glyph_language(wchar_t c);
static HFONT font_for_glyph(HDC dc,const hp_run *run,const wchar_t *text,int length) {
    static const wchar_t *candidates[]={L"Malgun Gothic",L"Segoe UI Symbol",L"Segoe UI Emoji",L"Cambria Math",L"Yu Gothic",L"Microsoft YaHei",L"Arial"};
    hp_run styled=*run;
    int lang=glyph_language(text[0]),substituted=0;
    wchar_t requested[LF_FACESIZE];
    HFONT font,old;
    wchar_t actual[LF_FACESIZE]={0}; wchar_t *source;
    if(run->script_faces[lang][0])styled.font_face=(char *)run->script_faces[lang];
    source=utf8_to_wide(styled.font_face);
    substituted=!map_face(dc,styled.font_face,requested,LF_FACESIZE);
    font=make_font(dc,&styled);old=(HFONT)SelectObject(dc,font);
    unsigned code=(unsigned)text[0]; int i;
    if(length==2 && text[0]>=0xd800 && text[0]<=0xdbff && text[1]>=0xdc00 && text[1]<=0xdfff)
        code=0x10000+((text[0]-0xd800)<<10)+(text[1]-0xdc00);
    if(!selected_font_has_text(dc,text,length)) {
        LOGFONTW lf; GetObjectW(font,sizeof(lf),&lf);
        for(i=0;i<(int)(sizeof(candidates)/sizeof(candidates[0]));i++) {
            HFONT candidate; wchar_t resolved[LF_FACESIZE]={0};
            wcscpy(lf.lfFaceName,candidates[i]);lf.lfCharSet=DEFAULT_CHARSET;
            candidate=CreateFontIndirectW(&lf);if(!candidate)continue;
            SelectObject(dc,candidate);GetTextFaceW(dc,LF_FACESIZE,resolved);
            /* GDI may return a localized family name (e.g. 맑은 고딕).
             * Coverage of the selected physical font is the deciding factor. */
            if(selected_font_has_text(dc,text,length)) {
                DeleteObject(font);font=candidate;substituted=1;break;
            }
            SelectObject(dc,font);DeleteObject(candidate);
        }
    }
    GetTextFaceW(dc,LF_FACESIZE,actual);
    if(substituted && source && source[0] && _wcsicmp(source,actual)!=0) {
        for(i=0;i<g_font_change_count;i++)if(g_font_changes[i].code==code &&
            wcscmp(g_font_changes[i].source,source)==0 && wcscmp(g_font_changes[i].target,actual)==0)break;
        if(i==g_font_change_count) {
            if(i<256) {
                wcsncpy(g_font_changes[i].source,source,LF_FACESIZE-1);
                wcscpy(g_font_changes[i].target,actual);g_font_changes[i].code=code;g_font_change_count++;
            } else g_font_changes_truncated=1;
        }
    }
    free(source);SelectObject(dc,old);return font;
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
            if (g_flat_count == g_flat_capacity) {
                int capacity = g_flat_capacity ? g_flat_capacity*2 : 256;
                flat_ref *next = (flat_ref *)realloc(g_flat, (size_t)capacity*sizeof(*next));
                if (!next) { g_flat_count=0; MessageBoxW(g_main,L"문서 레이아웃 메모리가 부족합니다.",L"한글패드",MB_ICONERROR); return; }
                g_flat=next; g_flat_capacity=capacity;
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

static int layout_saved_para(HDC hdc, const hp_paragraph *para, int left, int right,
                             int base, int draw, int *breaks, int capacity, int *hard_breaks, hp_cut_map *ink, int ink_origin);

static int layout_text_para(HDC hdc, const hp_paragraph *para, int left, int right, int base, int draw, int *ends, int capacity, int *hard, hp_cut_map *ink, int ink_origin);
static void draw_para(HDC hdc, const hp_paragraph *para, int left, int right, int *y);
static void draw_block(HDC hdc, const hp_block *b, int left, int right, int *y);
static int measure_para_height(HDC hdc, const hp_paragraph *para, int content_w) {
    if (!para) return 8;
    if(para->object_anchor_only)return 0;
    return hwp_px_i(para->space_before_hwp) + layout_text_para(hdc,para,0,content_w,0,0,NULL,0,NULL,NULL,0)
        + hwp_px_i(para->space_after_hwp);
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
    hr = conv->lpVtbl->Initialize(conv, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppPBGRA,
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
    int extra = 0;
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
    if (img->has_pos && !img->pos.treat_as_char) {
        int vo = (int)hp_hwp_to_px((uint32_t)(img->pos.vert_offset_hwp < 0 ? 0 : img->pos.vert_offset_hwp));
        if (img->pos.vert_offset_hwp > 0) {
            extra += vo;
        }
    }
    return h + extra;
}

static int measure_equation_height(const hp_equation *eq) {
    (void)eq;
    return 28;
}

static int measure_container_height(const hp_container *c, int content_w) {
    size_t i;
    int h = 0;
    int declared;
    if (!c) {
        return 8;
    }
    declared = (int)hp_hwp_to_px(c->height_hwp);
    for (i = 0; i < c->image_count; i++) {
        int ih = measure_image_height(c->images[i], content_w);
        if (ih > h) {
            h = ih;
        }
    }
    if (declared > h) {
        h = declared;
    }
    return h;
}

/* HWP cell margins may encode -1 (unspecified) as UINT32_MAX. */
static int padding_px(uint32_t value) {
    return (int32_t)value > 0 ? hp_line_px((int32_t)value) : 0;
}
static int table_geometry(HDC hdc, const hp_table *t, int content_w, int **columns, int **heights);
static int measure_table_height(HDC hdc, const hp_table *t, int content_w) {
    int *columns = NULL, *heights = NULL;
    int height = table_geometry(hdc, t, content_w, &columns, &heights);
    free(columns); free(heights);
    return height;
}
/* Final line spacing advances to a following line; it is not bottom padding
 * inside a cell. Keep real glyph ink if a substitute font is taller. */
/* Line box of a paragraph without saved lines, as 한글 computes it:
 * the largest character size, plus spacing = size * (percent - 100). */
static uint32_t relaid_line_size_hwp(const hp_paragraph *para) {
    uint32_t size=0;size_t ri;
    for(ri=0;ri<para->run_count;ri++)
        if(para->runs[ri].text && para->runs[ri].text[0] && para->runs[ri].font_size_hwp>size)size=para->runs[ri].font_size_hwp;
    if(!size)size=para->run_count && para->runs[0].font_size_hwp ? para->runs[0].font_size_hwp
                 : para->mark_size_hwp ? para->mark_size_hwp : 1000;
    return size>409600 ? 409600 : size;
}
static uint32_t relaid_line_spacing_hwp(const hp_paragraph *para) {
    int pct=para->line_spacing_percent>0 ? para->line_spacing_percent : 160;
    return pct>100 ? (uint32_t)((uint64_t)relaid_line_size_hwp(para)*(uint32_t)(pct-100)/100) : 0;
}
static int cell_tail_spacing(HDC dc,const hp_table_cell *cell,int width) {
    const hp_paragraph *p;hp_cut_map ink={0};size_t i;int full,trim,ink_bottom=0;
    if(!cell->paragraph_count)return 0;
    p=&cell->paragraphs[cell->paragraph_count-1];
    if(p->object_anchor_only)return 0;
    for(i=0;i<cell->nested_table_count;i++)if(!cell->nested_before_paragraph ||
        cell->nested_before_paragraph[i]>=cell->paragraph_count)return 0;
    /* the last line's spacing below the text: saved, or the re-laid-out line box */
    trim=hwp_px_i(p->seg_count ? (int32_t)p->segs[p->seg_count-1].spacing_hwp : (int32_t)relaid_line_spacing_hwp(p));
    if(trim<0)trim=0;
    full=layout_text_para(dc,p,0,width,0,0,NULL,0,NULL,&ink,0);
    if(ink.failed) {free(ink.bands);return 0;}
    for(i=0;i<ink.count;i++)if(ink.bands[i].bottom>ink_bottom)ink_bottom=ink.bands[i].bottom;
    free(ink.bands);
    if(trim>full-ink_bottom)trim=full-ink_bottom;
    return trim>0 ? trim : 0; /* ink below the line box must not grow the cell */
}
static int cell_content_height(HDC hdc,const hp_table_cell *cell,int inner) {
    int content=0;size_t pi;
    if(cell->image)content+=measure_image_height(cell->image,inner);
    if(cell->container)content+=measure_container_height(cell->container,inner);
    if(cell->equation)content+=measure_equation_height(cell->equation);
    for(pi=0;pi<cell->paragraph_count;pi++)content+=measure_para_height(hdc,&cell->paragraphs[pi],inner);
    for(pi=0;pi<cell->nested_table_count;pi++) {
        size_t at=cell->nested_before_paragraph ? cell->nested_before_paragraph[pi] : cell->paragraph_count;
        if(at>=cell->paragraph_count || !cell->paragraphs[at].inline_prefix_width_hwp)
            content+=measure_table_height(hdc,cell->nested_tables[pi],inner);
    }
    return content-cell_tail_spacing(hdc,cell,inner);
}
static int cell_vertical_offset(HDC dc,const hp_table_cell *cell,int inner,int height) {
    int spare;
    if(!cell->vertical_align)return 0;
    spare=height-padding_px(cell->pad_top_hwp)-padding_px(cell->pad_bottom_hwp)-cell_content_height(dc,cell,inner);
    return spare>0 ? (cell->vertical_align==1 ? spare/2 : spare) : 0;
}
static int table_geometry(HDC hdc, const hp_table *t, int content_w, int **columns, int **heights) {
    int rows, cols, table_w, sum = 0, total = 0, pass, r, c;
    size_t i;
    int *cw, *rh;
    if (!t || t->row_count < 1 || t->col_count < 1 || t->row_count > 4096 || t->col_count > 4096) return 20;
    rows = t->row_count; cols = t->col_count;
    table_w = t->width_hwp ? hp_line_px(t->width_hwp) : content_w;
    if (table_w > content_w) table_w = content_w;
    if (table_w < cols) table_w = cols;
    cw = (int *)calloc((size_t)cols, sizeof(int));
    rh = (int *)calloc((size_t)rows, sizeof(int));
    if (!cw || !rh) { free(cw); free(rh); return 20; }
    for (i=0; i<t->cell_count; i++) {
        const hp_table_cell *cell = &t->cells[i];
        if (cell->col >= 0 && cell->col < cols && cell->colspan == 1) {
            int width = hp_line_px(cell->width_hwp);
            if (width > cw[cell->col]) cw[cell->col] = width;
        }
    }
    /* Merged cells constrain the sum of their columns. Resolve missing columns
     * from those constraints before using an equal-width fallback. */
    for(pass=0;pass<cols;pass++) {
        int changed=0;
        for(i=0;i<t->cell_count;i++) {
            const hp_table_cell *cell=&t->cells[i];int known=0,missing=0,only=-1,end=cell->col+cell->colspan;
            if(cell->col<0 || end>cols || cell->colspan<1 || !cell->width_hwp)continue;
            for(c=cell->col;c<end;c++){known+=cw[c];if(!cw[c]){missing++;only=c;}}
            if(missing==1 && hp_line_px(cell->width_hwp)>known){cw[only]=hp_line_px(cell->width_hwp)-known;changed=1;}
        }
        if(!changed)break;
    }
    for (c=0; c<cols; c++) { if (!cw[c]) cw[c] = table_w/cols; sum += cw[c]; }
    if (sum > 0) {
        int source=0,previous=0;
        for(c=0;c<cols;c++){int edge;source+=cw[c];edge=MulDiv(source,table_w,sum);cw[c]=edge-previous;previous=edge;}
    }
    for (r=0; r<rows; r++) rh[r] = 1;
    /* Nonspanning rows first; spanning cells expand only the missing height. */
    for (pass=0; pass<2; pass++) for (i=0; i<t->cell_count; i++) {
        const hp_table_cell *cell = &t->cells[i];
        int width = 0, height, inner, bottom, existing = 0, content = 0;
        if (cell->row < 0 || cell->row >= rows || cell->col < 0 || cell->col >= cols) continue;
        if ((cell->rowspan > 1) != (pass == 1)) continue;
        for (c=cell->col; c<cols && c<cell->col+(cell->colspan > 0 ? cell->colspan : 1); c++) width += cw[c];
        inner = width - padding_px(cell->pad_left_hwp) - padding_px(cell->pad_right_hwp);
        if (inner < 1) inner = 1;
        content = padding_px(cell->pad_top_hwp) + padding_px(cell->pad_bottom_hwp);
        content+=cell_content_height(hdc,cell,inner);
        height = hp_line_px(cell->height_hwp);
        if (content > height) height = content;
        if (height < 1) height = 1;
        bottom = cell->row + (cell->rowspan > 0 ? cell->rowspan : 1);
        if (bottom > rows) bottom = rows;
        for (r=cell->row; r<bottom; r++) existing += rh[r];
        if (height > existing) {
            int missing=height-existing,nrows=bottom-cell->row;
            for(r=cell->row;r<bottom;r++)rh[r]+=missing/nrows+(r-cell->row<missing%nrows);
        }
    }
    for (r=0; r<rows; r++) total += rh[r];
    *columns = cw; *heights = rh;
    return total;
}

/* Occupied line/object bands are collected in the same coordinates used for
 * drawing, including every column of a merged cell and nested table. */
static int collect_table_ink(HDC hdc,const hp_table *table,int width,int base,hp_cut_map *ink,int depth) {
    int *columns=NULL,*rows=NULL,height,r;
    size_t ci,pi;
    if(depth>128) {ink->failed=1;return 0;}
    height=table_geometry(hdc,table,width,&columns,&rows);
    if(!columns || !rows) {free(columns);free(rows);return height;}
    for(ci=0;ci<table->cell_count;ci++) {
        const hp_table_cell *cell=&table->cells[ci];
        int y=base,inner=0;
        if(cell->row<0 || cell->row>=table->row_count || cell->col<0 || cell->col>=table->col_count)continue;
        for(r=0;r<cell->row;r++)y+=rows[r];
        for(r=cell->col;r<table->col_count && r<cell->col+(cell->colspan>0 ? cell->colspan : 1);r++)inner+=columns[r];
        inner-=padding_px(cell->pad_left_hwp)+padding_px(cell->pad_right_hwp);
        if(inner<1)inner=1;
        {
            int height=0;
            for(r=cell->row;r<table->row_count && r<cell->row+(cell->rowspan>0 ? cell->rowspan : 1);r++)height+=rows[r];
            y+=padding_px(cell->pad_top_hwp)+cell_vertical_offset(hdc,cell,inner,height);
        }
        if(cell->image) {int h=measure_image_height(cell->image,inner);hp_cut_add(ink,y,y+h);y+=h;}
        if(cell->container) {int h=measure_container_height(cell->container,inner);hp_cut_add(ink,y,y+h);y+=h;}
        if(cell->equation) {int h=measure_equation_height(cell->equation);hp_cut_add(ink,y,y+h);y+=h;}
        for(pi=0;pi<cell->paragraph_count;pi++) {
            const hp_paragraph *para=&cell->paragraphs[pi];size_t ni;
            for(ni=0;ni<cell->nested_table_count;ni++)if(cell->nested_before_paragraph && cell->nested_before_paragraph[ni]==pi)
            {
                int nh=collect_table_ink(hdc,cell->nested_tables[ni],inner,y,ink,depth+1);
                if(!para->inline_prefix_width_hwp)y+=nh;
            }
            if(para->object_anchor_only)continue;
            y+=hwp_px_i(para->space_before_hwp);
            y+=layout_text_para(hdc,para,0,inner,0,0,NULL,0,NULL,ink,y);
            y+=hwp_px_i(para->space_after_hwp);
        }
        for(pi=0;pi<cell->nested_table_count;pi++)if(!cell->nested_before_paragraph || cell->nested_before_paragraph[pi]>=cell->paragraph_count)y+=collect_table_ink(hdc,cell->nested_tables[pi],inner,y,ink,depth+1);
    }
    free(columns);free(rows);return height;
}
static hp_slice take_table_slice(hp_flow *flow,int offset,int height,const int *ends,int count,
                                const hp_cut_map *ink,int header,int previous_page,int previous_column,
                                int *reserved) {
    hp_flow initial=*flow,trial;
    hp_slice slice;
    int cut,attempt=0,allow_header=1;
retry:
    trial=initial;
    slice=hp_flow_take(&trial,offset,height,ends,count);
    *reserved=0;
    if(offset==0 && header>0 && slice.height<=header && initial.y>0) {
        hp_flow_break(&initial,0);goto retry;
    }
    if(allow_header && offset>=header && offset>0 && header>0 && header<flow->height &&
       (slice.page!=previous_page || slice.column!=previous_column)) {
        hp_flow body={slice.page,slice.column,0,flow->columns,flow->height-header};
        slice=hp_flow_take(&body,offset,height,ends,count);
        slice.y+=header;
        trial=body;trial.height=flow->height;trial.y+=header;
        *reserved=header;
    }
    cut=hp_cut_before(ink,offset,offset+slice.height);
    if(offset==0 && header>0 && cut<=header && slice.height>header) {
        if(slice.y>0) {
            initial=(hp_flow){slice.page,slice.column,slice.y,flow->columns,flow->height};
            hp_flow_break(&initial,0);goto retry;
        }
        /* If an atomic object itself is taller than the page, do not waste a
         * page displaying only its title band. Keep the explicit fallback. */
        cut=offset+slice.height;g_unsafe_table_cuts++;
    }
    if(cut<=offset && *reserved) {
        /* A very large header must not starve the body or cause endless pages. */
        initial=(hp_flow){slice.page,slice.column,0,flow->columns,flow->height};
        allow_header=0;g_suppressed_headers++;goto retry;
    }
    if(cut<=offset && slice.y>0 && attempt++<2) {
        initial=(hp_flow){slice.page,slice.column,slice.y,flow->columns,flow->height};
        hp_flow_break(&initial,0);goto retry;
    }
    if(cut>offset && cut<offset+slice.height) {
        trial.y-=offset+slice.height-cut;slice.height=cut-offset;
    } else if(cut<=offset) {
        /* An object/overlapping ink band taller than a full page has no common
         * safe cut. Keep a contiguous continuation rather than dropping data. */
        g_unsafe_table_cuts++;
    }
    *flow=trial;return slice;
}

/* The endnote separator above the first endnote (0.12.1): space above the
 * line, the line (a third of the width), space below, as 한글's defaults. */
#define ENDNOTE_ABOVE_HWP 850
#define ENDNOTE_BELOW_HWP 567
static int measure_block_content(HDC hdc, const hp_block *b, int content_w);
static int measure_block_height(HDC hdc, const hp_block *b, int content_w) {
    return measure_block_content(hdc, b, content_w) +
           (b && b->note_separator ? hwp_px_i(ENDNOTE_ABOVE_HWP + ENDNOTE_BELOW_HWP) : 0);
}
static int measure_block_content(HDC hdc, const hp_block *b, int content_w) {
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
    if (b->kind == HP_BLOCK_EQUATION && b->equation) {
        return measure_equation_height(b->equation);
    }
    if (b->kind == HP_BLOCK_CONTAINER && b->container) {
        return measure_container_height(b->container, content_w);
    }
    return 8;
}

static void page_metrics(int *out_ml, int *out_mr, int *out_mt, int *out_mb, int *out_cw, int *out_ch) {
    int ml = MARGIN_X, mr = MARGIN_X, mt = MARGIN_Y, mb = MARGIN_Y;
    if (g_doc && g_doc->section_count > 0) {
        const hp_page *pg = &g_doc->sections[g_layout_section].page;
        ml=padding_px(pg->margin_left);
        mr=padding_px(pg->margin_right);
        mt=padding_px(pg->margin_top)+padding_px(pg->header_height);
        mb=padding_px(pg->margin_bottom)+padding_px(pg->footer_height);
    }
    if (out_ml) {
        *out_ml = ml;
    }
    if (out_mr) {
        *out_mr = mr;
    }
    if (out_mt) {
        *out_mt = mt;
    }
    if (out_mb) {
        *out_mb = mb;
    }
    if (out_cw) {
        *out_cw = PAGE_W_PX - ml - mr;
    }
    if (out_ch) {
        *out_ch = PAGE_H_PX - mt - mb;
    }
}

static void use_section(size_t section) {
    const hp_page *page = &g_doc->sections[section].page;
    g_layout_section = section;
    g_paper_width = page->width ? hp_line_px(page->width) : 794;
    g_paper_height = page->height ? hp_line_px(page->height) : 1123;
    if (g_paper_width < 100) g_paper_width = 100;
    if (g_paper_height < 100) g_paper_height = 100;
}
static int append_piece(hp_slice slice, int block, size_t section) {
    if (slice.page >= g_page_capacity) {
        int capacity = g_page_capacity ? g_page_capacity*2 : 32;
        layout_page *next;
        if (capacity <= slice.page) capacity = slice.page+1;
        next = (layout_page *)realloc(g_pages,(size_t)capacity*sizeof(*next));
        if (!next) return 0;
        g_pages=next;g_page_capacity=capacity;
    }
    if (slice.page >= g_page_count) {
        layout_page *page=&g_pages[slice.page];
        page->first=g_piece_count;page->count=0;page->section=section;
        g_page_count=slice.page+1;
    }
    if (g_piece_count == g_piece_capacity) {
        int capacity=g_piece_capacity ? g_piece_capacity*2 : 256;
        layout_piece *next=(layout_piece *)realloc(g_pieces,(size_t)capacity*sizeof(*next));
        if (!next) return 0;
        g_pieces=next;g_piece_capacity=capacity;
    }
    g_pieces[g_piece_count++]=(layout_piece){block,slice.column,slice.y,slice.offset,slice.height,0};
    g_pages[slice.page].count++;
    if(block>=0 && block<g_block_page_capacity && g_block_page[block]<0)g_block_page[block]=slice.page;
    return 1;
}

/* ---- footnotes, headers and footers (0.12.0) ---- */

static int reserve_at(int page) {
    if(page>=g_reserve_capacity) {
        int capacity=g_reserve_capacity ? g_reserve_capacity*2 : 64,*next;
        while(capacity<=page)capacity*=2;
        next=(int *)realloc(g_page_reserve,(size_t)capacity*sizeof(*next));
        if(!next)return 0;
        memset(next+g_reserve_capacity,0,(size_t)(capacity-g_reserve_capacity)*sizeof(*next));
        g_page_reserve=next;g_reserve_capacity=capacity;
    }
    return g_page_reserve[page];
}

static int subtext_height(HDC hdc,const hp_subtext *sub,int width) {
    size_t i;int h=0;
    for(i=0;i<sub->block_count;i++)h+=measure_block_height(hdc,&sub->blocks[i],width);
    return h;
}

/* Footnotes whose reference is in flat block `flat` go to `page`: returns
 * the space they add there (separator first). */
static int notes_space(HDC hdc,int flat,int width,int page,int place) {
    const hp_section *sec;size_t si,base=0,k;int add=0,first;
    if(!g_doc || flat<0 || flat>=g_flat_count)return 0;
    sec=&g_doc->sections[g_flat[flat].sec];
    for(si=0;si<g_flat[flat].sec;si++)base+=g_doc->sections[si].block_count;
    first=reserve_at(page)==0;
    for(k=0;k<sec->subtext_count;k++) {
        const hp_subtext *sub=&sec->subtexts[k];
        size_t anchor=sub->block<sec->block_count ? sub->block : (sec->block_count ? sec->block_count-1 : 0);
        int h;
        if(sub->kind!=HP_SUB_FOOTNOTE || base+anchor!=(size_t)flat)continue;
        h=subtext_height(hdc,sub,width);
        add+=h+(first ? hwp_px_i((int32_t)(sec->footnote_shape.above_hwp+sec->footnote_shape.below_hwp))
                      : hwp_px_i((int32_t)sec->footnote_shape.between_hwp));
        first=0;
        if(place && g_note_count<g_note_capacity) {
            g_notes[g_note_count++]=(note_ref){sub,g_flat[flat].sec,page,h};
        }
    }
    return add;
}

/* Header or footer for a page: the last one in the document whose control
 * is on this page or before it and that applies to odd/even pages alike or
 * to this page's parity. */
static const hp_subtext *page_subtext(int page_index,hp_subtext_kind kind) {
    const hp_subtext *found=NULL;size_t si,k,base=0;int odd=g_pages[page_index].printed_number%2!=0;
    for(si=0;si<g_doc->section_count;si++) {
        const hp_section *sec=&g_doc->sections[si];
        for(k=0;k<sec->subtext_count;k++) {
            const hp_subtext *sub=&sec->subtexts[k];
            size_t anchor=sub->block<sec->block_count ? sub->block : (sec->block_count ? sec->block_count-1 : 0);
            int flat=(int)(base+anchor),from;
            if(sub->kind!=kind || (sub->apply==1 && odd) || (sub->apply==2 && !odd))continue;
            from=flat<g_block_page_capacity && g_block_page[flat]>=0 ? g_block_page[flat] : 0;
            if(from<=page_index)found=sub;
        }
        base+=sec->block_count;
    }
    return found;
}

/* Page number fields of a header/footer, also inside its tables (0.12.1):
 * their text points at this page's numbers while it is drawn, then back. */
typedef struct { char **slot; char *saved; } field_swap;
typedef struct { field_swap *v; size_t n, cap; char *numbers[2]; } field_set;
static void fields_in_table(hp_table *t,field_set *fs);
static void fields_in_para(hp_paragraph *p,field_set *fs) {
    size_t r;
    for(r=0;r<p->run_count;r++)if(p->runs[r].field>=1 && p->runs[r].field<=2) {
        if(fs->n==fs->cap) {
            size_t cap=fs->cap ? fs->cap*2 : 8;field_swap *g=(field_swap *)realloc(fs->v,cap*sizeof(*g));
            if(!g)return;
            fs->v=g;fs->cap=cap;
        }
        fs->v[fs->n].slot=&p->runs[r].text;fs->v[fs->n].saved=p->runs[r].text;fs->n++;
        p->runs[r].text=fs->numbers[p->runs[r].field-1];
    }
}
static void fields_in_table(hp_table *t,field_set *fs) {
    size_t c,k;
    if(!t)return;
    for(c=0;c<t->cell_count;c++) {
        for(k=0;k<t->cells[c].paragraph_count;k++)fields_in_para(&t->cells[c].paragraphs[k],fs);
        for(k=0;k<t->cells[c].nested_table_count;k++)fields_in_table(t->cells[c].nested_tables[k],fs);
    }
}

static void draw_subtext(HDC hdc,const hp_subtext *sub,int left,int right,int top,int area,int page_number) {
    size_t i;int y,h=subtext_height(hdc,sub,right-left);
    char numbers[2][16];field_set fs={0};
    snprintf(numbers[0],sizeof(numbers[0]),"%d",page_number);
    snprintf(numbers[1],sizeof(numbers[1]),"%d",g_page_count);
    fs.numbers[0]=numbers[0];fs.numbers[1]=numbers[1];
    for(i=0;i<sub->block_count;i++) {
        hp_block *b=&sub->blocks[i];
        if(b->kind==HP_BLOCK_PARAGRAPH)fields_in_para(&b->paragraph,&fs);
        else if(b->kind==HP_BLOCK_TABLE)fields_in_table(b->table,&fs);
    }
    y=top+(sub->vert_align==1 ? (area-h)/2 : sub->vert_align==2 ? area-h : 0);
    for(i=0;i<sub->block_count;i++)draw_block(hdc,&sub->blocks[i],left,right,&y);
    for(i=fs.n;i>0;i--)*fs.v[i-1].slot=fs.v[i-1].saved;
    free(fs.v);
}

static void draw_page_extras(HDC mem,int page_index,int ml,int mr,int mb,int col_w) {
    const layout_page *pg=&g_pages[page_index];
    const hp_section *section=&g_doc->sections[pg->section];
    const hp_subtext *sub;
    int k,saved;
    /* header: the header margin below the top margin; more text overlaps the body */
    if((sub=page_subtext(page_index,HP_SUB_HEADER))!=NULL)
        draw_subtext(mem,sub,ml,PAGE_W_PX-mr,padding_px(section->page.margin_top),padding_px(section->page.header_height),pg->printed_number);
    /* footer: above the bottom margin; what does not fit on the paper is cut */
    if((sub=page_subtext(page_index,HP_SUB_FOOTER))!=NULL) {
        int area=padding_px(section->page.footer_height),bottom=PAGE_H_PX-padding_px(section->page.margin_bottom);
        saved=SaveDC(mem);
        IntersectClipRect(mem,0,0,PAGE_W_PX,PAGE_H_PX);
        draw_subtext(mem,sub,ml,PAGE_W_PX-mr,bottom-area,area,pg->printed_number);
        RestoreDC(mem,saved);
    }
    /* footnotes: separator and notes at the bottom of the body */
    if(page_index<g_reserve_capacity && g_page_reserve[page_index]>0) {
        const hp_note_shape *ns=&section->footnote_shape;
        int y=PAGE_H_PX-mb-g_page_reserve[page_index],first=1;
        for(k=0;k<g_note_count;k++) {
            const note_ref *note=&g_notes[k];
            size_t i;
            if(note->page!=page_index)continue;
            if(first) {
                y+=hwp_px_i((int32_t)ns->above_hwp);
                if(ns->line_type) {
                    int length=ns->line_length_hwp>0 ? hp_line_px((uint32_t)ns->line_length_hwp) : col_w/3;
                    HPEN pen=CreatePen(PS_SOLID,1,RGB(ns->line_color>>16&255,ns->line_color>>8&255,ns->line_color&255)),old;
                    if(length>col_w)length=col_w;
                    old=(HPEN)SelectObject(mem,pen);
                    MoveToEx(mem,ml,y,NULL);LineTo(mem,ml+length,y);
                    SelectObject(mem,old);DeleteObject(pen);
                }
                y+=hwp_px_i((int32_t)ns->below_hwp);
                first=0;
            } else y+=hwp_px_i((int32_t)ns->between_hwp);
            for(i=0;i<note->sub->block_count;i++)draw_block(mem,&note->sub->blocks[i],ml,ml+col_w,&y);
        }
    }
}
static void assign_page_numbers(void) {
    int page,number=1,position=0;size_t previous_section=(size_t)-1;
    char side[16]={0};
    for(page=0;page<g_page_count;page++) {
        layout_page *pg=&g_pages[page];const hp_section *sec=&g_doc->sections[pg->section];int i;
        int first_in_section=pg->section!=previous_section;
        if(first_in_section) {
            if(sec->page_number_start>0)number=sec->page_number_start;
            if(sec->page_number_position>=0) {
                position=sec->page_number_position;
                memcpy(side,sec->page_number_side,sizeof(side));
            }
            previous_section=pg->section;
        }
        pg->number_hidden=first_in_section && sec->hide_first_page_number;
        for(i=0;i<pg->count;i++) {
            const layout_piece *piece=&g_pieces[pg->first+i];
            const hp_block *block=flat_block(piece->block);
            /* A continued table does not replay controls on each fragment. */
            if(piece->offset || !block)continue;
            if(block->page_number_reset>0)number=block->page_number_reset;
            if(block->hide_page_number)pg->number_hidden=1;
        }
        pg->printed_number=number++;pg->number_position=position;
        memcpy(pg->number_side,side,sizeof(side));
    }
}
static void rebuild_pages(HWND hwnd) {
    HDC hdc;
    hp_flow flow={0,0,0,1,1};
    int i, ml=0,mr=0,mt=0,mb=0,content_w=0,content_h=0,col_w=0;
    size_t section=(size_t)-1;
    int previous_start_page=-1,previous_start_column=-1;
    int ink_page=-1,ink_column=-1,previous_ink_bottom=0;
    /* Saved positions are page coordinates of the source layout. They are used
     * only while our page/column is the source's: after our layout pushed
     * content to another page (e.g. cells re-laid out taller), the next saved
     * page start re-synchronizes (0.10.1). */
    int synced=1,start_page,start_column;
    g_page_count=g_piece_count=g_unsafe_table_cuts=g_suppressed_headers=0;
    g_note_count=0;
    if(g_reserve_capacity)memset(g_page_reserve,0,(size_t)g_reserve_capacity*sizeof(*g_page_reserve));
    if (!g_flat_count) return;
    {
        /* every block's first page, and room for every footnote (0.12.0) */
        size_t si,k;int notes=0;
        if(g_block_page_capacity<g_flat_count) {
            int *next=(int *)realloc(g_block_page,(size_t)g_flat_count*sizeof(*next));
            if(next){g_block_page=next;g_block_page_capacity=g_flat_count;}
        }
        for(i=0;i<g_block_page_capacity;i++)g_block_page[i]=-1;
        for(si=0;si<g_doc->section_count;si++)for(k=0;k<g_doc->sections[si].subtext_count;k++)
            notes+=g_doc->sections[si].subtexts[k].kind==HP_SUB_FOOTNOTE;
        if(notes>g_note_capacity) {
            note_ref *next=(note_ref *)realloc(g_notes,(size_t)notes*sizeof(*next));
            if(next){g_notes=next;g_note_capacity=notes;}
        }
    }
    hdc=GetDC(hwnd);
    if (!hdc) return;
    for(i=0;i<g_flat_count;i++) {
        const hp_block *block=flat_block(i);
        const hp_section *sec=&g_doc->sections[g_flat[i].sec];
        int h,offset=0,*ends=NULL,*hard=NULL,capacity=0,count=0,limit,j;
        int header=0,previous_page=-1,previous_column=-1;
        hp_cut_map ink={0};
        if (section!=g_flat[i].sec) {
            section=g_flat[i].sec;
            use_section(section);
            page_metrics(&ml,&mr,&mt,&mb,&content_w,&content_h);
            flow.page=g_page_count;flow.column=flow.y=0;synced=1;
            flow.columns=sec->column_count > 0 && sec->column_count <= 32 ? sec->column_count : 1;
            flow.height=content_h > 0 ? content_h : 1;
            col_w=(content_w-(flow.columns-1)*hp_line_px(sec->column_gap_hwp))/flow.columns;
            if(col_w<1)col_w=1;
        }
        if(block->page_break_before) hp_flow_break(&flow,1);
        else if(block->column_break_before) hp_flow_break(&flow,0);
        else if(block->saved_break_before && synced && flow.page==previous_start_page && flow.column==previous_start_column)
            hp_flow_break(&flow,0);
        if(block->page_break_before || block->column_break_before || block->saved_break_before)synced=1;
        previous_start_page=flow.page;previous_start_column=flow.column;
        if(ink_page!=flow.page || ink_column!=flow.column)previous_ink_bottom=0;
        if(block->has_saved_y && synced) {
            int target=hp_line_px(block->saved_y_hwp);
            if(block->kind==HP_BLOCK_PARAGRAPH)target-=hwp_px_i(block->paragraph.space_before_hwp);
            /* Drop accumulated rounding/trailing leading only in empty space;
             * expanded text must never overlap preceding visible content. */
            if(target>=previous_ink_bottom && target<flow.height)flow.y=target;
        }
        /* footnotes referenced here take room at the bottom of this page; the
         * reference line moves on when the notes would not leave room for it */
        flow.height=content_h-reserve_at(flow.page);
        if(flow.height<1)flow.height=1;
        {
            int add=notes_space(hdc,i,col_w,flow.page,0);
            if(add>0) {
                if(flow.y>0 && flow.y+add+hwp_px_i(1600)>flow.height)hp_flow_break(&flow,1);
                add=notes_space(hdc,i,col_w,flow.page,1);
                if(reserve_at(flow.page)>=0 && flow.page<g_reserve_capacity)g_page_reserve[flow.page]+=add;
                flow.height=content_h-reserve_at(flow.page);
                if(flow.height<1)flow.height=1;
            }
        }
        h=measure_block_height(hdc,block,col_w);
        if(block->kind==HP_BLOCK_PARAGRAPH && block->paragraph.object_anchor_only)continue;
        if(h<1)h=1;
        if(block->kind==HP_BLOCK_PARAGRAPH) {
            const hp_paragraph *para=&block->paragraph;
            size_t r;
            capacity=(int)para->seg_count+2;
            for(r=0;r<para->run_count;r++)if(para->runs[r].text)capacity+=(int)strlen(para->runs[r].text);
            ends=(int *)calloc((size_t)capacity,sizeof(int));
            hard=(int *)calloc((size_t)capacity,sizeof(int));
            if(!ends || !hard) { free(ends);free(hard);goto failed; }
            layout_text_para(hdc,para,0,col_w,0,0,ends,capacity,hard,&ink,hwp_px_i(para->space_before_hwp)+(block->note_separator ? hwp_px_i(ENDNOTE_ABOVE_HWP+ENDNOTE_BELOW_HWP) : 0));
            hp_cut_normalize(&ink);
            if(ink.failed) {free(ends);free(hard);free(ink.bands);goto failed;}
            {
                int lead=hwp_px_i(para->space_before_hwp)+
                    (block->note_separator ? hwp_px_i(ENDNOTE_ABOVE_HWP+ENDNOTE_BELOW_HWP) : 0);
                for(j=0;j<capacity;j++) {
                    if(ends[j]>0) { ends[j]+=lead;count=j+1; }
                    if(hard[j]>0)hard[j]+=lead;
                }
            }
        } else if(block->kind==HP_BLOCK_TABLE && block->table) {
            int *columns=NULL,*rows=NULL,total=0;
            table_geometry(hdc,block->table,col_w,&columns,&rows);
            capacity=block->table->row_count;
            if(rows) {
                int header_rows=hp_table_header_rows(block->table);
                for(j=0;j<header_rows;j++)header+=rows[j];
                /* A repeated band cannot exceed the entire body of a column. */
                if(header>=flow.height)header=0;
            }
            collect_table_ink(hdc,block->table,col_w,0,&ink,0);
            hp_cut_normalize(&ink);
            if(ink.failed) {free(columns);free(rows);free(ink.bands);goto failed;}
            if(rows && capacity>0) {
                ends=(int *)calloc((size_t)capacity,sizeof(int));
                if(!ends) { free(columns);free(rows);free(ink.bands);goto failed; }
                {
                    size_t ci;int merged=0;
                    for(ci=0;ci<block->table->cell_count;ci++)if(block->table->cells[ci].rowspan>1)merged=1;
                    for(j=0;j<capacity;j++) {
                        total+=rows[j];
                        /* A merged cell continues through row boundaries. Fill
                         * the column using common line-safe cuts rather than
                         * moving each intersected row to another page. */
                        if(!merged && total>header)ends[count++]=total;
                    }
                }
            }
            free(columns);free(rows);
        }
        start_page=flow.page;start_column=flow.column;
        while(offset<h) {
            hp_slice slice;
            int reserved=0;
            flow.height=content_h-reserve_at(flow.page);
            if(flow.height<1)flow.height=1;
            limit=h;
            if(offset==0 && block->has_saved_y && ink.count && h>flow.height-flow.y) {
                int visible=ink.bands[ink.count-1].bottom;
                if(visible<=flow.height-flow.y)h=limit=flow.height-flow.y;
            }
            if(hard)for(j=0;j<count;j++)if(hard[j]>offset && hard[j]<limit)limit=hard[j];
            if(block->kind==HP_BLOCK_TABLE && block->table)
                slice=take_table_slice(&flow,offset,limit,ends,count,&ink,header,previous_page,previous_column,&reserved);
            else slice=hp_flow_take(&flow,offset,limit,ends,count);
            if(slice.height<1 || !append_piece(slice,i,section)) { free(ends);free(hard);free(ink.bands);goto failed; }
            g_pieces[g_piece_count-1].header_height=reserved;
            if(ink_page!=slice.page || ink_column!=slice.column)previous_ink_bottom=0;
            ink_page=slice.page;ink_column=slice.column;
            if(ink.count) {
                size_t band;
                for(band=0;band<ink.count;band++)if(ink.bands[band].top<offset+slice.height && ink.bands[band].bottom>offset) {
                    int bottom=ink.bands[band].bottom<offset+slice.height ? ink.bands[band].bottom : offset+slice.height;
                    bottom+=slice.y-offset;
                    if(bottom>previous_ink_bottom)previous_ink_bottom=bottom;
                }
            }
            /* A table's borders and cell fills are visible to its last row,
             * below the text ink inside the cells (0.10.1). */
            if(block->kind==HP_BLOCK_TABLE && slice.y+slice.height>previous_ink_bottom)previous_ink_bottom=slice.y+slice.height;
            if(!ink.count && (block->kind!=HP_BLOCK_PARAGRAPH || !para_is_blank(&block->paragraph)))previous_ink_bottom=slice.y+slice.height;
            previous_page=slice.page;previous_column=slice.column;
            offset+=slice.height;
            if(offset==limit && limit<h)hp_flow_break(&flow,0);
        }
        if(previous_page!=start_page || previous_column!=start_column)synced=0;
        free(ends);free(hard);free(ink.bands);
    }
    ReleaseDC(hwnd,hdc);
    assign_page_numbers();
    if(g_page_index>=g_page_count)g_page_index=g_page_count ? g_page_count-1 : 0;
    if(g_page_count)use_section(g_pages[g_page_index].section);
    return;
failed:
    ReleaseDC(hwnd,hdc);g_page_count=g_piece_count=0;
    MessageBoxW(hwnd,L"문서 레이아웃 메모리가 부족합니다.",L"한글패드",MB_ICONERROR);
}

static wchar_t *para_concat_wide(const hp_paragraph *para) {
    size_t cap = 64, len = 0, ri;
    wchar_t *all = (wchar_t *)malloc(cap * sizeof(wchar_t));
    if (!all) {
        return NULL;
    }
    all[0] = 0;
    for (ri = 0; ri < para->run_count; ri++) {
        wchar_t *w;
        size_t n;
        if (!para->runs[ri].text || !para->runs[ri].text[0]) {
            continue;
        }
        w = utf8_to_wide(para->runs[ri].text);
        if (!w) {
            continue;
        }
        n = wcslen(w);
        while (len + n + 1 > cap) {
            wchar_t *nw;
            cap *= 2;
            nw = (wchar_t *)realloc(all, cap * sizeof(wchar_t));
            if (!nw) {
                free(w);
                free(all);
                return NULL;
            }
            all = nw;
        }
        memcpy(all + len, w, (n + 1) * sizeof(wchar_t));
        len += n;
        free(w);
    }
    return all;
}

static const hp_run *run_at_char(const hp_paragraph *para, size_t char_index, size_t *out_local) {
    size_t ri, at = 0;
    for (ri = 0; ri < para->run_count; ri++) {
        wchar_t *w;
        size_t n;
        if (!para->runs[ri].text) {
            continue;
        }
        w = utf8_to_wide(para->runs[ri].text);
        if (!w) {
            continue;
        }
        n = wcslen(w);
        free(w);
        if (char_index < at + n) {
            if (out_local) {
                *out_local = char_index - at;
            }
            return &para->runs[ri];
        }
        at += n;
    }
    return para->run_count ? &para->runs[0] : NULL;
}

/* Language-specific width/spacing in HWP charPr; all geometry is in bitmap pixels. */
static int glyph_language(wchar_t c) {
    if ((c >= 0xAC00 && c <= 0xD7AF) || (c >= 0x1100 && c <= 0x11FF) || (c>=0x3130 && c<=0x318f)) return 0;
    if (c < 0x250) return 1;
    if ((c >= 0x4E00 && c <= 0x9FFF) || (c>=0x3400 && c<=0x4dbf)) return 2;
    if ((c >= 0x3040 && c <= 0x30FF) || (c>=0xff66 && c<=0xff9f)) return 3;
    if(c>=0xe000 && c<=0xf8ff)return 6;
    if(c>=0x2000 && c<=0x2bff)return 5;
    return 4;
}
/* A physical regular face selected with FW_BOLD is GDI synthetic bold. HWP
 * advances synthetic bold by 5% of the regular glyph, rather than GDI's
 * sometimes narrower hinted fake-bold metrics. Real bold faces keep theirs. */
static int selected_synthetic_bold(HDC dc) {
    static struct {wchar_t face[LF_FACESIZE];int synthetic;} cache[128];static int count;
    wchar_t face[LF_FACESIZE]={0};unsigned char weight[2];int i,synthetic;
    GetTextFaceW(dc,LF_FACESIZE,face);
    for(i=0;i<count;i++)if(wcscmp(cache[i].face,face)==0)return cache[i].synthetic;
    if(GetFontData(dc,0x322f534f,4,weight,2)!=2)return 0;
    synthetic=((unsigned)weight[0]*256+weight[1])<600;
    if(count<128){wcscpy(cache[count].face,face);cache[count++].synthetic=synthetic;}
    return synthetic;
}
static double regular_advance(HDC dc,HFONT selected,const wchar_t *text,int length,double fallback) {
    LOGFONTW lf;HFONT regular,old;SIZE size;int i,owned=0;
    GetObjectW(selected,sizeof(lf),&lf);lf.lfWeight=FW_NORMAL;
    for(i=0;i<g_regular_font_count;i++)if(memcmp(&lf,&g_regular_fonts[i].lf,sizeof(lf))==0)break;
    if(i<g_regular_font_count)regular=g_regular_fonts[i].font;
    else {
        regular=CreateFontIndirectW(&lf);if(!regular)return fallback;
        if(g_regular_font_count<64){g_regular_fonts[g_regular_font_count].lf=lf;g_regular_fonts[g_regular_font_count++].font=regular;}
        else owned=1;
    }
    old=SelectObject(dc,regular);GetTextExtentPoint32W(dc,text,length,&size);SelectObject(dc,old);
    if(owned){DeleteObject(regular);}return size.cx*1.05/FONT_SCALE;
}
static void draw_text_underline(HDC dc,double left,double right,int baseline,const hp_run *run){
    int em=hp_line_px(run->font_size_hwp ? run->font_size_hwp : 1000);
    int thickness=em/24,offset=em/14;RECT rect;COLORREF old;
    if(thickness<1)thickness=1;if(offset<1)offset=1;
    rect=(RECT){(int)(left+0.5),baseline+offset,(int)(right+0.5),baseline+offset+thickness};
    old=SetDCBrushColor(dc,RGB((run->color_rgb>>16)&255,(run->color_rgb>>8)&255,run->color_rgb&255));
    FillRect(dc,&rect,(HBRUSH)GetStockObject(DC_BRUSH));SetDCBrushColor(dc,old);
}
static double glyph_step(HDC hdc, const hp_run *run, const wchar_t *text, int length,
                      int x, int y, int draw) {
    unsigned code=(unsigned)text[0];
    if(run->script==1 || run->script==2) {
        /* superscript / subscript (0.12.1): 60% glyphs at the line's top, or lowered */
        hp_run small=*run;uint32_t size=run->font_size_hwp ? run->font_size_hwp : 1000;
        small.script=0;small.font_size_hwp=size*3/5;
        return glyph_step(hdc,&small,text,length,x,y+(run->script==2 ? hp_line_px(size)*2/5 : 0),draw);
    }
    if(length==2 && text[0]>=0xd800 && text[0]<=0xdbff && text[1]>=0xdc00 && text[1]<=0xdfff)
        code=0x10000+((text[0]-0xd800)<<10)+(text[1]-0xdc00);
    /* HWP's four boxed section digits have no Unicode font fallback on Windows.
     * Keep source text intact and draw their documented appearance explicitly. */
    if(code>=0xf02b1 && code<=0xf02b4) {
        int em=hp_line_px(run->font_size_hwp),side=em*9/10;
        if(draw) {
            int saved=SaveDC(hdc);wchar_t digit=(wchar_t)(L'1'+code-0xf02b1);
            RECT box={x,y+em/10,x+side,y+em/10+side};
            HPEN pen=CreatePen(PS_SOLID,1,RGB((run->color_rgb>>16)&255,(run->color_rgb>>8)&255,run->color_rgb&255));
            HFONT font=CreateFontW(-side,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,ANTIALIASED_QUALITY,0,L"Times New Roman");
            SelectObject(hdc,pen);SelectObject(hdc,GetStockObject(NULL_BRUSH));Rectangle(hdc,box.left,box.top,box.right,box.bottom);
            SelectObject(hdc,font);SetBkMode(hdc,TRANSPARENT);SetTextColor(hdc,RGB((run->color_rgb>>16)&255,(run->color_rgb>>8)&255,run->color_rgb&255));
            DrawTextW(hdc,&digit,1,&box,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
            RestoreDC(hdc,saved);DeleteObject(font);DeleteObject(pen);
        }
        return em;
    }
    if(text[0]=='\t' && run->tab_width_hwp) {
        double width=run->tab_width_hwp/75.0;
        if(draw && run->tab_leader) {
            int pos,baseline=y+hp_line_px(run->font_size_hwp)*4/5;
            COLORREF old=SetDCBrushColor(hdc,RGB((run->color_rgb>>16)&255,(run->color_rgb>>8)&255,run->color_rgb&255));
            for(pos=2;pos<(int)width-1;pos+=4){RECT dot={x+pos,baseline,x+pos+1,baseline+1};FillRect(hdc,&dot,(HBRUSH)GetStockObject(DC_BRUSH));}
            SetDCBrushColor(hdc,old);
        }
        return width;
    }
    HFONT font = font_for_glyph(hdc, run, text, length), old = (HFONT)SelectObject(hdc, font);
    SIZE size;
    int lang = glyph_language(text[0]);
    int ratio = run->width_percent[lang] > 0 ? run->width_percent[lang] : 100;
    double em=(run->font_size_hwp ? run->font_size_hwp : 1000)/75.0;
    double advance,base_width;
    GetTextExtentPoint32W(hdc, text, length, &size);
    base_width=size.cx/(double)FONT_SCALE;
    if(text[0]==' ' && !run->use_font_space)base_width=em/2;
    else if(run->bold && selected_synthetic_bold(hdc)) {
        base_width=regular_advance(hdc,font,text,length,base_width);
    }
    /* HWP spacing is a percentage of this glyph's advance, including half-em
     * spaces and narrow Latin punctuation, rather than a percentage of em. */
    advance=base_width*ratio/100.0*(100+run->spacing_percent[lang])/100.0;
    if (draw) {
        int saved = SaveDC(hdc);
        XFORM transform = {(FLOAT)ratio / (100.0f*FONT_SCALE), 0, 0, 1.0f/FONT_SCALE, (FLOAT)x, (FLOAT)y};
        SetTextColor(hdc, RGB((run->color_rgb >> 16) & 255, (run->color_rgb >> 8) & 255, run->color_rgb & 255));
        SetBkMode(hdc, TRANSPARENT);
        SetTextAlign(hdc,TA_LEFT | TA_TOP | TA_NOUPDATECP);
        SetGraphicsMode(hdc, GM_ADVANCED);
        SetWorldTransform(hdc, &transform);
        TextOutW(hdc, 0, 0, text, length);
        RestoreDC(hdc, saved);
    }
    SelectObject(hdc, old);
    DeleteObject(font);
    return advance > 0 ? advance : 1;
}
static int glyph_length(const wchar_t *text, size_t i, size_t end) {
    return text[i] >= 0xD800 && text[i] <= 0xDBFF && i + 1 < end &&
           text[i+1] >= 0xDC00 && text[i+1] <= 0xDFFF ? 2 : 1;
}
static int saved_line_box(const hp_paragraph *para, const hp_line_seg *seg, int left, int right, int first_line, int *origin) {
    int box_left=left+hwp_px_i(para->indent_left_hwp);
    int box_right=right-hwp_px_i(para->indent_right_hwp);
    int available,saved_width=hp_line_px(seg->horzsize_hwp);
    if(box_left<left)box_left=left;
    if(box_right>right)box_right=right;
    if(box_right<=box_left)box_right=box_left+1;
    /* Saved positions already include the paragraph's left margin. */
    *origin=left+hwp_px_i((int32_t)seg->horzpos_hwp);
    if(*origin<box_left || *origin>=box_right)*origin=box_left;
    if(first_line && para->indent_first_hwp>0)*origin+=hwp_px_i(para->indent_first_hwp);
    else if(!first_line && para->indent_first_hwp<0)*origin+=hwp_px_i(-para->indent_first_hwp);
    if(*origin>=box_right)*origin=box_right-1;
    available=box_right-*origin;
    if(first_line && para->inline_prefix_width_hwp) {
        int prefix=hp_line_px(para->inline_prefix_width_hwp);
        *origin+=prefix;available-=prefix;
        if(saved_width>prefix)saved_width-=prefix;
    }
    if(available<1)available=1;
    if(saved_width>0 && saved_width<available)available=saved_width;
    return available;
}
static double span_width(HDC hdc,const hp_paragraph *para,const wchar_t *all,size_t start,size_t end) {
    hp_run fallback={0};double width=0;fallback.font_size_hwp=1000;
    while(end>start && (all[end-1]==L' ' || all[end-1]==L'\t'))end--;
    while(start<end) {
        const hp_run *run=run_at_char(para,start,NULL);
        int len=glyph_length(all,start,end);
        if(all[start]!='\n' && all[start]!='\r')width+=glyph_step(hdc,run ? run : &fallback,all+start,len,0,0,0);
        start+=(size_t)len;
    }
    return width;
}
/* Justification stretches word spaces and East Asian character boundaries,
 * never the inside of a Latin word or the leading spaces used for bullets. */
static int justification_gap(const wchar_t *text,size_t at,size_t first,size_t end) {
    size_t next;int a,b;
    if(at<first||at>=end)return 0;
    next=at+(size_t)glyph_length(text,at,end);if(next>=end)return 0;
    if(text[at]==' ')return 1;
    a=glyph_language(text[at]);b=glyph_language(text[next]);
    return (a==0||a==2||a==3)&&(b==0||b==2||b==3);
}
/* Stored lines describe the original line breaks, even when physical GDI font
 * metrics differ slightly from Hangul's. Retain them at their original width;
 * a genuinely narrower cell or an edited paragraph still reflows normally. */
static int saved_line_fits(const hp_paragraph *para,const hp_line_seg *seg,int first,int available,double width) {
    double expected=seg->horzsize_hwp/75.0;
    if(first && para->indent_first_hwp>0)expected-=para->indent_first_hwp/75.0;
    if(!first && para->indent_first_hwp<0)expected+=para->indent_first_hwp/75.0;
    if(first)expected-=para->inline_prefix_width_hwp/75.0;
    return para->align==HP_ALIGN_JUSTIFY && seg->horzsize_hwp && available>=expected-2 && width<=available*1.15+2;
}
static double saved_fit_tolerance(int available){return available>200?available*0.01:2.0;}
static double justified_gap(const hp_paragraph *para,const wchar_t *text,size_t start,size_t end,
                            size_t text_length,double width,int available) {
    size_t ci,first=start,last=end;int gaps=0;
    if(para->align!=HP_ALIGN_JUSTIFY||end<=start||text[end-1]=='\n'||text[end-1]=='\r')return 0;
    if(end>=text_length&&width<=available)return 0;
    while(first<last&&text[first]==' ')first++;
    while(last>first&&(text[last-1]==' '||text[last-1]=='\t'))last--;
    for(ci=first;ci<last;ci+=(size_t)glyph_length(text,ci,last))gaps+=width>available ? ci+(size_t)glyph_length(text,ci,last)<last : justification_gap(text,ci,first,last);
    return gaps&&width<=available*1.15+2 ? (available-width)/gaps : 0;
}
static int text_ink_height(HDC hdc,const hp_paragraph *para,const wchar_t *all,size_t start,size_t end,int baseline,int *offset) {
    size_t ri,at=0;int height=0,visible=0,top=0;
    *offset=0;
    for(ri=start;ri<end;ri++)if(all[ri]!=' ' && all[ri]!='\t' && all[ri]!='\n' && all[ri]!='\r')visible=1;
    if(!visible)return 0;
    for(ri=0;ri<para->run_count;ri++) {
        wchar_t *text=utf8_to_wide(para->runs[ri].text ? para->runs[ri].text : "");
        size_t length=text ? wcslen(text) : 0;free(text);
        if(at<end && at+length>start) {
            HFONT font=make_font(hdc,&para->runs[ri]),old=(HFONT)SelectObject(hdc,font);
            TEXTMETRICW tm;GetTextMetricsW(hdc,&tm);
            {
                int ascent=(tm.tmAscent+FONT_SCALE/2)/FONT_SCALE;
                int t=baseline ? baseline-ascent : 0;
                int b=t+(tm.tmHeight+FONT_SCALE-1)/FONT_SCALE;
                if(t<top)top=t;
                if(b>height)height=b;
            }
            SelectObject(hdc,old);DeleteObject(font);
        }
        at+=length;
    }
    *offset=top;return height-top;
}
static int layout_saved_para(HDC hdc, const hp_paragraph *para, int left, int right,
                             int base, int draw, int *breaks, int capacity, int *hard_breaks, hp_cut_map *ink, int ink_origin) {
    wchar_t *all=para_concat_wide(para);
    hp_run fallback={0};
    size_t group=0,text_length;
    int bottom=0,line_count=0;
    if(!all)return 12;
    text_length=wcslen(all);fallback.font_size_hwp=1000;
    while(group<para->seg_count) {
        size_t last=group,si,stream,end_group;
        int reflow=0,group_base=bottom,top=group_base;
        /* lines past the saved ones: positions summed in HWPUNIT and rounded
         * once, as saved positions are (per-line rounding added up; 0.13.1) */
        int64_t next_hwp=0;
        int32_t source_origin=(int32_t)para->segs[group].vertpos_hwp;
        while(last+1<para->seg_count && (int32_t)para->segs[last+1].vertpos_hwp >= (int32_t)para->segs[last].vertpos_hwp)last++;
        stream=para->segs[group].textpos;
        end_group=last+1<para->seg_count ? para->segs[last+1].textpos : text_length;
        if(end_group>text_length)end_group=text_length;
        if(stream>end_group)stream=end_group;
        {
            size_t ci;
            for(ci=stream;ci<end_group;ci++)if(all[ci]=='\n' || all[ci]=='\r')reflow=1;
        }
        /* Keep saved soft breaks when they fit. If font substitution or a narrow
         * cell requires reflow, carry text across soft breaks instead of leaving
         * one overflowing character alone on a line before the next saved line. */
        for(si=group;si<=last;si++) {
            size_t begin=para->segs[si].textpos;
            size_t end=si<last ? para->segs[si+1].textpos : end_group;
            int origin,available=saved_line_box(para,&para->segs[si],left,right,si==0,&origin);
            if(end>text_length)end=text_length;
            if(begin>end)begin=end;
            {double natural=span_width(hdc,para,all,begin,end);
             if(natural>available+saved_fit_tolerance(available)&&!saved_line_fits(para,&para->segs[si],si==0,available,natural))reflow=1;}
        }
        if(group && hard_breaks && line_count<capacity)hard_breaks[line_count]=group_base;
        si=group;
        do {
            const hp_line_seg *seg=&para->segs[si<=last ? si : last];
            size_t start=reflow ? stream : seg->textpos;
            size_t end=reflow ? end_group : (si<last ? para->segs[si+1].textpos : end_group);
            size_t finish,ci,visible_end,visible_start;
            int origin,available=saved_line_box(para,seg,left,right,line_count==0,&origin);
            int pitch=hp_line_px(hp_line_height(seg));double width=0,x,gap,tab_adjust=0;size_t right_tab=(size_t)-1;
            if(pitch<1)pitch=1;
            if(end>text_length)end=text_length;
            if(start>end)start=end;
            if(si<=last) {
                top=group_base+hp_line_px((int32_t)seg->vertpos_hwp-source_origin);
                next_hwp=(int32_t)seg->vertpos_hwp-source_origin;
            } else top=group_base+hp_line_px((int32_t)next_hwp);
            next_hwp+=(int64_t)hp_line_height(seg);
            finish=start;
            while(finish<end) {
                const hp_run *run=run_at_char(para,finish,NULL);
                int len=glyph_length(all,finish,end);
                double advance;
                if(all[finish]=='\n' || all[finish]=='\r') {
                    if(all[finish]=='\r' && finish+1<end && all[finish+1]=='\n')finish++;
                    finish++;break;
                }
                advance=glyph_step(hdc,run ? run : &fallback,all+finish,len,0,0,0);
                /* GDI advances differ from 한글's by rounding: a line 한글 fills
                 * exactly may measure a fraction wider here, so allow 0.5%
                 * (justification then takes the excess back; 0.13.1) */
                /* spaces at the end of a line hang past it, as in 한글, instead of
                 * starting the next line */
                if(reflow && finish>start && all[finish]!=' ' && width+advance>available+(available>200 ? available*0.005 : 1.0))break;
                width+=advance;finish+=(size_t)len;
            }
            width=span_width(hdc,para,all,start,finish);
            /* A terminal right-aligned tab absorbs font metric differences;
             * following page numbers share the paragraph's right boundary. */
            for(ci=start;ci<finish;ci++)if(all[ci]=='\t') {
                const hp_run *tab=run_at_char(para,ci,NULL);
                right_tab=tab && tab->tab_align_right ? ci : (size_t)-1;
            }
            if(right_tab!=(size_t)-1) {
                const hp_run *tab=run_at_char(para,right_tab,NULL);
                tab_adjust=available-width;
                if(tab->tab_width_hwp/75.0+tab_adjust<1){right_tab=(size_t)-1;tab_adjust=0;}
                else width+=tab_adjust;
            }
            visible_start=start;visible_end=finish;
            while(visible_start<visible_end&&all[visible_start]==' ')visible_start++;
            while(visible_end>visible_start&&(all[visible_end-1]==' '||all[visible_end-1]=='\t'))visible_end--;
            gap=justified_gap(para,all,start,finish,text_length,width,available);
            x=origin;
            if(para->align==HP_ALIGN_CENTER && width<available)x+=(available-width)/2;
            else if(para->align==HP_ALIGN_RIGHT && width<available)x+=available-width;
            if(draw)for(ci=start;ci<finish;) {
                const hp_run *run=run_at_char(para,ci,NULL);
                int len=glyph_length(all,ci,finish);
                if(all[ci]!='\n' && all[ci]!='\r') {
                    int glyph_y=base+top,baseline=0;double glyph_x=x;
                    if(seg->baseline_hwp || (run && run->underline)) {
                        HFONT gf=font_for_glyph(hdc,run ? run : &fallback,all+ci,len),old=(HFONT)SelectObject(hdc,gf);
                        TEXTMETRICW tm;GetTextMetricsW(hdc,&tm);
                        if(seg->baseline_hwp)glyph_y+=hp_line_px(seg->baseline_hwp)-(tm.tmAscent+FONT_SCALE/2)/FONT_SCALE;
                        baseline=glyph_y+(tm.tmAscent+FONT_SCALE/2)/FONT_SCALE;
                        SelectObject(hdc,old);DeleteObject(gf);
                    }
                    if(ci==right_tab && run){hp_run tab=*run;
                        tab.tab_width_hwp=(uint32_t)(tab.tab_width_hwp+tab_adjust*75.0+0.5);
                        x+=glyph_step(hdc,&tab,all+ci,len,(int)(x+0.5),glyph_y,1);
                    }else x+=glyph_step(hdc,run ? run : &fallback,all+ci,len,(int)(x+0.5),glyph_y,1);
                    if(gap<0 ? ci>=visible_start&&ci+(size_t)len<visible_end : justification_gap(all,ci,visible_start,visible_end))x+=gap;
                    if(run && run->underline)draw_text_underline(hdc,glyph_x,x,baseline,run);
                    hit_add(para,ci,(size_t)len,glyph_x,x,base+top,base+top+pitch);
                }
                ci+=(size_t)len;
            }
            if(draw && start==finish)hit_add(para,start,0,x,x,base+top,base+top+pitch); /* empty line */
            if(ink) {
                int ink_top=0,ink_height=text_ink_height(hdc,para,all,start,finish,hp_line_px(seg->baseline_hwp),&ink_top);
                hp_cut_add(ink,ink_origin+top+ink_top,ink_origin+top+ink_top+ink_height);
            }
            if(si<=last) {
                if(top+pitch>bottom)bottom=top+pitch;
            } else if(group_base+hp_line_px((int32_t)next_hwp)>bottom)bottom=group_base+hp_line_px((int32_t)next_hwp);
            if(breaks && line_count<capacity)breaks[line_count]=bottom;
            line_count++;stream=finish;si++;
        } while(reflow ? stream<end_group : si<=last);
        group=last+1;
    }
    free(all);
    return bottom>0 ? bottom : 1;
}

static int layout_text_para(HDC hdc, const hp_paragraph *para, int left, int right, int base, int draw, int *ends, int capacity, int *hard, hp_cut_map *ink, int ink_origin) {
    hp_paragraph copy;
    hp_line_seg line={0};
    if(para->object_anchor_only)return 0;
    if(para->seg_count) return layout_saved_para(hdc,para,left,right,base,draw,ends,capacity,hard,ink,ink_origin);
    if(para_is_blank(para)) {
        /* an empty line still takes one line box in 한글 */
        int h=hp_line_px(relaid_line_size_hwp(para)+relaid_line_spacing_hwp(para));
        if(draw)hit_add(para,0,0,left,left,base,base+h);
        return h;
    }
    /* Line box as 한글 lays it out (the values a saved line array would hold);
     * font ascent/descent and a 14 px minimum made re-laid-out cells taller
     * than the source (0.10.1). */
    line.vertsize_hwp=line.textheight_hwp=relaid_line_size_hwp(para);
    line.spacing_hwp=relaid_line_spacing_hwp(para);
    /* 한글's baseline is 85% down the text height; without one the glyphs sat
     * at the top and their descent left the line box (0.13.1) */
    line.baseline_hwp=line.textheight_hwp*85/100;
    line.horzpos_hwp=0; /* paragraph indentation is applied by saved_line_box */
    copy=*para;copy.segs=&line;copy.seg_count=1;
    return layout_saved_para(hdc,&copy,left,right,base,draw,ends,capacity,hard,ink,ink_origin);
}
static void draw_para(HDC hdc, const hp_paragraph *para, int left, int right, int *y) {
    if(!para || para->object_anchor_only)return;
    *y+=hwp_px_i(para->space_before_hwp);
    *y+=layout_text_para(hdc,para,left,right,*y,1,NULL,0,NULL,NULL,0)+hwp_px_i(para->space_after_hwp);
}

static int hwp_delta_px(int32_t hwp) {
    if (hwp >= 0) {
        return (int)hp_hwp_to_px((uint32_t)hwp);
    }
    return -(int)hp_hwp_to_px((uint32_t)(-hwp));
}

static void apply_float_origin(const hp_float_pos *pos, int has_pos, int *x, int *y) {
    if (!has_pos || !pos || pos->treat_as_char) {
        return;
    }
    *x += hwp_delta_px(pos->horz_offset_hwp);
    *y += hwp_delta_px(pos->vert_offset_hwp);
}

static void draw_image(HDC hdc, const hp_image *img, int left, int right, int *y) {
    int iw = 0, ih = 0;
    int sx=0,sy=0,sw,sh;
    int dw, dh;
    int content_w = right - left;
    int x, yy;
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
    sw=iw;sh=ih;
    /* imgClip uses imgDim coordinates, not destination object dimensions.
     * A logo may be a crop of a much wider bitmap containing adjacent text. */
    if(img->image_dim_width && img->image_dim_height && img->clip_right>img->clip_left && img->clip_bottom>img->clip_top &&
       img->clip_right<=img->image_dim_width && img->clip_bottom<=img->image_dim_height) {
        sx=(int)((uint64_t)img->clip_left*iw/img->image_dim_width);
        sy=(int)((uint64_t)img->clip_top*ih/img->image_dim_height);
        sw=(int)((uint64_t)img->clip_right*iw/img->image_dim_width)-sx;
        sh=(int)((uint64_t)img->clip_bottom*ih/img->image_dim_height)-sy;
        if(sw<1 || sh<1) {sx=sy=0;sw=iw;sh=ih;}
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
    x = left + (content_w - dw) / 2;
    /* a floating picture's offset is measured from its alignment (0.12.1) */
    if (img->has_pos && !img->pos.treat_as_char)
        x = img->pos.horz_align == 1 ? x : img->pos.horz_align == 2 ? right - dw : left;
    yy = *y;
    apply_float_origin(&img->pos, img->has_pos, &x, &yy);
    mem = CreateCompatibleDC(hdc);
    old = SelectObject(mem, hbmp);
    SetStretchBltMode(hdc, HALFTONE);
    {
        DIBSECTION dib={0};int alpha=0;size_t k;
        GetObject(hbmp,sizeof(dib),&dib);
        if(dib.dsBm.bmBits)for(k=0;k<(size_t)iw*ih;k++)
            if(((const BYTE *)dib.dsBm.bmBits)[k*4+3]!=255){alpha=1;break;}
        if(!alpha)StretchBlt(hdc,x,yy,dw,dh,mem,sx,sy,sw,sh,SRCCOPY);
        else {
            /* AlphaBlend itself only uses nearest-neighbour scaling. Resample
             * premultiplied colors and alpha together before compositing. */
            IWICBitmap *source=NULL;IWICBitmapClipper *clip=NULL;IWICBitmapScaler *scale=NULL;
            HBITMAP resized=NULL;void *pixels=NULL;BITMAPINFO info={0};
            WICRect crop={sx,sy,sw,sh};BLENDFUNCTION blend={AC_SRC_OVER,0,255,AC_SRC_ALPHA};
            if(SUCCEEDED(g_wic->lpVtbl->CreateBitmapFromHBITMAP(g_wic,hbmp,NULL,WICBitmapUsePremultipliedAlpha,&source)) &&
               SUCCEEDED(g_wic->lpVtbl->CreateBitmapClipper(g_wic,&clip)) &&
               SUCCEEDED(clip->lpVtbl->Initialize(clip,(IWICBitmapSource *)source,&crop)) &&
               SUCCEEDED(g_wic->lpVtbl->CreateBitmapScaler(g_wic,&scale)) &&
               SUCCEEDED(scale->lpVtbl->Initialize(scale,(IWICBitmapSource *)clip,dw,dh,WICBitmapInterpolationModeFant))) {
                info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=dw;info.bmiHeader.biHeight=-dh;
                info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
                resized=CreateDIBSection(hdc,&info,DIB_RGB_COLORS,&pixels,NULL,0);
                if(resized && SUCCEEDED(scale->lpVtbl->CopyPixels(scale,NULL,(UINT)dw*4,(UINT)dw*dh*4,pixels))) {
                    SelectObject(mem,resized);AlphaBlend(hdc,x,yy,dw,dh,mem,0,0,dw,dh,blend);SelectObject(mem,hbmp);
                } else AlphaBlend(hdc,x,yy,dw,dh,mem,sx,sy,sw,sh,blend);
            } else AlphaBlend(hdc,x,yy,dw,dh,mem,sx,sy,sw,sh,blend);
            if(resized)DeleteObject(resized);
            if(scale)scale->lpVtbl->Release(scale);if(clip)clip->lpVtbl->Release(clip);if(source)source->lpVtbl->Release(source);
        }
    }
    SelectObject(mem, old);
    DeleteDC(mem);
    DeleteObject(hbmp);
    {
        int bottom = yy + dh;
        if (bottom < *y + dh) {
            bottom = *y + dh;
        }
        *y = bottom;
    }
}

static void draw_equation(HDC hdc, const hp_equation *eq, int left, int right, int *y) {
    wchar_t *w;
    HFONT font, old;
    RECT rc;
    const char *script;
    if (!eq) {
        return;
    }
    script = eq->script && eq->script[0] ? eq->script : "[수식]";
    w = utf8_to_wide(script);
    if (!w) {
        *y += 24;
        return;
    }
    font = CreateFontW(-(int)(eq->font_size_hwp ? hp_hwp_to_px(eq->font_size_hwp) : 14), 0, 0, 0, FW_NORMAL, TRUE, 0,
                       0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Malgun Gothic");
    old = (HFONT)SelectObject(hdc, font);
    SetTextColor(hdc, RGB(20, 40, 80));
    SetBkMode(hdc, TRANSPARENT);
    rc.left = left + 4;
    rc.top = *y + 2;
    rc.right = right - 4;
    rc.bottom = *y + 26;
    DrawTextW(hdc, w, -1, &rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    SelectObject(hdc, old);
    DeleteObject(font);
    free(w);
    *y += 28;
}

static void draw_container(HDC hdc, const hp_container *c, int left, int right, int *y) {
    size_t i;
    int x = left, yy = *y;
    int content_w = right - left;
    int max_bottom = *y;
    if (!c) {
        return;
    }
    apply_float_origin(&c->pos, c->has_pos, &x, &yy);
    for (i = 0; i < c->image_count; i++) {
        int iy = yy;
        int ir = x + content_w;
        if (c->images[i]) {
            draw_image(hdc, c->images[i], x, ir, &iy);
            if (iy > max_bottom) {
                max_bottom = iy;
            }
        }
    }
    *y = max_bottom > *y ? max_bottom : (*y + 8);
}

/* Device-space segments keep dotted/dashed borders patterned at every pen width;
 * Win32 cosmetic dashed pens otherwise turn thick lines into solid strokes. */
static void draw_axis_border(HDC dc,int x1,int y1,int x2,int y2,int width,uint32_t rgb,hp_border_line style){
    int vertical=x1==x2,length=vertical?y2-y1:x2-x1,pos=0,index=0;
    int pattern[8]={1,1},count=2;COLORREF old;
    if(width<1)width=1;if(length<=0)return;
    if(style==HP_BORDER_DOUBLE_SLIM) {
        int separation=width>2?width-1:2,offset=separation/2;
        draw_axis_border(dc,x1-(vertical?offset:0),y1-(vertical?0:offset),x2-(vertical?offset:0),y2-(vertical?0:offset),1,rgb,HP_BORDER_SOLID);
        draw_axis_border(dc,x1+(vertical?separation-offset:0),y1+(vertical?0:separation-offset),x2+(vertical?separation-offset:0),y2+(vertical?0:separation-offset),1,rgb,HP_BORDER_SOLID);
        return;
    }
    if(style==HP_BORDER_SOLID){pattern[0]=length;count=1;}
    else if(style==HP_BORDER_DOT){pattern[0]=1;pattern[1]=2;}
    else if(style==HP_BORDER_LONG_DASH){pattern[0]=4;pattern[1]=2;}
    else if(style==HP_BORDER_DASH_DOT||style==HP_BORDER_DASH_DOT_DOT){
        pattern[0]=3;pattern[1]=2;pattern[2]=1;pattern[3]=2;count=4;
        if(style==HP_BORDER_DASH_DOT_DOT){pattern[4]=1;pattern[5]=2;count=6;}
    }
    old=SetDCBrushColor(dc,RGB((rgb>>16)&255,(rgb>>8)&255,rgb&255));
    while(pos<length){
        int step=style==HP_BORDER_SOLID?length:pattern[index]*width;
        int end=pos+step;if(end>length)end=length;
        if(!(index&1)){
            RECT r=vertical?(RECT){x1-width/2,y1+pos,x1+(width+1)/2,y1+end}:
                (RECT){x1+pos,y1-width/2,x1+end,y1+(width+1)/2};
            FillRect(dc,&r,(HBRUSH)GetStockObject(DC_BRUSH));
        }
        pos=end;index=(index+1)%count;
    }
    SetDCBrushColor(dc,old);
}
/* Share one horizontal origin for cell content, borders and continuation edges.
 * Inline tables follow paragraph alignment, including their outer margins. */
static int table_origin_x(const hp_table *t,int left,int right,int width){
    int x=left+(right-left-width)/2;
    if(t->has_anchor_align){
        int l=left+hwp_px_i(t->anchor_left_hwp)+padding_px(t->outer_left_hwp);
        int r=right-hwp_px_i(t->anchor_right_hwp)-padding_px(t->outer_right_hwp);
        if(t->anchor_align==HP_ALIGN_RIGHT)x=r-width;
        else if(t->anchor_align==HP_ALIGN_CENTER)x=l+(r-l-width)/2;
        else x=l;
        if(x+width>right)x=right-width;
        if(x<left)x=left;
    }
    return x;
}
static void draw_table(HDC hdc, const hp_table *t, int left, int right, int *y) {
    int content_w = right-left;
    int rows, cols, table_w = 0, x0, c;
    int *col_w = NULL, *row_h = NULL;
    int r;
    size_t i;
    int height = table_geometry(hdc,t,content_w,&col_w,&row_h);
    if (!col_w || !row_h) { *y += height; return; }
    rows = t->row_count; cols = t->col_count;
    for (c=0; c<cols; c++) table_w += col_w[c];
    x0 = table_origin_x(t,left,right,table_w);
    {
        int yy = *y;
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
            {
                int bw=cell->borders.width_px>0?cell->borders.width_px:1;
                if(cell->borders.top)draw_axis_border(hdc,cx,cy,cx+cw,cy,cell->borders.width_top>0?cell->borders.width_top:bw,cell->borders.color_top,cell->borders.style_top);
                if(cell->borders.bottom)draw_axis_border(hdc,cx,cy+ch-(cell->row+cell->rowspan==rows),cx+cw,cy+ch-(cell->row+cell->rowspan==rows),cell->borders.width_bottom>0?cell->borders.width_bottom:bw,cell->borders.color_bottom,cell->borders.style_bottom);
                if(cell->borders.left)draw_axis_border(hdc,cx,cy,cx,cy+ch,cell->borders.width_left>0?cell->borders.width_left:bw,cell->borders.color_left,cell->borders.style_left);
                if(cell->borders.right)draw_axis_border(hdc,cx+cw-(cell->col+cell->colspan==cols),cy,cx+cw-(cell->col+cell->colspan==cols),cy+ch,cell->borders.width_right>0?cell->borders.width_right:bw,cell->borders.color_right,cell->borders.style_right);
            }
            {
                int pad_l = padding_px(cell->pad_left_hwp);
                int pad_r = padding_px(cell->pad_right_hwp);
                int pad_t = padding_px(cell->pad_top_hwp);
                int ty = cy + pad_t;
                int saved_cell = SaveDC(hdc);
                IntersectClipRect(hdc, cx, cy, cx+cw, cy+ch);
                int cl = cx + pad_l;
                int cr = cx + cw - pad_r;
                size_t n;
                ty+=cell_vertical_offset(hdc,cell,cr>cl ? cr-cl : 1,ch);
                if (cell->image) {
                    draw_image(hdc, cell->image, cl, cr, &ty);
                }
                if (cell->container) {
                    draw_container(hdc, cell->container, cl, cr, &ty);
                }
                if (cell->equation) {
                    draw_equation(hdc, cell->equation, cl, cr, &ty);
                }
                for (p = 0; p < cell->paragraph_count; p++) {
                    for(n=0;n<cell->nested_table_count;n++)if(cell->nested_before_paragraph && cell->nested_before_paragraph[n]==p)
                    {
                        int inline_y=ty;
                        draw_table(hdc,cell->nested_tables[n],cl,cell->paragraphs[p].inline_prefix_width_hwp ? cl+hp_line_px(cell->nested_tables[n]->width_hwp) : cr,cell->paragraphs[p].inline_prefix_width_hwp ? &inline_y : &ty);
                    }
                    draw_para(hdc, &cell->paragraphs[p], cl, cr, &ty);
                }
                for (n = 0; n < cell->nested_table_count; n++) {
                    if(!cell->nested_before_paragraph || cell->nested_before_paragraph[n]>=cell->paragraph_count)
                        draw_table(hdc, cell->nested_tables[n], cl, cr, &ty);
                }
                RestoreDC(hdc, saved_cell);
            }
        }
        for (r = 0; r < rows; r++) {
            yy += row_h[r];
        }
        *y = yy;
    }
    free(col_w);
    free(row_h);
}

static void draw_block(HDC hdc, const hp_block *b, int left, int right, int *y) {
    int x = left;
    int yy;
    if (!b) {
        return;
    }
    if (b->note_separator) {
        HPEN old = (HPEN)SelectObject(hdc, GetStockObject(BLACK_PEN));
        *y += hwp_px_i(ENDNOTE_ABOVE_HWP);
        MoveToEx(hdc, left, *y, NULL);
        LineTo(hdc, left + (right - left) / 3, *y);
        SelectObject(hdc, old);
        *y += hwp_px_i(ENDNOTE_BELOW_HWP);
    }
    yy = *y;
    if (b->kind == HP_BLOCK_PARAGRAPH) {
        draw_para(hdc, &b->paragraph, left, right, y);
    } else if (b->kind == HP_BLOCK_TABLE && b->table) {
        if (b->table->has_pos) {
            apply_float_origin(&b->table->pos, 1, &x, &yy);
            *y = yy;
            draw_table(hdc, b->table, x, right, y);
        } else {
            draw_table(hdc, b->table, left, right, y);
        }
    } else if (b->kind == HP_BLOCK_IMAGE && b->image) {
        draw_image(hdc, b->image, left, right, y);
    } else if (b->kind == HP_BLOCK_EQUATION && b->equation) {
        draw_equation(hdc, b->equation, left, right, y);
    } else if (b->kind == HP_BLOCK_CONTAINER && b->container) {
        draw_container(hdc, b->container, left, right, y);
    }
}

static void table_continuation_edges(HDC hdc,const hp_table *table,int left,int right,int origin_y,int top,int bottom) {
    int *columns=NULL,*rows=NULL,table_w=0,x0,r;
    size_t ci;
    table_geometry(hdc,table,right-left,&columns,&rows);
    if(!columns || !rows) {free(columns);free(rows);return;}
    for(r=0;r<table->col_count;r++)table_w+=columns[r];
    x0=table_origin_x(table,left,right,table_w);
    for(ci=0;ci<table->cell_count;ci++) {
        const hp_table_cell *cell=&table->cells[ci];
        int x=x0,y=origin_y,w=0,h=0,edge;
        if(cell->row<0 || cell->row>=table->row_count || cell->col<0 || cell->col>=table->col_count)continue;
        for(r=0;r<cell->row;r++)y+=rows[r];
        for(r=0;r<cell->col;r++)x+=columns[r];
        for(r=cell->col;r<table->col_count && r<cell->col+cell->colspan;r++)w+=columns[r];
        for(r=cell->row;r<table->row_count && r<cell->row+cell->rowspan;r++)h+=rows[r];
        for(edge=0;edge<2;edge++) {
            int cut=edge ? bottom : top;
            int any=cell->borders.left || cell->borders.right || cell->borders.top || cell->borders.bottom;
            if(y<cut && (cut<y+h || (edge && cut==y+h && cell->borders.bottom)) &&
               (!any || cell->borders.top || cell->borders.bottom)) {
                uint32_t color=edge ? cell->borders.color_bottom : cell->borders.color_top;
                hp_border_line style=edge?cell->borders.style_bottom:cell->borders.style_top;
                int bw=edge?cell->borders.width_bottom:cell->borders.width_top;
                draw_axis_border(hdc,x,edge?cut-1:cut,x+w,edge?cut-1:cut,
                    bw>0?bw:(cell->borders.width_px>0?cell->borders.width_px:1),
                    any?color:0xa0a0a0,any?style:HP_BORDER_SOLID);
            }
        }
    }
    free(columns);free(rows);
}
static void render_layout_piece(HDC hdc,const layout_piece *piece,int left,int top,int width) {
    const hp_block *block=flat_block(piece->block);
    int y,saved;
    if(piece->header_height>0) {
        saved=SaveDC(hdc);
        IntersectClipRect(hdc,left,top-piece->header_height,left+width,top);
        y=top-piece->header_height;
        draw_block(hdc,block,left,left+width,&y);
        RestoreDC(hdc,saved);
    }
    saved=SaveDC(hdc);
    IntersectClipRect(hdc,left,top,left+width,top+piece->height);
    y=top-piece->offset;
    draw_block(hdc,block,left,left+width,&y);
    if(block && block->kind==HP_BLOCK_TABLE && block->table)
        table_continuation_edges(hdc,block->table,left,left+width,top-piece->offset,top,top+piece->height);
    RestoreDC(hdc,saved);
}

static void paint_document_page(HDC mem,int page_index) {
    use_section(g_pages[page_index].section);
    {
        layout_page *pg = &g_pages[page_index];
        int ml, mr, mt, mb, content_w, content_h;
        int i;
        const hp_section *section=&g_doc->sections[pg->section];
        int cols=section->column_count > 0 && section->column_count <= 32 ? section->column_count : 1;
        int gap=hp_line_px(section->column_gap_hwp),col_w;
        page_metrics(&ml,&mr,&mt,&mb,&content_w,&content_h);
        col_w=(content_w-(cols-1)*gap)/cols;
        if(col_w<1)col_w=1;
        for(i=0;i<pg->count;i++) {
            const layout_piece *piece=&g_pieces[pg->first+i];
            int left=ml+piece->column*(col_w+gap);
            int top=mt+piece->y;
            render_layout_piece(mem,piece,left,top,col_w);
        }
        draw_page_extras(mem,page_index,ml,mr,mb,col_w);
        if(!pg->number_hidden && pg->number_position>0) {
            wchar_t pn[64];
            wchar_t *side=utf8_to_wide(pg->number_side);
            HFONT f, old;
            SIZE sz;
            int position=pg->number_position,x,y,top;
            if(side && side[0])swprintf(pn,64,L"%ls %d %ls",side,pg->printed_number,side);
            else swprintf(pn,64,L"%d",pg->printed_number);
            free(side);
            f = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                            L"Malgun Gothic");
            old = (HFONT)SelectObject(mem, f);
            SetTextColor(mem, RGB(90, 90, 90));
            SetBkMode(mem, TRANSPARENT);
            GetTextExtentPoint32W(mem, pn, (int)wcslen(pn), &sz);
            if(position>=7) {
                int outside=position==7 || position==8;
                int right=(pg->printed_number%2)!=0;
                if(!outside)right=!right;
                position=(position==7 || position==9 ? 1 : 4)+(right ? 2 : 0);
            }
            top=position<=3;
            x=position==1 || position==4 ? ml : position==3 || position==6 ? PAGE_W_PX-mr-sz.cx : (PAGE_W_PX-sz.cx)/2;
            y=top ? padding_px(section->page.margin_top) : PAGE_H_PX-padding_px(section->page.margin_bottom)-sz.cy;
            SetTextAlign(mem,TA_LEFT | TA_TOP | TA_NOUPDATECP);
            TextOutW(mem,x,y,pn,(int)wcslen(pn));
            SelectObject(mem, old);
            DeleteObject(f);
        }
    }
}

static void free_doc(void) {
    int font_index;for(font_index=0;font_index<g_regular_font_count;font_index++)DeleteObject(g_regular_fonts[font_index].font);g_regular_font_count=0;
    if (g_doc) {
        hp_document_free(g_doc);
        g_doc = NULL;
    }
    free(g_flat);g_flat=NULL;g_flat_capacity=0;
    free(g_pages);g_pages=NULL;g_page_capacity=0;
    free(g_pieces);g_pieces=NULL;g_piece_capacity=g_piece_count=0;
    g_flat_count = 0;
    g_page_count = 0;
    g_page_index = 0;
}

static void open_document(HWND hwnd, const wchar_t *path) {
    char *utf8;
    hp_editor *ed=NULL;
    if(!editor_discard())return;
    utf8=wide_to_utf8(path);
    hp_document *doc = NULL;
    wchar_t title[MAX_PATH + 32];
    const wchar_t *name;
    if (!utf8) {
        return;
    }
    {
        hp_status st = hp_document_read(utf8, &doc);
        if (st != HP_OK || !doc) {
            int is_hwp = hp_looks_like_hwp(utf8);
            free(utf8);
            MessageBoxW(hwnd,
                        st == HP_ERR_UNSUPPORTED && is_hwp
                            ? L"암호가 걸린 문서나 배포용 문서는 아직 열 수 없습니다."
                            : is_hwp ? L"HWP 문서를 읽지 못했습니다." : L"문서를 읽지 못했습니다. HWP 또는 HWPX 파일인지 확인하세요.",
                        L"한글패드", MB_ICONERROR);
            return;
        }
    }
    if(!hp_document_is_read_only(doc)&&hp_editor_open(utf8,&ed)!=HP_OK){hp_document_free(doc);free(utf8);MessageBoxW(hwnd,L"편집 문서를 준비하지 못했습니다.",L"한글패드",MB_ICONERROR);return;}
    if(g_editing)editor_mode(0);
    g_read_only=hp_document_is_read_only(doc);
    free(utf8);
    hp_editor_free(g_editor);g_editor=ed;g_edit_index=(size_t)-1;
    wcscpy(g_path,path);g_dirty=0;
    {char *u=wide_to_utf8(path);g_source_hwp=u?(hp_looks_like_hwp(u)||hp_looks_like_hwpml(u)):0;free(u);}
    editor_list(0);
    free_doc();
    g_doc = doc;
    g_font_change_count=0;g_font_changes_truncated=0;
    rebuild_flat();
    rebuild_pages(hwnd);
    g_page_index = 0;
    update_status();
    name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    swprintf(title, MAX_PATH + 32, L"한글패드 " HP_VERSION_STRING L" — %ls", name);
    SetWindowTextW(hwnd, title);
    editor_title();
    InvalidateRect(hwnd, NULL, TRUE);
}

#include "gui_edit.inc"

static void file_open(HWND hwnd) {
    OPENFILENAMEW ofn;
    wchar_t file[MAX_PATH];
    ZeroMemory(&ofn, sizeof(ofn));
    file[0] = 0;
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"한글 문서 (*.hwp;*.hwpx;*.hml)\0*.hwp;*.hwpx;*.hml\0HWPX 문서 (*.hwpx)\0*.hwpx\0HWP 문서 (*.hwp)\0*.hwp\0모든 파일 (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = L"hwpx";
    if (GetOpenFileNameW(&ofn)) {
        open_and_remember(hwnd, file);
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
                L"한글패드 " HP_VERSION_STRING L" (" HP_VERSION_DATE L")\n"
                L"HWP·HWPX 문서 보기, 편집, 인쇄\n\n"
                L"본문·표 안 텍스트 편집, 문단 추가·삭제, 글자·문단 모양,\n"
                L"찾기·바꾸기, 화면 확대, 인쇄와 HWPX 저장을 지원합니다.\n"
                L"F2: 본문 편집 / F5: 미리 보기 / 도움말 → 단축키 일람\n\n"
                L"본 제품은 한글과컴퓨터의 HWP 문서 파일(.hwp) 공개 문서를 "
                L"참고하여 개발하였습니다.",
                L"한글패드 정보", MB_OK | MB_ICONINFORMATION);
}

/* ---- high DPI ---- */

typedef BOOL(WINAPI *dpi_context_fn)(HANDLE);
typedef UINT(WINAPI *dpi_window_fn)(HWND);

/* Per-monitor v2 where available (Windows 10 1703+), else system DPI aware. */
static void enable_dpi_awareness(void) {
    HMODULE user = GetModuleHandleW(L"user32.dll");
    dpi_context_fn set_context = user ? (dpi_context_fn)(void (*)(void))GetProcAddress(user, "SetProcessDpiAwarenessContext") : NULL;
    if (set_context && set_context((HANDLE)(INT_PTR)-4)) { /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
        return;
    }
    if (set_context && set_context((HANDLE)(INT_PTR)-3)) { /* PER_MONITOR_AWARE */
        return;
    }
    SetProcessDPIAware();
}

static int window_dpi(HWND hwnd) {
    HMODULE user = GetModuleHandleW(L"user32.dll");
    dpi_window_fn get_dpi = user ? (dpi_window_fn)(void (*)(void))GetProcAddress(user, "GetDpiForWindow") : NULL;
    int dpi = get_dpi && hwnd ? (int)get_dpi(hwnd) : 0;
    if (dpi <= 0) {
        HDC screen = GetDC(NULL);
        dpi = screen ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
        if (screen) {
            ReleaseDC(NULL, screen);
        }
    }
    return dpi >= 48 && dpi <= 960 ? dpi : 96;
}

/* Windows common controls v6 (themed buttons, modern dialogs) through the
 * manifest in resource 2, activated for the whole process. */
static void enable_visual_styles(void) {
    ACTCTXW ctx;
    HANDLE handle;
    ULONG_PTR cookie;
    ZeroMemory(&ctx, sizeof(ctx));
    ctx.cbSize = sizeof(ctx);
    ctx.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
    ctx.hModule = GetModuleHandleW(NULL);
    ctx.lpResourceName = MAKEINTRESOURCEW(2);
    handle = CreateActCtxW(&ctx);
    if (handle != INVALID_HANDLE_VALUE) {
        ActivateActCtx(handle, &cookie);
    }
}

#include "gui_ui.inc"

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmd, int show) {
    WNDCLASSW wc;
    HRESULT hr;
    (void)hPrev;
    (void)cmd;

    enable_dpi_awareness();
    enable_visual_styles();
    {
        /* Explorer "open with" / double-click passes the document path. */
        int argc = 0;
        wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv && argc >= 2 && argv[1][0] && wcslen(argv[1]) < MAX_PATH) {
            wcscpy(g_startup_path, argv[1]);
        }
        if (argv) {
            LocalFree(argv);
        }
    }
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
    {
        /* 920 x 1100 at 100%, scaled to the display and kept inside the work area */
        RECT work;
        int dpi = window_dpi(NULL), w = MulDiv(920, dpi, 96), h = MulDiv(1100, dpi, 96);
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
            if (w > work.right - work.left) {
                w = work.right - work.left;
            }
            if (h > (work.bottom - work.top) * 96 / 100) {
                h = (work.bottom - work.top) * 96 / 100;
            }
        }
        g_main = CreateWindowExW(0, L"HangulPadMain", L"한글패드 " HP_VERSION_STRING,
                                 WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                                 w, h, NULL, NULL, hInst, NULL);
    }
    if (!g_main) {
        if (g_wic) {
            g_wic->lpVtbl->Release(g_wic);
        }
        CoUninitialize();
        return 1;
    }
    ShowWindow(g_main, show);
    ui_message_loop();
    if (g_wic) {
        g_wic->lpVtbl->Release(g_wic);
    }
    CoUninitialize();
    return 0;
}
















