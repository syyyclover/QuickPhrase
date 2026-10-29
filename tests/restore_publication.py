"""Restore publication ordering and DWM failure recovery; OS adapter, not visual QA."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/ui/main_window.c').read_text(encoding='utf-8')
shared = (root / 'src/ui/window_presentation.c').read_text(encoding='utf-8')
helpers = 'static int restore_cloaked;\n' + shared[shared.index('void qp_prepare_present'):]
cases = source[source.index('    case WM_QUERYOPEN:'):source.index('    case WM_QP_CHOICE_INPUT:')]
cases += source[source.index('    case WM_TIMER:'):source.index('    case WM_PAINT:')]
pre = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
typedef int HWND; typedef int BOOL; typedef intptr_t LRESULT; typedef intptr_t LPARAM;
typedef unsigned long WPARAM; typedef struct {unsigned flags;} WINDOWPOS;
#define TRUE 1
#define FALSE 0
#define DWMWA_CLOAK 13
#define SUCCEEDED(h) ((h)>=0)
#define RDW_INVALIDATE 1
#define RDW_ERASE 2
#define RDW_ALLCHILDREN 4
#define RDW_UPDATENOW 8
#define SWP_SHOWWINDOW 64
#define SWP_NOSIZE 1
#define WM_QUERYOPEN 1
#define WM_SHOWWINDOW 2
#define WM_WINDOWPOSCHANGED 3
#define WM_TIMER 4
#define QP_TIMER_PRESENT 3
#define TIMER_COPY_FEEDBACK 1
#define TIMER_SAVE_PLACEMENT 2
static int fail_begin,fail_end,actual_cloak,paints,gdi,composed,timer,visible,iconic,defaults;
static int DwmSetWindowAttribute(HWND h,int attr,const BOOL *v,int size){
 (void)h;(void)attr;(void)size;
 if(*v){if(fail_begin)return -1;actual_cloak=1;return 0;}
 assert(paints && gdi && composed);
 if(fail_end){--fail_end;return -1;}actual_cloak=0;return 0;
}
static void RedrawWindow(HWND h,void *r,void *rg,int flags){(void)h;(void)r;(void)rg;assert(flags==15);++paints;}
static void GdiFlush(void){assert(paints);gdi=1;}
static void DwmFlush(void){assert(gdi);composed=1;}
static int IsWindowVisible(HWND h){(void)h;return visible;}
static int IsIconic(HWND h){(void)h;return iconic;}
static LRESULT DefWindowProcW(HWND h,int m,WPARAM w,LPARAM l){(void)h;(void)m;(void)w;(void)l;++defaults;return 123;}
static void SetTimer(HWND h,int id,int ms,void *cb){(void)h;(void)id;(void)ms;(void)cb;timer=1;}
static void KillTimer(HWND h,int id){(void)h;(void)id;timer=0;}
static void ui_update_preview_mode_labels(void){}
static void save_placement(HWND h){(void)h;}
static void app_save_settings(void){}
'''
driver = r'''
static LRESULT dispatch(int msg,WPARAM wp,LPARAM lp){HWND hwnd=1;switch(msg){
'''+cases+r'''
}return 0;}
int main(void){
 WINDOWPOS show={SWP_SHOWWINDOW|SWP_NOSIZE},zorder={SWP_NOSIZE},restore={0};
 fail_begin=1;qp_prepare_present(1, &restore_cloaked);assert(!restore_cloaked&&!actual_cloak);qp_finish_present(1, &restore_cloaked);assert(!paints);
 fail_begin=0;dispatch(WM_SHOWWINDOW,1,0);assert(restore_cloaked&&actual_cloak&&!paints);
 visible=1;dispatch(WM_WINDOWPOSCHANGED,0,(LPARAM)&zorder);assert(actual_cloak&&!paints);
 assert(dispatch(WM_WINDOWPOSCHANGED,0,(LPARAM)&show)==123);assert(!actual_cloak&&!restore_cloaked&&paints==1);
 iconic=1;dispatch(WM_QUERYOPEN,0,0);assert(actual_cloak);iconic=0;
 dispatch(WM_WINDOWPOSCHANGED,0,(LPARAM)&restore);assert(!actual_cloak&&paints==2);
 qp_prepare_present(1, &restore_cloaked);fail_end=1;qp_finish_present(1, &restore_cloaked);
 assert(actual_cloak&&restore_cloaked&&timer); /* Must retain a recovery path. */
 dispatch(WM_TIMER,QP_TIMER_PRESENT,0);assert(!actual_cloak&&!restore_cloaked&&!timer);
 dispatch(WM_SHOWWINDOW,1,0);assert(!actual_cloak); /* Already visible: no cloak. */
}
'''
with tempfile.TemporaryDirectory(prefix='qp-publication-') as tmp:
    exe = Path(tmp) / 'probe'
    subprocess.run(['cc', '-x', 'c', '-', '-o', str(exe)], input=pre+helpers+driver, text=True, check=True)
    subprocess.run([str(exe)], check=True)
print('restore publication: prepare before exposure; paint/flush before release; unsupported API and failed release recovery')
