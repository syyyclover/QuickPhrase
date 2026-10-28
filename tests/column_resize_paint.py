"""Column changes must defer native list repaint to the enclosing layout."""
from pathlib import Path
import subprocess
import tempfile

s=(Path(__file__).resolve().parents[1]/'src/ui/text_paint.c').read_text(encoding='utf-8')
body=s[s.index('void qp_set_column_width_if_needed('):s.index('\nvoid qp_set_scroll_info_if_needed')]
pre=r'''
#include <assert.h>
#include <stddef.h>
typedef struct {int width,visible,painting,dirty,toggles;} Window;
typedef Window *HWND;
#define WM_SETREDRAW 1
#define FALSE 0
#define TRUE 1
#define RDW_INVALIDATE 1
#define RDW_ERASE 2
#define RDW_FRAME 4
#define RDW_ALLCHILDREN 8
static int ListView_GetColumnWidth(HWND h,int col){(void)col;return h->width;}
static void ListView_SetColumnWidth(HWND h,int col,int w){(void)col;h->width=w;if(h->visible)++h->painting;}
static int IsWindowVisible(HWND h){return h->visible;}
static void SendMessageW(HWND h,int msg,int on,int lp){(void)msg;(void)lp;h->visible=on;++h->toggles;}
static void RedrawWindow(HWND h,void *r,void *g,int flags){(void)r;(void)g;h->dirty|=flags;}
'''
driver=r'''
int main(void){
 Window w={100,1,0,0,0};
 qp_set_column_width_if_needed(&w,0,200);
 assert(w.width==200 && w.visible && !w.painting);
 assert(w.dirty==15 && w.toggles==2);
 w.dirty=w.toggles=0;
 qp_set_column_width_if_needed(&w,0,200);
 assert(!w.dirty && !w.toggles);
 w.visible=0;
 qp_set_column_width_if_needed(&w,0,300);
 assert(w.width==300 && !w.visible && !w.toggles && !w.painting);
 qp_set_column_width_if_needed(&w,0,-3);assert(w.width==0);
 qp_set_column_width_if_needed(NULL,0,200);
}
'''
with tempfile.TemporaryDirectory(prefix='qp-column-paint-') as tmp:
    exe=Path(tmp)/'probe'
    subprocess.run(['cc','-x','c','-','-o',str(exe)],input=pre+body+driver,text=True,check=True)
    subprocess.run([str(exe)],check=True)
print('column resize: defer paint, preserve hidden lists and skip unchanged widths')
