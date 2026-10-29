"""Exercise production control creation; Edit limits must match model buffers."""
from source_helpers import read_source, INCLUDE_FLAGS
import subprocess
import tempfile
from pathlib import Path

source=read_source('ui.c')
start=source.find('/* Input limits */')
if start<0: start=source.index('HWND qp_create(')
body=source[start:source.index('\nvoid qp_get_text(',start)]
pre=r'''
#include "engine.h"
#include "resource.h"
#include <assert.h>
#include <stdint.h>
#include <wchar.h>
typedef unsigned long DWORD;
typedef unsigned long WPARAM;
typedef long LPARAM;
typedef intptr_t INT_PTR;
typedef void *HMENU;
typedef struct { int limit; } Ctl;
typedef Ctl *HWND;
static Ctl control;
static void *g_hInst,*g_fontUI;
#define WS_CHILD 1
#define WS_VISIBLE 2
#define WM_SETFONT 3
#define EM_SETLIMITTEXT 4
#define TRUE 1
static HWND CreateWindowExW(DWORD ex,const wchar_t *cls,const wchar_t *text,DWORD style,int x,int y,int w,int h,HWND p,HMENU id,void *inst,void *v) {
    (void)ex;(void)cls;(void)text;(void)style;(void)x;(void)y;(void)w;(void)h;(void)p;(void)id;(void)inst;(void)v;
    control.limit=32767;return &control;
}
static void SendMessageW(HWND h,int msg,WPARAM wp,LPARAM lp) { (void)lp;if(msg==EM_SETLIMITTEXT)h->limit=(int)wp; }
static void theme_control(HWND h) { (void)h; }
static void qp_buffer_static(HWND h) { (void)h; }
static int lstrcmpiW(const wchar_t *a,const wchar_t *b) { return wcscmp(a,b); }
'''
driver=r'''
int main(void) {
    const int ids[]={999,IDC_ED_NAME,IDC_ED_SEG_LABEL,IDC_ED_SEG_CONTENT,IDC_ED_TABLE_FORMAT};
    const int want[]={2047,255,127,1023,32767};
    for(int i=0;i<5;++i) {
        HWND h=qp_create(L"Edit",L"",0,0,0,0,10,10,NULL,ids[i]);
        assert(h->limit==want[i]);
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='qp-input-') as tmp:
    exe=Path(tmp)/'test'
    subprocess.run(['cc','-std=gnu11','-Wall',*INCLUDE_FLAGS,'-x','c','-','-o',str(exe)],input=pre+body+driver,text=True,check=True)
    subprocess.run([str(exe)],check=True)
print('input limits: model, names, labels, content and table format capacities passed')
