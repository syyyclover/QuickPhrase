"""Execute the real Static subclass: text allocation failure must not erase text."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/ui/text_paint.c').read_text()
a=s.index('static LRESULT CALLBACK buffered_static(');b=s.index('\n}',a)+2
body=s[a:b]
pre=r'''
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <assert.h>
#include "test_fail_alloc.h"
typedef void *HWND;typedef void *HDC;typedef void *HBITMAP;typedef void *HGDIOBJ;typedef void *HFONT;typedef void *HBRUSH;
typedef unsigned UINT;typedef unsigned long DWORD;typedef unsigned long WPARAM;typedef long LPARAM;typedef long LRESULT;
typedef unsigned long UINT_PTR;typedef unsigned long DWORD_PTR;
typedef struct {int left,top,right,bottom;} RECT;typedef struct {RECT rcPaint;} PAINTSTRUCT;
#define CALLBACK
#define WM_ERASEBKGND 1
#define WM_PAINT 2
#define WM_CTLCOLORSTATIC 3
#define WM_GETFONT 4
#define WM_NCDESTROY 5
#define GWL_STYLE 1
#define SS_NOPREFIX 128
#define SS_TYPEMASK 31
#define SS_RIGHT 2
#define SS_CENTER 1
#define SS_ENDELLIPSIS 256
#define SS_CENTERIMAGE 512
#define DT_NOPREFIX 1
#define DT_RIGHT 2
#define DT_CENTER 4
#define DT_SINGLELINE 8
#define DT_END_ELLIPSIS 16
#define DT_WORDBREAK 32
#define DT_VCENTER 64
#define TRANSPARENT 1
#define SRCCOPY 1
static HBRUSH g_brushBk=(void *)6;static HFONT g_fontUI=(void *)4;
static int draws,native_paints,begun,ended;
static int GetWindowTextLengthW(HWND h) {(void)h;return 3;}
static int GetWindowTextW(HWND h,wchar_t *p,int n) {(void)h;(void)n;wcscpy(p,L"ABC");return 3;}
static long GetWindowLongPtrW(HWND h,int m) {(void)h;(void)m;return SS_ENDELLIPSIS;}
static HDC BeginPaint(HWND h,PAINTSTRUCT *p) {(void)h;(void)p;++begun;return (void *)1;}
static int EndPaint(HWND h,PAINTSTRUCT *p) {(void)h;(void)p;++ended;return 1;}
static int GetClientRect(HWND h,RECT *r) {(void)h;*r=(RECT){0,0,100,30};return 1;}
static HDC CreateCompatibleDC(HDC d) {(void)d;return (void *)2;}
static HBITMAP CreateCompatibleBitmap(HDC d,int w,int h) {(void)d;(void)w;(void)h;return (void *)3;}
static HGDIOBJ SelectObject(HDC d,HGDIOBJ o) {(void)d;(void)o;return (void *)4;}
static HWND GetParent(HWND h) {return h;}
static long SendMessageW(HWND h,UINT m,WPARAM w,LPARAM l) {(void)h;(void)w;(void)l;return m==WM_GETFONT?4:6;}
static int FillRect(HDC d,const RECT *r,HBRUSH b) {(void)d;(void)r;(void)b;return 1;}
static int IsWindowEnabled(HWND h) {(void)h;return 1;}
static int theme_dim(void) {return 0;}
static int SetTextColor(HDC d,int c) {(void)d;(void)c;return 1;}
static int SetBkMode(HDC d,int c) {(void)d;(void)c;return 1;}
static int DrawTextW(HDC d,const wchar_t *t,int n,RECT *r,UINT f) {(void)d;(void)t;(void)n;(void)r;(void)f;++draws;return 1;}
static int BitBlt(HDC a,int b,int c,int d,int e,HDC f,int g,int h,int i) {(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;return 1;}
static int DeleteObject(HGDIOBJ b) {(void)b;return 1;}
static int DeleteDC(HDC d) {(void)d;return 1;}
static int RemoveWindowSubclass(HWND h,void *p,UINT_PTR i) {(void)h;(void)p;(void)i;return 1;}
static long DefSubclassProc(HWND h,UINT m,WPARAM w,LPARAM l) {(void)h;(void)w;(void)l;if(m==WM_PAINT)++native_paints;return 0;}
'''
driver=r'''
int main(void) {
    fa_inject(1);buffered_static((void *)1,WM_PAINT,0,0,0,0);fa_disable();
    assert(draws || native_paints);assert(!fa_live && begun==ended);
    draws=native_paints=begun=ended=0;
    buffered_static((void *)1,WM_PAINT,0,0,0,0);
    assert(draws==1 && !native_paints && begun==ended && !fa_live);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='qp-static-failure-') as tmp:
    exe=Path(tmp)/'probe'
    subprocess.run(['cc','-std=gnu11','-Wall','-Wno-unused-function','-I',str(root/'tests'),'-x','c','-',
                    '-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free','-o',str(exe)],
                   input=pre+body+driver,text=True,check=True)
    subprocess.run([str(exe)],check=True)
print('Static paint: allocation failure delegates native drawing without validating blank label')
