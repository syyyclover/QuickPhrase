#include "table.h"
#include "storage.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* Declarations make the missing pre-refactor range interface an explicit RED. */
int qp_table_read_begin(void);
void qp_table_read_end(void);
int main(void)
{
    char dir[]="/tmp/qp-table-scope-XXXXXX",file[600];wchar_t path[600],out[100];
    assert(mkdtemp(dir));snprintf(file,sizeof(file),"%s/table.csv",dir);qp_utf8_to_wide(file,path,600);
    assert(storage_write_text(path,"key,value\nA,old\n"));
    DataTableRef *refs=NULL;
    for(int i=0;i<6;++i){DataTableRef *r=dtref_create(L"Scope",path);r->encoding=QP_ENC_UTF8;r->delimiter=QP_DELIM_COMMA;r->has_header=1;dtref_list_append(&refs,r);}
    wchar_t id[QP_ID_MAX];wcscpy(id,refs->id);qp_table_set_refs(refs);
    assert(qp_table_read_begin() && qp_table_read_begin());
    QpTable *first=qp_table_get(id,NULL,0);assert(first);
    for(DataTableRef *r=refs;r;r=r->next)assert(qp_table_get(r->id,NULL,0));
    assert(storage_write_text(path,"key,value\nA,new\n"));
    qp_table_invalidate_all();
    DataTableRef *next=dtref_create(L"New",path);wcscpy(next->id,id);next->encoding=QP_ENC_UTF8;next->delimiter=QP_DELIM_COMMA;next->has_header=1;
    qp_table_set_refs(next);dtref_list_free(refs);refs=NULL;
    assert(qp_table_get(id,NULL,0)==first);
    assert(qp_table_lookup(first,0,L"A",1,LOOKUP_FIRST,NULL,0,out,100) && !wcscmp(out,L"old"));
    qp_table_read_end();assert(qp_table_get(id,NULL,0)==first);qp_table_read_end();
    QpTable *current=qp_table_get(id,NULL,0);assert(current);
    assert(qp_table_lookup(current,0,L"A",1,LOOKUP_FIRST,NULL,0,out,100) && !wcscmp(out,L"new"));
    qp_table_invalidate_all();qp_table_set_refs(NULL);dtref_free(next);assert(!unlink(file) && !rmdir(dir));
    puts("table range: nested scopes survive cache eviction/invalidation/ref replacement and publish new generation after end");return 0;
}
