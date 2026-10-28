"""Prepare an isolated Win32 timing build; never modify production sources.

Build with the normal Makefile inside the emitted directory. Timing is buffered
in memory and written beside the executable only after closing the editor.
"""
from pathlib import Path
import shutil

root = Path(__file__).resolve().parents[1]
dest = root / 'build/editor-resize-trace/source'
dest.mkdir(parents=True, exist_ok=True)
for name in ('src', 'third_party', 'res', 'tests', 'tools', 'installer'):
    shutil.copytree(root / name, dest / name, dirs_exist_ok=True)
for name in ('Makefile', 'resource.rc'):
    shutil.copy2(root / name, dest / name)

(dest / 'src/ui/resize_trace.h').write_text(r'''
#include <windows.h>
LONGLONG qp_trace_start(void);
void qp_trace_end(const char *, UINT, LONGLONG);
void qp_trace_window_end(const char *, UINT, LONGLONG, HWND);
void qp_trace_hook_children(HWND);
void qp_trace_flush(void);
#define TRACE(label, expr) do { LONGLONG qt=qp_trace_start(); expr; qp_trace_end(label,0,qt); } while(0)
''', encoding='utf-8')
(dest / 'src/ui/resize_trace.c').write_text(r'''
#include "resize_trace.h"
#include <stdio.h>
#include <wchar.h>
#include <commctrl.h>
#include <stdlib.h>
typedef struct { const char *stage; UINT msg; LONGLONG start, ticks; int control,w,h; wchar_t cls[32]; } Trace;
static Trace records[65536];
static unsigned count, dropped;
LONGLONG qp_trace_start(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
void qp_trace_end(const char *stage, UINT msg, LONGLONG start) {
    LONGLONG end=qp_trace_start();
    if(count<65536) records[count++]=(Trace){.stage=stage,.msg=msg,.start=start,.ticks=end-start};
    else ++dropped;
}
void qp_trace_window_end(const char *stage, UINT msg, LONGLONG start, HWND hwnd) {
    unsigned index=count;
    qp_trace_end(stage,msg,start);
    if(count==index) return;
    Trace *r=&records[index]; RECT client={0};
    r->control=GetDlgCtrlID(hwnd);
    GetClientRect(hwnd,&client);r->w=client.right;r->h=client.bottom;
    GetClassNameW(hwnd,r->cls,32);
}
static LRESULT CALLBACK trace_child(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    (void)ref;
    if(msg==WM_NCDESTROY) { RemoveWindowSubclass(hwnd,trace_child,id);return DefSubclassProc(hwnd,msg,wp,lp); }
    int timed=msg==WM_SIZE || msg==WM_PAINT || msg==WM_ERASEBKGND || msg==WM_NCPAINT ||
              msg==WM_PRINTCLIENT || msg==LVM_SETCOLUMNWIDTH;
    LONGLONG start=timed?qp_trace_start():0;
    LRESULT result=DefSubclassProc(hwnd,msg,wp,lp);
    if(timed) qp_trace_window_end("child.message",msg,start,hwnd);
    return result;
}
static BOOL CALLBACK hook_child(HWND hwnd, LPARAM lp) {
    (void)lp; SetWindowSubclass(hwnd,trace_child,0x51505452,0);return TRUE;
}
void qp_trace_hook_children(HWND hwnd) { EnumChildWindows(hwnd,hook_child,0); }
void qp_trace_flush(void) {
    wchar_t path[MAX_PATH]; LARGE_INTEGER freq;
    DWORD n=GetModuleFileNameW(NULL,path,MAX_PATH);
    if(!n || n>=MAX_PATH) return;
    wchar_t *tail=wcsrchr(path,L'\\');
    if(!tail || tail-path>MAX_PATH-60) return;
    swprintf(tail+1,60,L"resize-timing-%lu.csv",(unsigned long)GetCurrentProcessId());
    FILE *f=_wfopen(path,L"ab");
    if(!f) return;
    QueryPerformanceFrequency(&freq);
    fprintf(f,"stage,message,start_ms,duration_ms (nested spans),control,width,height,class,dropped=%u\n",dropped);
    for(unsigned i=0;i<count;++i) {
        Trace *r=&records[i];
        fprintf(f,"%s,0x%04x,%.3f,%.3f,%d,%d,%d,%ls\n",r->stage,r->msg,
                1000.0*r->start/freq.QuadPart,1000.0*r->ticks/freq.QuadPart,
                r->control,r->w,r->h,r->cls);
    }
    fclose(f); count=0; dropped=0;
}
''', encoding='utf-8')

