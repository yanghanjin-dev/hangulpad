/* Exercise the real GDI measurement/page planner without driving desktop UI. */
#define wWinMain hp_unused_gui_entry
#include "../src/gui_win.c"
#undef wWinMain
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"GUI layout failure %d: %s\n",__LINE__,#x); free_doc(); return 1; } } while(0)
static void small_page(hp_section *sec) {
    sec->page=(hp_page){15000,15000,750,750,750,750};
}
static void table_document(int rows, int row_height) {
    hp_table *t=(hp_table *)calloc(1,sizeof(*t));
    int i;
    g_doc=hp_document_new();small_page(&g_doc->sections[0]);
    g_doc->sections[0].blocks=(hp_block *)calloc(1,sizeof(hp_block));
    g_doc->sections[0].block_count=1;
    g_doc->sections[0].blocks[0].kind=HP_BLOCK_TABLE;
    g_doc->sections[0].blocks[0].table=t;
    t->row_count=rows;t->col_count=1;t->width_hwp=12000;t->cell_count=(size_t)rows;
    t->cells=(hp_table_cell *)calloc((size_t)rows,sizeof(hp_table_cell));
    for(i=0;i<rows;i++) {
        t->cells[i].row=i;t->cells[i].rowspan=t->cells[i].colspan=1;
        t->cells[i].height_hwp=(uint32_t)(row_height*75);t->cells[i].width_hwp=12000;
    }
}
static void cell_text(hp_table_cell *cell,const char *text) {
    hp_paragraph *p=(hp_paragraph *)calloc(1,sizeof(*p));
    hp_run *run=(hp_run *)calloc(1,sizeof(*run));
    run->text=(char *)malloc(strlen(text)+1);strcpy(run->text,text);run->font_size_hwp=1000;
    p->runs=run;p->run_count=1;p->line_spacing_percent=200;
    cell->paragraphs=p;cell->paragraph_count=1;
}
static uint32_t *header_pixels(const layout_piece *piece,int height) {
    BITMAPINFO info={0};void *bits=NULL;uint32_t *copy;
    HDC dc=CreateCompatibleDC(NULL);HBITMAP bitmap,old;
    RECT rect={0,0,200,200};
    info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=200;
    info.bmiHeader.biHeight=-200;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    info.bmiHeader.biCompression=BI_RGB;
    bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,NULL,0);
    if(!bitmap || !bits) {DeleteDC(dc);return NULL;}
    old=(HBITMAP)SelectObject(dc,bitmap);FillRect(dc,&rect,(HBRUSH)GetStockObject(WHITE_BRUSH));
    render_layout_piece(dc,piece,0,piece->header_height,180);GdiFlush();
    copy=(uint32_t *)malloc((size_t)height*200*sizeof(uint32_t));
    if(copy)memcpy(copy,bits,(size_t)height*200*sizeof(uint32_t));
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);return copy;
}
int main(void) {
    int i,total=0,offset=0;
    HDC dc=GetDC(NULL);
    {
        hp_run original={0}; HFONT font,previous; wchar_t face[LF_FACESIZE];
        original.font_face="Arial";original.font_size_hwp=1000;
        g_font_change_count=0;
        font=font_for_glyph(dc,&original,L"A",1);previous=SelectObject(dc,font);
        GetTextFaceW(dc,LF_FACESIZE,face);
        CHECK(_wcsicmp(face,L"Arial")==0);CHECK(g_font_change_count==0);
        SelectObject(dc,previous);DeleteObject(font);
        /* Arial lacks Hangul: only that glyph changes, and it is reported once. */
        font=font_for_glyph(dc,&original,L"\xAC00",1);previous=SelectObject(dc,font);
        CHECK(selected_font_has_text(dc,L"\xAC00",1));
        CHECK(g_font_change_count==1);CHECK(g_font_changes[0].code==0xAC00);
        CHECK(wcscmp(g_font_changes[0].source,L"Arial")==0);
        SelectObject(dc,previous);DeleteObject(font);
        font=font_for_glyph(dc,&original,L"\xAC00",1);DeleteObject(font);
        CHECK(g_font_change_count==1);
        font=font_for_glyph(dc,&original,L"A",1);previous=SelectObject(dc,font);
        GetTextFaceW(dc,LF_FACESIZE,face);CHECK(_wcsicmp(face,L"Arial")==0);
        SelectObject(dc,previous);DeleteObject(font);g_font_change_count=0;
        /* A Latin override must not alter the original Hangul face. */
        strcpy(original.script_faces[1],"Times New Roman");
        original.italic=original.underline=original.strikeout=1;
        font=font_for_glyph(dc,&original,L"A",1);previous=SelectObject(dc,font);
        GetTextFaceW(dc,LF_FACESIZE,face);CHECK(_wcsicmp(face,L"Times New Roman")==0);
        {LOGFONTW lf;GetObjectW(font,sizeof(lf),&lf);CHECK(lf.lfItalic && lf.lfUnderline && lf.lfStrikeOut);}
        CHECK(strcmp(original.font_face,"Arial")==0 && g_font_change_count==0);
        SelectObject(dc,previous);DeleteObject(font);
        {hp_table_cell c={0};int top,center,bottom;
         c.vertical_align=0;top=cell_vertical_offset(dc,&c,100,100);
         c.vertical_align=1;center=cell_vertical_offset(dc,&c,100,100);
         c.vertical_align=2;bottom=cell_vertical_offset(dc,&c,100,100);
         CHECK(top==0 && center==50 && bottom==100);}
    }
    hp_run run={0};hp_line_seg line={0};hp_paragraph para={0};
    int ends[128]={0},h;
    run.text="Long table title that must wrap within its cell";
    run.font_size_hwp=1400;run.width_percent[1]=95;run.spacing_percent[1]=-5;
    para.runs=&run;para.run_count=1;para.segs=&line;para.seg_count=1;para.align=HP_ALIGN_CENTER;
    line.textheight_hwp=line.vertsize_hwp=1400;line.spacing_hwp=700;line.horzsize_hwp=7500;
    h=layout_saved_para(dc,&para,0,50,0,0,ends,128,NULL,NULL,0);
    CHECK(h>hp_line_px(2100));CHECK(ends[1]>ends[0]);
    CHECK(h==measure_para_height(dc,&para,50));
    run.text="A\nB";para.segs=NULL;para.seg_count=0;
    CHECK(measure_para_height(dc,&para,100)>30);
    ReleaseDC(NULL,dc);
    g_doc=hp_document_new();small_page(&g_doc->sections[0]);
    CHECK(hp_section_add_paragraph_text(&g_doc->sections[0],"first")==HP_OK);
    CHECK(hp_section_add_paragraph_text(&g_doc->sections[0],"forced")==HP_OK);
    g_doc->sections[0].blocks[1].page_break_before=1;
    rebuild_flat();rebuild_pages(NULL);CHECK(g_page_count==2);free_doc();
    /* A saved page restart inside one paragraph becomes two page fragments. */
    g_doc=hp_document_new();small_page(&g_doc->sections[0]);
    CHECK(hp_section_add_paragraph_text(&g_doc->sections[0],"ABCD")==HP_OK);
    {
        hp_paragraph *p=&g_doc->sections[0].blocks[0].paragraph;
        p->seg_count=4;p->segs=(hp_line_seg *)calloc(4,sizeof(hp_line_seg));
        for(i=0;i<4;i++) {
            p->segs[i].textpos=(uint32_t)i;
            p->segs[i].vertpos_hwp=(uint32_t)(i<2 ? 10000+i*1500 : (i-2)*1500);
            p->segs[i].vertsize_hwp=p->segs[i].textheight_hwp=1000;
            p->segs[i].spacing_hwp=500;
        }
    }
    rebuild_flat();rebuild_pages(NULL);CHECK(g_page_count==2 && g_piece_count==2);
    CHECK(g_pieces[0].offset==0 && g_pieces[0].height==40);
    CHECK(g_pieces[1].offset==40 && g_pieces[1].height==40);free_doc();
    table_document(3,100);
    g_doc->sections[0].blocks[0].table->cells[0].pad_left_hwp=UINT32_MAX;
    g_doc->sections[0].blocks[0].table->cells[0].pad_top_hwp=UINT32_MAX;
    rebuild_flat();rebuild_pages(NULL);
    CHECK(g_page_count==3);
    for(i=0;i<g_piece_count;i++) {
        CHECK(g_pieces[i].offset==offset);
        offset+=g_pieces[i].height;total+=g_pieces[i].height;
    }
    CHECK(total==300);free_doc();
    table_document(1,450);rebuild_flat();rebuild_pages(NULL);
    CHECK(g_page_count>=3);offset=0;
    for(i=0;i<g_piece_count;i++) { CHECK(g_pieces[i].offset==offset);offset+=g_pieces[i].height; }
    CHECK(offset==450);free_doc();
    /* Columns fill before advancing to another page. */
    table_document(3,100);g_doc->sections[0].column_count=2;
    rebuild_flat();rebuild_pages(NULL);CHECK(g_page_count==2);
    CHECK(g_pieces[0].column==0 && g_pieces[1].column==1);free_doc();
    /* Repeated titles reserve body space and reproduce the header pixels. */
    table_document(5,80);
    {
        hp_table *t=g_doc->sections[0].blocks[0].table;
        uint32_t *first,*again;
        t->repeat_header=1;t->cells[0].is_header=1;t->cells[0].height_hwp=40*75;
        t->cells[0].has_fill=1;t->cells[0].fill_rgb=0x112233;cell_text(&t->cells[0],"HEADER");
        rebuild_flat();rebuild_pages(NULL);CHECK(g_page_count==4 && g_piece_count==4);
        CHECK(g_pieces[0].header_height==0 && g_pieces[1].header_height==40);
        CHECK(g_pieces[1].y==40 && g_pieces[1].height<=140);
        first=header_pixels(&g_pieces[0],40);again=header_pixels(&g_pieces[1],40);
        CHECK(first && again);CHECK((first[35*200+20]&0xffffff)==0x112233);
        CHECK(memcmp(first,again,40*200*sizeof(uint32_t))==0);free(first);free(again);
        offset=0;for(i=0;i<g_piece_count;i++) {CHECK(g_pieces[i].offset==offset);offset+=g_pieces[i].height;}
        CHECK(offset==360);
        t->repeat_header=0;rebuild_pages(NULL);
        for(i=0;i<g_piece_count;i++)CHECK(g_pieces[i].header_height==0);
        t->repeat_header=1;t->cells[0].is_header=0;rebuild_pages(NULL);
        for(i=0;i<g_piece_count;i++)CHECK(g_pieces[i].header_height==0);
        t->cells[0].is_header=t->cells[1].is_header=1;t->cells[1].height_hwp=40*75;
        rebuild_pages(NULL);CHECK(g_pieces[1].header_height==80);
        t->cells[0].rowspan=2;t->cells[0].height_hwp=80*75;
        memmove(&t->cells[1],&t->cells[2],3*sizeof(hp_table_cell));t->cell_count=4;
        rebuild_pages(NULL);CHECK(g_pieces[1].header_height==80);
        t->cells[0].height_hwp=200*75;rebuild_pages(NULL);CHECK(g_page_count<10);
        for(i=0;i<g_piece_count;i++)CHECK(g_pieces[i].header_height==0);
    }
    free_doc();
    /* An image taller than a page has no line-safe cut: bounded continuation,
     * no title-only page, and no infinite retry trying to reserve a header. */
    table_document(2,100);
    {
        hp_table *t=g_doc->sections[0].blocks[0].table;
        t->repeat_header=1;t->cells[0].is_header=1;t->cells[0].height_hwp=40*75;
        t->cells[1].image=(hp_image *)calloc(1,sizeof(hp_image));
        t->cells[1].image->width_hwp=12000;t->cells[1].image->height_hwp=450*75;
        rebuild_flat();rebuild_pages(NULL);
        CHECK(g_page_count<10 && g_unsafe_table_cuts>0 && g_suppressed_headers>0);
        CHECK(g_pieces[0].height>40);
        offset=0;for(i=0;i<g_piece_count;i++) {CHECK(g_pieces[i].offset==offset);offset+=g_pieces[i].height;}
        CHECK(offset==490); /* 40px header + the declared 450px image, no invented gap */
    }
    free_doc();
    /* Vertical and horizontal merges, staggered text and nested table content. */
    table_document(4,100);
    {
        hp_table *t=g_doc->sections[0].blocks[0].table;
        hp_cut_map ink={0};HDC dc=GetDC(NULL);int source_height;
        free(t->cells);t->cell_count=5;t->col_count=2;t->cells=(hp_table_cell *)calloc(5,sizeof(hp_table_cell));
        t->repeat_header=1;
        t->cells[0]=(hp_table_cell){.row=0,.col=0,.rowspan=1,.colspan=2,.is_header=1,.height_hwp=3000};
        t->cells[1]=(hp_table_cell){.row=1,.col=0,.rowspan=3,.colspan=1,.height_hwp=22500,.width_hwp=6000};
        cell_text(&t->cells[0],"MERGED HEADER");
        cell_text(&t->cells[1],"LEFT01\nLEFT02\nLEFT03\nLEFT04\nLEFT05\nLEFT06\nLEFT07\nLEFT08\nLEFT09\nLEFT10\nLEFT11\nLEFT12");
        for(i=2;i<5;i++) {
            t->cells[i]=(hp_table_cell){.row=i-1,.col=1,.rowspan=1,.colspan=1,.height_hwp=7500,.width_hwp=6000};
            cell_text(&t->cells[i],"RIGHT1\nRIGHT2\nRIGHT3");
        }
        {
            hp_table *nested=(hp_table *)calloc(1,sizeof(*nested));
            nested->row_count=2;nested->col_count=1;nested->cell_count=2;
            nested->cells=(hp_table_cell *)calloc(2,sizeof(hp_table_cell));
            for(i=0;i<2;i++) {
                nested->cells[i]=(hp_table_cell){.row=i,.rowspan=1,.colspan=1,.height_hwp=1875};
                cell_text(&nested->cells[i],i ? "NESTED2" : "NESTED1");
            }
            t->cells[4].nested_tables=(hp_table **)calloc(1,sizeof(hp_table *));
            t->cells[4].nested_table_count=1;t->cells[4].nested_tables[0]=nested;
        }
        rebuild_flat();rebuild_pages(NULL);CHECK(g_page_count>=3 && g_unsafe_table_cuts==0);
        source_height=collect_table_ink(dc,t,180,0,&ink,0);hp_cut_normalize(&ink);
        offset=0;
        for(i=0;i<g_piece_count;i++) {
            CHECK(g_pieces[i].offset==offset);
            offset+=g_pieces[i].height;
            CHECK(hp_cut_before(&ink,0,offset)==offset);
            if(i>0) {
                int page,starts_page=0;
                for(page=0;page<g_page_count;page++)if(g_pages[page].first==i)starts_page=1;
                CHECK(g_pieces[i].header_height==(starts_page ? 40 : 0));
            }
        }
        CHECK(offset==source_height);free(ink.bands);ReleaseDC(NULL,dc);
    }
    free_doc();
    /* Saved restart after an already continued table must not add another page. */
    table_document(1,250);
    CHECK(hp_section_add_paragraph_text(&g_doc->sections[0],"after continuation")==HP_OK);
    g_doc->sections[0].blocks[1].saved_break_before=1;
    rebuild_flat();rebuild_pages(NULL);CHECK(g_page_count==2);
    CHECK(g_pages[1].count==2);free_doc();
    /* An object anchor is retained in IR, but cannot consume its object's height twice. */
    g_doc=hp_document_new();small_page(&g_doc->sections[0]);
    CHECK(hp_section_add_paragraph_text(&g_doc->sections[0]," ")==HP_OK);
    g_doc->sections[0].blocks[0].paragraph.object_anchor_only=1;
    CHECK(hp_section_add_paragraph_text(&g_doc->sections[0],"body")==HP_OK);
    rebuild_flat();rebuild_pages(NULL);CHECK(g_piece_count==1 && g_pieces[0].y==0);free_doc();
    /* Per-section geometry, even with a tiny preceding document. */
    g_doc=hp_document_new();
    CHECK(hp_section_add_paragraph_text(&g_doc->sections[0],"section one")==HP_OK);
    g_doc->sections=(hp_section *)realloc(g_doc->sections,2*sizeof(hp_section));
    memset(&g_doc->sections[1],0,sizeof(hp_section));g_doc->section_count=2;
    small_page(&g_doc->sections[1]);
    CHECK(hp_section_add_paragraph_text(&g_doc->sections[1],"section two")==HP_OK);
    rebuild_flat();rebuild_pages(NULL);CHECK(g_page_count==2);
    CHECK(g_pages[1].section==1);use_section(1);CHECK(PAGE_W_PX==200 && PAGE_H_PX==200);free_doc();
    {
        const char *samples[]={"sample/corpus/ethics.hwpx","sample/corpus/gyuje-규제합리화.hwpx","sample/corpus/mfds-프로프라놀롤.hwpx","tests/fixtures/table_pagination.hwpx"};
        int sample;
        for(sample=0;sample<4;sample++) {
            int block,repeats=0;
            CHECK(hp_hwpx_read(samples[sample],&g_doc)==HP_OK);
            rebuild_flat();rebuild_pages(NULL);CHECK(g_page_count>0 && g_page_count<100);
            for(i=0;i<g_piece_count;i++)if(g_pieces[i].header_height>0)repeats++;
            printf("sample %d: pages=%d fragments=%d blocks=%d repeated_headers=%d fallback_cuts=%d suppressed_headers=%d\n",sample,g_page_count,g_piece_count,g_flat_count,repeats,g_unsafe_table_cuts,g_suppressed_headers);
            {const int expected[]={17,20,4,2};CHECK(g_page_count==expected[sample] && g_unsafe_table_cuts==0);}
            if(sample==3)CHECK(repeats==1);
            for(block=0;block<g_flat_count;block++) {
                int end=0,found=0;
                for(i=0;i<g_piece_count;i++)if(g_pieces[i].block==block) {
                    CHECK(g_pieces[i].offset==end && g_pieces[i].height>0);
                    end+=g_pieces[i].height;found=1;
                }
                CHECK(found || (flat_block(block)->kind==HP_BLOCK_PARAGRAPH && flat_block(block)->paragraph.object_anchor_only));
            }
            /* A saved/explicit boundary cannot share the preceding page. */
            for(i=1;i<g_page_count;i++)CHECK(g_pages[i].first>=g_pages[i-1].first+g_pages[i-1].count);
            free_doc();
        }
    }
    puts("GDI text, section, column and table-fragment tests passed");return 0;
}
