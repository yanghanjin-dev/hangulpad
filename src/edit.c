/* Source-span editing: retain package parts and all controls outside edited text. */
#include "hp/edit.h"
#include "hp/hwp.h"
#include "hwpx_package.h"
#include "xml.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef struct { char *s; size_t n,cap; int failed; } buffer;
static void putn(buffer *b,const char *s,size_t n) {
    char *p; size_t cap;
    if(b->failed)return;
    if(n>(size_t)HP_PACKAGE_MAX_ENTRY_BYTES||b->n>(size_t)HP_PACKAGE_MAX_ENTRY_BYTES-n){b->failed=1;return;}
    if(b->n+n+1>b->cap) {cap=(b->n+n+1)*2;p=realloc(b->s,cap);if(!p){b->failed=1;return;}b->s=p;b->cap=cap;}
    memcpy(b->s+b->n,s,n);b->n+=n;b->s[b->n]=0;
}
static void puts_b(buffer *b,const char *s){putn(b,s,strlen(s));}
static char *copy(const char *s){size_t n=strlen(s)+1;char *p=malloc(n);if(p)memcpy(p,s,n);return p;}
static char *finish(buffer *b){if(b->failed){free(b->s);return NULL;}return b->s?b->s:copy("");}
static void text_of(buffer *b,const hp_xml_node *n){
    const hp_xml_node *c;
    for(c=n->first_child;c;c=c->next) {
        if(c->type==HP_XML_TEXT)putn(b,c->text,c->text_len);
        else if(hp_xml_is(c,"lineBreak"))puts_b(b,"\n");
        else if(hp_xml_is(c,"tab"))puts_b(b,"\t");
        else if(hp_xml_is(c,"fwSpace")||hp_xml_is(c,"nbSpace"))puts_b(b," ");
    }
}
static void escape(buffer *b,const char *s,const char *qname){
    const unsigned char *p=(const unsigned char *)s;const char *colon=strchr(qname,':');
    for(;*p;p++)switch(*p){
    case '&':puts_b(b,"&amp;");break;case '<':puts_b(b,"&lt;");break;case '>':puts_b(b,"&gt;");break;
    case '\r':break;
    case '\n':case '\t':puts_b(b,"<");if(colon)putn(b,qname,(size_t)(colon-qname)+1);puts_b(b,*p=='\n'?"lineBreak/>":"tab/>");break;
    default:putn(b,(const char *)p,1);break;
    }
}
typedef struct {size_t section;hp_xml_node *p;const unsigned char *source;char *original,*text;int deleted,inserted,simple;} item;
struct hp_editor {hp_document *doc;hp_xml_doc **xml;item *items;size_t count;};
size_t hp_editor_count(const hp_editor *e){return e?e->count:0;}
const char *hp_editor_text(const hp_editor *e,size_t i){return e&&i<e->count?e->items[i].text:"";}
size_t hp_editor_section(const hp_editor *e,size_t i){return e&&i<e->count?e->items[i].section:0;}
int hp_editor_is_deleted(const hp_editor *e,size_t i){return e&&i<e->count?e->items[i].deleted:1;}
int hp_editor_can_delete(const hp_editor *e,size_t i){return e&&i<e->count&&e->items[i].simple;}
void hp_editor_free(hp_editor *e){size_t i;if(!e)return;for(i=0;i<e->count;i++){free(e->items[i].text);free(e->items[i].original);}for(i=0;e->xml&&i<e->doc->section_count;i++)hp_xml_free(e->xml[i]);free(e->xml);free(e->items);hp_document_free(e->doc);free(e);}
hp_status hp_editor_open(const char *path,hp_editor **out){
    hp_editor *e=calloc(1,sizeof(*e));size_t si;hp_status st;if(!out||!path){free(e);return HP_ERR_INVALID;}*out=NULL;if(!e)return HP_ERR_NOMEM;
    st=hp_document_read(path,&e->doc);if(st!=HP_OK){hp_editor_free(e);return st;}
    if(hp_document_is_read_only(e->doc)){hp_editor_free(e);return HP_ERR_UNSUPPORTED;}
    e->xml=calloc(e->doc->section_count,sizeof(*e->xml));if(!e->xml){hp_editor_free(e);return HP_ERR_NOMEM;}
    for(si=0;si<e->doc->section_count;si++){
        const hp_package_entry *part=hp_package_find(e->doc->package,e->doc->package->section_parts[si]);hp_xml_node *root,*p;
        st=hp_xml_parse((const char *)part->data,part->len,&e->xml[si],NULL,0);if(st!=HP_OK){hp_editor_free(e);return st;}
        root=hp_xml_root(e->xml[si]);
        for(p=hp_xml_find(root,"p");p;p=hp_xml_find_next(root,p,"p")){
            hp_xml_node *run,*t,*n;buffer b={0};item it={0};item *arr;
            it.section=si;it.p=p;it.source=part->data;it.simple=1;
            for(n=hp_xml_find(p,NULL);n;n=hp_xml_find_next(p,n,NULL)){
                if(n->type==HP_XML_ELEMENT&&!hp_xml_is(n,"run")&&!hp_xml_is(n,"t")&&!hp_xml_is(n,"linesegarray")&&!hp_xml_is(n,"lineseg")&&!hp_xml_is(n,"lineBreak")&&!hp_xml_is(n,"tab")&&!hp_xml_is(n,"fwSpace")&&!hp_xml_is(n,"nbSpace"))it.simple=0;
            }
            for(run=hp_xml_child(p,"run");run;run=hp_xml_next(run,"run"))for(t=hp_xml_child(run,"t");t;t=hp_xml_next(t,"t"))text_of(&b,t);
            it.original=finish(&b);it.text=it.original?copy(it.original):NULL;
            if(!it.text){free(it.original);hp_editor_free(e);return HP_ERR_NOMEM;}
            arr=realloc(e->items,(e->count+1)*sizeof(*arr));if(!arr){free(it.text);free(it.original);hp_editor_free(e);return HP_ERR_NOMEM;}
            e->items=arr;e->items[e->count++]=it;
        }
    }*out=e;return HP_OK;
}
hp_status hp_editor_set(hp_editor *e,size_t i,const char *text){char *s;const unsigned char *p;if(!e||i>=e->count||!text)return HP_ERR_INVALID;
    for(p=(const unsigned char *)text;*p;p++)if(*p<32&&*p!='\n'&&*p!='\t'&&*p!='\r')return HP_ERR_INVALID;
    {hp_xml_node *r,*t,*ch;
        for(r=hp_xml_child(e->items[i].p,"run");r;r=hp_xml_next(r,"run"))for(t=hp_xml_child(r,"t");t;t=hp_xml_next(t,"t"))for(ch=t->first_child;ch;ch=ch->next)
            if(ch->type==HP_XML_ELEMENT&&!hp_xml_is(ch,"lineBreak")&&!hp_xml_is(ch,"tab")&&!hp_xml_is(ch,"fwSpace")&&!hp_xml_is(ch,"nbSpace"))return HP_ERR_UNSUPPORTED;
    }
    s=copy(text);if(!s)return HP_ERR_NOMEM;free(e->items[i].text);e->items[i].text=s;return HP_OK;}
