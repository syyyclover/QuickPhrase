#include "engine.h"
#include "table.h"
#include "storage.h"
#include "test_fail_alloc.h"
#include <assert.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    char dir[]="/tmp/qp-match-XXXXXX";assert(mkdtemp(dir));
    char path[256];snprintf(path,sizeof(path),"%s/table.csv",dir);
    FILE *f=fopen(path,"w");assert(f);fputs("id,a,b\n007,exact,one\n7,first,x\n7,,y\n7,last,z\n",f);assert(!fclose(f));
    wchar_t *wide=qp_utf8_to_wide_alloc(path);DataTableRef *ref=dtref_create(L"Match",wide);free(wide);
    qp_table_set_refs(ref);assert(qp_table_read_begin());QpTable *t=qp_table_get(ref->id,NULL,0);assert(t);
    wchar_t out[128];QpTableMatch *m=qp_table_match(t,0,L"7");assert(m);
    for(int mode=LOOKUP_FIRST;mode<=LOOKUP_ALL;++mode) {
        QpTableLookupResult r=qp_table_match_value(m,1,mode,L"|",0,out,128);
        assert(r.found && !r.failed && !r.truncated);
        assert(!wcscmp(out,mode==LOOKUP_FIRST?L"first":mode==LOOKUP_LAST?L"last":L"first|last"));
    }
    QpTableLookupResult r=qp_table_match_value(m,2,LOOKUP_ALL,L"|",0,out,128);
    assert(r.found && !wcscmp(out,L"x|y|z"));
    fa_inject(1);r=qp_table_match_value(m,2,LOOKUP_LAST,L"|",0,out,128);
    assert(r.found && !wcscmp(out,L"z") && !fa_calls);fa_disable();
    qp_table_match_free(m);
    m=qp_table_match(t,0,L"007");assert(m);
    r=qp_table_match_value(m,1,LOOKUP_FIRST,NULL,0,out,128);assert(!wcscmp(out,L"exact"));qp_table_match_free(m);
    m=qp_table_match(t,0,L"7.0");assert(m);
    r=qp_table_match_value(m,1,LOOKUP_FIRST,NULL,0,out,128);assert(!wcscmp(out,L"exact"));qp_table_match_free(m);
    m=qp_table_match(t,0,L"missing");assert(m);
    r=qp_table_match_value(m,1,LOOKUP_ALL,NULL,0,out,128);assert(!r.found && !r.failed);qp_table_match_free(m);
    fa_inject(1);m=qp_table_match(t,0,L"7");assert(!m && fa_hit);fa_disable();
    m=qp_table_match(t,0,L"7");assert(m);
    fa_inject(1);r=qp_table_match_value(m,1,LOOKUP_ALL,L"|",0,out,128);fa_disable();
    assert(fa_hit && r.failed && !out[0]);
    r=qp_table_match_value(m,1,LOOKUP_ALL,L"|",0,out,128);
    assert(!r.failed && !wcscmp(out,L"first|last"));qp_table_match_free(m);
    qp_table_read_end();qp_table_set_refs(NULL);dtref_list_free(ref);
    assert(!fa_live);assert(!unlink(path));assert(!rmdir(dir));
    puts("table match: exact priority, lazy first/last, one ordered match for multi-output and failure states checked");return 0;
}
