/* Integration tests exercise the GUI's own controls and commands in a hidden test window. */
#define wWinMain hp_unused_gui_entry
#include "../src/gui_win.c"
#undef wWinMain
#define CHECK(x) do{if(!(x)){fprintf(stderr,"GUI edit failure %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){
    WNDCLASSW wc={0};hp_editor *saved=NULL;wchar_t dir[MAX_PATH],path[MAX_PATH];char *utf8;
    wc.lpfnWndProc=WndProc;wc.hInstance=GetModuleHandleW(NULL);wc.lpszClassName=L"HangulPadEditTest";CHECK(RegisterClassW(&wc));
    g_main=CreateWindowW(wc.lpszClassName,L"Test",WS_OVERLAPPEDWINDOW,0,0,920,800,NULL,NULL,wc.hInstance,NULL);CHECK(g_main);
    editor_new();CHECK(g_editor&&g_editing&&hp_editor_count(g_editor)==1);
    SendMessageW(g_text,EM_REPLACESEL,TRUE,(LPARAM)L"첫 본문 😀");CHECK(g_dirty);
    SendMessageW(g_main,WM_COMMAND,IDM_ADD_PARA,0);CHECK(hp_editor_count(g_editor)==2);CHECK(g_edit_index==1);
    CHECK(!strcmp(hp_editor_text(g_editor,0),"첫 본문 😀"));
    SendMessageW(g_text,EM_REPLACESEL,TRUE,(LPARAM)L"두 번째 본문\r\n다음 줄");CHECK(editor_commit());
    CHECK(!strcmp(hp_editor_text(g_editor,1),"두 번째 본문\n다음 줄"));
    CHECK(GetTempPathW(MAX_PATH,dir));CHECK(GetTempFileNameW(dir,L"hpt",0,path));wcscpy(g_path,path);
    CHECK(editor_save(0));CHECK(!g_dirty);utf8=wide_to_utf8(path);CHECK(utf8);
    CHECK(hp_editor_open(utf8,&saved)==HP_OK);CHECK(hp_editor_count(saved)==2);
    CHECK(!strcmp(hp_editor_text(saved,1),"두 번째 본문\n다음 줄"));hp_editor_free(saved);saved=NULL;
    SendMessageW(g_text,EM_SETSEL,0,-1);SendMessageW(g_text,EM_REPLACESEL,TRUE,(LPARAM)L"수정 후 미리보기");
    editor_preview();CHECK(!g_editing&&g_doc&&g_dirty);CHECK(!strcmp(hp_editor_text(g_editor,1),"수정 후 미리보기"));
    editor_mode(1);CHECK(editor_save(0));CHECK(hp_editor_open(utf8,&saved)==HP_OK);
    CHECK(!strcmp(hp_editor_text(saved,1),"수정 후 미리보기"));hp_editor_free(saved);
    /* 0.10.0: formatting commands, find, zoom */
    {   hp_char_format cf;hp_para_format pf;
        list_select_item(0);SendMessageW(g_main,WM_COMMAND,IDM_BOLD,0);SendMessageW(g_main,WM_COMMAND,IDM_ALIGN_CENTER,0);
        SendMessageW(g_main,WM_COMMAND,IDM_SIZE_UP,0);SendMessageW(g_main,WM_COMMAND,IDM_COLOR_BASE+1,0);
        CHECK(hp_editor_get_format(g_editor,0,&cf,&pf)==HP_OK);CHECK(cf.bold&&pf.align==HP_ALIGN_CENTER&&cf.size_hwp==1100&&cf.color_rgb==0xFF0000);
        CHECK(SendMessageW(g_tb_fmt,TB_ISBUTTONCHECKED,IDM_BOLD,0));
        wcscpy(g_find_what,L"미리보기");CHECK(find_next(1,0,1));CHECK(g_edit_index==1);
        {DWORD a=0,b=0;SendMessageW(g_text,EM_GETSEL,(WPARAM)&a,(LPARAM)&b);CHECK(a==5&&b==9);}
        wcscpy(g_replace_with,L"확인");CHECK(replace_all(1)==1);CHECK(!strcmp(hp_editor_text(g_editor,1),"수정 후 확인"));
        set_zoom(ZOOM_PERCENT,150);CHECK(view_zoom_percent()==150);set_zoom(ZOOM_FIT_PAGE,100);CHECK(g_zoom_mode==ZOOM_FIT_PAGE);
        CHECK(editor_save(0));CHECK(hp_editor_open(utf8,&saved)==HP_OK);
        CHECK(hp_editor_get_format(saved,0,&cf,&pf)==HP_OK);CHECK(cf.bold&&pf.align==HP_ALIGN_CENTER&&cf.size_hwp==1100&&cf.color_rgb==0xFF0000);
        CHECK(!strcmp(hp_editor_text(saved,1),"수정 후 확인"));hp_editor_free(saved);saved=NULL;
    }
    list_select_item(1);
    editor_remove();CHECK(g_dirty);CHECK(editor_save(0));CHECK(hp_editor_open(utf8,&saved)==HP_OK);CHECK(hp_editor_count(saved)==1);hp_editor_free(saved);
    free(utf8);DeleteFileW(path);DestroyWindow(g_main);puts("Native GUI new/edit/insert/preview/format/find/replace/zoom/save/reedit/delete passed");return 0;
}


