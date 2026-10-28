"""Sweep display growth failures after a populated production preview.

Only HWND/GDI notification glue is doubled. Production display construction,
source mapping and raw-copy helpers must remain consistent after every fault.
"""
from source_helpers import read_source, INCLUDE_FLAGS, ROOT
import re
import subprocess
import tempfile
from pathlib import Path

source=read_source('preview.c')
def function(name):
    m=re.search(r'^(?:static )?(?:void|int) '+name+r'\([^;]+?\)\n\{',source,re.M)
    assert m,name
    return source[m.start():source.index('\n}',m.end())+2]
model=source[source.index('typedef struct { int off;'):source.index('} Preview;')+len('} Preview;')]
pre=r'''
#include "engine.h"
#include "textview.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <assert.h>
typedef void *HWND;
typedef void *HFONT;
static HFONT g_fontMono;
static int g_dpi=96;
#define PV_MAX_MARKS 512
#define FALSE 0
static const wchar_t *PV_TEXT_MARK=L"失败";
static int fault,attempts;
static void *allocate(size_t n) { return ++attempts==fault ? NULL : malloc(n); }
static void *resize(void *p,size_t n) __attribute__((unused));
static void *resize(void *p,size_t n) { return ++attempts==fault ? NULL : realloc(p,n); }
#define malloc allocate
#define realloc resize
'''
glue=r'''
static Preview *pv_of(HWND h) { return h; }
static void pv_relayout(HWND h) { Preview *p=h;p->measuredFont=g_fontMono;p->measuredDpi=g_dpi;p->measureDirty=0; }
static void InvalidateRect(HWND h,void *r,int erase) { (void)h;(void)r;(void)erase; }
'''
driver=r'''
#undef malloc
#undef realloc
int main(void) {
    int i;
    for(i=1;i<=5;++i) {
        Preview p={0};wchar_t copied[128];
        fault=0;attempts=0;qp_preview_set_text(&p,L"old",NULL,0);
        assert(p.disp && !wcscmp(p.disp,L"old"));
        fault=i;attempts=0;qp_preview_set_text(&p,L"new longer value",NULL,0);
        qp_tv_copy_raw(p.raw,p.sourceMap,p.dispLen,0,p.dispLen,copied,128);
        assert(p.disp && !wcscmp(p.disp,copied));
        free(p.disp);free(p.kind);free(p.sourceMap);free(p.glyphWidths);free(p.raw);
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='qp-preview-oom-') as tmp:
    exe=Path(tmp)/'test'
    subprocess.run(['cc','-std=gnu11','-Wall',*INCLUDE_FLAGS,'-x','c','-',
                    str(ROOT/'src/core/textview.c'),str(ROOT/'src/core/strings.c'),str(ROOT/'src/core/unicode.c'),'-o',str(exe)],
                   input=pre+model+glue+function('pv_build_disp')+function('qp_preview_set_text')+driver,text=True,check=True)
    subprocess.run([str(exe)],check=True)
print('preview allocations: every failed growth keeps display and copied raw text consistent')
