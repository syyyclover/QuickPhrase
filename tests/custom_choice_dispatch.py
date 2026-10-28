"""Single choice must finish native list selection before opening a modal."""
from pathlib import Path
import re,subprocess,tempfile
from source_helpers import ROOT,INCLUDE_FLAGS,CORE_SOURCES
s=(ROOT/'src/ui/main_window.c').read_text()
m=re.search(r'^static void handle_field_command\([^;]+?\)\n\{',s,re.M)
body=s[m.start():s.index('\n}',m.end())+2]
h=(ROOT/'src/ui/ui_internal.h').read_text();end=h.index('} FieldCtl;')+len('} FieldCtl;')
field=h[h.rfind('typedef struct {',0,end):end]
pre=r'''
#include "engine.h"
#include "resource.h"
#include <assert.h>
typedef void *HWND;typedef unsigned short WORD;typedef unsigned long WPARAM;typedef long LPARAM;
#define BN_CLICKED 0
#define EN_CHANGE 1
#define CBN_SELCHANGE 2
#define CBN_SELENDOK 3
#define CB_GETCURSEL 4
#define CB_GETLBTEXT 5
#define CB_ERR (-1)
static int selected=1,dialogs,queued,refreshes;
static long SendMessageW(HWND h,int msg,WPARAM w,LPARAM l){(void)h;(void)w;if(msg==CB_GETCURSEL)return selected;if(msg==CB_GETLBTEXT)wcscpy((wchar_t *)l,L"A");return 0;}
static void qp_get_text(HWND h,wchar_t *text,size_t cap){(void)h;(void)text;(void)cap;assert(0);}
static void ui_refresh_current(void){++refreshes;}
static void popup_open(int index,HWND h){(void)index;(void)h;assert(0);}
static void ui_choice_single(int index){(void)index;++dialogs;}
static void ui_choice_single_queue(int index,HWND h){(void)index;(void)h;++queued;}
'''
state='static EvalContext *g_ctx;static FieldCtl *g_fields;static int g_fieldCount=1,g_syncFields;\n'
driver=r'''
int main(void){Template *t=tmpl_create(L"choice");Segment *seg=seg_create(SEG_SELECT);seg_list_append(&t->segments,seg);
const wchar_t *opts[]={L"A",L"Other"};assert(qp_seg_set_options(seg,opts,2));seg->custom_option=2;
g_ctx=eval_create(t);FieldCtl ctl={.field_index=0,.hCtl=(void *)1,.extraIndex=-1};g_fields=&ctl;
handle_field_command(IDC_FIELD_BASE,CBN_SELCHANGE);assert(!dialogs && !queued);
handle_field_command(IDC_FIELD_BASE,CBN_SELENDOK);assert(!dialogs && queued==1);
selected=0;handle_field_command(IDC_FIELD_BASE,CBN_SELCHANGE);assert(refreshes==1);
handle_field_command(IDC_FIELD_BASE,CBN_SELENDOK);assert(refreshes==1 && queued==1);
eval_destroy(g_ctx);tmpl_free(t);}
'''
with tempfile.TemporaryDirectory(prefix='qp-custom-dispatch-') as tmp:
    exe=Path(tmp)/'probe'
    subprocess.run(['cc','-std=gnu11',*INCLUDE_FLAGS,'-x','c','-',*CORE_SOURCES,
        str(ROOT/'src/core/table.c'),str(ROOT/'src/core/storage.c'),str(ROOT/'src/core/regex.c'),str(ROOT/'third_party/cjson/cJSON.c'),'-o',str(exe)],
        input=pre+field+state+body+driver,text=True,check=True)
    subprocess.run([str(exe)],check=True)
print('single custom dispatch: modal deferred until selection end; ordinary choice retains existing notifications')