hp_status hp_editor_insert(hp_editor *e,size_t i,size_t *inserted_index){item it,*arr;size_t at;if(!e||i>=e->count)return HP_ERR_INVALID;
    /* Anchor new plain paragraphs after the source paragraph; controls stay in place. */
    at=i+1;
    if(!e->items[i].inserted)while(at<e->count&&e->items[at].section==e->items[i].section&&e->items[at].p->start>e->items[i].p->start&&e->items[at].p->start<e->items[i].p->end)at++;
    it=e->items[i];it.original=copy("");it.text=copy("");it.inserted=1;it.deleted=0;it.simple=1;
    if(!it.original||!it.text){free(it.original);free(it.text);return HP_ERR_NOMEM;}
    arr=realloc(e->items,(e->count+1)*sizeof(*arr));if(!arr){free(it.original);free(it.text);return HP_ERR_NOMEM;}
    e->items=arr;memmove(arr+at+1,arr+at,(e->count-at)*sizeof(*arr));arr[at]=it;e->count++;if(inserted_index)*inserted_index=at;return HP_OK;
}
hp_status hp_editor_delete(hp_editor *e,size_t i){size_t j,n=0;if(!e||i>=e->count)return HP_ERR_INVALID;
    if(!e->items[i].simple)return HP_ERR_UNSUPPORTED;
    for(j=0;j<e->count;j++)if(!e->items[j].deleted&&e->items[j].p->parent==e->items[i].p->parent)n++;
    if(n<=1)return HP_ERR_UNSUPPORTED;
    e->items[i].deleted=1;return HP_OK;}
