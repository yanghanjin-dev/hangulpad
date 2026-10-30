#include "hp/edit.h"
#include "../src/win_utf8.h"
#include "../src/hwpx_package.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"edit failure line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static int same_parts(const char *a,const char *b,int section_equal){
    hp_document *x=NULL,*y=NULL;size_t i;int ok=1;
    if(hp_hwpx_read(a,&x)!=HP_OK||hp_hwpx_read(b,&y)!=HP_OK)return 0;
    for(i=0;i<x->package->entry_count;i++){
        const hp_package_entry *p=&x->package->entries[i],*q=hp_package_find(y->package,p->name);
        if(!section_equal&&strstr(p->name,"Contents/section"))continue;
        if(!q||p->len!=q->len||memcmp(p->data,q->data,p->len))ok=0;
    }
    hp_document_free(x);hp_document_free(y);return ok;
}
static int styled(void){
    const char *xml="<hs:sec xmlns:hs=\"http://www.hancom.co.kr/hwpml/2011/section\" xmlns:hp=\"http://www.hancom.co.kr/hwpml/2011/paragraph\"><hp:p id=\"0\" paraPrIDRef=\"0\" styleIDRef=\"0\"><hp:run charPrIDRef=\"0\"><hp:t keep=\"yes\">가나다</hp:t></hp:run><hp:run charPrIDRef=\"0\"><hp:t>라마바사</hp:t></hp:run></hp:p><hp:p id=\"1\" paraPrIDRef=\"0\" styleIDRef=\"0\"/></hs:sec>";
    hp_document *d=NULL;hp_editor *e=NULL;hp_package_entry *p=NULL;size_t i;
    CHECK(hp_hwpx_read("tmp/edit-source.hwpx",&d)==HP_OK);
    for(i=0;i<d->package->entry_count;i++)if(!strcmp(d->package->entries[i].name,d->package->section_parts[0]))p=&d->package->entries[i];CHECK(p);
    free(p->data);p->len=strlen(xml);p->data=malloc(p->len+1);CHECK(p->data);memcpy(p->data,xml,p->len+1);
    CHECK(hp_hwpx_write(d,"tmp/edit-style.hwpx")==HP_OK);hp_document_free(d);d=NULL;
    CHECK(hp_editor_open("tmp/edit-style.hwpx",&e)==HP_OK);CHECK(hp_editor_count(e)==2);
    CHECK(hp_editor_set(e,0,"가나X라마바사")==HP_OK);CHECK(hp_editor_set(e,1,"빈 문단에 입력")==HP_OK);
    CHECK(hp_editor_save(e,"tmp/edit-style-out.hwpx")==HP_OK);
    CHECK(hp_hwpx_read("tmp/edit-style-out.hwpx",&d)==HP_OK);
    p=NULL;for(i=0;i<d->package->entry_count;i++)if(!strcmp(d->package->entries[i].name,d->package->section_parts[0]))p=&d->package->entries[i];CHECK(p);
    CHECK(strstr((const char *)p->data,"<hp:t keep=\"yes\">가나X</hp:t>"));
    CHECK(strstr((const char *)p->data,"<hp:t>라마바사</hp:t>"));
    hp_document_free(d);hp_editor_free(e);return 0;
}

