"""Actual private queue must replay positions and cover parent/child damage."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
p=root/'src/ui/layout_queue.c'
if p.exists(): body=p.read_text().replace('#include "ui.h"','')
else:
    s=(root/'src/ui/main_host.c').read_text()
    body=s[s.index('/* 批量移动控件：'):s.index('/* 内容变短 /')]
pre=r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <wchar.h>
#include "test_fail_alloc.h"
typedef void *HDWP;
typedef unsigned UINT;
typedef struct {int left,top,right,bottom;} RECT;
typedef struct {int x,y;} POINT;
typedef struct Window {RECT rect,damage;struct Window *parent;unsigned dirty;int combo,style;} *HWND;
#define GWL_STYLE 0
#define CBS_SIMPLE 1
static int GetClassNameW(HWND h,wchar_t *name,int cap){(void)cap;wcscpy(name,h->combo?L"ComboBox":L"Edit");return 1;}
static int lstrcmpiW(const wchar_t *a,const wchar_t *b){return wcscmp(a,b);}
static int GetWindowLongPtrW(HWND h,int index){(void)index;return h->style;}
#define SWP_NOZORDER 1
#define SWP_NOACTIVATE 2
#define SWP_NOCOPYBITS 4
#define SWP_NOREDRAW 256
#define RDW_INVALIDATE 8
#define RDW_ERASE 16
#define RDW_FRAME 32
#define RDW_ALLCHILDREN 64
#define RDW_NOCHILDREN 128
#define TRUE 1
#define FALSE 0
static int failure,mixed,starts,nn,intermediate_paints;
static struct {HWND h;int x,y,w,ht;UINT flags;} native[32];
static HWND native_parent;
static int UnionRect(RECT *out,const RECT *a,const RECT *b) {
    if(a->right<=a->left || a->bottom<=a->top){*out=*b;return 1;}
    if(b->right<=b->left || b->bottom<=b->top){*out=*a;return 1;}
    *out=(RECT){a->left<b->left?a->left:b->left,a->top<b->top?a->top:b->top,
                a->right>b->right?a->right:b->right,a->bottom>b->bottom?a->bottom:b->bottom};return 1;
}
static int RedrawWindow(HWND h,const RECT *r,void *region,UINT f) {
    (void)region;if(!h)return 0;RECT whole={0,0,h->rect.right-h->rect.left,h->rect.bottom-h->rect.top};
    UnionRect(&h->damage,&h->damage,r?r:&whole);h->dirty|=f;return 1;
}
static int SetWindowPos(HWND h,HWND z,int x,int y,int w,int ht,UINT f) {
    (void)z;
    if(!(f&SWP_NOREDRAW)) ++intermediate_paints;
    h->rect=(RECT){x,y,x+w,y+ht};return 1;
}
static HDWP BeginDeferWindowPos(int n) {(void)n;++starts;nn=0;native_parent=NULL;return failure==1?NULL:(HDWP)1;}
static HDWP DeferWindowPos(HDWP b,HWND h,HWND z,int x,int y,int w,int ht,UINT f) {
    (void)b;(void)z;
    if(nn && native_parent!=h->parent){++mixed;return NULL;}native_parent=h->parent;
    if(failure==2 && nn==1)return NULL;
    native[nn].h=h;native[nn].x=x;native[nn].y=y;native[nn].w=w;native[nn].ht=ht;native[nn].flags=f;++nn;return (HDWP)1;
}
static int EndDeferWindowPos(HDWP b) {
    (void)b;if(failure==3)return 0;
    for(int i=0;i<nn;++i)SetWindowPos(native[i].h,NULL,native[i].x,native[i].y,native[i].w,native[i].ht,native[i].flags);
    return 1;
}
static HWND GetParent(HWND h){return h->parent;}
static int GetWindowRect(HWND h,RECT *r){*r=h->rect;return 1;}
static int MapWindowPoints(HWND a,HWND b,POINT *p,int n){(void)a;(void)b;(void)p;(void)n;return 0;}
'''
driver=r'''
int main(void) {
    for(int separate=0;separate<=1;++separate)for(int oom=0;oom<=3;++oom)for(failure=0;failure<=3;++failure) {
        struct Window p={{0,0,400,400}},q={{0,0,400,400}};
        struct Window a={{0,0,80,20},{0},&p},b={{90,0,170,20},{0},separate?&q:&p},c={{0,30,80,50},{0},&p};
        intermediate_paints=mixed=starts=0;fa_inject(oom);HDWP batch=qp_layout_begin(1);
        batch=qp_defer(batch,&a,10,20,120,30);
        batch=qp_defer(batch,&b,50,60,80,20);
        batch=qp_defer(batch,&c,90,100,110,120);
        qp_layout_end(batch);fa_disable();
        assert(!fa_live && !mixed);
        assert(!intermediate_paints);
        assert(a.rect.left==10 && a.rect.bottom==50 && b.rect.top==60 && c.rect.bottom==220);
        assert((a.dirty&(RDW_INVALIDATE|RDW_FRAME|RDW_ALLCHILDREN))==(RDW_INVALIDATE|RDW_FRAME|RDW_ALLCHILDREN));
        assert(b.dirty & RDW_INVALIDATE);
        assert((a.dirty & (RDW_ERASE|RDW_FRAME)) == (RDW_ERASE|RDW_FRAME));
        assert(p.dirty & RDW_ERASE);
        assert(p.damage.left<=0 && p.damage.top<=0 && p.damage.right>=200 && p.damage.bottom>=220);
    }
    { struct Window c={{10,20,110,48},{0},NULL,0,1,3};
      failure=starts=0; HDWP batch=qp_layout_begin(1);
      batch=qp_defer(batch,&c,10,20,100,30);qp_layout_end(batch);
      assert(!starts && !c.dirty); /* Native font-determined height already matches. */
      batch=qp_layout_begin(1);batch=qp_defer(batch,&c,10,20,120,30);qp_layout_end(batch);
      assert(c.rect.right==130 && c.rect.bottom==48);
      c.style=CBS_SIMPLE;
      batch=qp_layout_begin(1);batch=qp_defer(batch,&c,10,20,120,90);qp_layout_end(batch);
      assert(c.rect.bottom==110); /* Simple combos retain their explicit list height. */
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='qp-layout-damage-') as tmp:
    exe=Path(tmp)/'probe'
    subprocess.run(['cc','-std=gnu11','-Wall','-Wno-unused-function','-Wno-missing-field-initializers',
                    '-I',str(root/'tests'),'-x','c','-',
                    '-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free','-o',str(exe)],
                   input=pre+body+driver,text=True,check=True)
    subprocess.run([str(exe)],check=True)
print('layout damage: 32 allocation/native/parent cases preserve positions, parent/frame/child invalidation')
