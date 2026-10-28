"""Run real multi-select publication/toggle; over-capacity selection is refused atomically."""
from pathlib import Path
import re, subprocess, tempfile
from source_helpers import ROOT, INCLUDE_FLAGS, CORE_SOURCES
s=(ROOT/'src/ui/main_popup.c').read_text()
def fn(name):
    m=re.search(r'^(?:static )?(?:void|int|const wchar_t \*)\s*'+name+r'\([^;]+?\)\n\{',s,re.M)
    assert m,name
    return s[m.start():s.index('\n}',m.end())+2]+'\n'
pre=r'''
#include "engine.h"
#include <assert.h>
#include <stdio.h>
static Settings g_settings;
static Segment *g_popupSeg;
static EvalContext *g_ctx;
static int g_popup=1,g_popupField,g_popupCount=2,g_hHint,refreshes,hints;
static unsigned char checked[2],*g_popupChecked=checked;
static void ui_choice_multi(int index,const unsigned char *mask,int count){(void)index;(void)mask;(void)count;assert(0);}
static void ui_refresh_current(void){++refreshes;}
static void popup_invalidate_row(int row){(void)row;}
static void qp_set_text_if_diff(int h,const wchar_t *s){(void)h;assert(s&&s[0]);++hints;}
'''
driver=r'''
int main(void){
    Template *t=tmpl_create(L"Multi");g_popupSeg=seg_create(SEG_MULTI_SELECT);assert(t&&g_popupSeg);
    seg_list_append(&t->segments,g_popupSeg);wchar_t a[1501],b[1501];
    wmemset(a,L'a',1500);a[1500]=0;wmemset(b,L'b',1500);b[1500]=0;
    const wchar_t *opts[]={a,b};assert(qp_seg_set_options(g_popupSeg,opts,2));
    g_ctx=eval_create(t);assert(g_ctx);popup_toggle(0);
    assert(checked[0] && !checked[1] && wcslen(g_ctx->fields[0].user_value)==1500);
    int before=refreshes;popup_toggle(1);
    assert(checked[0] && !checked[1]); /* Never claim both are selected after clipping. */
    assert(refreshes==before && hints && !wcscmp(g_ctx->fields[0].user_value,a));
    wchar_t shortb[547];wmemset(shortb,L'b',546);shortb[546]=0;
    opts[1]=shortb;assert(qp_seg_set_options(g_popupSeg,opts,2));popup_toggle(1);
    assert(checked[1] && wcslen(g_ctx->fields[0].user_value)==2047 && !g_ctx->fields[0].input_flags);
    /* A two-unit surrogate pair must be accepted whole or not selected at all. */
    wchar_t pair[]={0xD83D,0xDE00,0};opts[0]=g_ctx->fields[0].user_value;opts[1]=pair;
    assert(qp_seg_set_options(g_popupSeg,opts,2));checked[1]=0;popup_toggle(1);
    assert(!checked[1] && wcslen(g_ctx->fields[0].user_value)==2047);
    popup_toggle(0);assert(!g_ctx->fields[0].user_value[0]);
    eval_destroy(g_ctx);tmpl_free(t);puts("multi-select: overflow/UTF-16 refused atomically; exact fit and deselection work");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='qp-popup-limits-') as tmp:
    exe=Path(tmp)/'test'
    subprocess.run(['cc','-std=gnu11','-Wall',*INCLUDE_FLAGS,'-x','c','-',*CORE_SOURCES,
        str(ROOT/'src/core/regex.c'),str(ROOT/'src/core/storage.c'),str(ROOT/'src/core/table.c'),
        str(ROOT/'third_party/cjson/cJSON.c'),'-o',str(exe)],
        input=pre+''.join(fn(n) for n in ('multi_sep','popup_build_value','popup_apply','popup_toggle'))+driver,text=True,check=True)
    subprocess.run([str(exe)],check=True)
