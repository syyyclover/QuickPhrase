"""Cross-mode OOM must keep mode/model/raw as one visible generation."""
from pathlib import Path
import runpy,subprocess,tempfile
from source_helpers import ROOT,INCLUDE_FLAGS
base=runpy.run_path(str(Path(__file__).with_name('preview_table_width.py')))
fn=base['function']
glue=r'''
#include "textview.h"
#include <assert.h>
#include "test_fail_alloc.h"
#define QP_PREVIEW_TEXT 0
#define QP_PREVIEW_TABLE 1
static void pv_relayout(HWND h){Preview *p=h;p->measureDirty=0;p->measuredFont=g_fontMono;p->measuredDpi=g_dpi;}
'''
driver=r'''
#undef malloc
#undef calloc
int main(void){
    for(int point=1;point<=5;++point){
        Preview p={0};fault=0;attempts=0;
        qp_preview_set_text(&p,L"old",NULL,0);
        qp_preview_set_mode(&p,QP_PREVIEW_TABLE);
        qp_preview_set_table_marked(&p,L"TABLE",L"",NULL,0);
        assert(p.mode==QP_PREVIEW_TABLE && !wcscmp(p.raw,L"TABLE"));
        fa_inject(point);qp_preview_set_mode(&p,QP_PREVIEW_TEXT);
        assert(!qp_preview_set_text(&p,L"new longer value",NULL,0));fa_disable();assert(fa_hit);
        assert(p.mode==QP_PREVIEW_TABLE && p.modelMode==OUTPUT_TABLE && !wcscmp(p.raw,L"TABLE"));
        fault=0;qp_preview_set_text(&p,L"new longer value",NULL,0);
        wchar_t copied[100];qp_tv_copy_raw(p.raw,p.sourceMap,p.dispLen,0,p.dispLen,copied,100);
        assert(p.mode==QP_PREVIEW_TEXT && !wcscmp(copied,L"new longer value") && !wcscmp(copied,p.disp));
        fa_inject(point);qp_preview_set_mode(&p,QP_PREVIEW_TABLE);
        assert(!qp_preview_set_table_marked(&p,L"OTHER",L"",NULL,0));fa_disable();assert(fa_hit);
        assert(p.mode==QP_PREVIEW_TEXT && !wcscmp(p.raw,L"new longer value"));
        free(p.disp);free(p.kind);free(p.sourceMap);free(p.glyphWidths);free(p.raw);
        free(p.cells);free(p.colw);free(p.hdr);free(p.modelFormat);
        assert(!fa_live);
    }
    puts("preview: cross-mode allocation failures retain one mode/model/raw generation; successful copy matches");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='qp-preview-publish-') as tmp:
    pre=base['preamble'].replace('#define malloc allocate','').replace('#define calloc allocate_zero','')
    exe=Path(tmp)/'test';code=pre+base['structs']+base['consts']+base['helpers']+glue+fn('pv_build_disp')+fn('qp_preview_set_mode')+fn('qp_preview_set_text')+base['target']+driver
    subprocess.run(['cc','-std=gnu11','-Wall','-Wno-unused-variable','-Wno-unused-function',*INCLUDE_FLAGS,'-I',str(ROOT/'tests'),'-x','c','-',
        str(ROOT/'src/core/strings.c'),str(ROOT/'src/core/unicode.c'),str(ROOT/'src/core/textview.c'),
        '-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free','-o',str(exe)],input=code,text=True,check=True)
    subprocess.run([str(exe)],check=True)
