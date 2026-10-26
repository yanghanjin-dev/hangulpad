/*
 * HangulPad Windows GUI — HWPX engine layout viewer.
 * Renders paragraphs, tables (incl. nested), images, floats, containers, equations.
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include "hp/hwpx.h"
#include "hp/ir.h"
#include "hp/version.h"
#include "line_layout.h"
#include "pagination.h"
#include "table_pagination.h"

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
#define IDM_FONT_REPORT 40011

#define TOOLBAR_H 40
#define FONT_SCALE 8
static int g_paper_width = 794, g_paper_height = 1123;
static size_t g_layout_section;
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
} layout_page;
typedef struct { int block, column, y, offset, height, header_height; } layout_piece;
static int g_unsafe_table_cuts, g_suppressed_headers;

static HWND g_main;
static HWND g_btn_prev;
static HWND g_btn_next;
static HWND g_status;
static hp_document *g_doc;
static flat_ref *g_flat;
static int g_flat_capacity;
static int g_flat_count;
static layout_page *g_pages;
static int g_page_capacity;
static layout_piece *g_pieces;
static int g_piece_count, g_piece_capacity;
static int g_page_count;
static int g_page_index;
static IWICImagingFactory *g_wic;
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

static int para_line_pitch_px(const hp_paragraph *para, int font_px) {
    int pct = para && para->line_spacing_percent > 0 ? para->line_spacing_percent : 160;
    int pitch = font_px * pct / 100;
    if (pitch < font_px + 2) {
        pitch = font_px + 2;
    }
    return pitch;
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
    lf.lfItalic=(BYTE)run->italic;lf.lfUnderline=(BYTE)run->underline;lf.lfStrikeOut=(BYTE)run->strikeout;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
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

static void update_status(void) {
    wchar_t buf[200];
    if (g_doc && g_page_count > 0) {
        swprintf(buf, 200, L"페이지 %d / %d  —  엔진 렌더 (들여쓰기·행간·테두리·폰트)", g_page_index + 1,
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
static int cell_tail_spacing(HDC dc,const hp_table_cell *cell,int width) {
    const hp_paragraph *p;hp_cut_map ink={0};size_t i;int full,trim,ink_bottom=0;
    if(!cell->paragraph_count)return 0;
    p=&cell->paragraphs[cell->paragraph_count-1];
    if(p->object_anchor_only || !p->seg_count)return 0;
    for(i=0;i<cell->nested_table_count;i++)if(!cell->nested_before_paragraph ||
        cell->nested_before_paragraph[i]>=cell->paragraph_count)return 0;
    trim=hwp_px_i((int32_t)p->segs[p->seg_count-1].spacing_hwp);
    if(trim<0)trim=0;
    full=layout_text_para(dc,p,0,width,0,0,NULL,0,NULL,&ink,0);
    if(ink.failed) {free(ink.bands);return 0;}
    for(i=0;i<ink.count;i++)if(ink.bands[i].bottom>ink_bottom)ink_bottom=ink.bands[i].bottom;
    free(ink.bands);
    if(trim>full-ink_bottom)trim=full-ink_bottom;
    return trim;
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
    for (c=0; c<cols; c++) { if (!cw[c]) cw[c] = table_w/cols; sum += cw[c]; }
    if (sum > 0) for (c=0; c<cols; c++) cw[c] = MulDiv(cw[c],table_w,sum);
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
    return 1;
}
static void rebuild_pages(HWND hwnd) {
    HDC hdc;
    hp_flow flow={0,0,0,1,1};
    int i, ml=0,mr=0,mt=0,mb=0,content_w=0,content_h=0,col_w=0;
    size_t section=(size_t)-1;
    int previous_start_page=-1,previous_start_column=-1;
    int ink_page=-1,ink_column=-1,previous_ink_bottom=0;
    g_page_count=g_piece_count=g_unsafe_table_cuts=g_suppressed_headers=0;
    if (!g_flat_count) return;
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
            flow.page=g_page_count;flow.column=flow.y=0;
            flow.columns=sec->column_count > 0 && sec->column_count <= 32 ? sec->column_count : 1;
            flow.height=content_h > 0 ? content_h : 1;
            col_w=(content_w-(flow.columns-1)*hp_line_px(sec->column_gap_hwp))/flow.columns;
            if(col_w<1)col_w=1;
        }
        if(block->page_break_before) hp_flow_break(&flow,1);
        else if(block->column_break_before) hp_flow_break(&flow,0);
        else if(block->saved_break_before && flow.page==previous_start_page && flow.column==previous_start_column)
            hp_flow_break(&flow,0);
        previous_start_page=flow.page;previous_start_column=flow.column;
        if(ink_page!=flow.page || ink_column!=flow.column)previous_ink_bottom=0;
        if(block->has_saved_y) {
            int target=hp_line_px(block->saved_y_hwp);
            if(block->kind==HP_BLOCK_PARAGRAPH)target-=hwp_px_i(block->paragraph.space_before_hwp);
            /* Drop accumulated rounding/trailing leading only in empty space;
             * expanded text must never overlap preceding visible content. */
            if(target>=previous_ink_bottom && target<flow.height)flow.y=target;
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
            layout_text_para(hdc,para,0,col_w,0,0,ends,capacity,hard,&ink,hwp_px_i(para->space_before_hwp));
            hp_cut_normalize(&ink);
            if(ink.failed) {free(ends);free(hard);free(ink.bands);goto failed;}
            for(j=0;j<capacity;j++) {
                if(ends[j]>0) { ends[j]+=hwp_px_i(para->space_before_hwp);count=j+1; }
                if(hard[j]>0)hard[j]+=hwp_px_i(para->space_before_hwp);
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
        while(offset<h) {
            hp_slice slice;
            int reserved=0;
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
            } else if(block->kind!=HP_BLOCK_PARAGRAPH || !para_is_blank(&block->paragraph))previous_ink_bottom=slice.y+slice.height;
            previous_page=slice.page;previous_column=slice.column;
            offset+=slice.height;
            if(offset==limit && limit<h)hp_flow_break(&flow,0);
        }
        free(ends);free(hard);free(ink.bands);
    }
    ReleaseDC(hwnd,hdc);
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
static double glyph_step(HDC hdc, const hp_run *run, const wchar_t *text, int length,
                      int x, int y, int draw) {
    HFONT font = font_for_glyph(hdc, run, text, length), old = (HFONT)SelectObject(hdc, font);
    SIZE size;
    int lang = glyph_language(text[0]);
    int ratio = run->width_percent[lang] > 0 ? run->width_percent[lang] : 100;
    double em=(run->font_size_hwp ? run->font_size_hwp : 1000)/75.0;
    double advance;
    GetTextExtentPoint32W(hdc, text, length, &size);
    advance = size.cx*ratio/(100.0*FONT_SCALE) + em*run->spacing_percent[lang]/100.0;
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
static int saved_line_box(const hp_paragraph *para, const hp_line_seg *seg, int left, int right, int *origin) {
    int box_left=left+hwp_px_i(para->indent_left_hwp);
    int box_right=right-hwp_px_i(para->indent_right_hwp);
    int available,saved_width=hp_line_px(seg->horzsize_hwp);
    if(box_left<left)box_left=left;
    if(box_right>right)box_right=right;
    if(box_right<=box_left)box_right=box_left+1;
    *origin=box_left+hwp_px_i((int32_t)seg->horzpos_hwp);
    if(*origin<box_left || *origin>=box_right)*origin=box_left;
    available=box_right-*origin;
    if(seg==para->segs && para->inline_prefix_width_hwp) {
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
    while(start<end) {
        const hp_run *run=run_at_char(para,start,NULL);
        int len=glyph_length(all,start,end);
        if(all[start]!='\n' && all[start]!='\r')width+=glyph_step(hdc,run ? run : &fallback,all+start,len,0,0,0);
        start+=(size_t)len;
    }
    return width;
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
            int origin,available=saved_line_box(para,&para->segs[si],left,right,&origin);
            if(end>text_length)end=text_length;
            if(begin>end)begin=end;
            if(span_width(hdc,para,all,begin,end)>available)reflow=1;
        }
        if(group && hard_breaks && line_count<capacity)hard_breaks[line_count]=group_base;
        si=group;
        do {
            const hp_line_seg *seg=&para->segs[si<=last ? si : last];
            size_t start=reflow ? stream : seg->textpos;
            size_t end=reflow ? end_group : (si<last ? para->segs[si+1].textpos : end_group);
            size_t finish,ci;
            int origin,available=saved_line_box(para,seg,left,right,&origin);
            int pitch=hp_line_px(hp_line_height(seg));double width=0,x;
            if(pitch<1)pitch=1;
            if(end>text_length)end=text_length;
            if(start>end)start=end;
            if(si<=last)top=group_base+hp_line_px((int32_t)seg->vertpos_hwp-source_origin);
            else top=bottom;
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
                if(reflow && finish>start && width+advance>available)break;
                width+=advance;finish+=(size_t)len;
            }
            x=origin;
            if(para->align==HP_ALIGN_CENTER && width<available)x+=(available-width)/2;
            else if(para->align==HP_ALIGN_RIGHT && width<available)x+=available-width;
            if(draw)for(ci=start;ci<finish;) {
                const hp_run *run=run_at_char(para,ci,NULL);
                int len=glyph_length(all,ci,finish);
                if(all[ci]!='\n' && all[ci]!='\r') {
                    int glyph_y=base+top;
                    if(seg->baseline_hwp) {
                        HFONT gf=font_for_glyph(hdc,run ? run : &fallback,all+ci,len),old=(HFONT)SelectObject(hdc,gf);
                        TEXTMETRICW tm;GetTextMetricsW(hdc,&tm);
                        glyph_y+=hp_line_px(seg->baseline_hwp)-(tm.tmAscent+FONT_SCALE/2)/FONT_SCALE;
                        SelectObject(hdc,old);DeleteObject(gf);
                    }
                    x+=glyph_step(hdc,run ? run : &fallback,all+ci,len,(int)(x+0.5),glyph_y,1);
                }
                ci+=(size_t)len;
            }
            if(ink) {
                int ink_top=0,ink_height=text_ink_height(hdc,para,all,start,finish,hp_line_px(seg->baseline_hwp),&ink_top);
                hp_cut_add(ink,ink_origin+top+ink_top,ink_origin+top+ink_top+ink_height);
            }
            if(top+pitch>bottom)bottom=top+pitch;
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
    size_t ri;
    int height=14,pitch;
    if(para->object_anchor_only)return 0;
    if(para->seg_count) return layout_saved_para(hdc,para,left,right,base,draw,ends,capacity,hard,ink,ink_origin);
    if(para_is_blank(para)) {
        int fp=para->run_count && para->runs[0].font_size_hwp ? hp_line_px(para->runs[0].font_size_hwp) : 14;
        return para_line_pitch_px(para,fp)/2;
    }
    for(ri=0;ri<para->run_count;ri++) {
        HFONT font=make_font(hdc,&para->runs[ri]),old=(HFONT)SelectObject(hdc,font);
        TEXTMETRICW tm;GetTextMetricsW(hdc,&tm);
        if((tm.tmHeight+tm.tmExternalLeading+FONT_SCALE-1)/FONT_SCALE>height)height=(tm.tmHeight+tm.tmExternalLeading+FONT_SCALE-1)/FONT_SCALE;
        SelectObject(hdc,old);DeleteObject(font);
    }
    pitch=para_line_pitch_px(para,height);
    line.vertsize_hwp=line.textheight_hwp=(uint32_t)(height*75);
    line.spacing_hwp=(uint32_t)((pitch-height)*75);
    line.horzpos_hwp=(uint32_t)para->indent_first_hwp;
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
    yy = *y;
    apply_float_origin(&img->pos, img->has_pos, &x, &yy);
    mem = CreateCompatibleDC(hdc);
    old = SelectObject(mem, hbmp);
    SetStretchBltMode(hdc, HALFTONE);
    StretchBlt(hdc, x, yy, dw, dh, mem, sx, sy, sw, sh, SRCCOPY);
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
    x0 = left+(content_w-table_w)/2;
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
                int bw = cell->borders.width_px > 0 ? cell->borders.width_px : 1;
                int has_b =
                    cell->borders.left || cell->borders.right || cell->borders.top || cell->borders.bottom;
                if (has_b) {
                    if (cell->borders.top) {
                        HPEN pen = CreatePen(PS_SOLID, bw,
                                             RGB((cell->borders.color_top >> 16) & 255,
                                                 (cell->borders.color_top >> 8) & 255,
                                                 cell->borders.color_top & 255));
                        HGDIOBJ op = SelectObject(hdc, pen);
                        MoveToEx(hdc, cx, cy, NULL);
                        LineTo(hdc, cx + cw, cy);
                        SelectObject(hdc, op);
                        DeleteObject(pen);
                    }
                    if (cell->borders.bottom) {
                        HPEN pen = CreatePen(PS_SOLID, bw,
                                             RGB((cell->borders.color_bottom >> 16) & 255,
                                                 (cell->borders.color_bottom >> 8) & 255,
                                                 cell->borders.color_bottom & 255));
                        HGDIOBJ op = SelectObject(hdc, pen);
                        MoveToEx(hdc, cx, cy + ch - 1, NULL);
                        LineTo(hdc, cx + cw, cy + ch - 1);
                        SelectObject(hdc, op);
                        DeleteObject(pen);
                    }
                    if (cell->borders.left) {
                        HPEN pen = CreatePen(PS_SOLID, bw,
                                             RGB((cell->borders.color_left >> 16) & 255,
                                                 (cell->borders.color_left >> 8) & 255,
                                                 cell->borders.color_left & 255));
                        HGDIOBJ op = SelectObject(hdc, pen);
                        MoveToEx(hdc, cx, cy, NULL);
                        LineTo(hdc, cx, cy + ch);
                        SelectObject(hdc, op);
                        DeleteObject(pen);
                    }
                    if (cell->borders.right) {
                        HPEN pen = CreatePen(PS_SOLID, bw,
                                             RGB((cell->borders.color_right >> 16) & 255,
                                                 (cell->borders.color_right >> 8) & 255,
                                                 cell->borders.color_right & 255));
                        HGDIOBJ op = SelectObject(hdc, pen);
                        MoveToEx(hdc, cx + cw - 1, cy, NULL);
                        LineTo(hdc, cx + cw - 1, cy + ch);
                        SelectObject(hdc, op);
                        DeleteObject(pen);
                    }
                }
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
    int yy = *y;
    if (!b) {
        return;
    }
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
    x0=left+(right-left-table_w)/2;
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
            if(y<cut && cut<y+h && (!any || cell->borders.top || cell->borders.bottom)) {
                uint32_t color=edge ? cell->borders.color_bottom : cell->borders.color_top;
                HPEN pen=CreatePen(PS_SOLID,cell->borders.width_px>0 ? cell->borders.width_px : 1,
                    any ? RGB((color>>16)&255,(color>>8)&255,color&255) : RGB(160,160,160));
                HGDIOBJ old=SelectObject(hdc,pen);
                MoveToEx(hdc,x,edge ? cut-1 : cut,NULL);LineTo(hdc,x+w,edge ? cut-1 : cut);
                SelectObject(hdc,old);DeleteObject(pen);
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
        {
            wchar_t pn[32];
            HFONT f, old;
            SIZE sz;
            swprintf(pn, 32, L"- %d -", page_index + 1);
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

    use_section(g_pages[g_page_index].section);
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
    paint_document_page(mem,g_page_index);
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
    free(g_flat);g_flat=NULL;g_flat_capacity=0;
    free(g_pages);g_pages=NULL;g_page_capacity=0;
    free(g_pieces);g_pieces=NULL;g_piece_capacity=g_piece_count=0;
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
    g_font_change_count=0;g_font_changes_truncated=0;
    rebuild_flat();
    rebuild_pages(hwnd);
    g_page_index = 0;
    update_status();
    name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    swprintf(title, MAX_PATH + 32, L"한글패드 " HP_VERSION_STRING L" — %ls", name);
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
                L"한글패드 " HP_VERSION_STRING L" (" HP_VERSION_DATE L")\n"
                L"HWPX 엔진 레이아웃\n\n"
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
        AppendMenuW(help, MF_STRING, IDM_FONT_REPORT, L"대체 글꼴 내역");
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
        case IDM_FONT_REPORT:
            font_report(hwnd);
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
    g_main = CreateWindowExW(0, L"HangulPadMain", L"한글패드 " HP_VERSION_STRING,
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
