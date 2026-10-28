"""A late field notification cannot read an empty/new context during rebuild."""
from pathlib import Path
import re
import subprocess
import tempfile
from source_helpers import INCLUDE_FLAGS, CORE_SOURCES
root=Path(__file__).resolve().parents[1]
s=(root/'src/ui/main_window.c').read_text()
m=re.search(r'^static void handle_field_command\([^;]+?\)\n\{',s,re.M)
body=s[m.start():s.index('\n}',m.end())+2]
h=(root/'src/ui/ui_internal.h').read_text()
end=h.index('} FieldCtl;')+len('} FieldCtl;')
field=h[h.rfind('typedef struct {',0,end):end]
preamble=r'''
#include "engine.h"
#include "resource.h"
#include <assert.h>
typedef void *HWND;
typedef unsigned short WORD;
typedef unsigned long WPARAM;
typedef long LPARAM;
#define BN_CLICKED 0
#define EN_CHANGE 1
#define CBN_SELCHANGE 2
#define CBN_SELENDOK 5
#define CB_GETCURSEL 3
#define CB_GETLBTEXT 4
#define CB_ERR (-1)
static long SendMessageW(HWND h,int m,unsigned long w,long l) {(void)h;(void)m;(void)w;(void)l;return -1;}
static void qp_get_text(HWND h,wchar_t *s,size_t n) {(void)h;(void)s;(void)n;assert(!"stale control read");}
static void ui_refresh_current(void) {assert(!"stale notification evaluated");}
static void ui_choice_single_queue(int index,HWND h){(void)index;(void)h;assert(!"stale custom input");}
static void popup_open(int i,HWND h) {(void)i;(void)h;assert(!"stale popup");}
'''
driver=r'''
int main(void) {
    Template *empty=tmpl_create(L"Empty");g_ctx=eval_create(empty);assert(g_ctx && !g_ctx->count);
    FieldCtl field={0};g_fields=&field;g_fieldCount=1;g_syncFields=1;
    handle_field_command(IDC_FIELD_BASE,EN_CHANGE);
    g_syncFields=0;handle_field_command(IDC_FIELD_BASE,EN_CHANGE);
    eval_destroy(g_ctx);tmpl_free(empty);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='qp-field-life-') as tmp:
    exe=Path(tmp)/'probe'
    code=preamble+field+'\nstatic EvalContext *g_ctx;static FieldCtl *g_fields;static int g_fieldCount,g_syncFields;\n'+body+driver
    subprocess.run(['cc','-std=gnu11','-Wall',*INCLUDE_FLAGS,'-x','c','-',*CORE_SOURCES,
                    str(root/'src/core/table.c'),str(root/'src/core/storage.c'),str(root/'src/core/regex.c'),
                    str(root/'third_party/cjson/cJSON.c'),'-o',str(exe)],input=code,text=True,check=True)
    subprocess.run([str(exe)],cwd=tmp,check=True)
print('field lifetime: rebuilding/empty-context notifications ignored before dereference')
