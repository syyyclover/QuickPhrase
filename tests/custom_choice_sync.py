"""Custom single text equal to an ordinary option must keep its own UI identity."""
from pathlib import Path
import re,subprocess,tempfile
from source_helpers import ROOT,INCLUDE_FLAGS,CORE_SOURCES
s=(ROOT/'src/ui/main_fields.c').read_text();m=re.search(r'^void ui_sync_controls\([^;]+?\)\n\{',s,re.M)
body=s[m.start():s.index('\n}',m.end())+2]
h=(ROOT/'src/ui/ui_internal.h').read_text();end=h.index('} FieldCtl;')+len('} FieldCtl;')
field=h[h.rfind('typedef struct {',0,end):end]
pre=r'''
#include "engine.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
typedef struct {wchar_t *items[8];int count,selected;} Ctl,*HWND;
#define CB_FINDSTRINGEXACT 1
#define CB_DELETESTRING 2
#define CB_ADDSTRING 3
#define CB_GETCURSEL 4
#define CB_SETCURSEL 5
#define CB_GETLBTEXTLEN 6
#define CB_GETLBTEXT 7
#define EM_SETREADONLY 8
#define TRUE 1
#define FALSE 0
#define CB_ERR (-1)
#define _snwprintf swprintf
#define WPARAM unsigned long
#define LPARAM long
static Ctl control={.selected=-1};static HWND g_hwndMain;
static int g_popupField=-1,g_syncFields;
static HWND GetFocus(void){return &control;}
static void SetFocus(HWND h){(void)h;}
static void EnableWindow(HWND h,int on){(void)h;(void)on;}
static void InvalidateRect(HWND h,void *r,int erase){(void)h;(void)r;(void)erase;}
static void popup_close(void){}
static void qp_set_text_if_diff(HWND h,const wchar_t *v){(void)h;(void)v;}
static long SendMessageW(HWND h,int msg,WPARAM w,LPARAM l){
switch(msg){case CB_FINDSTRINGEXACT:for(int i=0;i<h->count;++i)if(!wcscmp(h->items[i],(wchar_t *)l))return i;return -1;
case CB_GETLBTEXTLEN:return (long)wcslen(h->items[w]);
case CB_GETLBTEXT:wcscpy((wchar_t *)l,h->items[w]);return (long)wcslen(h->items[w]);
case CB_ADDSTRING:assert(h->count<8);h->items[h->count]=qp_wcsdup((wchar_t *)l);return h->count++;
case CB_DELETESTRING:free(h->items[w]);for(int i=(int)w;i<h->count-1;++i)h->items[i]=h->items[i+1];--h->count;return 0;
case CB_GETCURSEL:return h->selected;case CB_SETCURSEL:h->selected=(int)w;return w;}return 0;}
'''
state='static EvalContext *g_ctx;static FieldCtl *g_fields;static int g_fieldCount=1;\n'
driver=r'''
int main(void){Template *t=tmpl_create(L"choice");Segment *seg=seg_create(SEG_SELECT);seg_list_append(&t->segments,seg);
const wchar_t *opts[]={L"A",L"Other"};assert(qp_seg_set_options(seg,opts,2));seg->custom_option=2;
g_ctx=eval_create(t);assert(g_ctx);EvalField *f=&g_ctx->fields[0];
eval_set_user_value(g_ctx,0,L"A");f->choice_tracked=f->custom_selected=1;wcscpy(f->custom_value,L"A");eval_run(g_ctx);
control.items[0]=qp_wcsdup(L"A");control.items[1]=qp_wcsdup(L"Other");control.count=2;
FieldCtl ctl={.field_index=0,.hCtl=&control,.extraIndex=-1};g_fields=&ctl;
ui_sync_controls();assert(ctl.extraIndex==2 && control.selected==2 && control.count==3);
assert(!wcscmp(seg->options[0],L"A") && !wcscmp(f->value,L"A"));
for(int i=0;i<100;++i)ui_sync_controls();assert(control.count==3 && ctl.extraIndex==2);
f->custom_selected=0;ui_sync_controls();assert(ctl.extraIndex==-1 && control.selected==0 && control.count==2);
assert(qp_choice_is_checked(f,0,L" / "));
for(int i=0;i<control.count;++i)free(control.items[i]);eval_destroy(g_ctx);tmpl_free(t);}
'''
with tempfile.TemporaryDirectory(prefix='qp-custom-sync-') as tmp:
    exe=Path(tmp)/'probe'
    subprocess.run(['cc','-std=gnu11',*INCLUDE_FLAGS,'-x','c','-',*CORE_SOURCES,
        str(ROOT/'src/core/table.c'),str(ROOT/'src/core/storage.c'),str(ROOT/'src/core/regex.c'),str(ROOT/'third_party/cjson/cJSON.c'),'-o',str(exe)],
        input=pre+field+state+body+driver,text=True,check=True)
    subprocess.run([str(exe)],check=True)
print('single custom identity: equal ordinary text stays custom; repeated refresh does not grow list; ordinary selection clears transient row')
