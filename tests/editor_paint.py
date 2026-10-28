"""Execute editor paint/erase/print branches including GDI allocation failure.

OS adapter only. Optional baseline directory holds pre-fix production sources.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
source_dir = Path(sys.argv[1]) if len(sys.argv)>1 else root/'src/ui'
editor = (source_dir/'dialog_editor.c').read_text(encoding='utf-8')
layout = (source_dir/'editor_layout.c').read_text(encoding='utf-8')
cases = editor[editor.index('    case WM_ERASEBKGND:'):editor.index('    case WM_SETCURSOR:')]
host = layout[layout.index('    case WM_ERASEBKGND:'):layout.index('    case WM_VSCROLL:')]
pre = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
typedef int *HDC;typedef void *HWND;typedef void *HBITMAP;typedef void *HGDIOBJ;typedef void *HBRUSH;
typedef intptr_t WPARAM;typedef intptr_t LPARAM;typedef intptr_t LRESULT;
typedef struct {int left,top,right,bottom;} RECT;
typedef struct {RECT rcPaint;} PAINTSTRUCT;
#define WM_ERASEBKGND 1
#define WM_PRINTCLIENT 2
#define WM_PAINT 3
#define ED_SPLIT_HIT 8
#define DPIX(x) (x)
#define SRCCOPY 1
static int fail,buffer,filled,blits,begins,ends;
static HBRUSH g_brushBk=(void *)1;
static int ed_splitter_y(int h){return h-80;}
static int theme_line(void){return 2;}
static int GetClientRect(HWND h,RECT *r){(void)h;*r=(RECT){0,0,800,600};return 1;}
static HDC BeginPaint(HWND h,PAINTSTRUCT *p){(void)h;++begins;p->rcPaint=(RECT){11,17,700,500};static int target;return &target;}
static int EndPaint(HWND h,PAINTSTRUCT *p){(void)h;(void)p;++ends;return 1;}
static HDC CreateCompatibleDC(HDC h){(void)h;buffer=0;return fail==1?NULL:&buffer;}
static HBITMAP CreateCompatibleBitmap(HDC h,int w,int y){(void)h;(void)w;(void)y;return fail==2?NULL:(void *)2;}
static HGDIOBJ SelectObject(HDC h,HGDIOBJ o){(void)h;(void)o;return (void *)3;}
static int SetViewportOrgEx(HDC h,int x,int y,void *p){(void)h;(void)x;(void)y;(void)p;return 1;}
static int FillRect(HDC h,const RECT *r,HBRUSH b){assert(h&&b);(void)r;++filled;*h=1;return 1;}
static HBRUSH CreateSolidBrush(int c){(void)c;return fail==3?NULL:(void *)4;}
static int DeleteObject(HGDIOBJ o){(void)o;return 1;}
static int DeleteDC(HDC h){(void)h;return 1;}
static int BitBlt(HDC to,int x,int y,int w,int h,HDC from,int sx,int sy,int op){
 (void)x;(void)y;(void)w;(void)h;(void)sx;(void)sy;(void)op;*to=*from;++blits;return 1;}
static void qp_draw_background(HWND h,HDC dc){RECT rc;if(!dc)return;GetClientRect(h,&rc);FillRect(dc,&rc,g_brushBk);}
static void qp_paint_background(HWND h){PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);qp_draw_background(h,dc);EndPaint(h,&ps);}
static LRESULT root_dispatch(int msg,HDC dc){HWND hwnd=(void *)1;WPARAM wp=(WPARAM)dc;switch(msg){
'''
driver = r'''
}return -1;}
int main(void){
 for(fail=0;fail<4;fail++)for(int msg=1;msg<=3;msg++){
  int target=0;filled=blits=begins=ends=0;
  assert(root_dispatch(msg,&target)==(msg==WM_ERASEBKGND));
  assert(filled>=1); /* A successful erase cannot leave an untouched black DC. */
  assert(begins==(msg==WM_PAINT) && ends==begins);
  if(msg!=WM_PAINT)assert(target);
  if(fail==1 || fail==2)assert(!blits);
 }
 for(int msg=1;msg<=3;msg++){
  int target=0;filled=begins=ends=0;
  assert(host_dispatch(msg,&target)==(msg==WM_ERASEBKGND));assert(filled==1);
  assert(begins==(msg==WM_PAINT) && ends==begins);
 }
}
'''
main = (root/'src/ui/main_window.c').read_text(encoding='utf-8')
a=main.index('void qp_draw_window_frame(');b=main.index('\nstatic void main_draw_client',a)
renderer=main[a:b]
extra='\n#define QP_SPLIT_HIT 8\n#define QP_HSPLIT_HIT 8\nstatic int splitter_x(void){return 150;}\nstatic int hsplitter_y(int h){return h-120;}\n'
pre=pre.replace('static LRESULT root_dispatch',extra+renderer+'\nstatic LRESULT root_dispatch')
code = pre+cases+'\n}return -1;}\nstatic LRESULT host_dispatch(int msg,HDC dc){HWND hwnd=(void *)1;WPARAM wp=(WPARAM)dc;switch(msg){\n'+host+driver
with tempfile.TemporaryDirectory(prefix='qp-editor-paint-') as tmp:
    exe = Path(tmp)/'probe'
    subprocess.run(['cc','-std=gnu11','-x','c','-','-o',str(exe)],input=code,text=True,check=True)
    subprocess.run([str(exe)],check=True)
print('editor paint: complete erase/print, normal paint validation, direct fallback on DC/bitmap/brush failures')