/* Formatting: apply to every k-th paragraph, save, reopen, and read the formats back. */
static int formatting(const char *path,const char *out,size_t step){
    hp_editor *e=NULL,*again=NULL;size_t i,n,done=0;hp_char_format cf,got;hp_para_format pf,gp;
    CHECK(hp_editor_open(path,&e)==HP_OK);n=hp_editor_count(e);
    memset(&cf,0,sizeof(cf));memset(&pf,0,sizeof(pf));
    CHECK(hp_editor_get_format(e,0,&got,&gp)==HP_OK);CHECK(got.size_hwp>=100);
    cf.fields=HP_CF_BOLD|HP_CF_ITALIC|HP_CF_SIZE|HP_CF_FACE|HP_CF_COLOR|HP_CF_UNDERLINE|HP_CF_STRIKEOUT;
    cf.bold=1;cf.italic=1;cf.size_hwp=1500;strcpy(cf.face,"새 글꼴 시험");cf.color_rgb=0xC00000;cf.underline=1;cf.strikeout=1;
    pf.fields=HP_PF_ALIGN|HP_PF_LINE_SPACING|HP_PF_LEFT|HP_PF_RIGHT|HP_PF_INDENT|HP_PF_BEFORE|HP_PF_AFTER;
    pf.align=HP_ALIGN_CENTER;pf.line_spacing_percent=200;pf.left_hwp=1000;pf.right_hwp=600;pf.indent_hwp=-400;pf.before_hwp=500;pf.after_hwp=300;
    { hp_char_format bad=cf;bad.size_hwp=5;CHECK(hp_editor_apply_char_format(e,0,&bad)==HP_ERR_INVALID);}
    { hp_para_format bad=pf;bad.align=9;CHECK(hp_editor_apply_para_format(e,0,&bad)==HP_ERR_INVALID);}
    for(i=0;i<n;i+=step){
        hp_char_format c=cf;hp_para_format f=pf;
        if((i/step)%2){c.fields=HP_CF_BOLD|HP_CF_SIZE;c.bold=0;c.size_hwp=2000;f.fields=HP_PF_ALIGN;f.align=HP_ALIGN_RIGHT;}
        CHECK(hp_editor_apply_char_format(e,i,&c)==HP_OK);CHECK(hp_editor_apply_para_format(e,i,&f)==HP_OK);done++;
    }
    CHECK(hp_editor_save(e,out)==HP_OK);
    CHECK(hp_editor_open(out,&again)==HP_OK);CHECK(hp_editor_count(again)==n);
    for(i=0;i<n;i++){
        hp_char_format a,b;hp_para_format x,y;
        CHECK(hp_editor_get_format(e,i,&a,&x)==HP_OK);CHECK(hp_editor_get_format(again,i,&b,&y)==HP_OK);
        CHECK(!strcmp(hp_editor_text(e,i),hp_editor_text(again,i)));
        if(a.size_hwp!=b.size_hwp||a.bold!=b.bold||a.italic!=b.italic||a.underline!=b.underline||a.strikeout!=b.strikeout||a.color_rgb!=b.color_rgb||strcmp(a.face,b.face)||
           x.align!=y.align||x.line_spacing_percent!=y.line_spacing_percent||x.left_hwp!=y.left_hwp||x.right_hwp!=y.right_hwp||x.indent_hwp!=y.indent_hwp||x.before_hwp!=y.before_hwp||x.after_hwp!=y.after_hwp){
            fprintf(stderr,"format mismatch at %zu: size %u/%u bold %d/%d face %s/%s align %d/%d ls %d/%d left %d/%d indent %d/%d\n",i,a.size_hwp,b.size_hwp,a.bold,b.bold,a.face,b.face,x.align,y.align,x.line_spacing_percent,y.line_spacing_percent,(int)x.left_hwp,(int)y.left_hwp,(int)x.indent_hwp,(int)y.indent_hwp);
            return 1;
        }
    }
    CHECK(hp_editor_get_format(again,0,&got,&gp)==HP_OK);
    CHECK(got.bold&&got.italic&&got.underline&&got.strikeout&&got.size_hwp==1500&&got.color_rgb==0xC00000&&!strcmp(got.face,"새 글꼴 시험"));
    CHECK(gp.align==HP_ALIGN_CENTER&&gp.line_spacing_percent==200&&gp.left_hwp==1000&&gp.indent_hwp==-400&&gp.after_hwp==300);
    {   hp_document *d=NULL;size_t k;int seen=0;
        CHECK(hp_hwpx_read(out,&d)==HP_OK);
        for(k=0;k<d->sections[0].block_count&&!seen;k++){const hp_block *bl=&d->sections[0].blocks[k];
            if(bl->kind==HP_BLOCK_PARAGRAPH&&bl->paragraph.run_count){const hp_paragraph *pp=&bl->paragraph;
                CHECK(pp->align==HP_ALIGN_CENTER);CHECK(pp->line_spacing_percent==200);CHECK(pp->runs[0].bold&&pp->runs[0].italic);
                CHECK(pp->runs[0].font_size_hwp==1500);CHECK(!strcmp(pp->runs[0].font_face,"새 글꼴 시험"));seen=1;}}
        CHECK(seen||d->sections[0].block_count==0||d->sections[0].blocks[0].kind!=HP_BLOCK_PARAGRAPH);hp_document_free(d);
    }
    printf("Formatted paragraphs saved and reread: %zu of %zu\n",done,n);
    hp_editor_free(e);hp_editor_free(again);return 0;
}

