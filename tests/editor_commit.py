"""Actual edit action preflights snapshots before accepting edits, not afterwards."""
from pathlib import Path
import runpy, subprocess, tempfile
from source_helpers import ROOT
base=runpy.run_path(str(Path(__file__).with_name('session_transactions.py')))
tree=(ROOT/'src/ui/main_tree.c').read_text()
fn=base['function']
pre=r'''
#define MB_ICONINFORMATION 4
#define QP_APP_TITLE L"Test"
static AppData *g_app;
static int edited,replaced;
static Template *tree_selected_template(void){return original_template;}
static int dlg_edit_template(HWND h,Template *t,int n){
    (void)h;(void)n;++edited;wcscpy(t->name,L"Accepted");
    fa_inject(1); /* Every subsequent allocation fails: accepted edits must survive. */
    return 1;
}
static int app_save_data(void){return 1;}
static void ui_rebuild_tree(const wchar_t *id){(void)id;++replaced;}
'''
driver=r'''
int main(void){
    original_template=tmpl_create(L"Original");seg_list_append(&original_template->segments,seg_create(SEG_TEXT));
    g_app=appdata_create();g_app->templates=original_template;
    original_session=calloc(1,sizeof(*original_session));g_curSess=original_session;
    g_ctx=eval_create(original_template);g_curTmpl=original_template;eval_set_user_value(g_ctx,0,L"LATEST");
    ui_edit_current();fa_disable();assert(!fa_calls);
    assert(edited==1 && replaced==1 && !wcscmp(g_app->templates->name,L"Accepted"));
    assert(original_session->count==1 && !wcscmp(original_session->vals[0],L"LATEST"));
    sess_drop_values(original_session);free(original_session);eval_destroy(g_ctx);appdata_free(g_app);assert(!fa_live);
    puts("editor: accepted candidate survives later allocation refusal after snapshot preflight");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='qp-editor-commit-') as tmp:
    exe=Path(tmp)/'test'
    code=base['preamble']+base['ctx']+base['state']+pre+fn(base['sessions'],'sess_drop_values')+base['snapshot']+fn(base['fields'],'ui_select_template')+fn(tree,'ui_edit_current')+driver
    subprocess.run(['cc','-std=gnu11','-Wall','-Wno-unused-function',*base['INCLUDE_FLAGS'],'-I',str(ROOT/'tests'),
        '-x','c','-',*base['CORE_SOURCES'],str(ROOT/'src/core/table.c'),str(ROOT/'src/core/storage.c'),
        str(ROOT/'src/core/regex.c'),str(ROOT/'third_party/cjson/cJSON.c'),
        '-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free','-o',str(exe)],input=code,text=True,check=True)
    subprocess.run([str(exe)],check=True)