typedef struct {size_t a,b,order;char *text;} patch;
static int add_patch(patch **ps,size_t *n,size_t a,size_t b,char *s){patch *p;if(!s)return 0;p=realloc(*ps,(*n+1)*sizeof(*p));if(!p){free(s);return 0;}*ps=p;p[*n]=(patch){a,b,*n,s};(*n)++;return 1;}
static int patch_cmp(const void *a,const void *b){const patch *x=a,*y=b;if(x->a!=y->a)return x->a<y->a?-1:1;if(x->b!=y->b)return x->b<y->b?-1:1;return x->order==y->order?0:(x->order<y->order?-1:1);}
static const char *style_ref(const char *s){return s&&*s&&strspn(s,"0123456789")==strlen(s)?s:"0";}
static void qualified(buffer *b,const char *sample,const char *local){
    const char *colon=strchr(sample,':');if(colon)putn(b,sample,(size_t)(colon-sample)+1);puts_b(b,local);
}
static char *new_para(item *it){
    buffer b={0};hp_xml_node *run=hp_xml_child(it->p,"run"),*t=run?hp_xml_child(run,"t"):NULL;
    buffer rb={0},tb={0};char *rq,*tq;
    if(run)puts_b(&rb,run->qname);else qualified(&rb,it->p->qname,"run");
    if(t)puts_b(&tb,t->qname);else qualified(&tb,it->p->qname,"t");
    rq=finish(&rb);tq=finish(&tb);if(!rq||!tq){free(rq);free(tq);return NULL;}
    puts_b(&b,"<");puts_b(&b,it->p->qname);puts_b(&b," id=\"0\" paraPrIDRef=\"");
    puts_b(&b,style_ref(hp_xml_attr(it->p,"paraPrIDRef")));puts_b(&b,"\" styleIDRef=\"");
    puts_b(&b,style_ref(hp_xml_attr(it->p,"styleIDRef")));puts_b(&b,"\" pageBreak=\"0\" columnBreak=\"0\" merged=\"0\"><");
    puts_b(&b,rq);puts_b(&b," charPrIDRef=\"");puts_b(&b,style_ref(run?hp_xml_attr(run,"charPrIDRef"):NULL));
    puts_b(&b,"\"><");puts_b(&b,tq);puts_b(&b,">");escape(&b,it->text,tq);
    puts_b(&b,"</");puts_b(&b,tq);puts_b(&b,"></");puts_b(&b,rq);puts_b(&b,"></");puts_b(&b,it->p->qname);puts_b(&b,">");
    free(rq);free(tq);return finish(&b);
}
static int edits(item *it,patch **ps,size_t *n){
    hp_xml_node *r,*t,*seg;size_t pre=0,suf=0,old=strlen(it->original),now=strlen(it->text),offset=0;int placed=0;
    if(it->deleted)return it->inserted?1:add_patch(ps,n,it->p->start,it->p->end,copy(""));
    if(it->inserted)return add_patch(ps,n,it->p->end,it->p->end,new_para(it));
    if(strcmp(it->original,it->text)==0)return 1;
    while(pre<old&&pre<now&&it->original[pre]==it->text[pre])pre++;
    while(pre&&((unsigned char)it->original[pre]&192)==128)pre--;
    while(suf<old-pre&&suf<now-pre&&it->original[old-1-suf]==it->text[now-1-suf])suf++;
    while(suf&&((unsigned char)it->original[old-suf]&192)==128)suf--;
    for(r=hp_xml_child(it->p,"run");r;r=hp_xml_next(r,"run"))for(t=hp_xml_child(r,"t");t;t=hp_xml_next(t,"t")){
        buffer orig={0},rep={0},xml={0};char *s,*result;size_t len,a,z;int insert;
        text_of(&orig,t);s=finish(&orig);if(!s)return 0;len=strlen(s);
        a=pre>offset?(pre-offset<len?pre-offset:len):0;
        z=old-suf>offset?(old-suf-offset<len?old-suf-offset:len):0;
        insert=!placed&&pre<=offset+len;
        if(insert||z>a){putn(&rep,s,a);if(insert){putn(&rep,it->text+pre,now-pre-suf);placed=1;}putn(&rep,s+z,len-z);result=finish(&rep);
            if(!result){free(s);return 0;}
            if(t->inner_start==t->end){putn(&xml,(const char *)it->source+t->start,t->end-t->start-2);puts_b(&xml,">");}
            else putn(&xml,(const char *)it->source+t->start,t->inner_start-t->start);
            escape(&xml,result,t->qname);
            if(t->inner_start==t->end){puts_b(&xml,"</");puts_b(&xml,t->qname);puts_b(&xml,">");}
            else putn(&xml,(const char *)it->source+t->inner_end,t->end-t->inner_end);
            free(result);
            if(!add_patch(ps,n,t->start,t->end,finish(&xml))){free(s);return 0;}
        }free(s);offset+=len;
    }
    if(!placed){/* Empty/object-only paragraphs acquire a run without removing controls. */
        buffer b={0};const char *colon;char prefix[128]="";size_t pn;
        r=hp_xml_child(it->p,"run");colon=strchr(it->p->qname,':');pn=colon?(size_t)(colon-it->p->qname)+1:0;
        if(pn>=sizeof(prefix))return 0;
        if(pn){memcpy(prefix,it->p->qname,pn);prefix[pn]=0;}
        if(!r){
            puts_b(&b,"<");puts_b(&b,prefix);puts_b(&b,"run charPrIDRef=\"0\">");
        }else if(r->inner_start==r->end){
            /* Expand a self-closing run; retain its attributes. */
            putn(&b,(const char *)it->source+r->start,r->end-r->start-2);puts_b(&b,">");
        }
        puts_b(&b,"<");puts_b(&b,prefix);puts_b(&b,"t>");escape(&b,it->text,it->p->qname);puts_b(&b,"</");puts_b(&b,prefix);puts_b(&b,"t>");
        if(!r||r->inner_start==r->end){puts_b(&b,"</");if(r)puts_b(&b,r->qname);else{puts_b(&b,prefix);puts_b(&b,"run");}puts_b(&b,">");}
        if(!r){
            if(it->p->inner_start==it->p->end){
                buffer expanded={0};char *content=finish(&b);if(!content)return 0;
                putn(&expanded,(const char *)it->source+it->p->start,it->p->end-it->p->start-2);puts_b(&expanded,">");puts_b(&expanded,content);free(content);puts_b(&expanded,"</");puts_b(&expanded,it->p->qname);puts_b(&expanded,">");
                return add_patch(ps,n,it->p->start,it->p->end,finish(&expanded));
            }
            if(!add_patch(ps,n,it->p->inner_end,it->p->inner_end,finish(&b)))return 0;
        }else if(!add_patch(ps,n,r->inner_start==r->end?r->start:r->inner_end,r->inner_start==r->end?r->end:r->inner_end,finish(&b)))return 0;
    }
    for(seg=hp_xml_child(it->p,"linesegarray");seg;seg=hp_xml_next(seg,"linesegarray"))if(!add_patch(ps,n,seg->start,seg->end,copy("")))return 0;
    return 1;
}
hp_status hp_editor_save(hp_editor *e,const char *path){
    hp_package pkg;hp_document doc;size_t si,i,j;char **owned;hp_status st=HP_OK;
    if(!e||!path)return HP_ERR_INVALID;
    pkg=*e->doc->package;doc=*e->doc;doc.package=&pkg;
    pkg.entries=malloc(pkg.entry_count*sizeof(*pkg.entries));owned=calloc(pkg.entry_count,sizeof(*owned));
    if(!pkg.entries||!owned){free(pkg.entries);free(owned);return HP_ERR_NOMEM;}memcpy(pkg.entries,e->doc->package->entries,pkg.entry_count*sizeof(*pkg.entries));
    for(si=0;si<pkg.section_part_count&&st==HP_OK;si++){
        patch *ps=NULL;size_t n=0,pos=0;buffer b={0};hp_package_entry *part=NULL;
        for(i=0;i<pkg.entry_count;i++)if(strcmp(pkg.entries[i].name,pkg.section_parts[si])==0){part=&pkg.entries[i];break;}
        if(!part){st=HP_ERR_INVALID;break;}
        for(j=0;j<e->count;j++)if(e->items[j].section==si&&!edits(&e->items[j],&ps,&n)){st=HP_ERR_NOMEM;break;}
        if(st==HP_OK&&n){qsort(ps,n,sizeof(*ps),patch_cmp);
            for(j=0;j<n;j++){if(ps[j].a<pos){st=HP_ERR_UNSUPPORTED;break;}putn(&b,(const char *)part->data+pos,ps[j].a-pos);puts_b(&b,ps[j].text);pos=ps[j].b;}
            if(st==HP_OK){putn(&b,(const char *)part->data+pos,part->len-pos);owned[i]=finish(&b);if(!owned[i])st=HP_ERR_NOMEM;else{part->data=(unsigned char *)owned[i];part->len=strlen(owned[i]);}}
            else free(b.s);
        }
        for(j=0;j<n;j++){free(ps[j].text);}free(ps);
    }
    if(st==HP_OK)st=hp_hwpx_write(&doc,path);
    for(i=0;i<pkg.entry_count;i++){free(owned[i]);}free(owned);free(pkg.entries);return st;
}