/* 0.11.0: character formats on part of a paragraph */
static int ranges(void){
    hp_document *g=hp_document_from_text("제목 (구역 설정이 있는 문단)");hp_editor *e=NULL,*again=NULL;hp_char_format cf,at;size_t i;
    CHECK(g);CHECK(hp_section_add_paragraph_text(&g->sections[0],"가나다라마바사")==HP_OK);CHECK(hp_section_add_paragraph_text(&g->sections[0],"Hello world & <ok>")==HP_OK);
    CHECK(hp_hwpx_write(g,"tmp/range-src.hwpx")==HP_OK);hp_document_free(g);g=NULL;
    CHECK(hp_editor_open("tmp/range-src.hwpx",&e)==HP_OK);
    memset(&cf,0,sizeof cf);cf.fields=HP_CF_BOLD|HP_CF_COLOR;cf.bold=1;cf.color_rgb=0x0000FF;
    CHECK(hp_editor_apply_char_format_range(e,1,6,12,&cf)==HP_OK);              /* 다라 */
    CHECK(hp_editor_apply_char_format_range(e,1,7,7,&cf)==HP_ERR_INVALID);CHECK(hp_editor_apply_char_format_range(e,0,0,3,&cf)==HP_ERR_UNSUPPORTED);
    memset(&cf,0,sizeof cf);cf.fields=HP_CF_ITALIC;cf.italic=1;
    CHECK(hp_editor_apply_char_format_range(e,2,6,11,&cf)==HP_OK);              /* world */
    CHECK(hp_editor_set(e,1,"앞에 가나다라마바사")==HP_OK);                       /* range moves with the text */
    CHECK(hp_editor_get_format_at(e,1,14,&at)==HP_OK&&at.bold);                  /* 다 */
    CHECK(hp_editor_get_format_at(e,1,3,&at)==HP_OK&&!at.bold);
    CHECK(hp_editor_save(e,"tmp/range-out.hwpx")==HP_OK);
    CHECK(hp_editor_open("tmp/range-out.hwpx",&again)==HP_OK);
    CHECK(!strcmp(hp_editor_text(again,1),"앞에 가나다라마바사"));CHECK(!strcmp(hp_editor_text(again,2),"Hello world & <ok>"));
    CHECK(hp_editor_get_format_at(again,1,14,&at)==HP_OK&&at.bold&&at.color_rgb==0x0000FF);
    CHECK(hp_editor_get_format_at(again,1,3,&at)==HP_OK&&!at.bold);
    CHECK(hp_editor_get_format_at(again,1,22,&at)==HP_OK&&!at.bold);             /* 마 */
    CHECK(hp_editor_get_format_at(again,2,7,&at)==HP_OK&&at.italic);
    CHECK(hp_editor_get_format_at(again,2,2,&at)==HP_OK&&!at.italic);
    {   hp_document *d=NULL;const hp_paragraph *p0,*p1;
        CHECK(hp_hwpx_read("tmp/range-out.hwpx",&d)==HP_OK);
        p0=&d->sections[0].blocks[1].paragraph;p1=&d->sections[0].blocks[2].paragraph;
        CHECK(p0->run_count==3&&!p0->runs[0].bold&&p0->runs[1].bold&&!strcmp(p0->runs[1].text,"다라")&&!p0->runs[2].bold);
        CHECK(p1->run_count==3&&p1->runs[1].italic&&!strcmp(p1->runs[1].text,"world")&&!strcmp(p1->runs[2].text," & <ok>"));
        hp_document_free(d);
    }
    /* a later whole-paragraph format and more ranges combine */
    memset(&cf,0,sizeof cf);cf.fields=HP_CF_SIZE;cf.size_hwp=1600;
    CHECK(hp_editor_apply_char_format(again,1,&cf)==HP_OK);
    memset(&cf,0,sizeof cf);cf.fields=HP_CF_UNDERLINE;cf.underline=1;
    CHECK(hp_editor_apply_char_format_range(again,1,0,6,&cf)==HP_OK);
    CHECK(hp_editor_save(again,"tmp/range-out2.hwpx")==HP_OK);hp_editor_free(again);again=NULL;
    CHECK(hp_editor_open("tmp/range-out2.hwpx",&again)==HP_OK);
    CHECK(hp_editor_get_format_at(again,1,1,&at)==HP_OK&&at.underline&&at.size_hwp==1600&&!at.bold);
    CHECK(hp_editor_get_format_at(again,1,14,&at)==HP_OK&&!at.underline&&at.size_hwp==1600&&at.bold);
    for(i=0;i<hp_editor_count(again);i++)CHECK(hp_editor_text(again,i)!=NULL);
    hp_editor_free(e);hp_editor_free(again);puts("Range character formats passed");return 0;
}
/* inline elements in the section parts: attributed tabs, full-width / no-break spaces, line breaks */
static long inline_count(const char *path){
    static const char *const tags[]={"<hp:tab ","<hp:fwSpace","<hp:nbSpace","<hp:lineBreak"};
    hp_document *d=NULL;size_t i,k;long n=0;
    if(hp_hwpx_read(path,&d)!=HP_OK)return -1;
    for(i=0;i<d->package->entry_count;i++){
        const hp_package_entry *p=&d->package->entries[i];char *s;const char *q;
        if(!strstr(p->name,"Contents/section"))continue;
        s=malloc(p->len+1);if(!s){n=-1;break;}memcpy(s,p->data,p->len);s[p->len]=0;
        for(k=0;k<4;k++)for(q=strstr(s,tags[k]);q;q=strstr(q+1,tags[k]))n++;
        free(s);
    }
    hp_document_free(d);return n;
}
static int corpus(const char *path){
    hp_editor *e=NULL,*again=NULL;size_t i,n,changed=0;char out[100],raw[100]; unsigned long key=0;const unsigned char *q=(const unsigned char *)path;for(;*q;q++)key=key*33+*q;snprintf(out,sizeof(out),"tmp/edit-corpus-%lu.hwpx",key);snprintf(raw,sizeof(raw),"tmp/edit-corpus-%lu-raw.hwpx",key);
    CHECK(hp_editor_open(path,&e)==HP_OK);n=hp_editor_count(e);CHECK(n>10);
    CHECK(hp_editor_save(e,raw)==HP_OK);CHECK(same_parts(path,raw,1));
    for(i=0;i<n;i++)if(*hp_editor_text(e,i)){
        const char *old=hp_editor_text(e,i);size_t len=strlen(old);char *s=malloc(len+32);CHECK(s);
        memcpy(s,old,len);strcpy(s+len," 수정 & 😀");CHECK(hp_editor_set(e,i,s)==HP_OK);free(s);changed++;
    }
    CHECK(changed>10);CHECK(hp_editor_save(e,out)==HP_OK);CHECK(same_parts(path,out,0));
    CHECK(hp_editor_open(out,&again)==HP_OK);CHECK(hp_editor_count(again)==n);
    for(i=0;i<n;i++)CHECK(!strcmp(hp_editor_text(e,i),hp_editor_text(again,i)));
    printf("Corpus paragraphs edited and reread: %zu; unchanged package parts preserved\n",changed);
    {char fmt[100];snprintf(fmt,sizeof(fmt),"tmp/edit-corpus-%lu-fmt.hwpx",key);CHECK(formatting(out,fmt,3)==0);}
    {   /* 0.11.2: edited and range-formatted text keeps tabs (width/leader) and full-width spaces */
        long before=inline_count(path);char rng[100];hp_char_format cf;size_t ranged=0;
        CHECK(before>=0);CHECK(inline_count(out)==before);
        hp_editor_free(again);again=NULL;CHECK(hp_editor_open(path,&again)==HP_OK);
        memset(&cf,0,sizeof cf);cf.fields=HP_CF_BOLD;cf.bold=1;
        for(i=0;i<n;i++){size_t len=strlen(hp_editor_text(again,i));
            if(len>2&&hp_editor_can_delete(again,i)&&hp_editor_apply_char_format_range(again,i,1,len,&cf)==HP_OK)ranged++;}
        snprintf(rng,sizeof(rng),"tmp/edit-corpus-%lu-range.hwpx",key);
        CHECK(ranged>10);CHECK(hp_editor_save(again,rng)==HP_OK);CHECK(inline_count(rng)==before);
        printf("Inline tab/space elements kept: %ld (range-formatted paragraphs: %zu)\n",before,ranged);
    }
    hp_editor_free(e);hp_editor_free(again);return 0;
}
int main(int argc,char **argv){
    hp_utf8_args(&argc,&argv);
    if(argc>1)return corpus(argv[1]);
    hp_editor *e=NULL,*again=NULL;hp_document *doc=hp_document_from_text("첫 문단");size_t i,n;const char *src="tmp/edit-source.hwpx",*out="tmp/edit-output.hwpx";
    CHECK(doc);CHECK(hp_section_add_paragraph_text(&doc->sections[0],"둘째 문단")==HP_OK);CHECK(hp_hwpx_write(doc,src)==HP_OK);hp_document_free(doc);
    CHECK(hp_editor_open(src,&e)==HP_OK);CHECK(hp_editor_count(e)==2);
    CHECK(hp_editor_save(e,out)==HP_OK);CHECK(same_parts(src,out,1));
    CHECK(hp_editor_set(e,0,"한글 & <수정> 😀\n다음 줄\t탭")==HP_OK);
    CHECK(hp_editor_insert(e,0,NULL)==HP_OK);CHECK(hp_editor_set(e,1,"추가 문단")==HP_OK);
    CHECK(hp_editor_insert(e,1,NULL)==HP_OK);CHECK(hp_editor_set(e,2,"두 번째 추가")==HP_OK);
    CHECK(hp_editor_delete(e,3)==HP_OK);CHECK(hp_editor_save(e,out)==HP_OK);
    CHECK(same_parts(src,out,0));CHECK(hp_editor_open(out,&again)==HP_OK);CHECK(hp_editor_count(again)==3);
    CHECK(!strcmp(hp_editor_text(again,0),"한글 & <수정> 😀\n다음 줄\t탭"));
    CHECK(!strcmp(hp_editor_text(again,1),"추가 문단"));CHECK(!strcmp(hp_editor_text(again,2),"두 번째 추가"));
    hp_editor_free(again);again=NULL;
    CHECK(hp_editor_save(e,out)==HP_OK);CHECK(hp_editor_set(e,0,"")==HP_OK);CHECK(hp_editor_save(e,src)==HP_OK);
    CHECK(hp_editor_open(src,&again)==HP_OK);CHECK(!strcmp(hp_editor_text(again,0),""));hp_editor_free(again);hp_editor_free(e);
    CHECK(hp_editor_open("tests/fixtures/table_pagination.hwpx",&e)==HP_OK);
    n=hp_editor_count(e);CHECK(n>2);
    for(i=0;i<n;i++){const char *s=hp_editor_text(e,i);if(*s){CHECK(hp_editor_set(e,i,"표 안과 본문 수정 & 보존")==HP_OK);}}
    CHECK(hp_editor_save(e,out)==HP_OK);CHECK(same_parts("tests/fixtures/table_pagination.hwpx",out,0));
    CHECK(hp_editor_open(out,&again)==HP_OK);CHECK(hp_editor_count(again)==n);
    for(i=0;i<n;i++)if(*hp_editor_text(again,i))CHECK(!strcmp(hp_editor_text(again,i),"표 안과 본문 수정 & 보존"));
    CHECK(hp_editor_save(e,"no-such-directory/out.hwpx")!=HP_OK);
    hp_editor_free(e);hp_editor_free(again);CHECK(styled()==0);CHECK(ranges()==0);{hp_document *g=hp_document_from_text("글자 모양 시험");CHECK(g);CHECK(hp_section_add_paragraph_text(&g->sections[0],"둘째")==HP_OK);CHECK(hp_hwpx_write(g,"tmp/edit-format-src.hwpx")==HP_OK);hp_document_free(g);}
    CHECK(formatting("tmp/edit-format-src.hwpx","tmp/edit-format.hwpx",1)==0);CHECK(formatting("tmp/edit-output.hwpx","tmp/edit-format-t2.hwpx",1)==0);CHECK(formatting("tests/fixtures/table_pagination.hwpx","tmp/edit-format-table.hwpx",2)==0);puts("Text replacement, insert/delete, Unicode, table preservation, repeat save passed");return 0;
}







