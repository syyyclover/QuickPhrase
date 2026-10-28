"""Actual copy action must reject incomplete/failed output before clipboard IO."""
from pathlib import Path
import re
import subprocess
import tempfile
from source_helpers import INCLUDE_FLAGS
root=Path(__file__).resolve().parents[1]
s=(root/'src/ui/main_actions.c').read_text();m=re.search(r'^void ui_copy_current\([^;]+?\)\n\{',s,re.M)
body=s[m.start():s.index('\n}',m.end())+2]
pre=r'''
#include "engine.h"
#include <assert.h>
#include <stdio.h>
#include <wchar.h>
typedef void *HWND;
#define TIMER_COPY_FEEDBACK 1
static EvalContext *g_ctx=(void *)1;
static HWND g_hwndMain,g_hHint,g_hBtnCopy;
static OutputMode g_previewMode=OUTPUT_TEXT;
static QpRenderStatus status;
static int copies,publish_ok=1,published,enabled;
#define TRUE 1
#define FALSE 0
static int EnableWindow(HWND h,int v){(void)h;enabled=v;return 1;}
static int ui_preview_show_result(const wchar_t *out,const int *m,int n){
    (void)m;(void)n;assert(!wcscmp(out,L"RESULT"));++published;return publish_ok;
}
static void ui_sync_preview_mode_controls(void) {}
static size_t ui_render_current(wchar_t *out,size_t n,int *m,int c,int *mc) {(void)n;(void)m;(void)c;(void)mc;wcscpy(out,L"RESULT");return 6;}
static QpRenderResult ui_render_current_checked(wchar_t *out,size_t n,int *m,int c) {
    (void)n;assert(m && c>0);wcscpy(out,L"RESULT");return (QpRenderResult){status,6,0};
}
static int qp_copy_to_clipboard(HWND h,const wchar_t *s) {(void)h;assert(!wcscmp(s,L"RESULT"));++copies;return 1;}
static int SetWindowTextW(HWND h,const wchar_t *s) {(void)h;(void)s;return 1;}
static void qp_set_text_if_diff(HWND h,const wchar_t *s) {(void)h;(void)s;}
static const wchar_t *qp_render_status_hint(QpRenderStatus s) {(void)s;return L"FAILED";}
static int SetTimer(HWND h,int n,int t,void *f) {(void)h;(void)n;(void)t;(void)f;return 1;}
'''
driver=r'''
int main(void) {
    status=QP_RENDER_TRUNCATED;ui_copy_current();assert(!copies);
    status=QP_RENDER_NOMEM;ui_copy_current();assert(!copies);
    status=QP_RENDER_INVALID;ui_copy_current();assert(!copies);
    status=QP_RENDER_OK;publish_ok=0;ui_copy_current();assert(!copies && !enabled);
    publish_ok=1;ui_copy_current();assert(copies==1 && published==2);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='qp-copy-status-') as tmp:
    exe=Path(tmp)/'probe'
    subprocess.run(['cc','-std=gnu11','-Wall','-Wno-unused-function',*INCLUDE_FLAGS,'-x','c','-','-o',str(exe)],
                   input=pre+body+driver,text=True,check=True)
    subprocess.run([str(exe)],check=True)
print('copy: incomplete/failed/invalid results never reach clipboard; complete result does')
