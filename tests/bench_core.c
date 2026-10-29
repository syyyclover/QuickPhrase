/* Fixed synthetic workloads; assertions run before timing results are emitted.
   Host timings/peak RSS do not establish native Windows UI performance. */
#include "engine.h"
#include "table.h"
#include "storage.h"
#include "regex.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/resource.h>
#include <unistd.h>

static double now(void)
{
    struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));
    return (double)t.tv_sec+(double)t.tv_nsec/1e9;
}
static double eval_bench(int count)
{
    Template *t=tmpl_create(L"Bench");assert(t);
    for(int i=0;i<count;++i) {
        Segment *s=seg_create(SEG_TEXT);assert(s);
        swprintf(s->label,QP_LABEL_MAX,L"v%d",i);wcscpy(s->default_value,L"value");
        seg_list_append(&t->segments,s);
    }
    EvalContext *ctx=eval_create(t);assert(ctx);
    wchar_t out[QP_OUTPUT_MAX];
    QpRenderResult r=qp_eval_render_checked(ctx,OUTPUT_TEXT,0,out,QP_OUTPUT_MAX,NULL,0);
    assert(r.status==QP_RENDER_OK && r.written==(size_t)count*5);
    double start=now();
    int repeats=count==10?500000:count==100?100000:20000;
    for(int i=0;i<repeats;++i)assert(qp_eval_run_checked(ctx));
    double elapsed=now()-start;
    assert(!wcscmp(ctx->fields[count-1].value,L"value"));
    eval_destroy(ctx);tmpl_free(t);return elapsed*1000;
}
int main(void)
{
    double ordinary=eval_bench(10),hundred=eval_bench(100),fivehundred=eval_bench(500);
    QpRegexSpan spans[QP_REGEX_MAX_GROUPS];wchar_t pattern[100],text[100];
    assert(qp_regex_search(L"device-([0-9]+)",L"device-123",spans,QP_REGEX_MAX_GROUPS));
    double start=now();
    for(int round=0;round<40;++round)for(int i=0;i<4000;++i) {
        swprintf(pattern,100,L"device-%d-([0-9]+)",i);
        swprintf(text,100,L"device-%d-123",i);
        assert(qp_regex_search(pattern,text,spans,QP_REGEX_MAX_GROUPS));
    }
    double regex=(now()-start)*1000;
    char dir[]="/tmp/qp-bench-XXXXXX";assert(mkdtemp(dir));
    char path[256];snprintf(path,sizeof(path),"%s/table.csv",dir);
    FILE *f=fopen(path,"w");assert(f);fputs("key,a,b\n",f);
    for(int i=0;i<100000;++i)assert(fprintf(f,"%d,a%d,b%d\n",i%2000,i,i)>0);
    assert(!fclose(f));
    wchar_t *wide=qp_utf8_to_wide_alloc(path);assert(wide);
    DataTableRef *ref=dtref_create(L"Bench",wide);free(wide);assert(ref);
    ref->encoding=QP_ENC_UTF8;ref->delimiter=QP_DELIM_COMMA;ref->has_header=1;qp_table_set_refs(ref);
    QpTable *table=NULL;QpTableMatch *m;
    wchar_t out[QP_VALUE_MAX];QpTableLookupResult r;
    double load=0;
    /* Eight independent cold cache/index builds reduce sub-20ms timer noise. */
    for(int repeat=0;repeat<8;++repeat) {
        qp_table_invalidate_all();assert(qp_table_read_begin());start=now();
        table=qp_table_get(ref->id,NULL,0);assert(table && qp_table_rows(table)==100000);
        m=qp_table_match(table,0,L"7");assert(m);
        r=qp_table_match_value(m,1,LOOKUP_LAST,NULL,0,out,QP_VALUE_MAX);
        assert(r.found && !r.failed && !wcscmp(out,L"a98007"));qp_table_match_free(m);
        load+=(now()-start)*1000;
        if(repeat<7)qp_table_read_end();
    }
    load/=8;start=now();
    for(int i=0;i<300000;++i) {
        m=qp_table_match(table,0,L"7");assert(m);
        r=qp_table_match_value(m,1,LOOKUP_FIRST,NULL,0,out,QP_VALUE_MAX);assert(r.found && !wcscmp(out,L"a7"));
        r=qp_table_match_value(m,2,LOOKUP_LAST,NULL,0,out,QP_VALUE_MAX);assert(r.found && !wcscmp(out,L"b98007"));
        qp_table_match_free(m);
    }
    double query=(now()-start)*1000;
    qp_table_read_end();qp_table_set_refs(NULL);dtref_free(ref);
    assert(!unlink(path));assert(!rmdir(dir));
    struct rusage usage;assert(!getrusage(RUSAGE_SELF,&usage));
    printf("{\"schema\":1,\"eval_10_ms\":%.3f,\"eval_100_ms\":%.3f,\"eval_500_ms\":%.3f,"
           "\"regex_4000_ms\":%.3f,\"table_load_100k_ms\":%.3f,\"table_query_ms\":%.3f,\"peak_rss_kib\":%ld}"
           "\n",ordinary,hundred,fivehundred,regex,load,query,usage.ru_maxrss);
    return 0;
}
