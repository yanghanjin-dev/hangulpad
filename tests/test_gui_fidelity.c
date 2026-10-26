/* PDF fidelity regressions: alpha, saved positions, tab leaders and symbols. */
#define wWinMain hp_unused_gui_entry
#include "../src/gui_win.c"
#undef wWinMain
#define CHECK(x) do{if(!(x)){fprintf(stderr,"Fidelity failure %d: %s\n",__LINE__,#x);return 1;}}while(0)
static const unsigned char alpha_png[]={137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,3,0,0,0,1,8,6,0,0,0,27,224,20,180,0,0,0,18,73,68,65,84,120,156,99,96,0,131,255,13,12,255,25,254,3,0,12,131,3,126,46,168,49,230,0,0,0,0,73,69,78,68,174,66,96,130};

int main(void){
 HDC dc=CreateCompatibleDC(NULL);BITMAPINFO info={0};uint32_t *bits;HBITMAP bitmap,old;RECT area={0,0,300,160};int i,y=0;
 info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=300;info.bmiHeader.biHeight=-160;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
 bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,(void **)&bits,NULL,0);CHECK(bitmap);old=SelectObject(dc,bitmap);
 CoInitialize(NULL);CHECK(SUCCEEDED(CoCreateInstance(&CLSID_WICImagingFactory,NULL,CLSCTX_INPROC_SERVER,&IID_IWICImagingFactory,(void **)&g_wic)));
 {hp_image image={0};image.data=(unsigned char *)alpha_png;image.data_len=sizeof(alpha_png);image.width_hwp=225;image.height_hwp=75;
  SetDCBrushColor(dc,RGB(255,0,0));FillRect(dc,&area,(HBRUSH)GetStockObject(DC_BRUSH));draw_image(dc,&image,0,3,&y);GdiFlush();
  CHECK((bits[0]&0xffffff)==0xff0000);CHECK((bits[1]&0xffffff)==0x7f0080);CHECK((bits[2]&0xffffff)==0x00ff00);}
 {hp_paragraph p={0};hp_line_seg line={0};int origin;
  p.indent_left_hwp=300;line.horzpos_hwp=300;line.horzsize_hwp=3104;
  CHECK(saved_line_box(&p,&line,0,45,1,&origin)==41);CHECK(origin==4);}
 {hp_run run={0};double advance;int dots=0,inside=0;
  run.font_size_hwp=1500;run.tab_width_hwp=7500;run.tab_leader=3;
  FillRect(dc,&area,(HBRUSH)GetStockObject(WHITE_BRUSH));advance=glyph_step(dc,&run,L"\t",1,0,0,1);GdiFlush();CHECK(advance==100);
  for(i=0;i<100;i++)if((bits[16*300+i]&0xffffff)==0)dots++;CHECK(dots>20&&dots<30);
  FillRect(dc,&area,(HBRUSH)GetStockObject(WHITE_BRUSH));glyph_step(dc,&run,L"\U000f02b1",2,10,10,1);GdiFlush();
  for(y=15;y<27;y++)for(i=15;i<23;i++)if((bits[y*300+i]&0xffffff)!=0xffffff)inside++;CHECK(inside>5);}
 {hp_table t={0};hp_table_cell cells[2]={{0}};int *cw=NULL,*rh=NULL;
  t.col_count=2;t.row_count=1;t.width_hwp=7500;t.cells=cells;t.cell_count=2;
  cells[0].colspan=1;cells[0].rowspan=1;cells[0].width_hwp=1500;
  cells[1].colspan=2;cells[1].rowspan=1;cells[1].width_hwp=7500;
  table_geometry(dc,&t,100,&cw,&rh);CHECK(cw&&cw[0]==20&&cw[1]==80);free(cw);free(rh);}
 {hp_table t={0};hp_table_cell cells[2]={{0}};int dark=0;
  t.col_count=1;t.row_count=2;t.width_hwp=7500;t.cells=cells;t.cell_count=2;
  for(i=0;i<2;i++){cells[i].row=i;cells[i].colspan=cells[i].rowspan=1;cells[i].width_hwp=7500;cells[i].height_hwp=7500;cells[i].borders.bottom=1;cells[i].borders.width_px=1;}
  FillRect(dc,&area,(HBRUSH)GetStockObject(WHITE_BRUSH));table_continuation_edges(dc,&t,0,100,0,0,100);GdiFlush();
  for(i=0;i<100;i++)if((bits[99*300+i]&0xffffff)==0)dark++;CHECK(dark==100);
  cells[0].borders.bottom=0;FillRect(dc,&area,(HBRUSH)GetStockObject(WHITE_BRUSH));
  table_continuation_edges(dc,&t,0,100,0,0,100);GdiFlush();
  for(i=0;i<100;i++)CHECK((bits[99*300+i]&0xffffff)==0xffffff);}
 CHECK(hp_hwpx_read("sample/corpus/ethics.hwpx",&g_doc)==HP_OK);rebuild_flat();rebuild_pages(NULL);
 {int found=0;for(i=0;i<g_piece_count;i++){const hp_block *b=flat_block(g_pieces[i].block);
  if(b->kind==HP_BLOCK_PARAGRAPH){wchar_t *s=para_concat_wide(&b->paragraph);
   if(wcsstr(s,L"항목별 세부")){CHECK(i>=g_pages[12].first && i<g_pages[12].first+g_pages[12].count);found++;}free(s);}}
  CHECK(found==1);}
 free_doc();g_wic->lpVtbl->Release(g_wic);g_wic=NULL;CoUninitialize();SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
 puts("Alpha composition, tab leaders, boxed digits, saved margins, merged widths and anchor note passed");return 0;
}
