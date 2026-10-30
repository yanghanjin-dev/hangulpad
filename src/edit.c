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
typedef struct {size_t start,end;hp_char_format cf;} span; /* character format on bytes [start,end) of the item text */
typedef struct {size_t section;hp_xml_node *p;const unsigned char *source;char *original,*text;int deleted,inserted,simple;hp_char_format cf;hp_para_format pf;span *spans;size_t span_count;} item;
/* header: Contents/header.xml parsed once (character/paragraph shapes for formatting). */
struct hp_editor {hp_document *doc;hp_xml_doc **xml;item *items;size_t count;hp_xml_doc *header;const char *header_name;};
size_t hp_editor_count(const hp_editor *e){return e?e->count:0;}
const char *hp_editor_text(const hp_editor *e,size_t i){return e&&i<e->count?e->items[i].text:"";}
size_t hp_editor_section(const hp_editor *e,size_t i){return e&&i<e->count?e->items[i].section:0;}
int hp_editor_is_deleted(const hp_editor *e,size_t i){return e&&i<e->count?e->items[i].deleted:1;}
int hp_editor_can_delete(const hp_editor *e,size_t i){return e&&i<e->count&&e->items[i].simple;}
void hp_editor_free(hp_editor *e){size_t i;if(!e)return;for(i=0;i<e->count;i++){free(e->items[i].text);free(e->items[i].original);free(e->items[i].spans);}for(i=0;e->xml&&i<e->doc->section_count;i++)hp_xml_free(e->xml[i]);free(e->xml);hp_xml_free(e->header);free(e->items);hp_document_free(e->doc);free(e);}
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
    }
    {   /* character / paragraph shapes, for formatting (optional) */
        const hp_package_entry *h=hp_package_find(e->doc->package,"Contents/header.xml");
        if(h&&hp_xml_parse((const char *)h->data,h->len,&e->header,NULL,0)==HP_OK)e->header_name="Contents/header.xml";
    }
    *out=e;return HP_OK;
}
/* Keep character ranges on the same characters across a text change: the
 * common prefix and suffix stay, the changed middle maps to its replacement. */
static void move_spans(item *it,const char *old_text,const char *new_text){
    size_t pre=0,suf=0,old=strlen(old_text),now=strlen(new_text),k,w=0;
    if(!it->span_count)return;
    while(pre<old&&pre<now&&old_text[pre]==new_text[pre])pre++;
    while(pre&&((unsigned char)old_text[pre]&192)==128)pre--;
    while(suf<old-pre&&suf<now-pre&&old_text[old-1-suf]==new_text[now-1-suf])suf++;
    while(suf&&((unsigned char)old_text[old-suf]&192)==128)suf--;
    for(k=0;k<it->span_count;k++){
        span sp=it->spans[k];
        size_t *ends[2]={&sp.start,&sp.end};int j;
        for(j=0;j<2;j++){
            size_t x=*ends[j];
            if(x<=pre){}
            else if(x>=old-suf)x=x-(old-suf)+(now-suf);
            else x=j?now-suf:pre; /* inside the replaced text: cover all of the replacement */
            *ends[j]=x;
        }
        if(sp.end>sp.start)it->spans[w++]=sp;
    }
    it->span_count=w;
}
hp_status hp_editor_set(hp_editor *e,size_t i,const char *text){char *s;const unsigned char *p;if(!e||i>=e->count||!text)return HP_ERR_INVALID;
    for(p=(const unsigned char *)text;*p;p++)if(*p<32&&*p!='\n'&&*p!='\t'&&*p!='\r')return HP_ERR_INVALID;
    {hp_xml_node *r,*t,*ch;
        for(r=hp_xml_child(e->items[i].p,"run");r;r=hp_xml_next(r,"run"))for(t=hp_xml_child(r,"t");t;t=hp_xml_next(t,"t"))for(ch=t->first_child;ch;ch=ch->next)
            if(ch->type==HP_XML_ELEMENT&&!hp_xml_is(ch,"lineBreak")&&!hp_xml_is(ch,"tab")&&!hp_xml_is(ch,"fwSpace")&&!hp_xml_is(ch,"nbSpace"))return HP_ERR_UNSUPPORTED;
    }
    s=copy(text);if(!s)return HP_ERR_NOMEM;move_spans(&e->items[i],e->items[i].text,s);free(e->items[i].text);e->items[i].text=s;return HP_OK;}
