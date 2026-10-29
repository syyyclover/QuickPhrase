"""Actual preview paint cannot validate an empty frame when its buffer fails."""
from pathlib import Path
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/ui/preview.c').read_text()
m=re.search(r'^static void pv_paint\([^;]+?\)\n\{',s,re.M)
body=s[m.start():s.index('\n}',m.end())+2]
extra=re.search(r'^static void pv_draw\([^;]+?\)\n\{',s,re.M)
if extra: body=s[extra.start():s.index('\n}',extra.end())+2]+'\n'+body
part=s[s.index('    case WM_PRINTCLIENT:'):].split('\n    case ',1)[0] if '    case WM_PRINTCLIENT:' in s else '    case WM_PRINTCLIENT: return 0;'
body+='\nstatic int print_preview(HWND hwnd,unsigned long wp){unsigned msg=WM_PRINTCLIENT;'+part.split(':',1)[1]+'\n}\n'
erase=s[s.index('    case WM_ERASEBKGND:'):].split('\n    case ',1)[0]
# Shared case labels belong to the same production handler.
if not erase.split(':',1)[1].strip():
    erase=part
body+='\nstatic int erase_preview(HWND hwnd,unsigned long wp){unsigned msg=WM_ERASEBKGND;'+erase.split(':',1)[1]+'\n}\n'
pre=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
typedef void *HWND;typedef void *HDC;typedef void *HBITMAP;typedef void *HGDIOBJ;typedef void *HFONT;typedef void *HBRUSH;
typedef struct {int left,top,right,bottom;} RECT;
typedef struct {RECT rcPaint;} PAINTSTRUCT;
typedef struct {int mode;} Preview;
#define QP_PREVIEW_TABLE 1
#define TRANSPARENT 1
#define SRCCOPY 1
#define WM_PRINTCLIENT 0x318
#define WM_ERASEBKGND 0x14
static int SaveDC(HDC dc){(void)dc;return 1;}
static int RestoreDC(HDC dc,int save){(void)dc;(void)save;return 1;}
#define DC_BRUSH 1
static int fail,drawn,filled,ended,blits;
static HFONT g_fontMono=(void *)4;
static HBRUSH g_brushCtl=(void *)5;
static Preview model;
static Preview *pv_of(HWND h) {(void)h;return &model;}
static HDC BeginPaint(HWND h,PAINTSTRUCT *p) {(void)h;p->rcPaint=(RECT){0,0,100,70};return (void *)1;}
static int EndPaint(HWND h,PAINTSTRUCT *p) {(void)h;(void)p;++ended;return 1;}
static int GetClientRect(HWND h,RECT *r) {(void)h;*r=(RECT){0,0,100,70};return 1;}
static HDC CreateCompatibleDC(HDC dc) {(void)dc;return fail==1?NULL:(void *)2;}
static HBITMAP CreateCompatibleBitmap(HDC d,int w,int h) {(void)d;(void)w;(void)h;return fail==2?NULL:(void *)3;}
static HGDIOBJ SelectObject(HDC dc,HGDIOBJ o) {(void)dc;(void)o;return (void *)4;}
static int DeleteObject(HGDIOBJ o) {(void)o;return 1;}
static int DeleteDC(HDC d) {(void)d;return 1;}
static int SetBkMode(HDC d,int m) {(void)d;(void)m;return 1;}
static int theme_ctl_bk(void) {return 7;}
static HBRUSH CreateSolidBrush(int c) {(void)c;return fail==3?NULL:(void *)6;}
static HGDIOBJ GetStockObject(int t) {(void)t;return (void *)7;}
static int SetDCBrushColor(HDC d,int c) {(void)d;(void)c;return 1;}
static int FillRect(HDC d,const RECT *r,HBRUSH b) {(void)d;(void)r;assert(b);++filled;return 1;}
static int BitBlt(HDC a,int b,int c,int d,int e,HDC f,int g,int h,int i) {
    (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;++blits;return 1;
}
static void pv_paint_text(HDC d,Preview *p,const RECT *r) {(void)d;(void)p;(void)r;++drawn;}
static void pv_paint_table(HDC d,Preview *p,const RECT *r) {(void)d;(void)p;(void)r;++drawn;}
'''
driver=r'''
int main(void) {
    for(model.mode=0;model.mode<=1;++model.mode)for(fail=0;fail<=3;++fail) {
        g_brushCtl=fail==3?NULL:(void *)5;
        drawn=filled=ended=blits=0;pv_paint((void *)1);
        assert(drawn==1 && filled==1 && ended==1);
        if(fail==1||fail==2)assert(!blits);
        drawn=filled=ended=blits=0;
        assert(erase_preview((void *)1,(unsigned long)(void *)1)==1);
        assert(drawn==1 && filled==1 && ended==0);
        if(fail==1||fail==2)assert(!blits);
        drawn=filled=ended=blits=0;print_preview((void *)1,(unsigned long)(void *)1);
        assert(drawn==1 && filled==1 && ended==0);
        if(fail==1||fail==2)assert(!blits);
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='qp-paint-failure-') as tmp:
    exe=Path(tmp)/'probe'
    subprocess.run(['cc','-std=gnu11','-Wall','-Wno-unused-function','-x','c','-','-o',str(exe)],
                   input=pre+body+driver,text=True,check=True)
    subprocess.run([str(exe)],check=True)
print('paint: text/table direct fallback covers bitmap/DC/brush failures without empty validated frames')
