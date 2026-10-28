"""Execute editor transition dispatch with an OS adapter, not a visual test."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/ui/dialog_editor.c').read_text(encoding='utf-8')
start = source.find('    case WM_WINDOWPOSCHANGING:')
end = source.find('    case WM_GETMINMAXINFO:')
cases = source[start:end] if start >= 0 else ''
pre = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
typedef int HWND;typedef intptr_t LPARAM;typedef intptr_t LRESULT;typedef unsigned long WPARAM;
typedef struct {unsigned flags;} WINDOWPOS;
typedef struct {int maximized,restore_cloaked;} EditorCtx;
#define WM_WINDOWPOSCHANGING 1
#define WM_WINDOWPOSCHANGED 2
#define WM_TIMER 3
#define QP_TIMER_PRESENT 3
#define SWP_NOSIZE 1
#define SWP_SHOWWINDOW 64
static int zoomed,visible=1,iconic,prepared,presented,layout_done,nested;
static EditorCtx ctx;
static int IsZoomed(HWND h){(void)h;return zoomed;}
static int IsWindowVisible(HWND h){(void)h;return visible;}
static int IsIconic(HWND h){(void)h;return iconic;}
static void qp_prepare_present(HWND h,int *pending){(void)h;assert(!layout_done);++prepared;*pending=1;}
static void qp_finish_present(HWND h,int *pending){(void)h;if(!*pending)return;assert(layout_done);++presented;*pending=0;}
static LRESULT dispatch(int msg,WPARAM wp,LPARAM lp);
static LRESULT DefWindowProcW(HWND h,int msg,WPARAM wp,LPARAM lp){
 (void)h;(void)msg;(void)wp;(void)lp;
 if(!nested){WINDOWPOS z={SWP_NOSIZE};nested=1;dispatch(WM_WINDOWPOSCHANGED,0,(LPARAM)&z);nested=0;}
 if(!nested)layout_done=1;
 return 123;
}
static LRESULT dispatch(int msg,WPARAM wp,LPARAM lp){HWND hwnd=1;EditorCtx *c=&ctx;switch(msg){
'''
driver = r'''
}return -1;}
int main(void){
 WINDOWPOS resized={0},zorder={SWP_NOSIZE};
 zoomed=1;dispatch(WM_WINDOWPOSCHANGING,0,(LPARAM)&resized);
 assert(prepared==1 && ctx.restore_cloaked); /* Must hide before first erase. */
 nested=1;dispatch(WM_WINDOWPOSCHANGED,0,(LPARAM)&zorder);nested=0;
 assert(!presented && ctx.restore_cloaked);
 assert(dispatch(WM_WINDOWPOSCHANGED,0,(LPARAM)&resized)==123);
 assert(layout_done && presented==1 && !ctx.restore_cloaked);
 layout_done=0;dispatch(WM_WINDOWPOSCHANGING,0,(LPARAM)&resized);
 assert(prepared==1); /* Border sizing alone must not cloak the editor. */
 zoomed=0;dispatch(WM_WINDOWPOSCHANGING,0,(LPARAM)&resized);
 assert(prepared==2 && ctx.restore_cloaked);
 dispatch(WM_WINDOWPOSCHANGED,0,(LPARAM)&resized);assert(presented==2);
 ctx.restore_cloaked=1;dispatch(WM_TIMER,QP_TIMER_PRESENT,0);assert(presented==3);
 layout_done=0;visible=0;zoomed=1;dispatch(WM_WINDOWPOSCHANGING,0,(LPARAM)&resized);assert(prepared==2);
 visible=1;iconic=1;dispatch(WM_WINDOWPOSCHANGING,0,(LPARAM)&resized);assert(prepared==2);
}
'''
with tempfile.TemporaryDirectory(prefix='qp-editor-resize-') as tmp:
    exe = Path(tmp) / 'probe'
    subprocess.run(['cc', '-std=gnu11', '-x', 'c', '-', '-o', str(exe)],
                   input=pre+cases+driver, text=True, check=True)
    subprocess.run([str(exe)], check=True)
print('editor resize: prepare before maximize; finish after layout; nested z-order, retry and ordinary sizing')
