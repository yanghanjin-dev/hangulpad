/* Offscreen rendering uses the exact GDI page painter used by the desktop UI. */
#define wWinMain hp_unused_gui_entry
#include "../src/gui_win.c"
#undef wWinMain
#include "../src/win_utf8.h"
static FILE *open_utf8(const char *path,const wchar_t *mode) {
    wchar_t *wide=utf8_to_wide(path);FILE *f=wide ? _wfopen(wide,mode) : NULL;free(wide);return f;
}
static void json_text(FILE *f,const char *s) {
    const unsigned char *p=(const unsigned char *)(s ? s : "");fputc('"',f);
    for(;*p;p++) {if(*p=='"' || *p=='\\')fprintf(f,"\\%c",*p);else if(*p<32)fprintf(f,"\\u%04x",*p);else fputc(*p,f);}
    fputc('"',f);
}
static int bitmap_page(const char *path,int page) {
    HDC dc=CreateCompatibleDC(NULL);BITMAPINFO bi={0};BITMAPFILEHEADER bh={0};
    HBITMAP bm,old;void *pixels=NULL;FILE *f;int ok;
    use_section(g_pages[page].section);
    bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=PAGE_W_PX;
    bi.bmiHeader.biHeight=-PAGE_H_PX;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;
    bm=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&pixels,NULL,0);
    if(!bm || !pixels) {DeleteDC(dc);return 0;}
    old=(HBITMAP)SelectObject(dc,bm);
    {RECT rect={0,0,PAGE_W_PX,PAGE_H_PX};FillRect(dc,&rect,(HBRUSH)GetStockObject(WHITE_BRUSH));}
    paint_document_page(dc,page);GdiFlush();
    bh.bfType=0x4d42;bh.bfOffBits=sizeof(bh)+sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biSizeImage=(DWORD)(PAGE_W_PX*PAGE_H_PX*4);bh.bfSize=bh.bfOffBits+bi.bmiHeader.biSizeImage;
    f=open_utf8(path,L"wb");ok=f && fwrite(&bh,sizeof(bh),1,f)==1 && fwrite(&bi.bmiHeader,sizeof(BITMAPINFOHEADER),1,f)==1 && fwrite(pixels,bi.bmiHeader.biSizeImage,1,f)==1;
    if(f && fclose(f)!=0)ok=0;
    SelectObject(dc,old);DeleteObject(bm);DeleteDC(dc);return ok;
}
int main(int argc,char **argv) {
    FILE *f;char path[4096];int i,j,first=1,last;HRESULT hr;
    hp_utf8_args(&argc,&argv);
    if(argc<3 || argc>5) {fprintf(stderr,"usage: layout_render input.hwpx existing-output-directory [first-page [last-page]]\n");return 2;}
    if(hp_hwpx_read(argv[1],&g_doc)!=HP_OK)return 1;
    hr=CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
    if(SUCCEEDED(hr))CoCreateInstance(&CLSID_WICImagingFactory,NULL,CLSCTX_INPROC_SERVER,&IID_IWICImagingFactory,(void **)&g_wic);
    rebuild_flat();rebuild_pages(NULL);last=g_page_count;
    if(argc>3)first=atoi(argv[3]);
    if(argc>4)last=atoi(argv[4]);
    if(first<1 || last<first || last>g_page_count) {free_doc();return 2;}
    snprintf(path,sizeof(path),"%s/layout.json",argv[2]);f=open_utf8(path,L"wb");if(!f)return 1;
    fprintf(f,"{\"version\":\"%s\",\"pages\":%d,\"fallback_cuts\":%d,\"suppressed_headers\":%d,\"pieces\":[",HP_VERSION_STRING,g_page_count,g_unsafe_table_cuts,g_suppressed_headers);
    for(i=0;i<g_page_count;i++)for(j=0;j<g_pages[i].count;j++) {
        int index=g_pages[i].first+j;const layout_piece *p=&g_pieces[index];const hp_block *b=flat_block(p->block);
        if(index)fputc(',',f);
        fprintf(f,"{\"page\":%d,\"block\":%d,\"kind\":%d,\"column\":%d,\"y\":%d,\"offset\":%d,\"height\":%d,\"header\":%d,\"text\":",i+1,p->block,b->kind,p->column,p->y,p->offset,p->height,p->header_height);
        if(b->kind==HP_BLOCK_PARAGRAPH && b->paragraph.run_count)json_text(f,b->paragraph.runs[0].text);else json_text(f,"");fputc('}',f);
    }
    fputs("]}\n",f);if(fclose(f)!=0)return 1;
    for(i=first;i<=last;i++) {snprintf(path,sizeof(path),"%s/page-%03d.bmp",argv[2],i);if(!bitmap_page(path,i-1))return 1;}
    printf("pages=%d rendered=%d..%d fallback_cuts=%d\n",g_page_count,first,last,g_unsafe_table_cuts);
    free_doc();if(g_wic)g_wic->lpVtbl->Release(g_wic);if(SUCCEEDED(hr))CoUninitialize();return 0;
}
