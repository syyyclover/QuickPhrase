"""Execute real import/directory-switch handlers with only the Win32 UI doubled.

Models, JSON files, table cache and directory state are real. Save failures are
injected at the persistence boundary; no user profile or Windows GUI is needed.
"""
from source_helpers import read_source, INCLUDE_FLAGS, ROOT
import re
import subprocess
import tempfile
import sys
from pathlib import Path

def function(source, name):
    m=re.search(r'^(?:static )?(?:void|int) '+name+r'\([^;]+?\)\n\{',source,re.M)
    assert m,name
    return source[m.start():source.index('\n}',m.end())+2]

source=read_source('ui.c')
importer=function(source,'ui_import')
settings=(ROOT/'src/ui/dialog_settings.c').read_text()
switcher=function(settings,'change_data_dir')
ctx=settings[settings.index('typedef struct {'):settings.index('} SetCtx;')+len('} SetCtx;')]
pre=r'''
#include "engine.h"
#include "storage.h"
#include "table.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <sys/stat.h>
typedef void *HWND;
typedef void *PIDLIST_ABSOLUTE;
typedef unsigned DWORD;
typedef struct { size_t lStructSize; HWND hwndOwner; const wchar_t *lpstrFilter; wchar_t *lpstrFile; unsigned nMaxFile; const wchar_t *lpstrTitle,*lpstrDefExt; unsigned Flags; } OPENFILENAMEW;
typedef struct { HWND hwndOwner; unsigned ulFlags; const wchar_t *lpszTitle; } BROWSEINFOW;
#define BIF_RETURNONLYFSDIRS 1
#define BIF_NEWDIALOGSTYLE 2
#define MAX_PATH 260
#define OFN_FILEMUSTEXIST 1
#define OFN_PATHMUSTEXIST 2
#define OFN_EXPLORER 4
#define MB_YESNOCANCEL 1
#define MB_YESNO 2
#define MB_ICONQUESTION 4
#define MB_ICONWARNING 8
#define MB_OK 16
#define MB_ICONERROR 32
#define MB_ICONINFORMATION 64
#define IDYES 6
#define IDCANCEL 2
#define QP_APP_TITLE L"test"
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
static AppData *g_app;
static Settings g_settings;
static HWND g_hwndMain;
static int fail_old,fail_settings,clears;
static int qp_msgbox(HWND h,unsigned type,const wchar_t *title,const wchar_t *fmt,...) { (void)h;(void)type;(void)title;(void)fmt; return IDYES; }
static int GetOpenFileNameW(OPENFILENAMEW *o) { wcscpy(o->lpstrFile,L"import.json");return 1; }
static PIDLIST_ABSOLUTE SHBrowseForFolderW(BROWSEINFOW *b) { (void)b;return (void*)1; }
static int SHGetPathFromIDListW(PIDLIST_ABSOLUTE p,wchar_t *out) { (void)p;wcscpy(out,L"./next");return 1; }
static void CoTaskMemFree(void *p) { (void)p; }
static DWORD GetFileAttributesW(const wchar_t *p) { char b[1024];struct stat st;wcstombs(b,p,1024);return stat(b,&st)?INVALID_FILE_ATTRIBUTES:0; }
static int fail_snapshot;
static int ui_select_template(Template *t) { (void)t;return !fail_snapshot; }
static void ui_rebuild_tree(const wchar_t *id) { (void)id; }
static int ui_reset_selection(void) { if(fail_snapshot)return 0;++clears;return 1; }
static int sess_capture_current(void) { return !fail_snapshot; }
static int app_save_data(void) { return 1; }
static int SetWindowTextW(HWND h,const wchar_t *s) { (void)h;(void)s;return 1; }
static int save_data(const AppData *d) {
    wchar_t path[QP_PATH_MAX];storage_get_data_dir(path,QP_PATH_MAX);
    if(fail_old && !wcscmp(path,L"./old")) return 0;
    return storage_save(d);
}
static int save_settings(const Settings *s) { (void)s;return !fail_settings; }
#define storage_save save_data
#define settings_save save_settings
'''
driver=r'''
#undef storage_save
#undef settings_save
static void switching(int old_failure,int settings_failure,int snapshot_failure) {
    AppData *destination=appdata_create_seed(),*original;
    SetCtx c={0};wchar_t dir[QP_PATH_MAX];
    mkdir("old",0700);mkdir("next",0700);
    settings_defaults(&g_settings);wcscpy(g_settings.data_dir,L"./old");
    g_app=appdata_create_seed();original=g_app;qp_table_set_refs(g_app->tables);
    storage_set_data_dir(L"./old");assert(storage_save(g_app));
    wcscpy(g_app->templates->name,L"unsaved-edit");
    wcscpy(destination->templates->name,L"destination");
    storage_set_data_dir(L"./next");assert(storage_save(destination));appdata_free(destination);
    storage_set_data_dir(L"./old");fail_old=old_failure;fail_settings=settings_failure;fail_snapshot=snapshot_failure;clears=0;
    change_data_dir(NULL,&c);storage_get_data_dir(dir,QP_PATH_MAX);
    if(old_failure || settings_failure || snapshot_failure) {
        assert(g_app==original && !wcscmp(g_app->templates->name,L"unsaved-edit"));
        assert(!wcscmp(dir,L"./old") && !wcscmp(g_settings.data_dir,L"./old") && clears==0);
    } else {
        AppData *saved;
        assert(!wcscmp(g_app->templates->name,L"destination") && clears==1);
        storage_set_data_dir(L"./old");saved=storage_load(NULL);
        assert(saved && !wcscmp(saved->templates->name,L"unsaved-edit"));appdata_free(saved);
    }
    appdata_free(g_app);g_app=NULL;qp_table_invalidate_all();
}
static void importing(void) {
    QpTable *table;wchar_t err[256];
    g_app=appdata_create();qp_table_set_refs(NULL);
    assert(storage_write_text(L"table.csv","key,value\nA,old\n"));
    assert(storage_write_text(L"import.json","{\"dataTables\":[{\"id\":\"import-table\",\"name\":\"table\",\"path\":\"table.csv\",\"encoding\":\"utf8\",\"delimiter\":\"comma\",\"hasHeader\":true}]}"));
    ui_import();table=qp_table_get(L"import-table",err,256);
    assert(table && qp_table_cols(table)==2);
    assert(storage_write_text(L"import.json","{\"dataTables\":[{\"id\":\"import-table\",\"name\":\"table\",\"path\":\"table.csv\",\"encoding\":\"utf8\",\"delimiter\":\"tab\",\"hasHeader\":false}]}"));
    ui_import();table=qp_table_get(L"import-table",err,256);
    assert(table && qp_table_cols(table)==1 && qp_table_rows(table)==2);
    appdata_free(g_app);g_app=NULL;qp_table_invalidate_all();
}
int main(int argc,char **argv) {
    if(argc>1 && !strcmp(argv[1],"import")) importing();
    else { switching(1,0,0);switching(0,1,0);switching(0,0,1);switching(0,0,0); }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='qp-data-ui-') as tmp:
    exe=Path(tmp)/'test'
    sources=[str(p) for p in (ROOT/'src/core').glob('*.c')]+[str(ROOT/'third_party/cjson/cJSON.c')]
    subprocess.run(['cc','-std=gnu11','-Wall',*INCLUDE_FLAGS,'-x','c','-',*sources,'-o',str(exe)],
                   input=pre+ctx+switcher+importer+driver,text=True,check=True)
    for case in (sys.argv[1:] or ('import','switch')):
        subprocess.run([str(exe),case],cwd=tmp,check=True)
print('data UI: import publishes refs/cache; directory failures retain unsaved model')
