#include "../src/table_pagination.h"
#include <stdio.h>
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"table pagination failure %d: %s\n",__LINE__,#x);return 1;} } while(0)
int main(void) {
    hp_cut_map map={0};
    hp_table table={0};hp_table_cell cells[4]={{0}};
    hp_cut_add(&map,25,45);hp_cut_add(&map,10,30);hp_cut_add(&map,45,65);hp_cut_add(&map,70,90);
    hp_cut_normalize(&map);CHECK(map.count==3);
    CHECK(hp_cut_before(&map,0,40)==10);
    CHECK(hp_cut_before(&map,0,45)==45);
    CHECK(hp_cut_before(&map,45,85)==70);
    CHECK(hp_cut_before(&map,15,20)==15);
    free(map.bands);
    table.cells=cells;table.cell_count=4;table.row_count=5;table.repeat_header=1;
    cells[0].is_header=1;cells[0].rowspan=2;
    cells[1].is_header=1;cells[1].row=2;cells[1].rowspan=1;
    cells[2].row=3;cells[2].rowspan=1;cells[3].row=4;cells[3].rowspan=1;
    CHECK(hp_table_header_rows(&table)==3);
    table.repeat_header=0;CHECK(hp_table_header_rows(&table)==0);
    table.repeat_header=1;cells[0].is_header=0;CHECK(hp_table_header_rows(&table)==0);
    cells[0].is_header=1;cells[2].row=2;cells[2].rowspan=3;
    CHECK(hp_table_header_rows(&table)==0); /* Body merge crosses the title band. */
    cells[2].row=3;cells[2].rowspan=1;table.row_count=3;
    CHECK(hp_table_header_rows(&table)==0); /* Never repeat the entire table. */
    puts("table header and safe-cut tests passed");return 0;
}
