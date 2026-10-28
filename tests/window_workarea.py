"""Real restore/centering must fit the owner's monitor, including negative X."""
from pathlib import Path
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
sources=[(root/'src/ui/main_common.c').read_text(),(root/'src/ui/dialog_editor.c').read_text()]
p=root/'src/ui/editor_layout.c'
if p.exists():sources.append(p.read_text())
def function(name):
    for source in sources:
        m=re.search(r'^(?:static )?(?:void|int) '+name+r'\([^;]+?\)\n\{',source,re.M)
        if m:return source[m.start():source.index('\n}',m.end())+2]
    return ''
pre=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct {int left,top,right,bottom;} RECT;
typedef struct {RECT rect;} *HWND;
typedef unsigned DWORD;typedef int HMONITOR;
typedef struct {unsigned cbSize;RECT rcMonitor,rcWork;unsigned dwFlags;} MONITORINFO;
#define MONITOR_DEFAULTTONEAREST 1
#define SPI_GETWORKAREA 1
#define SM_CXSCREEN 1
#define SM_CYSCREEN 2
#define GWL_STYLE 1
#define GWL_EXSTYLE 2
#define SWP_NOSIZE 1
#define SWP_NOZORDER 2
#define SWP_NOMOVE 4
#define SWP_NOACTIVATE 8
#define FALSE 0
#define ED_MIN_W 880
#define ED_MIN_H 690
#define ED_MIN_TRACK_W 600
#define ED_MIN_TRACK_H 320
static int g_dpi=192;
static struct {int editor_w,editor_h;} g_settings;
static RECT work={-1366,0,0,728};
static int DPIX(int n){return (n*g_dpi+48)/96;}
static int GetWindowRect(HWND h,RECT *r){*r=h->rect;return 1;}
static int IsWindowVisible(HWND h){return h!=NULL;}
static HMONITOR MonitorFromWindow(HWND h,int flags){(void)h;(void)flags;return 1;}
static int GetMonitorInfoW(HMONITOR h,MONITORINFO *info){(void)h;info->rcWork=work;return 1;}
static int SystemParametersInfoW(int a,int b,void *p,int d){(void)a;(void)b;(void)d;*(RECT *)p=(RECT){0,0,1366,728};return 1;}
static int GetSystemMetrics(int n){return n==SM_CXSCREEN?1366:768;}
static long GetWindowLongPtrW(HWND h,int n){(void)h;(void)n;return 0;}
static int AdjustWindowRectEx(RECT *r,DWORD s,int m,DWORD e){(void)s;(void)m;(void)e;r->left-=8;r->right+=8;r->top-=30;r->bottom+=8;return 1;}
static int SetWindowPos(HWND h,HWND z,int x,int y,int w,int height,unsigned f){
    (void)z;int oldw=h->rect.right-h->rect.left,oldh=h->rect.bottom-h->rect.top;
    if(f&SWP_NOSIZE){w=oldw;height=oldh;}if(f&SWP_NOMOVE){x=h->rect.left;y=h->rect.top;}
    h->rect=(RECT){x,y,x+w,y+height};return 1;
}
'''
driver=r'''
int main(void){
    struct {RECT rect;} owner={{-1300,20,-500,700}},window={{200,200,200+DPIX(880)+16,200+DPIX(690)+38}};
    ed_restore_size(&window,&owner);
    assert(window.rect.right-window.rect.left<=1366 && window.rect.bottom-window.rect.top<=728);
    assert(window.rect.left>=work.left && window.rect.right<=work.right && window.rect.top>=work.top && window.rect.bottom<=work.bottom);
    window.rect=(RECT){0,0,400,300};qp_center_window(&window,&owner);
    assert(window.rect.left<0 && window.rect.left>=work.left && window.rect.right<=work.right);
    g_settings.editor_w=5000;g_settings.editor_h=5000;ed_restore_size(&window,&owner);
    assert(window.rect.right-window.rect.left<=1366 && window.rect.bottom-window.rect.top<=728);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='qp-workarea-') as tmp:
    exe=Path(tmp)/'probe'
    code=pre+function('qp_window_work_area')+function('qp_center_window')+function('ed_restore_size')+driver
    subprocess.run(['cc','-std=gnu11','-Wall','-Wno-unused-function','-Wno-incompatible-pointer-types',
                    '-x','c','-','-o',str(exe)],input=code,text=True,check=True)
    subprocess.run([str(exe)],check=True)
print('workarea: first/remembered editor size and centering use owner monitor, including negative coordinates')
