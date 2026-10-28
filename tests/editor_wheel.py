"""Execute the editor subclass with OS adapters; not a native GUI test."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'src/ui/editor_layout.c').read_text()
start = source.index('static LRESULT CALLBACK ed_prop_focus_proc(')
body = source[start:source.index('\n}', start) + 2]
pre = r'''
#include <assert.h>
#include <wchar.h>
typedef void *HWND; typedef unsigned UINT; typedef long WPARAM,LPARAM,LRESULT;
typedef unsigned long UINT_PTR,DWORD_PTR;
typedef struct {int left,top,right,bottom;} RECT;
typedef struct {int narrow,activePage,propsScroll;HWND hPropsHost;} EditorCtx;
#define CALLBACK
#define NULL_HWND 0
#define WM_SETFOCUS 1
#define WM_NCDESTROY 2
#define WM_MOUSEWHEEL 3
static int selection,scrolls,combo=1;
static int IsChild(HWND parent,HWND child){(void)parent;(void)child;return 1;}
static void ed_layout(EditorCtx *c){(void)c;}
static void ed_props_scroll_to(EditorCtx *c,int p){(void)c;(void)p;}
static int GetClassNameW(HWND h,wchar_t *out,int cap){(void)h;(void)cap;wcscpy(out,combo?L"ComboBox":L"Edit");return 1;}
static int lstrcmpiW(const wchar_t *a,const wchar_t *b){return wcscmp(a,b);}
static LRESULT SendMessageW(HWND h,UINT msg,WPARAM wp,LPARAM lp){assert(h==(HWND)7 && msg==WM_MOUSEWHEEL && wp==120 && lp==45);++scrolls;return 0;}
static LRESULT DefSubclassProc(HWND h,UINT msg,WPARAM wp,LPARAM lp){(void)h;(void)wp;(void)lp;if(msg==WM_MOUSEWHEEL)++selection;return 9;}
static void GetClientRect(HWND h,RECT *r){(void)h;*r=(RECT){0,0,100,100};}
static void GetWindowRect(HWND h,RECT *r){GetClientRect(h,r);}
typedef RECT POINT;
static void MapWindowPoints(HWND a,HWND b,POINT *p,int n){(void)a;(void)b;(void)p;(void)n;}
static void RemoveWindowSubclass(HWND h,void *p,UINT_PTR id){(void)h;(void)p;(void)id;}
'''
driver = r'''
int main(void){EditorCtx c={0};c.hPropsHost=(HWND)7;
assert(ed_prop_focus_proc((HWND)1,WM_MOUSEWHEEL,120,45,0,(DWORD_PTR)&c)==0);
assert(selection==0 && scrolls==1);
combo=0;assert(ed_prop_focus_proc((HWND)1,WM_MOUSEWHEEL,120,45,0,(DWORD_PTR)&c)==9);
assert(selection==1 && scrolls==1);
assert(ed_prop_focus_proc((HWND)1,99,0,0,0,(DWORD_PTR)&c)==9);}
'''
with tempfile.TemporaryDirectory(prefix='qp-editor-wheel-') as tmp:
    exe = Path(tmp) / 'probe'
    subprocess.run(['cc', '-x', 'c', '-', '-o', str(exe)], input=pre+body+driver, text=True, check=True)
    subprocess.run([str(exe)], check=True)
print('editor wheel: combo selection unchanged; wheel routed to properties')