def edit(name, replacements):
    path = dest / 'src/ui' / name
    text = path.read_text(encoding='utf-8')
    for old, new in replacements:
        assert old in text, (name, old)
        text = text.replace(old, new)
    path.write_text('#include "resize_trace.h"\n' + text, encoding='utf-8')

edit('editor_layout.c', [
    ('qp_set_column_width_if_needed(c->hSegList,2,out.segment.w-DPIX(116));',
     'TRACE("segment.column", qp_set_column_width_if_needed(c->hSegList,2,out.segment.w-DPIX(116)));'),
    ('qp_layout_end(dwp);', 'TRACE("props.geometry", qp_layout_end(dwp));'),
    ('qp_layout_end(batch);', 'TRACE("editor.geometry", qp_layout_end(batch));'),
    ('ed_layout_props(c,out.body_bottom);', 'TRACE("props.total", ed_layout_props(c,out.body_bottom));'),
    ('qp_set_scroll_info_if_needed(c->hPropsHost, SB_VERT, &si);',
     'TRACE("props.scrollbar", qp_set_scroll_info_if_needed(c->hPropsHost, SB_VERT, &si));'),
    ('RedrawWindow(c->hwnd,NULL,NULL,RDW_UPDATENOW|RDW_ALLCHILDREN);',
     'TRACE("editor.paint", RedrawWindow(c->hwnd,NULL,NULL,RDW_UPDATENOW|RDW_ALLCHILDREN));'),
])
edit('window_presentation.c', [
    ('RedrawWindow(hwnd, NULL, NULL,\n                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);',
     'TRACE("present.paint", RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW));'),
    ('GdiFlush();', 'TRACE("present.gdi", GdiFlush());'),
    ('DwmFlush();', 'TRACE("present.dwm", DwmFlush());'),
])
edit('dialog_editor.c', [
    ('qp_theme_children(hwnd);', 'qp_theme_children(hwnd);\n        qp_trace_hook_children(hwnd);'),
    ('static LRESULT CALLBACK editor_proc(', 'static LRESULT CALLBACK editor_proc_impl('),
    ('ed_layout(c);', 'TRACE("editor.layout", ed_layout(c));'),
    ('qp_prepare_present(hwnd, &c->restore_cloaked);',
     'TRACE("present.prepare", qp_prepare_present(hwnd, &c->restore_cloaked));'),
    ('qp_finish_present(hwnd, &c->restore_cloaked);',
     'TRACE("present.finish", qp_finish_present(hwnd, &c->restore_cloaked));'),
    ('int dlg_edit_template(HWND owner, Template *working, int is_new)', '''
static LRESULT CALLBACK editor_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    int timed=msg==WM_SIZE || msg==WM_WINDOWPOSCHANGING || msg==WM_WINDOWPOSCHANGED ||
              msg==WM_PAINT || msg==WM_ERASEBKGND || msg==WM_NCPAINT || msg==WM_PRINTCLIENT ||
              msg==WM_SYSCOMMAND || msg==WM_ENTERSIZEMOVE || msg==WM_EXITSIZEMOVE ||
              msg==WM_LBUTTONDOWN || msg==WM_LBUTTONUP || msg==WM_MOUSEMOVE;
    LONGLONG start=timed?qp_trace_start():0;
    LRESULT result=editor_proc_impl(hwnd,msg,wp,lp);
    if(timed) qp_trace_window_end("editor.message",msg,start,hwnd);
    return result;
}
int dlg_edit_template(HWND owner, Template *working, int is_new)'''),
    ('int accepted=run_modal(owner, dlg, &c.result);',
     'int accepted=run_modal(owner, dlg, &c.result);\n    qp_trace_flush();'),
])
print(dest)