hp_status hp_editor_insert(hp_editor *e,size_t i,size_t *inserted_index){item it,*arr;size_t at;if(!e||i>=e->count)return HP_ERR_INVALID;
    /* Anchor new plain paragraphs after the source paragraph; controls stay in place. */
    at=i+1;
    if(!e->items[i].inserted)while(at<e->count&&e->items[at].section==e->items[i].section&&e->items[at].p->start>e->items[i].p->start&&e->items[at].p->start<e->items[i].p->end)at++;
    it=e->items[i];it.original=copy("");it.text=copy("");it.inserted=1;it.deleted=0;it.simple=1;it.spans=NULL;it.span_count=0;
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

/* ===================== paragraph formatting (0.10.0) =====================
 * A formatted paragraph keeps its text edits (first pass, above). A second pass
 * re-parses the patched section and points the paragraph's paraPrIDRef and its
 * runs' charPrIDRef at new header entries: copies of each run's own charPr (or
 * the paragraph's paraPr) with the requested changes, appended to header.xml
 * with consecutive ids. Fonts that the header lacks are added to every
 * language's fontface. Saved line layout of a formatted paragraph is dropped. */

static hp_xml_node *by_id(const hp_xml_node *root, const char *name, const char *id) {
    hp_xml_node *n;
    if (!root || !id) {
        return NULL;
    }
    for (n = hp_xml_find(root, name); n; n = hp_xml_find_after(root, n, name)) {
        const char *v = hp_xml_attr(n, "id");
        if (v && strcmp(v, id) == 0) {
            return n;
        }
    }
    return NULL;
}

static const char *first_char_ref(const hp_xml_node *p) {
    const hp_xml_node *run = hp_xml_child(p, "run");
    const char *id = run ? hp_xml_attr(run, "charPrIDRef") : NULL;
    return id ? id : "0";
}

static uint32_t parse_rgb(const char *s) {
    unsigned r = 0, g = 0, b = 0;
    if (s && s[0] == '#' && strlen(s) >= 7 && sscanf(s + strlen(s) - 6, "%02x%02x%02x", &r, &g, &b) == 3) {
        return (r << 16) | (g << 8) | b;
    }
    return 0;
}

static const hp_xml_node *metric_scope(const hp_xml_node *pp) {
    hp_xml_node *c;
    for (c = hp_xml_find(pp, "case"); c; c = hp_xml_find_after(pp, c, "case")) {
        const char *r = hp_xml_attr(c, "required-namespace");
        if (r && strstr(r, "HwpUnitChar")) {
            return c;
        }
    }
    return pp;
}

static hp_xml_node *find_last_in(const hp_xml_node *scope, const char *name) {
    hp_xml_node *n, *last = NULL;
    for (n = hp_xml_find(scope, name); n; n = hp_xml_find_next(scope, n, name)) {
        last = n;
    }
    return last;
}

static const char *font_face(const hp_xml_node *root, const char *lang, const char *id) {
    hp_xml_node *ff;
    for (ff = hp_xml_find(root, "fontface"); ff; ff = hp_xml_find_after(root, ff, "fontface")) {
        const char *l = hp_xml_attr(ff, "lang");
        hp_xml_node *f;
        if (!l || strcmp(l, lang) != 0) {
            continue;
        }
        for (f = hp_xml_child(ff, "font"); f; f = hp_xml_next(f, "font")) {
            const char *fid = hp_xml_attr(f, "id");
            if (fid && id && strcmp(fid, id) == 0) {
                return hp_xml_attr(f, "face");
            }
        }
    }
    return NULL;
}

static void overlay_char(hp_char_format *dst, const hp_char_format *src) {
    if (src->fields & HP_CF_FACE) {
        memcpy(dst->face, src->face, sizeof(dst->face));
    }
    if (src->fields & HP_CF_SIZE) {
        dst->size_hwp = src->size_hwp;
    }
    if (src->fields & HP_CF_BOLD) {
        dst->bold = src->bold;
    }
    if (src->fields & HP_CF_ITALIC) {
        dst->italic = src->italic;
    }
    if (src->fields & HP_CF_UNDERLINE) {
        dst->underline = src->underline;
    }
    if (src->fields & HP_CF_STRIKEOUT) {
        dst->strikeout = src->strikeout;
    }
    if (src->fields & HP_CF_COLOR) {
        dst->color_rgb = src->color_rgb;
    }
    dst->fields |= src->fields;
}

static void overlay_para(hp_para_format *dst, const hp_para_format *src) {
    if (src->fields & HP_PF_ALIGN) {
        dst->align = src->align;
    }
    if (src->fields & HP_PF_LINE_SPACING) {
        dst->line_spacing_percent = src->line_spacing_percent;
    }
    if (src->fields & HP_PF_LEFT) {
        dst->left_hwp = src->left_hwp;
    }
    if (src->fields & HP_PF_RIGHT) {
        dst->right_hwp = src->right_hwp;
    }
    if (src->fields & HP_PF_INDENT) {
        dst->indent_hwp = src->indent_hwp;
    }
    if (src->fields & HP_PF_BEFORE) {
        dst->before_hwp = src->before_hwp;
    }
    if (src->fields & HP_PF_AFTER) {
        dst->after_hwp = src->after_hwp;
    }
    dst->fields |= src->fields;
}

hp_status hp_editor_get_format(const hp_editor *e, size_t i, hp_char_format *cf, hp_para_format *pf) {
    const item *it;
    const hp_xml_node *root;
    if (!e || i >= e->count) {
        return HP_ERR_INVALID;
    }
    if (!e->header) {
        return HP_ERR_UNSUPPORTED;
    }
    it = &e->items[i];
    root = hp_xml_root(e->header);
    if (cf) {
        const hp_xml_node *cp = by_id(root, "charPr", first_char_ref(it->p));
        memset(cf, 0, sizeof(*cf));
        cf->size_hwp = 1000;
        if (cp) {
            const hp_xml_node *u = hp_xml_child(cp, "underline"), *so = hp_xml_child(cp, "strikeout");
            const hp_xml_node *ref = hp_xml_child(cp, "fontRef");
            const char *ut = hp_xml_attr(u, "type"), *ss = hp_xml_attr(so, "shape"), *sc = hp_xml_attr(so, "color");
            const char *face = font_face(root, "HANGUL", ref ? hp_xml_attr(ref, "hangul") : "0");
            cf->size_hwp = (uint32_t)hp_xml_attr_ulong(cp, "height", 1000);
            cf->color_rgb = parse_rgb(hp_xml_attr(cp, "textColor"));
            cf->bold = hp_xml_child(cp, "bold") != NULL;
            cf->italic = hp_xml_child(cp, "italic") != NULL;
            cf->underline = ut && strcmp(ut, "NONE") != 0;
            cf->strikeout = ss && strcmp(ss, "SOLID") == 0 && sc && strcmp(sc, "none") != 0;
            if (face) {
                strncpy(cf->face, face, sizeof(cf->face) - 1);
            }
        }
        cf->fields = HP_CF_FACE | HP_CF_SIZE | HP_CF_BOLD | HP_CF_ITALIC | HP_CF_UNDERLINE | HP_CF_STRIKEOUT | HP_CF_COLOR;
        overlay_char(cf, &it->cf);
    }
    if (pf) {
        const char *ref = hp_xml_attr(it->p, "paraPrIDRef");
        const hp_xml_node *pp = by_id(root, "paraPr", ref ? ref : "0");
        memset(pf, 0, sizeof(*pf));
        pf->align = HP_ALIGN_JUSTIFY;
        pf->line_spacing_percent = 160;
        if (pp) {
            const char *h = hp_xml_attr(hp_xml_child(pp, "align"), "horizontal");
            const hp_xml_node *scope = metric_scope(pp), *m = find_last_in(scope, "margin");
            const hp_xml_node *ls = find_last_in(scope, "lineSpacing");
            if (h) {
                pf->align = strcmp(h, "LEFT") == 0 ? HP_ALIGN_LEFT
                            : strcmp(h, "CENTER") == 0 ? HP_ALIGN_CENTER
                            : strcmp(h, "RIGHT") == 0 ? HP_ALIGN_RIGHT
                            : strncmp(h, "DISTRIBUTE", 10) == 0 ? HP_ALIGN_DISTRIBUTE : HP_ALIGN_JUSTIFY;
            }
            if (m) {
                pf->indent_hwp = (int32_t)hp_xml_attr_long(hp_xml_child(m, "intent"), "value", 0);
                pf->left_hwp = (int32_t)hp_xml_attr_long(hp_xml_child(m, "left"), "value", 0);
                pf->right_hwp = (int32_t)hp_xml_attr_long(hp_xml_child(m, "right"), "value", 0);
                pf->before_hwp = (int32_t)hp_xml_attr_long(hp_xml_child(m, "prev"), "value", 0);
                pf->after_hwp = (int32_t)hp_xml_attr_long(hp_xml_child(m, "next"), "value", 0);
            }
            if (ls) {
                const char *type = hp_xml_attr(ls, "type");
                long long v = hp_xml_attr_long(ls, "value", 160);
                pf->line_spacing_percent = type && strcmp(type, "PERCENT") == 0 && v > 0 && v < 10000 ? (int)v : 0;
            }
        }
        pf->fields = HP_PF_ALIGN | HP_PF_LINE_SPACING | HP_PF_LEFT | HP_PF_RIGHT | HP_PF_INDENT | HP_PF_BEFORE |
                     HP_PF_AFTER;
        overlay_para(pf, &it->pf);
    }
    return HP_OK;
}

hp_status hp_editor_apply_char_format(hp_editor *e, size_t i, const hp_char_format *cf) {
    if (!e || i >= e->count || !cf || e->items[i].deleted) {
        return HP_ERR_INVALID;
    }
    if (!e->header) {
        return HP_ERR_UNSUPPORTED;
    }
    if ((cf->fields & HP_CF_SIZE) && (cf->size_hwp < 100 || cf->size_hwp > 409600)) {
        return HP_ERR_INVALID;
    }
    if ((cf->fields & HP_CF_FACE) && (!cf->face[0] || strpbrk(cf->face, "<>&\"") != NULL)) {
        return HP_ERR_INVALID;
    }
    overlay_char(&e->items[i].cf, cf);
    return HP_OK;
}

hp_status hp_editor_apply_para_format(hp_editor *e, size_t i, const hp_para_format *pf) {
    if (!e || i >= e->count || !pf || e->items[i].deleted) {
        return HP_ERR_INVALID;
    }
    if (!e->header) {
        return HP_ERR_UNSUPPORTED;
    }
    if ((pf->fields & HP_PF_ALIGN) && (pf->align < 0 || pf->align > HP_ALIGN_DISTRIBUTE)) {
        return HP_ERR_INVALID;
    }
    if ((pf->fields & HP_PF_LINE_SPACING) && (pf->line_spacing_percent < 10 || pf->line_spacing_percent > 1000)) {
        return HP_ERR_INVALID;
    }
    overlay_para(&e->items[i].pf, pf);
    return HP_OK;
}


static size_t utf8_floor(const char *s, size_t n, size_t at) {
    if (at > n) {
        at = n;
    }
    while (at > 0 && at < n && ((unsigned char)s[at] & 192) == 128) {
        at--;
    }
    return at;
}

static size_t utf8_ceil(const char *s, size_t n, size_t at) {
    if (at > n) {
        at = n;
    }
    while (at < n && ((unsigned char)s[at] & 192) == 128) {
        at++;
    }
    return at;
}

hp_status hp_editor_apply_char_format_range(hp_editor *e, size_t i, size_t start, size_t end, const hp_char_format *cf) {
    item *it;
    size_t n;
    span *g;
    hp_status st;
    if (!e || i >= e->count || !cf || e->items[i].deleted || start >= end) {
        return HP_ERR_INVALID;
    }
    it = &e->items[i];
    n = strlen(it->text);
    start = utf8_floor(it->text, n, start);
    end = utf8_ceil(it->text, n, end);
    if (start >= end) {
        return HP_ERR_INVALID;
    }
    if (start == 0 && end == n) {
        return hp_editor_apply_char_format(e, i, cf); /* the whole paragraph */
    }
    if (!it->simple) {
        return HP_ERR_UNSUPPORTED;
    }
    {
        /* same validation as the whole-paragraph form */
        hp_char_format probe = it->cf;
        st = hp_editor_apply_char_format(e, i, cf);
        if (st != HP_OK) {
            return st;
        }
        it->cf = probe;
    }
    g = (span *)realloc(it->spans, (it->span_count + 1) * sizeof(*g));
    if (!g) {
        return HP_ERR_NOMEM;
    }
    it->spans = g;
    g[it->span_count].start = start;
    g[it->span_count].end = end;
    g[it->span_count].cf = *cf;
    it->span_count++;
    return HP_OK;
}

hp_status hp_editor_get_format_at(const hp_editor *e, size_t i, size_t offset, hp_char_format *cf) {
    const item *it;
    const hp_xml_node *root, *run, *t, *found = NULL;
    size_t k, pos = 0;
    if (!e || i >= e->count || !cf) {
        return HP_ERR_INVALID;
    }
    if (!e->header) {
        return HP_ERR_UNSUPPORTED;
    }
    it = &e->items[i];
    root = hp_xml_root(e->header);
    /* the source run holding this offset (original text) */
    for (run = hp_xml_child(it->p, "run"); run && !found; run = hp_xml_next(run, "run")) {
        for (t = hp_xml_child(run, "t"); t; t = hp_xml_next(t, "t")) {
            buffer b = {0};
            char *s;
            size_t len;
            text_of(&b, t);
            s = finish(&b);
            len = s ? strlen(s) : 0;
            free(s);
            if (offset < pos + len || (offset == pos + len && offset > 0 && !found)) {
                found = run;
            }
            pos += len;
        }
    }
    {
        hp_status st = hp_editor_get_format(e, i, cf, NULL);
        const hp_xml_node *cp;
        if (st != HP_OK) {
            return st;
        }
        if (found && (cp = by_id(root, "charPr", hp_xml_attr(found, "charPrIDRef") ? hp_xml_attr(found, "charPrIDRef") : "0"))) {
            hp_char_format base;
            const hp_xml_node *u = hp_xml_child(cp, "underline"), *so = hp_xml_child(cp, "strikeout");
            const hp_xml_node *ref = hp_xml_child(cp, "fontRef");
            const char *ut = hp_xml_attr(u, "type"), *ss = hp_xml_attr(so, "shape"), *sc = hp_xml_attr(so, "color");
            const char *face = font_face(root, "HANGUL", ref ? hp_xml_attr(ref, "hangul") : "0");
            memset(&base, 0, sizeof(base));
            base.size_hwp = (uint32_t)hp_xml_attr_ulong(cp, "height", 1000);
            base.color_rgb = parse_rgb(hp_xml_attr(cp, "textColor"));
            base.bold = hp_xml_child(cp, "bold") != NULL;
            base.italic = hp_xml_child(cp, "italic") != NULL;
            base.underline = ut && strcmp(ut, "NONE") != 0;
            base.strikeout = ss && strcmp(ss, "SOLID") == 0 && sc && strcmp(sc, "none") != 0;
            if (face) {
                snprintf(base.face, sizeof(base.face), "%s", face);
            }
            base.fields = cf->fields;
            overlay_char(&base, &it->cf);
            *cf = base;
        }
    }
    for (k = 0; k < it->span_count; k++) {
        if (it->spans[k].start <= offset && (offset < it->spans[k].end || (offset == it->spans[k].end && offset > it->spans[k].start))) {
            overlay_char(cf, &it->spans[k].cf);
        }
    }
    return HP_OK;
}

/* ---- string surgery on XML text ---- */

/* Start tag of the next element with this local name at or after `from`,
 * before `to`: [*a, *b) covers '<' .. '>' (an empty element is the whole tag). */
static int find_tag(const char *s, size_t from, size_t to, const char *local, size_t *a, size_t *b) {
    size_t i, ln = strlen(local);
    for (i = from; i < to; i++) {
        size_t j, name_start, name_end;
        const char *colon;
        if (s[i] != '<' || i + 1 >= to || s[i + 1] == '/' || s[i + 1] == '!' || s[i + 1] == '?') {
            continue;
        }
        name_start = i + 1;
        for (j = name_start; j < to && s[j] != ' ' && s[j] != '/' && s[j] != '>' && s[j] != '\t' && s[j] != '\n' &&
                             s[j] != '\r';
             j++) {
        }
        name_end = j;
        colon = memchr(s + name_start, ':', name_end - name_start);
        if (colon) {
            name_start = (size_t)(colon - s) + 1;
        }
        if (name_end - name_start == ln && memcmp(s + name_start, local, ln) == 0) {
            const char *gt = memchr(s + name_end, '>', to - name_end);
            if (!gt) {
                return 0;
            }
            *a = i;
            *b = (size_t)(gt - s) + 1;
            return 1;
        }
    }
    return 0;
}

/* Replace s[a,b) with ins (s is malloc'd and NUL-terminated). */
static int splice(char **s, size_t a, size_t b, const char *ins) {
    size_t len = strlen(*s), n = strlen(ins);
    char *out = (char *)malloc(len - (b - a) + n + 1);
    if (!out) {
        return 0;
    }
    memcpy(out, *s, a);
    memcpy(out + a, ins, n);
    memcpy(out + a + n, *s + b, len - b + 1);
    free(*s);
    *s = out;
    return 1;
}

/* Set attr in the tag at s[a,b) (adds it when missing). Updates *b. */
static int set_attr(char **s, size_t a, size_t *b, const char *attr, const char *value) {
    size_t i, an = strlen(attr);
    char ins[512];
    for (i = a + 1; i + an + 2 < *b; i++) {
        char before = (*s)[i - 1];
        if ((before == ' ' || before == '\t' || before == '\n' || before == '\r') && memcmp(*s + i, attr, an) == 0 &&
            (*s)[i + an] == '=' && ((*s)[i + an + 1] == '"' || (*s)[i + an + 1] == '\'')) {
            char quote = (*s)[i + an + 1];
            const char *end = memchr(*s + i + an + 2, quote, *b - (i + an + 2));
            size_t old_len, new_len;
            if (!end) {
                return 0;
            }
            old_len = (size_t)(end - (*s + i + an + 2));
            new_len = strlen(value);
            if (!splice(s, i + an + 2, i + an + 2 + old_len, value)) {
                return 0;
            }
            *b = *b - old_len + new_len;
            return 1;
        }
    }
    /* missing: insert before '>' or '/>' */
    i = *b - 1;
    if (i > a && (*s)[i - 1] == '/') {
        i--;
    }
    snprintf(ins, sizeof(ins), " %s=\"%s\"", attr, value);
    if (!splice(s, i, i, ins)) {
        return 0;
    }
    *b += strlen(ins);
    return 1;
}

static void prefix_of(const char *s, char *out, size_t n) {
    /* "<hh:charPr ..." -> "hh:" */
    size_t i;
    out[0] = '\0';
    for (i = 1; s[i] && s[i] != ' ' && s[i] != '>' && s[i] != '/'; i++) {
        if (s[i] == ':') {
            if (i < n) {
                memcpy(out, s + 1, i);
                out[i] = '\0';
            }
            return;
        }
    }
}

/* ---- registry of new header entries ---- */

typedef struct {
    int is_para;
    char base[32];
    hp_char_format cf;
    hp_para_format pf;
    unsigned id;
} new_shape;

typedef struct {
    new_shape *v;
    size_t n;
    unsigned next_char, next_para;
} shape_reg;

static void normalize_char(hp_char_format *c) {
    hp_char_format z;
    memset(&z, 0, sizeof(z));
    z.fields = c->fields;
    if (c->fields & HP_CF_FACE) {
        snprintf(z.face, sizeof(z.face), "%s", c->face);
    }
    z.size_hwp = c->fields & HP_CF_SIZE ? c->size_hwp : 0;
    z.bold = c->fields & HP_CF_BOLD ? c->bold != 0 : 0;
    z.italic = c->fields & HP_CF_ITALIC ? c->italic != 0 : 0;
    z.underline = c->fields & HP_CF_UNDERLINE ? c->underline != 0 : 0;
    z.strikeout = c->fields & HP_CF_STRIKEOUT ? c->strikeout != 0 : 0;
    z.color_rgb = c->fields & HP_CF_COLOR ? c->color_rgb & 0xFFFFFFu : 0;
    *c = z;
}

static void normalize_para(hp_para_format *p) {
    hp_para_format z;
    memset(&z, 0, sizeof(z));
    z.fields = p->fields;
    z.align = p->fields & HP_PF_ALIGN ? p->align : 0;
    z.line_spacing_percent = p->fields & HP_PF_LINE_SPACING ? p->line_spacing_percent : 0;
    z.left_hwp = p->fields & HP_PF_LEFT ? p->left_hwp : 0;
    z.right_hwp = p->fields & HP_PF_RIGHT ? p->right_hwp : 0;
    z.indent_hwp = p->fields & HP_PF_INDENT ? p->indent_hwp : 0;
    z.before_hwp = p->fields & HP_PF_BEFORE ? p->before_hwp : 0;
    z.after_hwp = p->fields & HP_PF_AFTER ? p->after_hwp : 0;
    *p = z;
}

/* New id for (base, format); 0 on OOM. */
static int reg_id(shape_reg *r, int is_para, const char *base, const hp_char_format *cf, const hp_para_format *pf,
                  unsigned *out) {
    new_shape key, *g;
    size_t i;
    memset(&key, 0, sizeof(key));
    key.is_para = is_para;
    strncpy(key.base, base, sizeof(key.base) - 1);
    if (cf) {
        key.cf = *cf;
        normalize_char(&key.cf);
    }
    if (pf) {
        key.pf = *pf;
        normalize_para(&key.pf);
    }
    for (i = 0; i < r->n; i++) {
        if (r->v[i].is_para == is_para && strcmp(r->v[i].base, key.base) == 0 &&
            memcmp(&r->v[i].cf, &key.cf, sizeof(key.cf)) == 0 && memcmp(&r->v[i].pf, &key.pf, sizeof(key.pf)) == 0) {
            *out = r->v[i].id;
            return 1;
        }
    }
    g = (new_shape *)realloc(r->v, (r->n + 1) * sizeof(*g));
    if (!g) {
        return 0;
    }
    r->v = g;
    key.id = is_para ? r->next_para++ : r->next_char++;
    r->v[r->n++] = key;
    *out = key.id;
    return 1;
}

/* count (or with want_next, max id + 1) of the container's children */
static unsigned count_children(const hp_xml_node *root, const char *container, const char *name, int want_next) {
    const hp_xml_node *c = hp_xml_find(root, container), *n;
    unsigned k = 0, next = 0;
    for (n = c ? hp_xml_child(c, name) : NULL; n; n = hp_xml_next(n, name)) {
        unsigned id = (unsigned)hp_xml_attr_ulong(n, "id", k);
        k++;
        if (id + 1 > next) {
            next = id + 1;
        }
    }
    return want_next ? next : k;
}

/* Range formats (0.11.0): a simple paragraph's runs are rewritten as one run per
 * stretch of text with the same source shape and the same requested format. */
static hp_status rebuild_runs(const char *xml, const hp_xml_node *p, const item *it, shape_reg *reg, patch **ps,
                              size_t *np) {
    const hp_xml_node *r, *first = NULL, *last = NULL, *sample_t = NULL;
    buffer text = {0}, out = {0}, tail = {0};
    const hp_xml_node **owner = NULL;
    size_t len = 0, cap = 0, i, k;
    char *all, *rq = NULL, *tq = NULL;
    hp_status st = HP_OK;
    for (r = hp_xml_child(p, "run"); r; r = hp_xml_next(r, "run")) {
        const hp_xml_node *t;
        size_t before = text.n;
        if (!first) {
            first = r;
        }
        last = r;
        for (t = hp_xml_child(r, "t"); t; t = hp_xml_next(t, "t")) {
            if (!sample_t) {
                sample_t = t;
            }
            text_of(&text, t);
        }
        if (text.failed) {
            free(text.s);
            free(owner);
            return HP_ERR_NOMEM;
        }
        if (text.n > cap) {
            const hp_xml_node **g = (const hp_xml_node **)realloc(owner, text.n * sizeof(*g));
            if (!g) {
                free(text.s);
                free(owner);
                return HP_ERR_NOMEM;
            }
            owner = g;
            cap = text.n;
        }
        for (i = before; i < text.n; i++) {
            owner[i] = r;
        }
        if (text.n == before && text.n > 0) {
            /* a run without text after the text (paragraph-mark shape): keep it */
            putn(&tail, xml + r->start, r->end - r->start);
        }
    }
    len = text.n;
    all = text.s;
    if (!first || !len) {
        free(all);
        free(owner);
        free(tail.s);
        return HP_OK;
    }
    {
        buffer q = {0};
        puts_b(&q, first->qname);
        rq = finish(&q);
        memset(&q, 0, sizeof(q));
        if (sample_t) {
            puts_b(&q, sample_t->qname);
        } else {
            qualified(&q, first->qname, "t");
        }
        tq = finish(&q);
    }
    for (i = 0; i < len && st == HP_OK && rq && tq;) {
        hp_char_format want = it->cf, next_want;
        size_t j;
        const char *base = hp_xml_attr(owner[i], "charPrIDRef");
        unsigned id;
        char num[16];
        for (k = 0; k < it->span_count; k++) {
            if (it->spans[k].start <= i && i < it->spans[k].end) {
                overlay_char(&want, &it->spans[k].cf);
            }
        }
        normalize_char(&want);
        for (j = i + 1; j < len; j++) {
            if (owner[j] != owner[i]) {
                break;
            }
            next_want = it->cf;
            for (k = 0; k < it->span_count; k++) {
                if (it->spans[k].start <= j && j < it->spans[k].end) {
                    overlay_char(&next_want, &it->spans[k].cf);
                }
            }
            normalize_char(&next_want);
            if (memcmp(&next_want, &want, sizeof(want)) != 0) {
                break;
            }
        }
        if (want.fields) {
            if (!reg_id(reg, 0, base ? base : "0", &want, NULL, &id)) {
                st = HP_ERR_NOMEM;
                break;
            }
            snprintf(num, sizeof(num), "%u", id);
        } else {
            snprintf(num, sizeof(num), "%s", base ? base : "0");
        }
        {
            char saved = all[j], *piece;
            all[j] = '\0';
            piece = all + i;
            puts_b(&out, "<");
            puts_b(&out, rq);
            puts_b(&out, " charPrIDRef=\"");
            puts_b(&out, num);
            puts_b(&out, "\"><");
            puts_b(&out, tq);
            puts_b(&out, ">");
            escape(&out, piece, tq);
            puts_b(&out, "</");
            puts_b(&out, tq);
            puts_b(&out, "></");
            puts_b(&out, rq);
            puts_b(&out, ">");
            all[j] = saved;
        }
        i = j;
    }
    if (st == HP_OK && (!rq || !tq)) {
        st = HP_ERR_NOMEM;
    }
    if (st == HP_OK) {
        char *t = finish(&tail);
        if (t) {
            puts_b(&out, t);
        }
        free(t);
        memset(&tail, 0, sizeof(tail));
        if (!add_patch(ps, np, first->start, last->end, finish(&out))) {
            st = HP_ERR_NOMEM;
        }
        memset(&out, 0, sizeof(out));
    }
    free(out.s);
    free(tail.s);
    free(all);
    free(owner);
    free(rq);
    free(tq);
    return st;
}

/* Second pass over one patched section: point formatted paragraphs and their runs
 * at new shapes. `xml` is replaced on success. */
static hp_status format_section(hp_editor *e, size_t si, char **xml, shape_reg *reg) {
    hp_xml_doc *d = NULL;
    hp_xml_node *root, *p;
    patch *ps = NULL;
    size_t n = 0, k, pos = 0, j = 0, any = 0;
    buffer b = {0};
    hp_status st;
    for (k = 0; k < e->count; k++) {
        if (e->items[k].section == si && !e->items[k].deleted &&
            (e->items[k].cf.fields || e->items[k].pf.fields || e->items[k].span_count)) {
            any = 1;
        }
    }
    if (!any) {
        return HP_OK;
    }
    st = hp_xml_parse(*xml, strlen(*xml), &d, NULL, 0);
    if (st != HP_OK) {
        return st;
    }
    root = hp_xml_root(d);
    for (p = hp_xml_find(root, "p"); p && st == HP_OK; p = hp_xml_find_next(root, p, "p")) {
        item *it;
        hp_xml_node *r, *seg;
        while (j < e->count && (e->items[j].section != si || e->items[j].deleted)) {
            j++;
        }
        if (j >= e->count) {
            st = HP_ERR_UNSUPPORTED; /* paragraph list out of step with the XML */
            break;
        }
        it = &e->items[j++];
        if (!it->cf.fields && !it->pf.fields && !it->span_count) {
            continue;
        }
        if (it->pf.fields) {
            const char *base = hp_xml_attr(p, "paraPrIDRef");
            unsigned id;
            char num[16], *tag;
            size_t tb = p->inner_start == p->end ? p->end : p->inner_start;
            if (!reg_id(reg, 1, base ? base : "0", NULL, &it->pf, &id)) {
                st = HP_ERR_NOMEM;
                break;
            }
            tag = (char *)malloc(tb - p->start + 1);
            if (!tag) {
                st = HP_ERR_NOMEM;
                break;
            }
            memcpy(tag, *xml + p->start, tb - p->start);
            tag[tb - p->start] = '\0';
            {
                size_t te = strlen(tag);
                snprintf(num, sizeof(num), "%u", id);
                if (!set_attr(&tag, 0, &te, "paraPrIDRef", num) || !add_patch(&ps, &n, p->start, tb, tag)) {
                    st = HP_ERR_NOMEM;
                    break;
                }
            }
        }
        if (it->span_count && it->simple) {
            st = rebuild_runs(*xml, p, it, reg, &ps, &n);
        }
        for (r = hp_xml_child(p, "run"); r && it->cf.fields && !(it->span_count && it->simple) && st == HP_OK;
             r = hp_xml_next(r, "run")) {
            const char *base = hp_xml_attr(r, "charPrIDRef");
            unsigned id;
            char num[16], *tag;
            size_t tb = r->inner_start == r->end ? r->end : r->inner_start, te;
            if (!reg_id(reg, 0, base ? base : "0", &it->cf, NULL, &id)) {
                st = HP_ERR_NOMEM;
                break;
            }
            tag = (char *)malloc(tb - r->start + 1);
            if (!tag) {
                st = HP_ERR_NOMEM;
                break;
            }
            memcpy(tag, *xml + r->start, tb - r->start);
            tag[tb - r->start] = '\0';
            te = strlen(tag);
            snprintf(num, sizeof(num), "%u", id);
            if (!set_attr(&tag, 0, &te, "charPrIDRef", num) || !add_patch(&ps, &n, r->start, tb, tag)) {
                st = HP_ERR_NOMEM;
            }
        }
        /* the saved line layout no longer fits the new shape */
        for (seg = hp_xml_child(p, "linesegarray"); seg && st == HP_OK; seg = hp_xml_next(seg, "linesegarray")) {
            if (!add_patch(&ps, &n, seg->start, seg->end, copy(""))) {
                st = HP_ERR_NOMEM;
            }
        }
    }
    if (st == HP_OK && n) {
        qsort(ps, n, sizeof(*ps), patch_cmp);
        for (k = 0; k < n; k++) {
            if (ps[k].a < pos) {
                st = HP_ERR_UNSUPPORTED;
                break;
            }
            putn(&b, *xml + pos, ps[k].a - pos);
            puts_b(&b, ps[k].text);
            pos = ps[k].b;
        }
        if (st == HP_OK) {
            char *out;
            puts_b(&b, *xml + pos);
            out = finish(&b);
            if (!out) {
                st = HP_ERR_NOMEM;
            } else {
                free(*xml);
                *xml = out;
            }
        } else {
            free(b.s);
        }
    }
    for (k = 0; k < n; k++) {
        free(ps[k].text);
    }
    free(ps);
    hp_xml_free(d);
    return st;
}

static const char *const LANGS_UP[7] = {"HANGUL", "LATIN", "HANJA", "JAPANESE", "OTHER", "SYMBOL", "USER"};
static const char *const LANGS_LOW[7] = {"hangul", "latin", "hanja", "japanese", "other", "symbol", "user"};

typedef struct {
    char face[64];
    unsigned id[7];
} face_ids;

#define NO_FONT 0xFFFFFFFFu

static int lang_present(const hp_xml_node *root, const char *lang) {
    hp_xml_node *ff;
    for (ff = hp_xml_find(root, "fontface"); ff; ff = hp_xml_find_after(root, ff, "fontface")) {
        const char *l = hp_xml_attr(ff, "lang");
        if (l && strcmp(l, lang) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Font id of `face` in each language, adding <font> elements (text in add[]) as needed. */
static int face_lookup(const hp_xml_node *root, face_ids **faces, size_t *nf, const char *face, buffer add[7],
                       unsigned next[7], const char *prefix, unsigned out[7]) {
    size_t i;
    int lang;
    face_ids *g, rec;
    for (i = 0; i < *nf; i++) {
        if (strcmp((*faces)[i].face, face) == 0) {
            memcpy(out, (*faces)[i].id, sizeof((*faces)[i].id));
            return 1;
        }
    }
    memset(&rec, 0, sizeof(rec));
    strncpy(rec.face, face, sizeof(rec.face) - 1);
    for (lang = 0; lang < 7; lang++) {
        hp_xml_node *ff, *f;
        int found = 0;
        for (ff = hp_xml_find(root, "fontface"); ff && !found; ff = hp_xml_find_after(root, ff, "fontface")) {
            const char *l = hp_xml_attr(ff, "lang");
            if (!l || strcmp(l, LANGS_UP[lang]) != 0) {
                continue;
            }
            for (f = hp_xml_child(ff, "font"); f; f = hp_xml_next(f, "font")) {
                const char *fc = hp_xml_attr(f, "face");
                if (fc && strcmp(fc, face) == 0) {
                    rec.id[lang] = (unsigned)hp_xml_attr_ulong(f, "id", 0);
                    found = 1;
                    break;
                }
            }
        }
        if (!found && !lang_present(root, LANGS_UP[lang])) {
            rec.id[lang] = NO_FONT; /* the header has no fonts for this language */
        } else if (!found) {
            char tmp[256];
            rec.id[lang] = next[lang]++;
            snprintf(tmp, sizeof(tmp), "<%sfont id=\"%u\" face=\"%s\" type=\"TTF\" isEmbedded=\"0\"/>", prefix, rec.id[lang],
                     face);
            puts_b(&add[lang], tmp);
        }
    }
    g = (face_ids *)realloc(*faces, (*nf + 1) * sizeof(*g));
    if (!g) {
        return 0;
    }
    *faces = g;
    g[(*nf)++] = rec;
    memcpy(out, rec.id, sizeof(rec.id));
    return 1;
}

/* Where an optional charPr child goes: before the first of the later children. */
static size_t char_child_slot(const char *s, const char *const *later) {
    size_t a, b, best = (size_t)-1, len = strlen(s);
    int k;
    for (k = 0; later[k]; k++) {
        if (find_tag(s, 1, len, later[k], &a, &b) && a < best) {
            best = a;
        }
    }
    if (best == (size_t)-1) {
        const char *close = strstr(s, "</");
        best = close ? (size_t)(close - s) : len;
    }
    return best;
}

static int remove_empty_child(char **s, const char *local) {
    size_t a, b;
    if (find_tag(*s, 1, strlen(*s), local, &a, &b)) {
        return splice(s, a, b, "");
    }
    return 1;
}

static char *make_char_shape(const char *base, size_t base_len, const new_shape *ns, const unsigned font_ids[7]) {
    static const char *const after_italic[] = {"bold", "underline", "strikeout", "outline", "shadow", "emboss",
                                               "engrave", "supscript", "subscript", NULL};
    static const char *const after_bold[] = {"underline", "strikeout", "outline", "shadow", "emboss", "engrave",
                                             "supscript", "subscript", NULL};
    static const char *const after_underline[] = {"strikeout", "outline", "shadow", "emboss", "engrave",
                                                  "supscript", "subscript", NULL};
    static const char *const after_strike[] = {"outline", "shadow", "emboss", "engrave", "supscript", "subscript", NULL};
    const hp_char_format *c = &ns->cf;
    char *s = (char *)malloc(base_len + 1), prefix[32], num[32], tmp[256];
    size_t a = 0, b, len;
    if (!s) {
        return NULL;
    }
    memcpy(s, base, base_len);
    s[base_len] = '\0';
    prefix_of(s, prefix, sizeof(prefix));
    len = strlen(s);
    b = (size_t)(strchr(s, '>') - s) + 1;
    snprintf(num, sizeof(num), "%u", ns->id);
    if (!set_attr(&s, 0, &b, "id", num)) {
        goto fail;
    }
    if (c->fields & HP_CF_SIZE) {
        snprintf(num, sizeof(num), "%u", c->size_hwp);
        if (!set_attr(&s, 0, &b, "height", num)) {
            goto fail;
        }
    }
    if (c->fields & HP_CF_COLOR) {
        snprintf(num, sizeof(num), "#%06X", c->color_rgb & 0xFFFFFFu);
        if (!set_attr(&s, 0, &b, "textColor", num)) {
            goto fail;
        }
    }
    len = strlen(s);
    if ((c->fields & HP_CF_FACE) && find_tag(s, 1, len, "fontRef", &a, &b)) {
        int lang;
        for (lang = 0; lang < 7; lang++) {
            if (font_ids[lang] == NO_FONT) {
                continue;
            }
            snprintf(num, sizeof(num), "%u", font_ids[lang]);
            if (!set_attr(&s, a, &b, LANGS_LOW[lang], num)) {
                goto fail;
            }
        }
    }
    if (c->fields & HP_CF_ITALIC) {
        if (!remove_empty_child(&s, "italic")) {
            goto fail;
        }
        if (c->italic) {
            size_t at = char_child_slot(s, after_italic);
            snprintf(tmp, sizeof(tmp), "<%sitalic/>", prefix);
            if (!splice(&s, at, at, tmp)) {
                goto fail;
            }
        }
    }
    if (c->fields & HP_CF_BOLD) {
        if (!remove_empty_child(&s, "bold")) {
            goto fail;
        }
        if (c->bold) {
            size_t at = char_child_slot(s, after_bold);
            snprintf(tmp, sizeof(tmp), "<%sbold/>", prefix);
            if (!splice(&s, at, at, tmp)) {
                goto fail;
            }
        }
    }
    len = strlen(s);
    if (c->fields & HP_CF_UNDERLINE) {
        if (find_tag(s, 1, len, "underline", &a, &b)) {
            if (!set_attr(&s, a, &b, "type", c->underline ? "BOTTOM" : "NONE")) {
                goto fail;
            }
        } else if (c->underline) {
            size_t at = char_child_slot(s, after_underline);
            snprintf(tmp, sizeof(tmp), "<%sunderline type=\"BOTTOM\" shape=\"SOLID\" color=\"#000000\"/>", prefix);
            if (!splice(&s, at, at, tmp)) {
                goto fail;
            }
        }
    }
    len = strlen(s);
    if (c->fields & HP_CF_STRIKEOUT) {
        if (find_tag(s, 1, len, "strikeout", &a, &b)) {
            if (!set_attr(&s, a, &b, "shape", c->strikeout ? "SOLID" : "NONE")) {
                goto fail;
            }
            if (c->strikeout && !set_attr(&s, a, &b, "color", "#000000")) {
                goto fail;
            }
        } else if (c->strikeout) {
            size_t at = char_child_slot(s, after_strike);
            snprintf(tmp, sizeof(tmp), "<%sstrikeout shape=\"SOLID\" color=\"#000000\"/>", prefix);
            if (!splice(&s, at, at, tmp)) {
                goto fail;
            }
        }
    }
    return s;
fail:
    free(s);
    return NULL;
}

/* Set one margin child (intent/left/...) in s[from,to); returns new `to`. */
static int set_margin_child(char **s, size_t from, size_t *to, const char *local, long long value) {
    size_t a, b, old_b;
    char num[32];
    if (!find_tag(*s, from, *to, local, &a, &b)) {
        return 1; /* absent: leave the default */
    }
    old_b = b;
    snprintf(num, sizeof(num), "%lld", value);
    if (!set_attr(s, a, &b, "value", num) || !set_attr(s, a, &b, "unit", "HWPUNIT")) {
        return 0;
    }
    *to += b - old_b;
    return 1;
}

/* Is offset `at` inside a <..:default> branch of an hp:switch? */
static int in_default(const char *s, size_t at) {
    const char *q = s, *open = NULL, *close = NULL, *hit;
    while ((hit = strstr(q, ":default>")) != NULL && (size_t)(hit - s) < at) {
        const char *lt = hit;
        while (lt > s && *lt != '<') {
            lt--;
        }
        if (lt[1] == '/') {
            close = hit;
        } else {
            open = hit;
        }
        q = hit + 1;
    }
    return open && (!close || open > close);
}

static char *make_para_shape(const char *base, size_t base_len, const new_shape *ns) {
    static const char *const align_names[] = {"LEFT", "CENTER", "RIGHT", "JUSTIFY", "DISTRIBUTE"};
    const hp_para_format *f = &ns->pf;
    char *s = (char *)malloc(base_len + 1), num[32], prefix[32], tmp[160];
    size_t a, b, from;
    int has_unitchar;
    if (!s) {
        return NULL;
    }
    memcpy(s, base, base_len);
    s[base_len] = '\0';
    prefix_of(s, prefix, sizeof(prefix));
    has_unitchar = strstr(s, "HwpUnitChar") != NULL;
    b = (size_t)(strchr(s, '>') - s) + 1;
    snprintf(num, sizeof(num), "%u", ns->id);
    if (!set_attr(&s, 0, &b, "id", num)) {
        goto fail;
    }
    if (f->fields & HP_PF_ALIGN) {
        const char *name = align_names[f->align >= 0 && f->align <= 4 ? f->align : 3];
        if (find_tag(s, 1, strlen(s), "align", &a, &b)) {
            if (!set_attr(&s, a, &b, "horizontal", name)) {
                goto fail;
            }
        } else {
            size_t at = (size_t)(strchr(s, '>') - s) + 1;
            snprintf(tmp, sizeof(tmp), "<%salign horizontal=\"%s\" vertical=\"BASELINE\"/>", prefix, name);
            if (!splice(&s, at, at, tmp)) {
                goto fail;
            }
        }
    }
    /* a childless <margin .../> (older writers) becomes the standard form */
    if (find_tag(s, 1, strlen(s), "margin", &a, &b) && s[b - 2] == '/') {
        char rep[512];
        snprintf(rep, sizeof(rep),
                 "<%smargin><hc:intent value=\"0\" unit=\"HWPUNIT\"/><hc:left value=\"0\" unit=\"HWPUNIT\"/>"
                 "<hc:right value=\"0\" unit=\"HWPUNIT\"/><hc:prev value=\"0\" unit=\"HWPUNIT\"/>"
                 "<hc:next value=\"0\" unit=\"HWPUNIT\"/></%smargin>",
                 prefix, prefix);
        if (!splice(&s, a, b, rep)) {
            goto fail;
        }
    }
    /* every <margin> (HwpUnitChar case, legacy default, or plain) */
    from = 1;
    while (find_tag(s, from, strlen(s), "margin", &a, &b)) {
        const char *close;
        size_t to;
        long long factor = has_unitchar && in_default(s, a) ? 2 : 1;
        close = strstr(s + b, "margin>");
        to = close ? (size_t)(close - s) : strlen(s);
        if (((f->fields & HP_PF_INDENT) && !set_margin_child(&s, b, &to, "intent", (long long)f->indent_hwp * factor)) ||
            ((f->fields & HP_PF_LEFT) && !set_margin_child(&s, b, &to, "left", (long long)f->left_hwp * factor)) ||
            ((f->fields & HP_PF_RIGHT) && !set_margin_child(&s, b, &to, "right", (long long)f->right_hwp * factor)) ||
            ((f->fields & HP_PF_BEFORE) && !set_margin_child(&s, b, &to, "prev", (long long)f->before_hwp * factor)) ||
            ((f->fields & HP_PF_AFTER) && !set_margin_child(&s, b, &to, "next", (long long)f->after_hwp * factor))) {
            goto fail;
        }
        from = to;
    }
    if (f->fields & HP_PF_LINE_SPACING) {
        from = 1;
        while (find_tag(s, from, strlen(s), "lineSpacing", &a, &b)) {
            snprintf(num, sizeof(num), "%d", f->line_spacing_percent);
            if (!set_attr(&s, a, &b, "type", "PERCENT") || !set_attr(&s, a, &b, "value", num)) {
                goto fail;
            }
            from = b;
        }
    }
    return s;
fail:
    free(s);
    return NULL;
}

/* Add the registered shapes (and fonts) to header.xml. *out = new header text. */
static hp_status build_header(hp_editor *e, const shape_reg *reg, const hp_package_entry *part, char **out) {
    const hp_xml_node *root = hp_xml_root(e->header), *chars = hp_xml_find(root, "charProperties"),
                      *paras = hp_xml_find(root, "paraProperties");
    const char *src = (const char *)part->data;
    buffer add_chars = {0}, add_paras = {0}, add_fonts[7];
    face_ids *faces = NULL;
    size_t nf = 0, i;
    unsigned next_font[7];
    char font_prefix[32] = "hh:";
    patch *ps = NULL;
    size_t n = 0, pos = 0;
    buffer b = {0};
    hp_status st = HP_OK;
    int lang;
    memset(add_fonts, 0, sizeof(add_fonts));
    *out = NULL;
    if (!chars || !paras) {
        return HP_ERR_UNSUPPORTED;
    }
    for (lang = 0; lang < 7; lang++) {
        hp_xml_node *ff;
        next_font[lang] = 0;
        for (ff = hp_xml_find(root, "fontface"); ff; ff = hp_xml_find_after(root, ff, "fontface")) {
            const char *l = hp_xml_attr(ff, "lang");
            hp_xml_node *f;
            if (!l || strcmp(l, LANGS_UP[lang]) != 0) {
                continue;
            }
            prefix_of(src + ff->start, font_prefix, sizeof(font_prefix));
            for (f = hp_xml_child(ff, "font"); f; f = hp_xml_next(f, "font")) {
                unsigned id = (unsigned)hp_xml_attr_ulong(f, "id", 0);
                if (id + 1 > next_font[lang]) {
                    next_font[lang] = id + 1;
                }
            }
        }
    }
    for (i = 0; i < reg->n && st == HP_OK; i++) {
        const new_shape *ns = &reg->v[i];
        const hp_xml_node *base = by_id(root, ns->is_para ? "paraPr" : "charPr", ns->base);
        char *text;
        if (!base) {
            /* referenced shape missing from the header: copy the first one */
            base = hp_xml_find(root, ns->is_para ? "paraPr" : "charPr");
        }
        if (!base) {
            st = HP_ERR_UNSUPPORTED;
            break;
        }
        if (ns->is_para) {
            text = make_para_shape(src + base->start, base->end - base->start, ns);
        } else {
            unsigned ids[7] = {0};
            if ((ns->cf.fields & HP_CF_FACE) &&
                !face_lookup(root, &faces, &nf, ns->cf.face, add_fonts, next_font, font_prefix, ids)) {
                st = HP_ERR_NOMEM;
                break;
            }
            text = make_char_shape(src + base->start, base->end - base->start, ns, ids);
        }
        if (!text) {
            st = HP_ERR_NOMEM;
            break;
        }
        puts_b(ns->is_para ? &add_paras : &add_chars, text);
        free(text);
    }
    /* patches: new elements before the container end tags, updated counts */
    if (st == HP_OK) {
        const hp_xml_node *containers[2] = {chars, paras};
        buffer *adds[2] = {&add_chars, &add_paras};
        unsigned totals[2] = {count_children(root, "charProperties", "charPr", 0),
                              count_children(root, "paraProperties", "paraPr", 0)};
        for (i = 0; i < reg->n; i++) {
            totals[reg->v[i].is_para]++;
        }
        int c;
        for (c = 0; c < 2 && st == HP_OK; c++) {
            char *body = finish(adds[c]), *tag, num[16];
            size_t tb, te;
            memset(adds[c], 0, sizeof(*adds[c]));
            if (!body) {
                st = HP_ERR_NOMEM;
                break;
            }
            if (!*body || !containers[c]) {
                free(body);
                continue;
            }
            if (containers[c]->inner_start == containers[c]->end) {
                free(body);
                st = HP_ERR_UNSUPPORTED;
                break;
            }
            if (!add_patch(&ps, &n, containers[c]->inner_end, containers[c]->inner_end, body)) {
                st = HP_ERR_NOMEM;
                break;
            }
            if (hp_xml_attr(containers[c], "itemCnt")) {
                tb = containers[c]->inner_start;
                tag = (char *)malloc(tb - containers[c]->start + 1);
                if (!tag) {
                    st = HP_ERR_NOMEM;
                    break;
                }
                memcpy(tag, src + containers[c]->start, tb - containers[c]->start);
                tag[tb - containers[c]->start] = '\0';
                te = strlen(tag);
                snprintf(num, sizeof(num), "%u", totals[c]);
                if (!set_attr(&tag, 0, &te, "itemCnt", num) || !add_patch(&ps, &n, containers[c]->start, tb, tag)) {
                    st = HP_ERR_NOMEM;
                }
            }
        }
    }
    for (lang = 0; lang < 7 && st == HP_OK; lang++) {
        hp_xml_node *ff;
        char *body = finish(&add_fonts[lang]);
        memset(&add_fonts[lang], 0, sizeof(add_fonts[lang]));
        if (!body) {
            st = HP_ERR_NOMEM;
            break;
        }
        if (!*body) {
            free(body);
            continue;
        }
        for (ff = hp_xml_find(root, "fontface"); ff; ff = hp_xml_find_after(root, ff, "fontface")) {
            const char *l = hp_xml_attr(ff, "lang");
            if (l && strcmp(l, LANGS_UP[lang]) == 0) {
                break;
            }
        }
        if (!ff || ff->inner_start == ff->end) {
            free(body);
            st = HP_ERR_UNSUPPORTED; /* no place for the font in this language */
            break;
        }
        if (!add_patch(&ps, &n, ff->inner_end, ff->inner_end, body)) {
            st = HP_ERR_NOMEM;
            break;
        }
        if (hp_xml_attr(ff, "fontCnt")) {
            size_t tb = ff->inner_start, te;
            char *tag = (char *)malloc(tb - ff->start + 1), num[16];
            if (!tag) {
                st = HP_ERR_NOMEM;
                break;
            }
            memcpy(tag, src + ff->start, tb - ff->start);
            tag[tb - ff->start] = '\0';
            te = strlen(tag);
            snprintf(num, sizeof(num), "%u", next_font[lang]);
            if (!set_attr(&tag, 0, &te, "fontCnt", num) || !add_patch(&ps, &n, ff->start, tb, tag)) {
                st = HP_ERR_NOMEM;
            }
        }
    }
    if (st == HP_OK) {
        qsort(ps, n, sizeof(*ps), patch_cmp);
        for (i = 0; i < n; i++) {
            if (ps[i].a < pos) {
                st = HP_ERR_UNSUPPORTED;
                break;
            }
            putn(&b, src + pos, ps[i].a - pos);
            puts_b(&b, ps[i].text);
            pos = ps[i].b;
        }
        if (st == HP_OK) {
            putn(&b, src + pos, part->len - pos);
            *out = finish(&b);
            if (!*out) {
                st = HP_ERR_NOMEM;
            }
        } else {
            free(b.s);
        }
    }
    for (i = 0; i < n; i++) {
        free(ps[i].text);
    }
    free(ps);
    free(add_chars.s);
    free(add_paras.s);
    for (lang = 0; lang < 7; lang++) {
        free(add_fonts[lang].s);
    }
    free(faces);
    return st;
}

hp_status hp_editor_save(hp_editor *e,const char *path){
    hp_package pkg;hp_document doc;size_t si,i,j;char **owned;hp_status st=HP_OK;shape_reg reg;
    if(!e||!path)return HP_ERR_INVALID;
    memset(&reg,0,sizeof(reg));
    if(e->header){
        reg.next_char=count_children(hp_xml_root(e->header),"charProperties","charPr",1);
        reg.next_para=count_children(hp_xml_root(e->header),"paraProperties","paraPr",1);
    }
    pkg=*e->doc->package;doc=*e->doc;doc.package=&pkg;
    pkg.entries=malloc(pkg.entry_count*sizeof(*pkg.entries));owned=calloc(pkg.entry_count,sizeof(*owned));
    if(!pkg.entries||!owned){free(pkg.entries);free(owned);return HP_ERR_NOMEM;}memcpy(pkg.entries,e->doc->package->entries,pkg.entry_count*sizeof(*pkg.entries));
    for(si=0;si<pkg.section_part_count&&st==HP_OK;si++){
        patch *ps=NULL;size_t n=0,pos=0;buffer b={0};hp_package_entry *part=NULL;char *text=NULL;
        for(i=0;i<pkg.entry_count;i++)if(strcmp(pkg.entries[i].name,pkg.section_parts[si])==0){part=&pkg.entries[i];break;}
        if(!part){st=HP_ERR_INVALID;break;}
        for(j=0;j<e->count;j++)if(e->items[j].section==si&&!edits(&e->items[j],&ps,&n)){st=HP_ERR_NOMEM;break;}
        if(st==HP_OK){
            if(n){qsort(ps,n,sizeof(*ps),patch_cmp);
                for(j=0;j<n;j++){if(ps[j].a<pos){st=HP_ERR_UNSUPPORTED;break;}putn(&b,(const char *)part->data+pos,ps[j].a-pos);puts_b(&b,ps[j].text);pos=ps[j].b;}
                if(st==HP_OK){putn(&b,(const char *)part->data+pos,part->len-pos);text=finish(&b);if(!text)st=HP_ERR_NOMEM;}
                else free(b.s);
            }else{text=malloc(part->len+1);if(text){memcpy(text,part->data,part->len);text[part->len]=0;}else st=HP_ERR_NOMEM;}
        }
        if(st==HP_OK)st=format_section(e,si,&text,&reg);
        if(st==HP_OK&&(n||strlen(text)!=part->len||memcmp(text,part->data,part->len))){owned[i]=text;part->data=(unsigned char *)text;part->len=strlen(text);text=NULL;}
        free(text);
        for(j=0;j<n;j++){free(ps[j].text);}free(ps);
    }
    if(st==HP_OK&&reg.n){
        /* new shapes go into header.xml */
        char *header=NULL;size_t hi;hp_package_entry *hpart=NULL;
        for(hi=0;hi<pkg.entry_count;hi++)if(e->header_name&&strcmp(pkg.entries[hi].name,e->header_name)==0){hpart=&pkg.entries[hi];break;}
        if(!hpart)st=HP_ERR_UNSUPPORTED;
        else{st=build_header(e,&reg,hpart,&header);if(st==HP_OK){owned[hi]=header;hpart->data=(unsigned char *)header;hpart->len=strlen(header);}}
    }
    free(reg.v);
    if(st==HP_OK)st=hp_hwpx_write(&doc,path);
    for(i=0;i<pkg.entry_count;i++){free(owned[i]);}free(owned);free(pkg.entries);return st;
}
