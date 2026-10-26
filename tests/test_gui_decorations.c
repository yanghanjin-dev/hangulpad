/* Verify complete rendered underline spans and real HWPX border patterns. */
#define wWinMain hp_unused_gui_entry
#include "../src/gui_win.c"
#undef wWinMain
#define CHECK(x) do{if(!(x)){fprintf(stderr,"Decoration failure %d: %s\n",__LINE__,#x);return 1;}}while(0)
static int longest(const uint32_t *pixels,int width,int top,int bottom){
    int y,x,best=0;
    for(y=top;y<bottom;y++){int length=0;for(x=0;x<width;x++){
        if((pixels[y*width+x]&0xffffff)==0){length++;if(length>best)best=length;}else length=0;
    }}return best;
}
static int dark_row(const uint32_t *pixels,int width,int y,int left,int right){int x,n=0;for(x=left;x<right;x++)if((pixels[y*width+x]&0xffffff)==0)n++;return n;}
int main(void){
    HDC dc=CreateCompatibleDC(NULL);BITMAPINFO bi={0};HBITMAP bm,old;uint32_t *bits=NULL;RECT rect={0,0,640,180};
    hp_paragraph *title=NULL;hp_table *callout=NULL;size_t si,pi,ri;
    bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=640;bi.bmiHeader.biHeight=-180;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;
    bm=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,(void **)&bits,NULL,0);CHECK(bm&&bits);old=SelectObject(dc,bm);
    CHECK(hp_hwpx_read("sample/corpus/ethics.hwpx",&g_doc)==HP_OK);
    for(si=0;si<g_doc->section_count;si++)for(pi=0;pi<g_doc->sections[si].block_count;pi++){
        hp_block *b=&g_doc->sections[si].blocks[pi];
        if(b->kind==HP_BLOCK_PARAGRAPH){
            wchar_t *text=para_concat_wide(&b->paragraph);int match=text&&wcscmp(text,L"공공부문 인공지능 윤리기준")==0;free(text);
            if(match)for(ri=0;ri<b->paragraph.run_count;ri++)if(b->paragraph.runs[ri].underline&&b->paragraph.runs[ri].font_size_hwp==2400)title=&b->paragraph;
        }else if(b->kind==HP_BLOCK_TABLE&&b->table&&b->table->width_hwp==44511)callout=b->table;
    }
    CHECK(title&&callout&&callout->cell_count==1);
    CHECK(callout->cells[0].borders.style_left==HP_BORDER_DASH&&callout->cells[0].borders.style_right==HP_BORDER_DASH);
    CHECK(callout->cells[0].borders.style_top==HP_BORDER_DASH&&callout->cells[0].borders.style_bottom==HP_BORDER_DASH);
    FillRect(dc,&rect,(HBRUSH)GetStockObject(WHITE_BRUSH));
    layout_text_para(dc,title,0,640,0,1,NULL,0,NULL,NULL,0);GdiFlush();
    CHECK(longest(bits,640,20,50)>=415); /* continuous across both word spaces */
    {
        hp_paragraph split=*title;hp_run runs[2];
        for(ri=0;ri<title->run_count;ri++)if(title->runs[ri].font_size_hwp==2400)break;
        CHECK(ri<title->run_count);runs[0]=runs[1]=title->runs[ri];
        runs[0].text="공공부문 인공지능 ";runs[1].text="윤리기준";split.runs=runs;split.run_count=2;
        FillRect(dc,&rect,(HBRUSH)GetStockObject(WHITE_BRUSH));layout_text_para(dc,&split,0,640,0,1,NULL,0,NULL,NULL,0);GdiFlush();
        CHECK(longest(bits,640,20,50)>=415); /* continuous across a run boundary */
        runs[0].underline=runs[1].underline=0;
        FillRect(dc,&rect,(HBRUSH)GetStockObject(WHITE_BRUSH));layout_text_para(dc,&split,0,640,0,1,NULL,0,NULL,NULL,0);GdiFlush();
        CHECK(longest(bits,640,20,50)<300); /* ordinary text never gains a line */
    }
    FillRect(dc,&rect,(HBRUSH)GetStockObject(WHITE_BRUSH));
    draw_axis_border(dc,10,70,210,70,1,0,HP_BORDER_DASH);
    draw_axis_border(dc,10,80,210,80,1,0,HP_BORDER_SOLID);
    draw_axis_border(dc,10,90,210,90,3,0,HP_BORDER_DASH);
    draw_axis_border(dc,220,70,220,170,1,0,HP_BORDER_DASH);GdiFlush();
    CHECK(dark_row(bits,640,70,10,210)==100);CHECK(dark_row(bits,640,80,10,210)==200);
    CHECK(dark_row(bits,640,90,10,210)>80&&dark_row(bits,640,90,10,210)<120);
    draw_axis_border(dc,10,120,210,120,3,0,HP_BORDER_DOUBLE_SLIM);GdiFlush();
    CHECK(dark_row(bits,640,119,10,210)==200 && dark_row(bits,640,120,10,210)==0 && dark_row(bits,640,121,10,210)==200);
    {int y,n=0;for(y=70;y<170;y++)if((bits[y*640+220]&0xffffff)==0)n++;CHECK(n==50);}
    SelectObject(dc,old);DeleteObject(bm);DeleteDC(dc);free_doc();puts("Continuous title underline and dashed/solid/thick/vertical borders passed");return 0;
}
