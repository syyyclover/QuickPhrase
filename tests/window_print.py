"""Custom window print requests must draw into the supplied DC, without paint validation.

Runs production renderers/dispatch with GDI adapters, not a native animation test.
"""
from pathlib import Path
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
def function(source,name):
    m=re.search(r'^(?:static )?(?:void|int) '+name+r'\([^;]+?\)\n\{',source,re.M)
    if not m:return ''
    return source[m.start():source.index('\n}',m.end())+2]+'\n'
main=(root/'src/ui/main_window.c').read_text()
host=(root/'src/ui/main_host.c').read_text()
paint=(root/'src/ui/text_paint.c').read_text()
def print_case(source):
    if 'case WM_PRINTCLIENT:' not in source:return 'return 0;'
    part=source[source.index('case WM_PRINTCLIENT:'):]
    return part[len('case WM_PRINTCLIENT:'):part.index('\n    case ')]
pre=r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
typedef int *HDC;typedef void *HWND;typedef void *HBITMAP;typedef void *HBRUSH;typedef void *HGDIOBJ;
typedef intptr_t LRESULT;typedef uintptr_t WPARAM;typedef intptr_t LPARAM;
typedef struct {int left,top,right,bottom;} RECT;
#define SRCCOPY 1
#define WM_PRINTCLIENT 0x318
#define WM_ERASEBKGND 0x14
static int SaveDC(HDC dc){(void)dc;return 1;}
static int RestoreDC(HDC dc,int save){(void)dc;(void)save;return 1;}
#define DC_BRUSH 1
static int fail,buffer,paint_calls,blits;
static HBRUSH g_brushBk=(void *)1;
static int DPIX(int x){return x;}
#define QP_SPLIT_HIT 8
#define QP_HSPLIT_HIT 8
static int splitter_x(void){return 150;}
static int hsplitter_y(int h){return h-120;}
static int theme_line(void){return 2;}
static int theme_bk(void){return 1;}
static int GetClientRect(HWND h,RECT *r){(void)h;*r=(RECT){0,0,900,620};return 1;}
static HDC CreateCompatibleDC(HDC d){(void)d;buffer=0;return fail==1?NULL:&buffer;}
static HBITMAP CreateCompatibleBitmap(HDC d,int w,int h){(void)d;(void)w;(void)h;return fail==2?NULL:(void *)1;}
static HGDIOBJ SelectObject(HDC d,HGDIOBJ o){(void)d;(void)o;return (void *)1;}
static int SetViewportOrgEx(HDC d,int x,int y,void *old){(void)d;(void)x;(void)y;(void)old;return 1;}
static HBRUSH CreateSolidBrush(int c){(void)c;return (void *)2;}
static int DeleteObject(HGDIOBJ o){(void)o;return 1;}
static int DeleteDC(HDC d){(void)d;return 1;}
static HGDIOBJ GetStockObject(int type){(void)type;return (void *)1;}
static int SetDCBrushColor(HDC d,int c){(void)d;(void)c;return 1;}
static int FillRect(HDC d,const RECT *r,HBRUSH b){(void)r;assert(d && b);*d=1;return 1;}
static int BitBlt(HDC to,int x,int y,int w,int h,HDC from,int sx,int sy,int op){
(void)x;(void)y;(void)w;(void)h;(void)sx;(void)sy;(void)op;*to=*from;++blits;return 1;}
'''
tabs=(root/'src/ui/main_sessions.c').read_text()
pre+=r''' 
#include <wchar.h>
typedef void *HFONT;typedef void *HPEN;typedef unsigned long COLORREF;
#define TRANSPARENT 1
#define PS_SOLID 1
#define DT_SINGLELINE 1
#define DT_VCENTER 2
#define DT_NOPREFIX 4
#define DT_END_ELLIPSIS 8
#define QP_SESS_MAX 9
#define RGB(r,g,b) ((r)+(g)+(b))
typedef struct {wchar_t name[32];} Session;
static Session g_sess={L"test"},*g_curSess=&g_sess;
static HFONT g_fontUI=(void *)3;
static int g_tabCount=1,g_tabHot=-1,g_tabHotX,g_tabPlusW=28,g_tabPlusX=100,g_tabPlusHot,g_dark,text_draws;
static struct {int x,w;} g_tabBox[1]={{1,90}};
static void tabs_measure(void){}
static int sess_visible_index(Session *s){(void)s;return 0;}
static Session *sess_visible_at(int n){(void)n;return &g_sess;}
static int tabs_idle_bk(void){return 1;}static int tabs_hot_bk(void){return 2;}
static int theme_ctl_bk(void){return 3;}static int theme_text(void){return 4;}static int theme_dim(void){return 5;}
static int sess_color_of(Session *s){(void)s;return 6;}
static int tabs_close_w(void){return 16;}
static HPEN CreatePen(int type,int w,int c){(void)type;(void)w;(void)c;return (void *)4;}
static int SetBkMode(HDC d,int m){(void)d;(void)m;return 1;}
static int SetTextColor(HDC d,int c){(void)d;(void)c;return 1;}
static int Ellipse(HDC d,int a,int b,int c,int e){(void)d;(void)a;(void)b;(void)c;(void)e;return 1;}
static int DrawTextW(HDC d,const wchar_t *text,int n,RECT *r,int flags){(void)d;(void)text;(void)n;(void)r;(void)flags;++text_draws;return 1;}
static void tabs_draw_glyph(HDC d,int a,int b,int c,int e,int color){(void)d;(void)a;(void)b;(void)c;(void)e;(void)color;}
'''
helpers=function(tabs,'tabs_draw')+function(main,'qp_draw_window_frame')+function(main,'main_draw_client')+function(paint,'qp_draw_background')
dispatch='static LRESULT print_main(HWND hwnd,WPARAM wp,LPARAM lp){(void)lp;'+print_case(main)+'}\n'
dispatch+='static LRESULT print_host(HWND hwnd,WPARAM wp,LPARAM lp){(void)lp;'+print_case(host)+'}\n'
dispatch+='static LRESULT print_tabs(HWND hwnd,WPARAM wp,LPARAM lp){unsigned msg=WM_PRINTCLIENT;(void)lp;'+print_case(tabs)+'}\n'
erase=tabs[tabs.index('    case WM_ERASEBKGND:'):].split('\n    case ',1)[0].split(':',1)[1]
if not erase.strip():erase=print_case(tabs)
dispatch+='static LRESULT erase_tabs(HWND hwnd,WPARAM wp){unsigned msg=WM_ERASEBKGND;'+erase+'}\n'
driver=r'''
int main(void){for(int n=0;n<300;++n){fail=n%3;int dc=0;blits=0;
print_main((void *)1,(WPARAM)&dc,0);assert(dc==1);assert(blits==(fail==0));
dc=0;print_host((void *)1,(WPARAM)&dc,0);assert(dc==1);assert(paint_calls==0);
dc=0;text_draws=0;print_tabs((void *)1,(WPARAM)&dc,0);assert(dc==1 && text_draws==1);
dc=0;text_draws=0;assert(erase_tabs((void *)1,(WPARAM)&dc)==1);
assert(dc==1 && text_draws==1);}}
'''
with tempfile.TemporaryDirectory(prefix='qp-window-print-') as tmp:
    exe=Path(tmp)/'probe'
    subprocess.run(['cc','-std=gnu11','-Wno-unused-function','-x','c','-','-o',str(exe)],input=pre+helpers+dispatch+driver,text=True,check=True)
    subprocess.run([str(exe)],check=True)
print('window print: 300 supplied-DC requests, buffer/DC allocation fallback, no paint validation')
