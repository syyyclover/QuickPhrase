"""Production resize dispatch through an OS adapter; no native visual claim."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
source=(root/'src/ui/main_window.c').read_text()
size=source[source.index('    case WM_SIZE:'):source.index('    case WM_MOVE:')]
pre=r'''
#include <assert.h>
#include <stddef.h>
typedef void *HWND;
typedef unsigned long WPARAM;
#define WM_SIZE 1
#define SIZE_MINIMIZED 1
#define SIZE_RESTORED 0
#define SIZE_MAXIMIZED 2
#define TIMER_SAVE_PLACEMENT 1
static int layouts,timers,redraws;
#define RDW_INVALIDATE 1
#define RDW_ERASE 2
#define RDW_ALLCHILDREN 4
#define RDW_UPDATENOW 8
static void RedrawWindow(HWND h,void *r,void *region,int flags){
    (void)h;(void)r;(void)region;
    assert((flags & 15)==15);++redraws;
}
static void popup_close(void){}
static void ui_layout(HWND h){(void)h;++layouts;}
static void SetTimer(HWND h,int id,int ms,void *p){(void)h;(void)id;(void)ms;(void)p;++timers;}
static int dispatch(int wp){HWND hwnd=(HWND)1;switch(WM_SIZE){
'''
driver=r'''
}return -1;}
int main(void){dispatch(SIZE_MINIMIZED);assert(layouts==0 && timers==0 && redraws==0);
dispatch(SIZE_RESTORED);dispatch(SIZE_MAXIMIZED);assert(layouts==2 && timers==2);
assert(redraws==2);
dispatch(SIZE_RESTORED);assert(redraws==3);
dispatch(SIZE_RESTORED);assert(redraws==3 && layouts==4);}
'''
with tempfile.TemporaryDirectory(prefix='qp-transition-') as tmp:
    exe=Path(tmp)/'probe'
    subprocess.run(['cc','-x','c','-','-o',str(exe)],input=pre+size+driver,text=True,check=True)
    subprocess.run([str(exe)],check=True)
host=(root/'src/ui/main_host.c').read_text()
assert 'case WM_PAINT:' in host, 'field host must paint invalidated background without an erase message'
print('window transitions: minimized geometry preserved; restored/maximized layout; field host owns paint')
