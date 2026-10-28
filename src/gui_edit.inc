/* Native Unicode text editing with IME, selection, clipboard and undo. */
static void layout_children(int cx,int cy);
static void editor_title(void) {
    wchar_t title[MAX_PATH+80];const wchar_t *name=wcsrchr(g_path,L'\\');
    swprintf(title,MAX_PATH+80,L"%ls%ls — 한글패드 " HP_VERSION_STRING,g_dirty?L"* ":L"",g_path[0]?(name?name+1:g_path):L"새 문서");
    SetWindowTextW(g_main,title);
}
static int editor_commit(void) {
    int n;wchar_t *w;char *s;hp_status st;
    if(!g_editor||g_edit_index==(size_t)-1||!SendMessageW(g_text,EM_GETMODIFY,0,0))return 1;
    n=GetWindowTextLengthW(g_text);w=malloc(((size_t)n+1)*sizeof(*w));if(!w)return 0;
    GetWindowTextW(g_text,w,n+1);s=wide_to_utf8(w);free(w);if(!s)return 0;
    /* Windows edit controls use CRLF; the engine stores a single LF. */
    {char *a=s,*b=s;for(;*a;a++)if(*a!='\r')*b++=*a;*b=0;}
    st=hp_editor_set(g_editor,g_edit_index,s);free(s);
    if(st!=HP_OK){MessageBoxW(g_main,L"편집 내용을 반영하지 못했습니다.",L"한글패드",MB_ICONERROR);return 0;}
    SendMessageW(g_text,EM_SETMODIFY,FALSE,0);return 1;
}
static void editor_select(size_t i) {
    const char *s;char *crlf,*b;const char *a;wchar_t *w;
    if(!g_editor||i>=hp_editor_count(g_editor))return;
    s=hp_editor_text(g_editor,i);crlf=malloc(strlen(s)*2+1);if(!crlf)return;
    b=crlf;for(a=s;*a;a++){if(*a=='\n')*b++='\r';*b++=*a;}*b=0;
    w=utf8_to_wide(crlf);free(crlf);if(!w)return;
    g_loading=1;SetWindowTextW(g_text,w);free(w);g_loading=0;
    SendMessageW(g_text,EM_SETMODIFY,FALSE,0);g_edit_index=i;
}
static void editor_list(size_t selected) {
    size_t i;int row=0,sel=0;wchar_t label[160];
    SendMessageW(g_paralist,LB_RESETCONTENT,0,0);
    for(i=0;g_editor&&i<hp_editor_count(g_editor);i++) {
        wchar_t *w;const char *s=hp_editor_text(g_editor,i);int index;
        if(hp_editor_is_deleted(g_editor,i))continue;
        w=utf8_to_wide(s);if(!w)continue;
        {size_t j;for(j=0;w[j];j++)if(w[j]=='\r'||w[j]=='\n'||w[j]=='\t')w[j]=' ';}
        swprintf(label,160,L"%zu구역 · %zu  %.100ls",hp_editor_section(g_editor,i)+1,i+1,w[0]?w:L"(빈 문단)");free(w);
        index=(int)SendMessageW(g_paralist,LB_ADDSTRING,0,(LPARAM)label);
        SendMessageW(g_paralist,LB_SETITEMDATA,index,(LPARAM)i);if(i==selected)sel=row;row++;
    }
    SendMessageW(g_paralist,LB_SETCURSEL,sel,0);
    if(row)editor_select((size_t)SendMessageW(g_paralist,LB_GETITEMDATA,sel,0));
}
static void editor_mode(int editing) {
    RECT r;g_editing=editing;
    ShowWindow(g_paralist,editing?SW_SHOW:SW_HIDE);ShowWindow(g_text,editing?SW_SHOW:SW_HIDE);
    GetClientRect(g_main,&r);layout_children(r.right,r.bottom);InvalidateRect(g_main,NULL,TRUE);
    CheckMenuItem(GetMenu(g_main),IDM_EDIT_MODE,MF_BYCOMMAND|(editing?MF_CHECKED:MF_UNCHECKED));
    if(editing){SetWindowTextW(g_status,L"Enter: 줄바꿈 · Ctrl+Enter: 문단 추가");SetFocus(g_text);}else update_status();
}
static int editor_save(int save_as) {
    wchar_t path[MAX_PATH];char *utf8;hp_status st;OPENFILENAMEW ofn;
    if(!g_editor||!editor_commit())return 0;
    wcscpy(path,g_path);
    if(g_source_hwp&&path[0]) {
        /* Suggest the same name with .hwpx; the original .hwp is kept as it is. */
        wchar_t *dot=wcsrchr(path,L'.'),*slash=wcsrchr(path,L'\\');
        if(dot&&(!slash||dot>slash)&&(size_t)(dot-path)+6<MAX_PATH)wcscpy(dot,L".hwpx");
        if(!save_as)MessageBoxW(g_main,L"HWP 문서는 HWPX 형식으로 변환하여 새 파일로 저장합니다. 원본 HWP 파일은 바뀌지 않습니다.",L"한글패드",MB_ICONINFORMATION);
    }
    if(save_as||!path[0]||g_source_hwp) {
        ZeroMemory(&ofn,sizeof(ofn));ofn.lStructSize=sizeof(ofn);ofn.hwndOwner=g_main;
        ofn.lpstrFile=path;ofn.nMaxFile=MAX_PATH;ofn.lpstrFilter=L"HWPX 문서 (*.hwpx)\0*.hwpx\0";
        ofn.lpstrDefExt=L"hwpx";ofn.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
        if(!GetSaveFileNameW(&ofn))return 0;
    }
    utf8=wide_to_utf8(path);if(!utf8)return 0;st=hp_editor_save(g_editor,utf8);free(utf8);
    if(st!=HP_OK){MessageBoxW(g_main,L"저장하지 못했습니다. 파일 경로와 쓰기 권한을 확인하세요. 편집 내용은 유지됩니다.",L"한글패드",MB_ICONERROR);return 0;}
    wcscpy(g_path,path);g_dirty=0;g_source_hwp=0;editor_title();return 1;
}
static int editor_discard(void) {
    int answer;if(!g_dirty)return 1;
    answer=MessageBoxW(g_main,L"변경한 내용을 저장하시겠습니까?",L"한글패드",MB_YESNOCANCEL|MB_ICONQUESTION);
    return answer==IDYES?editor_save(0):answer==IDNO;
}
static void editor_preview(void) {
    wchar_t dir[MAX_PATH],path[MAX_PATH];char *s;hp_document *doc=NULL;
    if(!g_editor||!editor_commit())return;
    if(!GetTempPathW(MAX_PATH,dir)||!GetTempFileNameW(dir,L"hpe",0,path))return;
    s=wide_to_utf8(path);
    if(s&&hp_editor_save(g_editor,s)==HP_OK&&hp_hwpx_read(s,&doc)==HP_OK) {
        free_doc();g_doc=doc;rebuild_flat();rebuild_pages(g_main);update_status();editor_mode(0);
    }else MessageBoxW(g_main,L"미리보기를 만들지 못했습니다.",L"한글패드",MB_ICONERROR);
    free(s);DeleteFileW(path);
}
static void editor_new(void) {
    wchar_t dir[MAX_PATH],path[MAX_PATH];char *s;hp_document *doc;
    if(!editor_discard())return;
    if(!GetTempPathW(MAX_PATH,dir)||!GetTempFileNameW(dir,L"hpn",0,path))return;
    s=wide_to_utf8(path);doc=hp_document_from_text("");
    if(s&&doc&&hp_hwpx_write(doc,s)==HP_OK){
        /* open_document checks pending changes; they were handled above. */
        g_dirty=0;open_document(g_main,path);g_path[0]=0;g_source_hwp=0;g_dirty=1;editor_title();editor_mode(1);
    }
    hp_document_free(doc);free(s);DeleteFileW(path);
}
static void editor_add(void) {
    hp_status st;size_t added;if(!editor_commit()||g_edit_index==(size_t)-1)return;
    st=hp_editor_insert(g_editor,g_edit_index,&added);
    if(st==HP_OK){g_dirty=1;editor_title();editor_list(added);SetFocus(g_text);}
    else MessageBoxW(g_main,L"이 위치에 문단을 추가하지 못했습니다.",L"한글패드",MB_ICONERROR);
}
static void editor_remove(void) {
    size_t i=g_edit_index;if(!editor_commit()||i==(size_t)-1)return;
    if(hp_editor_delete(g_editor,i)!=HP_OK){MessageBoxW(g_main,L"구역·표·그림 등의 개체가 있는 문단과 영역의 마지막 문단은 삭제할 수 없습니다. 문단의 텍스트는 편집할 수 있습니다.",L"한글패드",MB_ICONINFORMATION);return;}
    g_dirty=1;editor_title();editor_list(0);
}


