"""Real cache cannot pair decoded old CSV bytes with a newer file stamp."""
from pathlib import Path
import subprocess
import tempfile
from source_helpers import INCLUDE_FLAGS
root=Path(__file__).resolve().parents[1]
code=r'''
#include "table.h"
#include "storage.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
static const char *path;
static int changing,reads,writes;
size_t __real_fread(void *,size_t,size_t,FILE *);
size_t __wrap_fread(void *p,size_t size,size_t n,FILE *f) {
    size_t got=__real_fread(p,size,n,f);++reads;
    if(changing && got) {
        ++writes;FILE *out=fopen(path,"wb");assert(out);
        fputs(writes%2?"key,value\nA,new\n":"key,value\nA,old\n",out);assert(!fclose(out));
        struct timespec stamp[2]={{1700000000,writes*1000},{1700000000,writes*1000}};
        assert(!utimensat(AT_FDCWD,path,stamp,0));
        if(changing==1)changing=0;
    }
    return got;
}
int main(int argc,char **argv) {
    assert(argc==2);path=argv[1];wchar_t wide[600],out[100],err[256];qp_utf8_to_wide(path,wide,600);
    DataTableRef *ref=dtref_create(L"Race",wide);ref->encoding=QP_ENC_UTF8;ref->delimiter=QP_DELIM_COMMA;ref->has_header=1;
    qp_table_set_refs(ref);assert(storage_write_text(wide,"key,value\nA,old\n"));
    changing=1;QpTable *t=qp_table_get(ref->id,err,256);assert(t);
    assert(qp_table_lookup(t,0,L"A",1,LOOKUP_FIRST,NULL,0,out,100) && !wcscmp(out,L"new"));
    t=qp_table_get(ref->id,err,256);assert(t);
    assert(qp_table_lookup(t,0,L"A",1,LOOKUP_FIRST,NULL,0,out,100) && !wcscmp(out,L"new"));
    qp_table_invalidate_all();changing=2;reads=0;
    t=qp_table_get(ref->id,err,256);assert(!t && err[0] && reads==3);
    changing=0;t=qp_table_get(ref->id,err,256);assert(t);
    qp_table_invalidate_all();qp_table_set_refs(NULL);dtref_free(ref);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='qp-cache-race-') as tmp:
    exe=Path(tmp)/'probe';path=Path(tmp)/'table.csv'
    sources=[str(p) for p in (root/'src/core').glob('*.c')]+[str(root/'third_party/cjson/cJSON.c')]
    subprocess.run(['cc','-std=gnu11','-Wall',*INCLUDE_FLAGS,'-x','c','-',*sources,
                    '-Wl,--wrap=fread','-o',str(exe)],input=code,text=True,check=True)
    subprocess.run([str(exe),str(path)],cwd=tmp,check=True)
print('cache: read/stamp race retries consistently, repeated changes stop after three reads and recover')
