"""Tray show/restore must repaint unchanged children; OS adapter, not visual QA."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'src/platform/main.c').read_text()
start = source.index('void app_show_main(void)')
body = source[start:source.index('\n}', start) + 2]
pre = r'''
#include <assert.h>
#include <stddef.h>
typedef int HWND;
static HWND g_hwndMain=1;
static struct {int win_maximized;} g_settings;
static int visible,iconic,redraws,shows,command,order;
#define SW_SHOWMAXIMIZED 3
#define SW_SHOW 5
#define SW_RESTORE 9
#define RDW_INVALIDATE 1
#define RDW_ERASE 2
#define RDW_ALLCHILDREN 4
#define RDW_UPDATENOW 8
static int IsWindowVisible(HWND h){(void)h;return visible;}
static int IsIconic(HWND h){(void)h;return iconic;}
static void ShowWindow(HWND h,int cmd){(void)h;command=cmd;++shows;order=1;}
static void RedrawWindow(HWND h,void *r,void *rg,int flags){(void)h;(void)r;(void)rg;assert(flags==15);++redraws;order=2;}
static void BringWindowToTop(HWND h){(void)h;assert(order==2);}
static void SetForegroundWindow(HWND h){(void)h;}
static void SetActiveWindow(HWND h){(void)h;}
'''
driver = r'''
int main(void){app_show_main();assert(shows==1 && command==SW_SHOW && redraws==1);
g_settings.win_maximized=1;app_show_main();assert(command==SW_SHOWMAXIMIZED && redraws==2);
visible=1;iconic=1;app_show_main();assert(command==SW_RESTORE && redraws==3);
iconic=0;app_show_main();assert(shows==3 && redraws==4);
g_hwndMain=0;app_show_main();assert(redraws==4);}
'''
with tempfile.TemporaryDirectory(prefix='qp-show-') as tmp:
    exe = Path(tmp) / 'probe'
    subprocess.run(['cc', '-x', 'c', '-', '-o', str(exe)], input=pre+body+driver, text=True, check=True)
    subprocess.run([str(exe)], check=True)
print('window show: hidden/visible/minimized/maximized repaint before activation')
