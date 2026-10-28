"""Real session snapshots/selection with only UI calls doubled; sweep OOM."""
from pathlib import Path
import re
import subprocess
import tempfile
from source_helpers import INCLUDE_FLAGS, CORE_SOURCES

root=Path(__file__).resolve().parents[1]
sessions=(root/'src/ui/main_sessions.c').read_text()
fields=(root/'src/ui/main_fields.c').read_text()
internal=(root/'src/ui/ui_internal.h').read_text()
end=internal.index('} Session;')+len('} Session;')
ctx=internal[internal.rfind('typedef struct {',0,end):end]
def function(source,name):
    m=re.search(r'^(?:static )?(?:void|int) '+name+r'\([^;]+?\)\n\{',source,re.M)
    assert m,name
    return source[m.start():source.index('\n}',m.end())+2]
start=sessions.index('/* ---------------------- 取值的保存与恢复')
end=sessions.index('/* ------------------------- 新建 / 挑选',start)
snapshot=sessions[start:end]
preamble=r'''
#include "engine.h"
#include "test_fail_alloc.h"
#include <stdio.h>
#include <string.h>
typedef void *HWND;
#define TRUE 1
#define FALSE 0
#define MB_OK 1
#define MB_ICONERROR 2
#define _snwprintf swprintf
'''
state=r'''
static EvalContext *g_ctx;
static Session *g_curSess,*original_session,*target_session;
static Template *g_curTmpl,*original_template,*target_template;
static unsigned g_sessTick;
static OutputMode g_previewMode;
static HWND g_hwndMain,g_hPreview,g_hTitle,g_hHint,g_hBtnCopy,g_hBtnReset,g_hBtnEdit;
static int clears,builds;
static void ui_clear_fields(void) {++clears;}
static void ui_sync_preview_mode_controls(void) {}
static void qp_preview_clear(HWND h) {(void)h;}
static void qp_set_text_if_diff(HWND h,const wchar_t *s) {(void)h;(void)s;}
static void SetWindowTextW(HWND h,const wchar_t *s) {(void)h;(void)s;}
static void EnableWindow(HWND h,int e) {(void)h;(void)e;}
static void tabs_refresh(void) {}
static void ui_layout(HWND h) {(void)h;}
static void ui_build_fields(void) {++builds;}
static void ui_refresh_current(void) {}
static Session *sess_pick_for(const wchar_t *id) {
    return !wcscmp(id,target_template->id)?target_session:original_session;
}
static void sess_discard_pending(Session *s) {(void)s;}
static int qp_msgbox(HWND h,unsigned f,const wchar_t *t,const wchar_t *s,...) {
    (void)h;(void)f;(void)t;(void)s;return 0;
}
/* 真实 multi_sep 还要看全局默认分隔符（UI 设置）；探针只测会话快照与 OOM 回滚，
   按片段自带的 separator 打桩即可 —— 片段没设分隔符时与真实路径同为顿号。 */
static const wchar_t *multi_sep(const Segment *s) {
    return s && s->separator[0] ? s->separator : L"、";
}
'''
driver=r'''
static Session *new_session(Template *t,const wchar_t *saved) {
    Session *s=calloc(1,sizeof(*s));wcscpy(s->tmpl_id,t->id);s->used=1;s->count=2;
    s->ids=calloc(2,sizeof(*s->ids));s->vals=calloc(2,sizeof(*s->vals));
    for(int i=0;i<2;++i) wcscpy(s->ids[i],seg_list_at(t->segments,i)->id);
    s->vals[0]=qp_wcsdup(saved);return s;
}
static void prepare(void) {
    original_template=tmpl_create(L"Original");target_template=tmpl_create(L"Target");
    for(int i=0;i<2;++i) {
        seg_list_append(&original_template->segments,seg_create(SEG_TEXT));
        seg_list_append(&target_template->segments,seg_create(SEG_TEXT));
    }
    original_session=new_session(original_template,L"PREVIOUS");
    target_session=new_session(target_template,L"TARGET_VALUE");
    g_ctx=eval_create(original_template);assert(g_ctx);
    eval_set_user_value(g_ctx,0,L"LATEST");eval_set_user_value(g_ctx,1,L"");eval_run(g_ctx);
    g_curSess=original_session;g_curTmpl=original_template;clears=builds=0;
}
static void cleanup(void) {
    eval_destroy(g_ctx);g_ctx=NULL;
    sess_drop_values(original_session);free(original_session);
    sess_drop_values(target_session);free(target_session);
    tmpl_free(original_template);tmpl_free(target_template);assert(!fa_live);
}
static void action(const char *mode) {
    if(!strcmp(mode,"save")) sess_save(original_session);
    else if(!strcmp(mode,"deactivate")) sess_deactivate();
    else if(!strcmp(mode,"activate")) sess_activate(target_session,target_template);
    else ui_select_template(target_template);
}
int main(int argc,char **argv) {
    assert(argc==2);const char *mode=argv[1];prepare();fa_inject(0);action(mode);
    size_t count=fa_calls;cleanup();
    for(size_t i=1;i<=count;++i) {
        prepare();EvalContext *live=g_ctx;wchar_t *old=original_session->vals[0];size_t base=fa_live;
        fa_inject(i);action(mode);fa_disable();assert(fa_hit);
        assert(g_ctx==live && g_curSess==original_session);
        assert(g_curTmpl==original_template && clears==0 && builds==0);
        assert(original_session->vals[0]==old && !wcscmp(old,L"PREVIOUS"));
        assert(original_session->vals[1]==NULL && !wcscmp(g_ctx->fields[0].user_value,L"LATEST"));
        assert(g_ctx->fields[1].user_touched && !g_ctx->fields[1].user_value[0]);
        assert(fa_live==base);cleanup();
    }
    prepare();action(mode);
    if(!strcmp(mode,"save")) {
        assert(!wcscmp(original_session->vals[0],L"LATEST"));
        assert(original_session->vals[1] && !original_session->vals[1][0]);
    } else if(!strcmp(mode,"deactivate")) assert(!g_ctx && !g_curSess);
    else {
        assert(g_curSess==target_session && g_ctx->tmpl==target_template);
        assert(!wcscmp(g_ctx->fields[0].user_value,L"TARGET_VALUE"));
        assert(!g_ctx->fields[1].user_touched);
        assert(!wcscmp(original_session->vals[0],L"LATEST"));
        assert(original_session->vals[1] && !original_session->vals[1][0]);
    }
    cleanup();printf("session %s: %zu allocation failures preserve live input/state\n",mode,count);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='qp-session-txn-') as tmp:
    exe=Path(tmp)/'probe'
    code=preamble+ctx+state+function(sessions,'sess_drop_values')+snapshot+function(fields,'ui_select_template')+driver
    subprocess.run(['cc','-std=gnu11','-Wall','-Wno-unused-function',*INCLUDE_FLAGS,'-I',str(root/'tests'),
                    '-x','c','-',*CORE_SOURCES,str(root/'src/core/table.c'),str(root/'src/core/storage.c'),
                    str(root/'src/core/regex.c'),str(root/'third_party/cjson/cJSON.c'),
                    '-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free','-o',str(exe)],
                   input=code,text=True,check=True)
    results=[]
    for mode in ('save','deactivate','activate','select'):
        r=subprocess.run([str(exe),mode],cwd=tmp);results.append(r.returncode)
    assert not any(results),results
