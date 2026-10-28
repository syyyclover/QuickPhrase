"""Execute author and filler helpers; model/engine real, only modal OS calls adapted."""
from pathlib import Path
import re,subprocess,tempfile
from source_helpers import ROOT,INCLUDE_FLAGS,CORE_SOURCES
editor=(ROOT/'src/ui/dialog_editor.c').read_text()
choice=(ROOT/'src/ui/main_choices.c').read_text()
def fn(source,name):
    m=re.search(r'^(?:static )?(?:void|int|HWND) '+name+r'\([^;]+?\)\n\{',source,re.M)
    assert m,name
    return source[m.start():source.index('\n}',m.end())+2]+'\n'
pre=r'''
#include "engine.h"
#include "test_fail_alloc.h"
#include <string.h>
#include <stdio.h>
typedef void *HWND;
typedef struct {HWND hwnd,hOptList;} EditorCtx;
typedef struct {int field_index;HWND hCtl;int extraIndex;} FieldCtl;
static EvalContext *g_ctx;
static Segment *active;
static int g_fieldCount=1,accept=1,custom=1,refreshes,syncs,opens,dialogs,closed,hints;
static HWND g_hwndMain=(void *)1,g_hHint=(void *)2;
static FieldCtl controls[1]={{0,(void *)3,-1}},*g_fields=controls;
static int selected=1,posted,enabled=1;
#define CB_GETCURSEL 1
#define WM_QP_CHOICE_INPUT 42
static int PostMessageW(HWND h,int m,int w,int l){(void)h;(void)m;(void)w;(void)l;++posted;return 1;}
static HWND GetFocus(void){return g_fields[0].hCtl;}
static int IsWindowEnabled(HWND h){(void)h;return enabled;}
static long SendMessageW(HWND h,int m,int w,int l){(void)h;(void)m;(void)w;(void)l;return selected;}
static unsigned char *popup_mask;
static wchar_t entered[QP_VALUE_MAX]=L"Other";
#define MB_OK 1
#define MB_ICONERROR 2
static Segment *ed_cur_seg(EditorCtx *c){(void)c;return active;}
static int lv_sel(HWND h){(void)h;return active->option_count-1;}
static void lv_select(HWND h,int i){(void)h;(void)i;}
static void ed_fill_optlist(EditorCtx *c){(void)c;}
static void ed_update_preview(EditorCtx *c){(void)c;}
static int dlg_option(HWND h,const wchar_t *title,wchar_t *text,size_t cap,int *flag,int available){
(void)h;(void)title;++dialogs;if(!accept)return 0;qp_strlcpy(text,entered,cap);*flag=available?custom:0;return 1;}
static int dlg_input(HWND h,const wchar_t *title,const wchar_t *prompt,wchar_t *text,size_t cap){
(void)h;(void)title;(void)prompt;++dialogs;if(!accept)return 0;qp_strlcpy(text,entered,cap);return 1;}
static int qp_msgbox(HWND h,int flags,const wchar_t *title,const wchar_t *text){(void)h;(void)flags;(void)title;(void)text;return 0;}
static void ui_refresh_current(void){++refreshes;eval_run(g_ctx);}
static void ui_sync_controls(void){++syncs;}
static void qp_set_text_if_diff(HWND h,const wchar_t *s){(void)h;(void)s;++hints;}
static int IsWindow(HWND h){return h!=NULL;}
static void popup_close(void){++closed;free(popup_mask);popup_mask=NULL;}
static void popup_open(int index,HWND anchor){(void)index;(void)anchor;++opens;}
const wchar_t *multi_sep(const Segment *s){return s->separator;}
'''
driver=r'''
int main(void){
EditorCtx ed={0};Template *t=tmpl_create(L"Choices");active=seg_create(SEG_SELECT);seg_list_append(&t->segments,active);
const wchar_t *options[]={L"A"};assert(qp_seg_set_options(active,options,1));
ed_opt_add(&ed);assert(active->option_count==2 && active->custom_option==2);
accept=0;wcscpy(entered,L"changed");ed_opt_edit(&ed);assert(!wcscmp(active->options[1],L"Other") && active->custom_option==2);
accept=1;fa_inject(1);ed_opt_edit(&ed);fa_disable();assert(!wcscmp(active->options[1],L"Other") && active->custom_option==2);
g_ctx=eval_create(t);assert(g_ctx);wcscpy(entered,L"own / text");
int was_dialogs=dialogs;ui_choice_single_queue(0,controls[0].hCtl);assert(dialogs==was_dialogs && posted==1);
ui_choice_dispatch();assert(dialogs==was_dialogs+1);
ui_choice_single_queue(0,controls[0].hCtl);ui_choice_cancel_pending();ui_choice_dispatch();assert(dialogs==was_dialogs+1);
ui_choice_single_queue(0,controls[0].hCtl);EvalContext *live=g_ctx;g_ctx=NULL;ui_choice_dispatch();g_ctx=live;assert(dialogs==was_dialogs+1);
selected=0;ui_choice_single_queue(0,controls[0].hCtl);ui_choice_dispatch();selected=1;assert(dialogs==was_dialogs+1);

assert(g_ctx->fields[0].custom_selected && !wcscmp(g_ctx->fields[0].value,L"own / text"));
assert(!wcscmp(active->options[1],L"Other"));accept=0;ui_choice_single(0);
assert(syncs==1 && !wcscmp(g_ctx->fields[0].value,L"own / text"));
eval_destroy(g_ctx);active->type=SEG_MULTI_SELECT;wcscpy(active->separator,L" / ");g_ctx=eval_create(t);assert(g_ctx);
popup_mask=calloc(2,1);popup_mask[0]=1;accept=1;wcscpy(entered,L"A / own");
ui_choice_multi(0,popup_mask,2);assert(closed==1 && !popup_mask && opens==1);
assert(!wcscmp(g_ctx->fields[0].value,L"A / A / own"));
assert(!wcscmp(g_ctx->fields[0].choice_base,L"A") && !wcscmp(g_ctx->fields[0].custom_value,L"A / own"));
popup_mask=calloc(2,1);popup_mask[0]=popup_mask[1]=1;accept=0;ui_choice_multi(0,popup_mask,2);
assert(!wcscmp(g_ctx->fields[0].value,L"A / A / own") && opens==2);
popup_mask=calloc(2,1);popup_mask[0]=1;accept=1;wmemset(entered,L'x',QP_VALUE_MAX-1);entered[QP_VALUE_MAX-1]=0;
ui_choice_multi(0,popup_mask,2);assert(hints && !wcscmp(g_ctx->fields[0].value,L"A / A / own"));
popup_mask=calloc(2,1);int before=dialogs;fa_inject(1);ui_choice_multi(0,popup_mask,2);fa_disable();
assert(dialogs==before && popup_mask);free(popup_mask);popup_mask=NULL;
eval_destroy(g_ctx);tmpl_free(t);assert(!fa_live);
puts("custom choices: author mark/cancel/OOM, single fill/cancel, multi mask ownership/merge/cancel/overflow/OOM");}
'''
pending=choice[choice.index('static int pending_index='):choice.index('void ui_choice_cancel_pending(')]
code=pre+pending+fn(choice,'ui_choice_cancel_pending')+fn(editor,'ed_opt_add')+fn(editor,'ed_opt_edit')+fn(choice,'choice_anchor')+fn(choice,'ui_choice_single')+fn(choice,'ui_choice_multi')+fn(choice,'ui_choice_single_queue')+fn(choice,'ui_choice_dispatch')+driver
with tempfile.TemporaryDirectory(prefix='qp-custom-choice-') as tmp:
    exe=Path(tmp)/'probe'
    subprocess.run(['cc','-std=gnu11','-Wall',*INCLUDE_FLAGS,'-I',str(ROOT/'tests'),'-x','c','-',*CORE_SOURCES,
        str(ROOT/'src/core/table.c'),str(ROOT/'src/core/storage.c'),str(ROOT/'src/core/regex.c'),str(ROOT/'third_party/cjson/cJSON.c'),
        '-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free','-o',str(exe)],input=code,text=True,check=True)
    subprocess.run([str(exe)],check=True)
