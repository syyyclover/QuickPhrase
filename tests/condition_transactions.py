"""Actual condition commit must not mix old match data with new headers on OOM."""
from pathlib import Path
import re
import subprocess
import tempfile
from source_helpers import INCLUDE_FLAGS, CORE_SOURCES

root=Path(__file__).resolve().parents[1]
source=(root/'src/ui/dialog_rules.c').read_text()
end=source.index('} CondDlgCtx;')+len('} CondDlgCtx;')
ctx=source[source.rfind('typedef struct {',0,end):end]
m=re.search(r'^static (?:void|int) cond_commit_term\([^;]+?\)\n\{',source,re.M)
body=source[m.start():source.index('\n}',m.end())+2]
preamble=r'''
#include "engine.h"
#include "test_fail_alloc.h"
#include <stdio.h>
#include <string.h>
typedef void *HWND;
#define BST_CHECKED 1
#define IDC_RL_NEGATE 1
#define MB_OK 1
#define MB_ICONERROR 2
static Segment trigger;
static int lv_sel(HWND h) {(void)h;return 0;}
static const Segment *combo_selected_seg(HWND h) {(void)h;return &trigger;}
static CondOperator cond_op_from_combo(HWND h) {(void)h;return OP_IN;}
static int IsDlgButtonChecked(HWND h,int i) {(void)h;(void)i;return BST_CHECKED;}
static void qp_get_text(HWND h,wchar_t *out,size_t cap) {(void)h;qp_strlcpy(out,L"New,Second",cap);}
static int qp_msgbox(HWND h,unsigned f,const wchar_t *t,const wchar_t *s,...) {
    (void)h;(void)f;(void)t;(void)s;return 0;
}
'''
driver=r'''
int main(void) {
    ConditionRule *rule=cond_rule_create();CondTerm *term=rule->terms;
    cond_term_set_match(term,L"Old");wcscpy(term->trigger_segment_id,L"OLD_TRIGGER");
    term->op=OP_EQUALS;wcscpy(trigger.id,L"NEW_TRIGGER");CondDlgCtx c={0};c.rule=rule;
    size_t base=fa_live;fa_inject(1);cond_commit_term(&c);fa_disable();
    assert(!wcscmp(term->trigger_segment_id,L"OLD_TRIGGER"));
    assert(term->op==OP_EQUALS && !term->negate && !wcscmp(term->match_value,L"Old"));
    assert(fa_live==base);
    cond_commit_term(&c);assert(!wcscmp(term->trigger_segment_id,L"NEW_TRIGGER"));
    assert(term->op==OP_IN && term->negate && term->match_value_count==2);
    cond_rule_free(rule);assert(!fa_live);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='qp-cond-txn-') as tmp:
    exe=Path(tmp)/'probe'
    code=preamble+ctx+'\nstatic void cond_row_text(CondDlgCtx *c,int row) {(void)c;(void)row;}\n'+body+driver
    subprocess.run(['cc','-std=gnu11','-Wall','-Wno-unused-function',*INCLUDE_FLAGS,'-I',str(root/'tests'),
                    '-x','c','-',*CORE_SOURCES,str(root/'src/core/table.c'),str(root/'src/core/storage.c'),
                    str(root/'src/core/regex.c'),str(root/'third_party/cjson/cJSON.c'),
                    '-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free','-o',str(exe)],
                   input=code,text=True,check=True)
    subprocess.run([str(exe)],cwd=tmp,check=True)
print('condition edit: match/header transaction preserves old state on failure')
