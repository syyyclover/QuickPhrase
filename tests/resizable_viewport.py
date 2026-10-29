"""Real factory and viewport under small workarea; OS double, not native acceptance."""
from pathlib import Path
import subprocess,tempfile
from source_helpers import ROOT,INCLUDE_FLAGS
common=(ROOT/'src/ui/dialog_common.c').read_text();start=common.index('HWND make_dlg_ex(')
factory=common[start:common.index('\n}',start)+2]
viewport=(ROOT/'src/ui/dialog_viewport.c').read_text()
viewport='\n'.join(line for line in viewport.splitlines() if not line.startswith('#include'))
pre=r'''
#include "dialog_viewport.h"
#include <assert.h>
#include <stdlib.h>
#include <wchar.h>
#include <stdint.h>
#include <stdio.h>
typedef struct {int left,top,right,bottom;} RECT;
typedef struct {int x,y;} POINT;
typedef long LRESULT,LPARAM,LONG_PTR;typedef unsigned long WPARAM,UINT_PTR,DWORD_PTR;
typedef unsigned UINT,DWORD;typedef int BOOL;typedef void *HDWP;
typedef struct {POINT ptReserved,ptMaxSize,ptMaxPosition,ptMinTrackSize,ptMaxTrackSize;} MINMAXINFO;
typedef struct {unsigned cbSize,fMask;int nMin,nMax;unsigned nPage;int nPos,nTrackPos;} SCROLLINFO;
typedef struct Window {RECT outer,content;DWORD style;struct Window *parent,*next;DWORD_PTR subclass;void *proc;} Window;
typedef Window *HWND;
typedef struct {HWND hwnd,after;int x,y,cx,cy;unsigned flags;} WINDOWPOS;
typedef LRESULT (*WNDPROC)(HWND,UINT,WPARAM,LPARAM);
static Window root,children[2];static int g_hInst,layouts;
#define CALLBACK
#define TRUE 1
#define FALSE 0
#define WM_APP 1000
#define WM_NCDESTROY 1
#define WM_SIZE 2
#define WM_SETFOCUS 3
#define WM_MOUSEWHEEL 4
#define WM_VSCROLL 5
#define WM_HSCROLL 6
#define WM_GETMINMAXINFO 7
#define WM_WINDOWPOSCHANGING 8
#define SIZE_MINIMIZED 1
#define LOWORD(v) ((unsigned)(v)&65535)
#define HIWORD(v) (((unsigned)(v)>>16)&65535)
#define GW_CHILD 1
#define GW_HWNDNEXT 2
#define GW_OWNER 3
#define GWL_STYLE 1
#define GWL_EXSTYLE 2
#define WS_POPUP 4
#define WS_CAPTION 8
#define WS_SYSMENU 16
#define WS_CLIPCHILDREN 32
#define WS_THICKFRAME 64
#define WS_EX_DLGMODALFRAME 128
#define WS_EX_CONTROLPARENT 256
#define WS_HSCROLL 512
#define WS_VSCROLL 1024
#define SWP_NOMOVE 1
#define SWP_NOZORDER 2
#define SWP_NOACTIVATE 4
#define SWP_FRAMECHANGED 8
#define SWP_NOSIZE 16
#define SB_HORZ 1
#define SB_VERT 2
#define SB_LINEUP 0
#define SB_LINEDOWN 1
#define SB_PAGEUP 2
#define SB_PAGEDOWN 3
#define SB_TOP 6
#define SB_BOTTOM 7
#define SB_THUMBPOSITION 4
#define SB_THUMBTRACK 5
#define SIF_RANGE 1
#define SIF_PAGE 2
#define SIF_POS 4
#define SIF_TRACKPOS 8
#define SIF_DISABLENOSCROLL 16
#define MB_OK 1
#define MB_ICONERROR 2
#define DPIX(v) ((v)*2)
static LRESULT dialog_view_proc(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
void qp_dialog_content_rect(HWND,RECT *);
HDWP qp_dialog_defer(HDWP,HWND,int,int,int,int);
static void qp_window_work_area(HWND h,RECT *r){(void)h;*r=(RECT){-1366,0,0,728};}
static int GetWindowRect(HWND h,RECT *r){*r=h->outer;return 1;}
static void GetClientRect(HWND h,RECT *r){*r=h->content;}
static HWND GetWindow(HWND h,int k){return k==GW_CHILD?&children[0]:k==GW_HWNDNEXT?h->next:NULL;}
static HWND GetParent(HWND h){return h->parent;}
static int MapWindowPoints(HWND a,HWND b,POINT *p,int n){(void)a;(void)b;(void)p;(void)n;return 1;}
static long GetWindowLongPtrW(HWND h,int k){return k==GWL_STYLE?h->style:0;}
static void SetWindowLongPtrW(HWND h,int k,long v){if(k==GWL_STYLE)h->style=v;}
static int SetWindowSubclass(HWND h,void *fn,UINT_PTR id,DWORD_PTR ref){(void)id;h->subclass=ref;h->proc=fn;return 1;}
static int GetWindowSubclass(HWND h,void *fn,UINT_PTR id,DWORD_PTR *ref){(void)id;if(h->proc!=fn)return 0;*ref=h->subclass;return 1;}
static int RemoveWindowSubclass(HWND h,void *fn,UINT_PTR id){(void)fn;(void)id;h->subclass=0;h->proc=NULL;return 1;}
static int EnumChildWindows(HWND h,BOOL (*fn)(HWND,LPARAM),LPARAM p){(void)h;for(int i=0;i<2;++i)fn(&children[i],p);return 0;}
static int SetScrollInfo(HWND h,int b,SCROLLINFO *s,int r){(void)h;(void)b;(void)s;(void)r;return 0;}
static int GetScrollInfo(HWND h,int b,SCROLLINFO *s){(void)h;(void)b;s->nPage=400;return 1;}
static HDWP qp_layout_begin(int n){(void)n;return (void *)1;}
static HDWP qp_defer(HDWP q,HWND h,int x,int y,int w,int height){h->outer=(RECT){x,y,x+w,y+height};return q;}
static void qp_layout_end(HDWP q){(void)q;}
static int IsChild(HWND h,HWND c){return c&&c->parent==h;}
static int IsWindow(HWND h){return h!=NULL;}
static HWND GetFocus(void){return NULL;}
static LRESULT SendMessageW(HWND h,UINT m,WPARAM w,LPARAM l){return dialog_view_proc(h,m,w,l,0,h->subclass);}
static LRESULT DefSubclassProc(HWND h,UINT m,WPARAM w,LPARAM l){
    (void)w;
    if(m==WM_GETMINMAXINFO){MINMAXINFO *m=(void *)l;m->ptMinTrackSize=(POINT){1200,1200};}
    if(m==WM_SIZE){
        RECT r;qp_dialog_content_rect(h,&r);++layouts;
        qp_dialog_defer((void *)1,&children[0],20,20,200,50);
        qp_dialog_defer((void *)1,&children[1],r.right-220,r.bottom-80,200,50);
    }
    return 0;
}
static int SetWindowPos(HWND h,HWND z,int x,int y,int w,int height,unsigned flags){
    (void)z;(void)x;(void)y;(void)flags;h->outer=(RECT){0,0,w,height};
    h->content=(RECT){0,0,w-32,height-80};
    if(h->subclass)SendMessageW(h,WM_SIZE,0,0);return 1;
}
static int AdjustWindowRectEx(RECT *r,DWORD s,int m,DWORD e){(void)s;(void)m;(void)e;r->right+=16;r->bottom+=60;return 1;}
static void ensure_class(const wchar_t *c,WNDPROC p){(void)c;(void)p;}
static HWND CreateWindowExW(DWORD ex,const wchar_t *cls,const wchar_t *title,DWORD style,int x,int y,int w,int h,HWND owner,void *menu,int inst,void *p){
    (void)ex;(void)cls;(void)title;(void)x;(void)y;(void)owner;(void)menu;(void)inst;(void)p;
    root=(Window){{0,0,w,h},{0,0,w-16,h-60},style,NULL,NULL,0,NULL};
    children[0]=(Window){{20,20,220,70},{0},0,&root,&children[1],0,NULL};
    children[1]=(Window){{w-236,h-140,w-36,h-90},{0},0,&root,NULL,0,NULL};return &root;
}
static void DestroyWindow(HWND h){if(h->subclass)SendMessageW(h,WM_NCDESTROY,0,0);}
static int qp_msgbox(HWND h,unsigned f,const wchar_t *title,const wchar_t *s){(void)h;(void)f;(void)title;(void)s;return 0;}
static void qp_center_window(HWND h,HWND owner){(void)h;(void)owner;}
'''
driver=r'''
int main(void){
    HWND h=make_dlg_ex(L"QuickPhraseTables",NULL,L"Tables",760,560,NULL,NULL,WS_THICKFRAME);
    assert(h && h->outer.right<=1366 && h->outer.bottom<=728);
    RECT r;qp_dialog_content_rect(h,&r);assert(r.right>=1160 && r.bottom>=880);
    MINMAXINFO min={0};SendMessageW(h,WM_GETMINMAXINFO,0,(LPARAM)&min);
    assert(min.ptMinTrackSize.x<=1366 && min.ptMinTrackSize.y<=728);
    WINDOWPOS pos={0};pos.cx=2000;pos.cy=1600;
    SendMessageW(h,WM_WINDOWPOSCHANGING,0,(LPARAM)&pos);assert(pos.cx<=1366 && pos.cy<=728);
    SendMessageW(h,WM_VSCROLL,SB_BOTTOM,0);
    assert(children[1].outer.bottom<=h->content.bottom && children[1].outer.top>=0);
    SetWindowPos(h,NULL,0,0,1000,500,0);
    SendMessageW(h,WM_VSCROLL,SB_BOTTOM,0);
    assert(children[1].outer.bottom<=h->content.bottom);
    SendMessageW(h,QP_DLG_VIEW_FOCUS,0,(LPARAM)&children[0]);
    assert(children[0].outer.top>=0);DestroyWindow(h);
    puts("resizable modal: small workarea bounds, virtual canvas, resize/scroll/focus and min/max tracking checked");return 0;
}
'''
# Old code lacks virtual-canvas accessors: doubles leave its behavior unchanged,
# allowing the real old factory's overflowing outer rectangle to fail first.
if 'void qp_dialog_content_rect(' not in viewport:
    viewport += '\nvoid qp_dialog_content_rect(HWND h,RECT *r){GetClientRect(h,r);}\nHDWP qp_dialog_defer(HDWP q,HWND h,int x,int y,int w,int t){return qp_defer(q,h,x,y,w,t);}\n'
with tempfile.TemporaryDirectory(prefix='qp-resizable-view-') as tmp:
    exe=Path(tmp)/'test'
    subprocess.run(['cc','-std=gnu11','-Wall','-Wno-unused-function',*INCLUDE_FLAGS,'-x','c','-',str(ROOT/'src/core/dialog_viewport.c'),'-o',str(exe)],input=pre+viewport+factory+driver,text=True,check=True)
    subprocess.run([str(exe)],check=True)
