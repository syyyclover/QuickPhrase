"""Real column binding refuses clipped names; combo restoration uses full identity."""
from pathlib import Path
import re,subprocess,tempfile
from source_helpers import ROOT,INCLUDE_FLAGS,CORE_SOURCES
s=(ROOT/'src/ui/dialog_lookup.c').read_text()
def fn(name):
    m=re.search(r'^static (?:void|int) '+name+r'\([^;]+?\)\n\{',s,re.M)
    return s[m.start():s.index('\n}',m.end())+2]+'\n' if m else ''
pre=r'''
#include "engine.h"
#include "storage.h"
#include "table.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <wctype.h>
typedef int HWND;typedef unsigned long WPARAM;typedef long LPARAM;
#define _snwprintf swprintf
#define CB_RESETCONTENT 1
#define CB_ADDSTRING 2
#define CB_SETCURSEL 3
static int selection,items;
static long SendMessageW(HWND h,int msg,WPARAM wp,LPARAM lp){
    (void)h;(void)lp;if(msg==CB_RESETCONTENT)items=0;
    if(msg==CB_ADDSTRING)++items;if(msg==CB_SETCURSEL)selection=(int)wp;return 0;
}
'''
driver=r'''
int main(void){
    char dir[]="/tmp/qp-lookup-col-XXXXXX",file[600];wchar_t path[600],csv[1000],name[QP_LABEL_MAX],a[301],b[301];
    assert(mkdtemp(dir));snprintf(file,600,"%s/table.csv",dir);qp_utf8_to_wide(file,path,600);
    wmemset(a,L'a',299);a[299]=L'X';a[300]=0;wcscpy(b,a);b[299]=L'Y';
    swprintf(csv,1000,L"short,%ls,%ls\n1,2,3\n",a,b);char *utf=qp_wide_to_utf8_alloc(csv);assert(utf);
    assert(storage_write_text(path,utf));free(utf);
    DataTableRef *ref=dtref_create(L"Columns",path);ref->has_header=1;ref->encoding=QP_ENC_UTF8;ref->delimiter=QP_DELIM_COMMA;qp_table_set_refs(ref);
    QpTable *t=qp_table_get(ref->id,NULL,0);assert(t);int index=-1;wcscpy(name,L"short");
    lookup_store_col(t,2,name,QP_LABEL_MAX,&index);
    assert(index==-1 && !wcscmp(name,L"short")); /* Rejection never alters saved mapping. */
    lookup_fill_col_combo(0,t,b,-1);assert(items==3 && selection==2);
    b[127]=0;lookup_fill_col_combo(0,t,b,-1);assert(selection==-1); /* No prefix-only false restoration. */
    lookup_store_col(t,0,name,QP_LABEL_MAX,&index);assert(!wcscmp(name,L"short") && index==-1);
    qp_table_invalidate_all();qp_table_set_refs(NULL);dtref_free(ref);assert(!unlink(file)&&!rmdir(dir));
    puts("lookup columns: overlong binding refused unchanged; full names and missing-name restoration exact");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='qp-lk-col-') as tmp:
    exe=Path(tmp)/'test';code=pre+fn('ui_col_key')+fn('lookup_fill_col_combo')+fn('lookup_store_col')+driver
    subprocess.run(['cc','-std=gnu11','-Wall',*INCLUDE_FLAGS,'-x','c','-',*CORE_SOURCES,
        str(ROOT/'src/core/storage.c'),str(ROOT/'src/core/table.c'),str(ROOT/'src/core/regex.c'),str(ROOT/'third_party/cjson/cJSON.c'),'-o',str(exe)],input=code,text=True,check=True)
    subprocess.run([str(exe)],check=True)
