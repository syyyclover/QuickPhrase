"""Same-result publication must retain reading position; production C adapter."""
import runpy
import subprocess
import tempfile
from pathlib import Path

base=runpy.run_path(str(Path(__file__).with_name('preview_allocations.py')))
glue='static int layouts;\n'+base['glue'].replace('Preview *p=h;', '++layouts;Preview *p=h;')
driver=r'''
#undef malloc
#undef realloc
int main(void) {
    Preview p={0};int marks[]={1};
    qp_preview_set_text(&p,L"long unchanged text",marks,1);
    p.scrollX=9;p.scrollY=72;p.selAnchor=2;p.selEnd=8;
    int before=layouts;attempts=0;
    qp_preview_set_text(&p,L"long unchanged text",marks,1);
    assert(p.scrollX==9 && p.scrollY==72 && p.selAnchor==2 && p.selEnd==8);
    assert(layouts==before && !attempts);
    g_fontMono=(HFONT)1;g_dpi=144;
    qp_preview_set_text(&p,L"long unchanged text",marks,1);
    assert(layouts==before+1 && p.measuredFont==g_fontMono && p.measuredDpi==144);
    assert(p.selAnchor==2 && p.selEnd==8 && p.scrollY==72);
    marks[0]=3;qp_preview_set_text(&p,L"long unchanged text",marks,1);
    assert(p.selEnd==-1 && layouts>before);
    free(p.disp);free(p.kind);free(p.sourceMap);free(p.glyphWidths);free(p.raw);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='qp-preview-refresh-') as tmp:
    exe=Path(tmp)/'test'
    subprocess.run(['cc','-std=gnu11','-Wall',*base['INCLUDE_FLAGS'],'-x','c','-',
                    str(base['ROOT']/'src/core/textview.c'),str(base['ROOT']/'src/core/strings.c'),
                    str(base['ROOT']/'src/core/unicode.c'),'-o',str(exe)],
                   input=base['pre']+base['model']+glue+base['function']('pv_build_disp')+
                         base['function']('qp_preview_set_text')+driver,text=True,check=True)
    subprocess.run([str(exe)],check=True)
print('preview refresh: identical text/marks keep selection, scroll and model; changed marks publish')
