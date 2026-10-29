"""Resize dispatch must not repaint every child a second time after layout."""
from pathlib import Path
import subprocess
import tempfile

s=(Path(__file__).resolve().parents[1]/'src/ui/dialog_editor.c').read_text(encoding='utf-8')
case=s[s.index('    case WM_SIZE:'):s.index('    /* 拖完边框')]
pre=r'''
#include <assert.h>
#include <stddef.h>
typedef int HWND;
typedef struct {int left,top,right,bottom;} RECT;
typedef struct {int lastClientW,lastClientH;} EditorCtx;
#define WM_SIZE 1
#define SIZE_RESTORED 0
#define SIZE_MINIMIZED 1
#define SIZE_MAXIMIZED 2
#define RDW_INVALIDATE 1
#define RDW_ERASE 2
#define RDW_ALLCHILDREN 4
#define RDW_UPDATENOW 8
static struct {int editor_w,editor_h;} g_settings;
static int layouts,full_redraws,width=900,height=700;
static void GetClientRect(HWND h,RECT *r){(void)h;*r=(RECT){0,0,width,height};}
static void ed_layout(EditorCtx *c){(void)c;++layouts;}
static void RedrawWindow(HWND h,void *r,void *g,int f){(void)h;(void)r;(void)g;if((f&12)==12)++full_redraws;}
static int dispatch(EditorCtx *c,int wp){HWND hwnd=1;switch(WM_SIZE){
'''
driver=r'''
}return -1;}
int main(void){EditorCtx c={0};
 dispatch(&c,SIZE_RESTORED);assert(layouts==1 && !full_redraws);
 assert(g_settings.editor_w==900 && g_settings.editor_h==700);
 dispatch(&c,SIZE_RESTORED);assert(layouts==1);
 width=1800;height=1100;dispatch(&c,SIZE_MAXIMIZED);
 assert(layouts==2 && !full_redraws && g_settings.editor_w==900);
 dispatch(&c,SIZE_MINIMIZED);assert(layouts==2);
}
'''
with tempfile.TemporaryDirectory(prefix='qp-editor-cost-') as tmp:
    exe=Path(tmp)/'probe'
    subprocess.run(['cc','-x','c','-','-o',str(exe)],input=pre+case+driver,text=True,check=True)
    subprocess.run([str(exe)],check=True)
print('editor resize cost: one layout, no redundant full synchronous child redraw, normal-size persistence preserved')
