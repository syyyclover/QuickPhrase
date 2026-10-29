"""Run the layout queue with injected Begin/Defer/End failures.

Windows positioning is doubled; assertions check requested final rectangles,
including positions that a failed deferred transaction would have discarded.
"""
from source_helpers import read_source
import pathlib
import subprocess
import tempfile

queue = pathlib.Path(__file__).resolve().parents[1] / 'src/ui/layout_queue.c'
if queue.exists():
    body = queue.read_text().replace('#include "ui.h"','')
else:
    source = read_source('ui.c')
    start = source.index('/* 批量移动控件：')
    end = source.index('/* 内容变短 /', start)
    body = source[start:end]
preamble = r'''
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <wchar.h>
typedef void *HDWP;
typedef struct { int left,top,right,bottom; } RECT;
typedef struct { int x,y; } POINT;
typedef struct Window { RECT rect; } *HWND;
typedef unsigned UINT;
#define GWL_STYLE 0
#define CBS_SIMPLE 1
static int GetClassNameW(HWND h,wchar_t *name,int cap){(void)h;(void)cap;wcscpy(name,L"Edit");return 1;}
static int lstrcmpiW(const wchar_t *a,const wchar_t *b){return wcscmp(a,b);}
static int GetWindowLongPtrW(HWND h,int index){(void)h;(void)index;return 0;}
#define SWP_NOZORDER 1
#define SWP_NOACTIVATE 2
#define SWP_NOCOPYBITS 4
#define SWP_NOREDRAW 256
#define RDW_INVALIDATE 8
#define RDW_ERASE 16
#define RDW_FRAME 32
#define RDW_ALLCHILDREN 64
#define RDW_NOCHILDREN 128
static int UnionRect(RECT *r,const RECT *a,const RECT *b) { (void)a;*r=*b;return 1; }
static int RedrawWindow(HWND h,const RECT *r,void *q,UINT f) { (void)h;(void)r;(void)q;(void)f;return 1; }
static int failure, calls;
static HDWP BeginDeferWindowPos(int n) { (void)n; calls=0; return failure==1 ? NULL : (HDWP)1; }
static HDWP DeferWindowPos(HDWP p,HWND h,HWND z,int x,int y,int w,int ht,UINT f) {
    (void)p;(void)h;(void)z;(void)x;(void)y;(void)w;(void)ht;(void)f;
    return failure==2 && ++calls==2 ? NULL : (HDWP)1;
}
static int EndDeferWindowPos(HDWP p) { (void)p; return failure!=3; }
static int SetWindowPos(HWND h,HWND z,int x,int y,int w,int ht,UINT f) {
    (void)z;(void)f; h->rect=(RECT){x,y,x+w,y+ht}; return 1;
}
static HWND GetParent(HWND h) { (void)h; return NULL; }
static int GetWindowRect(HWND h,RECT *r) { *r=h->rect; return 1; }
static int MapWindowPoints(HWND a,HWND b,POINT *p,int n) { (void)a;(void)b;(void)p;(void)n; return 0; }
'''
if 'HDWP qp_layout_begin(' not in body:
    preamble += '\n#define qp_layout_begin BeginDeferWindowPos\n#define qp_layout_end EndDeferWindowPos\n'
driver = r'''
int main(void) {
    for(failure=1;failure<=3;++failure) {
        struct Window a={{0}},b={{0}},c={{0}};
        HDWP batch=qp_layout_begin(1);
        batch=qp_defer(batch,&a,10,20,30,40);
        batch=qp_defer(batch,&b,50,60,70,80);
        batch=qp_defer(batch,&c,90,100,110,120);
        qp_layout_end(batch);
        assert(a.rect.left==10 && a.rect.bottom==60);
        assert(b.rect.left==50 && b.rect.bottom==140);
        assert(c.rect.left==90 && c.rect.bottom==220);
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='qp-layout-') as tmp:
    path = pathlib.Path(tmp)
    subprocess.run(['cc','-std=gnu11','-Wall','-x','c','-','-o',str(path/'layout')],
                   input=preamble+body+driver, text=True, check=True)
    subprocess.run([str(path/'layout')], check=True)
print('layout: Begin/Defer/End failure replays every requested rectangle')
