"""Execute fixed-modal fitting; EnumChildWindows' return is deliberately zero."""
from pathlib import Path
import re,subprocess,tempfile
from source_helpers import ROOT,INCLUDE_FLAGS
source=(ROOT/'src/ui/dialog_viewport.c').read_text()
def function(name):
    m=re.search(r'^(?:static )?[^\n]+\b'+name+r'\([^;]+?\)\n\{',source,re.M);assert m,name
    return source[m.start():source.index('\n}',m.end())+2]
types=source[source.index('typedef struct {'):source.index(';',source.index('enum { QP_DLG_VIEW_FOCUS'))+1]
pre=r'''
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
typedef struct {int left,top,right,bottom;} RECT;
typedef long LPARAM,LONG_PTR,LRESULT;typedef unsigned long DWORD_PTR,WPARAM,UINT_PTR;typedef unsigned UINT;typedef int BOOL;
typedef struct {RECT outer,content;long style;int x,y;} Window;
typedef Window *HWND;static Window root;
#define WM_APP 1000
#define FALSE 0
#define TRUE 1
#define CALLBACK
#define GWL_STYLE 0
#define WS_HSCROLL 1
#define WS_VSCROLL 2
#define SWP_NOMOVE 4
#define SWP_NOZORDER 8
#define SWP_NOACTIVATE 16
#define SWP_FRAMECHANGED 32
static int subclass_fail,position_fail,enumerate_fail,scrolled;
static void *owned;
static void qp_window_work_area(HWND h,RECT *r){(void)h;*r=(RECT){-1366,-728,0,0};}
static int GetWindowRect(HWND h,RECT *r){*r=h->outer;return 1;}
static void GetClientRect(HWND h,RECT *r){*r=h->content;}
static long GetWindowLongPtrW(HWND h,int k){(void)k;return h->style;}
static void SetWindowLongPtrW(HWND h,int k,long v){(void)k;h->style=v;}
static LRESULT dialog_view_proc(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR i,DWORD_PTR r){return 0;}
static LRESULT dialog_view_focus(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR i,DWORD_PTR r){return 0;}
static int SetWindowSubclass(HWND h,void *fn,UINT_PTR i,DWORD_PTR data) {
    (void)i;if(subclass_fail)return 0;if(h==&root && fn==(void *)dialog_view_proc)owned=(void *)data;
    return 1;
}
static int EnumChildWindows(HWND h,BOOL (*fn)(HWND,LPARAM),LPARAM data) {
    (void)h;Window child={0};if(enumerate_fail)subclass_fail=1;
    fn(&child,data);return 0; /* This return value has no documented meaning. */
}
static int SetWindowPos(HWND h,HWND p,int x,int y,int w,int height,int flags) {
    (void)p;(void)x;(void)y;(void)flags;if(position_fail)return 0;
    h->outer.right=w;h->outer.bottom=height;h->content=(RECT){0,0,w-33,height-65};return 1;
}
static void dialog_view_scroll(HWND h,void *v,int x,int y){(void)h;(void)v;(void)x;(void)y;++scrolled;}
static HWND GetFocus(void){return NULL;}
static int IsChild(HWND h,HWND c){return 0;}
static void SendMessageW(HWND h,UINT m,WPARAM w,LPARAM l){}
'''
driver=r'''
int main(void) {
    root=(Window){{0,0,1416,1305},{0,0,1400,1240},0,0,0};
    assert(qp_dialog_fit_fixed(&root,NULL));
    assert(root.outer.right<=1366 && root.outer.bottom<=728 && root.style==3 && scrolled);
    free(owned);owned=NULL;
    root=(Window){{0,0,600,500},{0,0,584,452},0,0,0};
    assert(qp_dialog_fit_fixed(&root,NULL) && !owned && !root.style);
    for(int i=0;i<3;++i) {
        root=(Window){{0,0,1416,1305},{0,0,1400,1240},0,0,0};
        subclass_fail=i==0;enumerate_fail=i==1;position_fail=i==2;
        assert(!qp_dialog_fit_fixed(&root,NULL));free(owned);owned=NULL;
        subclass_fail=enumerate_fail=position_fail=0;
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='qp-modal-fit-') as tmp:
    exe=Path(tmp)/'test'
    subprocess.run(['cc','-std=gnu11',*INCLUDE_FLAGS,'-x','c','-','-o',str(exe)],
                   input=pre+types+function('dialog_view_hook')+function('dialog_view_fit')+function('qp_dialog_fit_fixed')+driver,text=True,check=True)
    subprocess.run([str(exe)],check=True)
print('modal fitting: workarea clipping, ordinary no-op, subclass/enumeration/position refusal checked')
