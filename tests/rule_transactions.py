"""Run real editor/lookup modal ownership boundaries; double only OS UI calls."""
from pathlib import Path
import re
import subprocess
import tempfile
from source_helpers import INCLUDE_FLAGS, CORE_SOURCES

root=Path(__file__).resolve().parents[1]
editor=(root/'src/ui/dialog_editor.c').read_text()
lookup=(root/'src/ui/dialog_lookup.c').read_text()
def function(source,name):
    m=re.search(r'^(?:static )?(?:void|int) '+name+r'\([^;]+?\)\n\{',source,re.M)
    assert m,name
    return source[m.start():source.index('\n}',m.end())+2]
end=lookup.index('} LookupDlgCtx;')+len('} LookupDlgCtx;')
ctx=lookup[lookup.rfind('typedef struct {',0,end):end]
preamble=r'''
#include "engine.h"
#include "table.h"
#include "test_fail_alloc.h"
#include <stdio.h>
#include <string.h>
typedef void *HWND;
typedef struct { HWND hwnd; Template *t; int cur; HWND hParseList, hOptList; } EditorCtx;
static Segment *active;
static int accepted, reject_outer, modal_calls;
static Segment *ed_cur_seg(EditorCtx *c) {(void)c;return active;}
static int lv_sel(HWND h) {(void)h;return 0;}
static void ed_fill_rulelists(EditorCtx *c) {(void)c;}
static void lv_select(HWND h,int i) {(void)h;(void)i;}
static void ed_update_preview(EditorCtx *c) {(void)c;}
static void ed_fill_optlist(EditorCtx *c) {(void)c;}
static int dlg_input(HWND h,const wchar_t *t,const wchar_t *p,wchar_t *out,size_t cap) {
    (void)h;(void)t;(void)p;qp_strlcpy(out,L"New",cap);return 1;
}
static int dlg_option(HWND h,const wchar_t *t,wchar_t *out,size_t cap,int *custom,int available) {
    (void)available;*custom=0;return dlg_input(h,t,L"",out,cap);
}
static int qp_msgbox(HWND h,unsigned u,const wchar_t *t,const wchar_t *s,...) {
    (void)h;(void)u;(void)t;(void)s;return 0;
}
#define MB_OK 1
#define MB_ICONERROR 2
#define lookup_dlg_proc NULL
int dlg_lookup_rule(HWND,ParseRule *,const Template *);
static int dlg_parse_rule(HWND h,ParseRule *r,const Template *t,const Segment *s) {
    (void)s;int inner=dlg_lookup_rule(h,r,t);return reject_outer?0:inner;
}
'''
fake_modal=r'''
static HWND make_dlg(const wchar_t *a,void *b,const wchar_t *d,int w,int h,HWND o,void *p) {
    (void)a;(void)b;(void)d;(void)w;(void)h;(void)o;return p;
}
static int run_modal(HWND owner,HWND window,const int *flag) {
    (void)owner;(void)flag;LookupDlgCtx *c=window;++modal_calls;
    wcscpy(c->rule->lk_table_id,L"CHANGED");
    lookup_out_list_free(c->rule->lk_outs);
    c->rule->lk_outs=lookup_out_create();
    if(!c->rule->lk_outs) return 0;
    wcscpy(c->rule->lk_outs->col,L"NEW_COLUMN");
    return accepted; /* OS loop outcome; native command paths checked separately. */
}
'''
driver=r'''
static void prepare(void) {
    active=seg_create(SEG_TEXT);active->parse_rules=parse_rule_create();
    wcscpy(active->parse_rules->lk_table_id,L"ORIGINAL");
    active->parse_rules->lk_outs=lookup_out_create();
    wcscpy(active->parse_rules->lk_outs->col,L"OLD_COLUMN");
    active->parse_rules->next=parse_rule_create();
}
int main(int argc,char **argv) {
    assert(argc==2);prepare();ParseRule *orig=active->parse_rules;
    wchar_t id[QP_ID_MAX];wcscpy(id,orig->id);ParseRule *next=orig->next;
    size_t base=fa_live;
    if(!strcmp(argv[1],"cancel")) {
        accepted=0;assert(!dlg_lookup_rule(NULL,orig,NULL));
        assert(!wcscmp(orig->lk_table_id,L"ORIGINAL"));
        assert(!wcscmp(orig->lk_outs->col,L"OLD_COLUMN"));
    } else if(!strcmp(argv[1],"confirm")) {
        accepted=1;EditorCtx c={0};
        for(int i=0;i<100;++i) {
            ed_parse_edit(&c);assert(fa_live==base);
            assert(!wcscmp(orig->id,id) && orig->next==next);
            assert(!wcscmp(orig->lk_outs->col,L"NEW_COLUMN"));
        }
    } else if(!strcmp(argv[1],"outer-cancel")) {
        accepted=1;reject_outer=1;EditorCtx c={0};ed_parse_edit(&c);
        assert(!wcscmp(orig->lk_table_id,L"ORIGINAL"));
    } else if(!strcmp(argv[1],"clone-failure")) {
        accepted=1;fa_inject(1);assert(!dlg_lookup_rule(NULL,orig,NULL));fa_disable();
        assert(!modal_calls && !wcscmp(orig->lk_table_id,L"ORIGINAL"));
    } else if(!strcmp(argv[1],"option-edit")) {
        const wchar_t *values[]={L"Old"};seg_set_options(active,values,1);base=fa_live;
        EditorCtx c={0};fa_inject(1);ed_opt_edit(&c);fa_disable();
        assert(active->options[0] && !wcscmp(active->options[0],L"Old"));
    } else return 2;
    assert(fa_live==base);seg_free(active);assert(!fa_live);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='qp-rule-txn-') as tmp:
    exe=Path(tmp)/'probe'
    source=preamble+ctx+fake_modal+function(lookup,'dlg_lookup_rule')+function(editor,'ed_parse_edit')+function(editor,'ed_opt_edit')+driver
    subprocess.run(['cc','-std=gnu11','-Wall','-Wno-unused-function',*INCLUDE_FLAGS,
                    '-I',str(root/'tests'),'-x','c','-',*CORE_SOURCES,str(root/'src/core/table.c'),
                    str(root/'src/core/storage.c'),str(root/'src/core/regex.c'),str(root/'third_party/cjson/cJSON.c'),
                    '-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free','-o',str(exe)],
                   input=source,text=True,check=True)
    results=[]
    for case in ('cancel','confirm','outer-cancel','clone-failure','option-edit'):
        result=subprocess.run([str(exe),case],cwd=tmp)
        results.append(result.returncode)
    assert not any(results),results
print('rule transactions: cancel, 100 confirmations, outer cancel, clone failure passed')
