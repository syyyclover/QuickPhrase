#include "storage.h"
#include "table.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fault;
size_t __real_fread(void *,size_t,size_t,FILE *);
int __real_ferror(FILE *);
int __real_fseek(FILE *,long,int);
int __real_fclose(FILE *);
size_t __wrap_fread(void *p,size_t s,size_t n,FILE *f)
{ return __real_fread(p,s,fault==1?n/2:n,f); }
int __wrap_ferror(FILE *f)
{ if(fault==2){errno=EIO;return 1;}return __real_ferror(f); }
int __wrap_fseek(FILE *f,long n,int w)
{ if(fault==3 && w==SEEK_SET){errno=EIO;return -1;}return __real_fseek(f,n,w); }
int __wrap_fclose(FILE *f)
{ int r=__real_fclose(f);if(fault==4){errno=EIO;return EOF;}return r; }

int main(void)
{
    char dir[]="/tmp/qp-read-fail-XXXXXX",file[600];wchar_t path[600],err[600];
    assert(mkdtemp(dir));snprintf(file,sizeof(file),"%s/table.csv",dir);
    qp_utf8_to_wide(file,path,600);
    assert(storage_write_text(path,"key,value\nA,one\nB,two\n"));
    DataTableRef *ref=dtref_create(L"Reads",path);assert(ref);
    ref->encoding=QP_ENC_UTF8;ref->delimiter=QP_DELIM_COMMA;ref->has_header=1;
    qp_table_set_refs(ref);
    for(fault=1;fault<=4;++fault){
        size_t size=123;char *bytes=storage_read_bytes(path,&size);
        assert(!bytes && size==0); /* A partial stream is never a complete file. */
        qp_table_invalidate_all();err[0]=0;
        assert(!qp_table_get(ref->id,err,600) && err[0]);
    }
    fault=0;assert(qp_table_get(ref->id,err,600));
    qp_table_invalidate_all();qp_table_set_refs(NULL);dtref_free(ref);
    assert(!unlink(file) && !rmdir(dir));puts("read faults: short/error/seek/close refuse publication and recover");
    return 0;
}
