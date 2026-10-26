#include "../src/pagination.h"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"page flow failure %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main(void) {
    hp_flow f={0,0,0,2,100};
    hp_slice s;
    int rows[]={60,120,180}, offset=0, total=0;
    hp_flow_break(&f,1); CHECK(f.page==0); /* no leading blank page */
    s=hp_flow_take(&f,0,30,NULL,0); CHECK(s.height==30);
    hp_flow_break(&f,0); CHECK(f.page==0 && f.column==1 && f.y==0);
    hp_flow_break(&f,1); CHECK(f.page==1 && f.column==0);
    while(offset<180) {
        s=hp_flow_take(&f,offset,180,rows,3);
        CHECK(s.height==60 && s.y==0);
        CHECK(s.offset==offset);
        offset+=s.height; total+=s.height;
    }
    CHECK(total==180 && f.page==2 && f.column==0);
    f=(hp_flow){0,0,0,1,100};offset=total=0;
    while(offset<275) {
        s=hp_flow_take(&f,offset,275,NULL,0);
        CHECK(s.height>0 && s.height<=100 && s.offset==offset);
        offset+=s.height; total+=s.height;
    }
    CHECK(total==275 && f.page==2 && f.y==75);
    /* Move a short row to the next column rather than cut it at the bottom. */
    f=(hp_flow){0,0,80,1,100};
    s=hp_flow_take(&f,0,60,rows,1);
    CHECK(s.page==1 && s.y==0 && s.height==60);
    puts("page flow tests passed");return 0;
}
