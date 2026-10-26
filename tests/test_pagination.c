#include "hp/hwpx.h"
#include "hp/ir.h"
#include <zip.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"pagination failure line %d: %s\n",__LINE__,#x); return 1; } } while(0)
static int part(zip_t *z, const char *name, const char *xml) {
    zip_source_t *s = zip_source_buffer(z, xml, strlen(xml), 0);
    if (!s) return 0;
    if (zip_file_add(z, name, s, 0) < 0) { zip_source_free(s); return 0; }
    return 1;
}
int main(void) {
    const char *path = "tmp/pagination.hwpx";
    hp_document *doc = NULL;
    zip_t *z;
    int error = 0;
    const char *xml =
        "<s:sec xmlns:s='s' xmlns:p='p'>"
        "<p:p><p:run charPrIDRef='0'><p:t>first</p:t></p:run>"
        "<p:linesegarray><p:lineseg vertpos='10000' textheight='1000'/></p:linesegarray></p:p>"
        "<p:p pageBreak='1'><p:run><p:tbl rowCnt='2' colCnt='1' repeatHeader='1'>"
        "<p:tr><p:tc header='1'><p:subList>"
        "<p:p><p:run><p:tbl rowCnt='1' colCnt='1'><p:sz width='750'/><p:pos treatAsChar='1'/></p:tbl>"
        "<p:t>ABCDEFGH</p:t></p:run><p:linesegarray><p:lineseg textpos='0'/><p:lineseg textpos='10' vertpos='1200'/></p:linesegarray></p:p>"
        "<p:p><p:run><p:tbl rowCnt='1' colCnt='1'/></p:run></p:p>"
        "<p:p><p:run><p:t>after nested</p:t></p:run></p:p>"
        "</p:subList><p:cellAddr colAddr='0' rowAddr='0'/>"
        "<p:cellSpan rowSpan='1' colSpan='1'/></p:tc></p:tr></p:tbl></p:run>"
        "<p:linesegarray><p:lineseg vertpos='20000'/></p:linesegarray></p:p>"
        "<p:p columnBreak='1'><p:linesegarray><p:lineseg vertpos='25000'/></p:linesegarray></p:p>"
        "<p:p paraPrIDRef='1'><p:run><p:t>reset</p:t></p:run>"
        "<p:linesegarray><p:lineseg vertpos='0'/></p:linesegarray></p:p>"
        "<p:p pageBreak='1'><p:run><p:t>before</p:t><p:tbl rowCnt='1' colCnt='1'/><p:t>after</p:t></p:run></p:p>"
        "<p:p><p:run><p:tbl rowCnt='1' colCnt='1'/><p:t>visible after object</p:t></p:run></p:p>"
        "</s:sec>";
    const char *header = "<head><fontface lang='HANGUL'><font id='0' face='Arial'/></fontface>"
        "<charPr id='0' height='2300'><fontRef hangul='0'/><ratio hangul='99' latin='95'/>"
        "<spacing hangul='-14' latin='-5'/></charPr>"
        "<paraPr id='1'><breakSetting pageBreakBefore='1'/>"
        "<switch><case required-namespace='http://www.hancom.co.kr/hwpml/2016/HwpUnitChar'>"
        "<margin><prev value='1500'/><intent value='-500'/></margin></case>"
        "<default><margin><prev value='3000'/><intent value='-1000'/></margin></default></switch></paraPr></head>";
    z = zip_open(path, ZIP_CREATE | ZIP_TRUNCATE, &error);
    CHECK(z != NULL);
    CHECK(part(z,"mimetype",HP_HWPX_MIMETYPE));
    CHECK(part(z,"Contents/header.xml",header));
    CHECK(part(z,"Contents/section0.xml",xml));
    CHECK(zip_close(z) == 0);
    CHECK(hp_hwpx_read(path,&doc) == HP_OK);
    CHECK(doc->sections[0].block_count == 9);
    CHECK(doc->sections[0].blocks[1].kind == HP_BLOCK_TABLE);
    CHECK(doc->sections[0].blocks[1].page_break_before == 1);
    CHECK(doc->sections[0].blocks[1].table->repeat_header == 1);
    CHECK(doc->sections[0].blocks[1].table->cells[0].is_header == 1);
    {
        hp_table_cell *c=&doc->sections[0].blocks[1].table->cells[0];
        CHECK(c->paragraph_count==3 && c->nested_table_count==2);
        CHECK(c->nested_before_paragraph[0]==0 && c->nested_before_paragraph[1]==1);
        CHECK(c->paragraphs[0].inline_prefix_width_hwp==750);
        CHECK(c->paragraphs[0].segs[1].textpos==2);
        CHECK(c->paragraphs[1].object_anchor_only==1);
        CHECK(c->paragraphs[2].object_anchor_only==0);
    }
    CHECK(doc->sections[0].blocks[5].table->repeat_header == 0);
    CHECK(doc->sections[0].blocks[2].column_break_before == 1);
    CHECK(doc->sections[0].blocks[3].saved_break_before == 1);
    CHECK(doc->sections[0].blocks[3].paragraph.space_before_hwp == 1500);
    CHECK(doc->sections[0].blocks[3].paragraph.indent_first_hwp == -500);
    CHECK(doc->sections[0].blocks[2].paragraph.seg_count == 1);
    CHECK(doc->sections[0].blocks[3].page_break_before == 1);
    CHECK(doc->sections[0].blocks[4].page_break_before == 1);
    CHECK(doc->sections[0].blocks[5].page_break_before == 0);
    CHECK(doc->sections[0].blocks[6].page_break_before == 0);
    CHECK(doc->sections[0].blocks[0].paragraph.runs[0].width_percent[0] == 99);
    CHECK(doc->sections[0].blocks[0].paragraph.runs[0].width_percent[1] == 95);
    CHECK(doc->sections[0].blocks[0].paragraph.runs[0].spacing_percent[0] == -14);
    CHECK(doc->sections[0].blocks[8].paragraph.object_anchor_only==0);
    CHECK(strcmp(doc->sections[0].blocks[8].paragraph.runs[0].text,"visible after object")==0);
    hp_document_free(doc);
    puts("pagination metadata tests passed");
    return 0;
}
