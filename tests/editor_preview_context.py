"""Editor scratch reuse must never retain freed/reordered segment ownership."""
from source_helpers import ROOT, INCLUDE_FLAGS, CORE_SOURCES
from pathlib import Path
import re,subprocess,tempfile
src=(ROOT/'src/ui/dialog_editor.c').read_text()
m=re.search(r'^static EvalContext \*ed_preview_context\([^;]+?\)\n\{',src,re.M)
if not m: raise SystemExit('RED: missing editor preview context lifetime helper')
fn=src[m.start():src.index('\n}',m.end())+2]
code=r'''
#include "engine.h"
#include <assert.h>
typedef struct {Template *t; EvalContext *previewCtx;} EditorCtx;
'''+fn+r'''
int main(void) {
    Template *t=tmpl_create(L"Editor");Segment *a=seg_create(SEG_TEXT),*b=seg_create(SEG_TEXT);
    seg_list_append(&t->segments,a);seg_list_append(&t->segments,b);
    EditorCtx c={t,0};EvalContext *p=ed_preview_context(&c);assert(p);
    assert(ed_preview_context(&c)==p);
    seg_list_move(&t->segments,0,1);
    p=ed_preview_context(&c);assert(p && p->fields[0].seg==b && p->fields[1].seg==a);
    Segment *removed=seg_list_remove_at(&t->segments,0);seg_free(removed);
    p=ed_preview_context(&c);assert(p && p->count==1 && p->fields[0].seg==a);
    eval_destroy(c.previewCtx);tmpl_free(t);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='qp-editor-context-') as tmp:
    exe=Path(tmp)/'test'
    sources=[str(p) for p in (ROOT/'src/core').glob('*.c')]+[str(ROOT/'third_party/cjson/cJSON.c')]
    subprocess.run(['cc','-std=gnu11',*INCLUDE_FLAGS,'-x','c','-',*sources,'-o',str(exe)],input=code,text=True,check=True)
    subprocess.run([str(exe)],check=True)
print('editor context: reuse, reorder and deleted borrowed segment lifetime checked')
