#include "table.h"
#include "storage.h"
#include "table_key.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static DataTableRef *ref;
static wchar_t path[600];
static QpTable *load(const wchar_t *csv)
{
    char *text=qp_wide_to_utf8_alloc(csv);assert(text);
    qp_table_invalidate_all();assert(storage_write_text(path,text));free(text);
    return qp_table_get(ref->id,NULL,0);
}
static void precision(void)
{
    wchar_t out[QP_VALUE_MAX];
    QpTable *t=load(L"key,value\n9007199254740992,A\n1.2345678901234,B\n1e30,C\n007,D\n7,E\n1.3800138E+10,PHONE\n");assert(t);
    assert(!qp_table_lookup(t,0,L"9007199254740993",1,LOOKUP_FIRST,NULL,0,out,QP_VALUE_MAX));
    assert(!qp_table_lookup(t,0,L"1.2345678901235",1,LOOKUP_FIRST,NULL,0,out,QP_VALUE_MAX));
    assert(qp_table_lookup(t,0,L"007",1,LOOKUP_FIRST,NULL,0,out,QP_VALUE_MAX) && !wcscmp(out,L"D"));
    assert(qp_table_lookup(t,0,L"7",1,LOOKUP_FIRST,NULL,0,out,QP_VALUE_MAX) && !wcscmp(out,L"E"));
    assert(qp_table_lookup(t,0,L"7.0",1,LOOKUP_FIRST,NULL,0,out,QP_VALUE_MAX) && !wcscmp(out,L"D"));
    assert(qp_table_lookup(t,0,L"13800138000",1,LOOKUP_FIRST,NULL,0,out,QP_VALUE_MAX) && !wcscmp(out,L"PHONE"));
}
static void long_keys(void)
{
    wchar_t csv[1000]=L"key,value\n",key[304],out[QP_VALUE_MAX];
    for(int i=0;i<300;++i)key[i]=L'a';
    key[300]=L'X';key[301]=0;
    wcscat(csv,key);wcscat(csv,L",RIGHT\n");QpTable *t=load(csv);assert(t);
    key[300]=L'Y';assert(!qp_table_lookup(t,0,key,1,LOOKUP_FIRST,NULL,0,out,QP_VALUE_MAX));
    key[300]=L'X';assert(qp_table_lookup(t,0,key,1,LOOKUP_FIRST,NULL,0,out,QP_VALUE_MAX) && !wcscmp(out,L"RIGHT"));
}
static void decimal_forms(void)
{
    const struct {const wchar_t *input,*want;} cases[]={
        {L"007",L"7e0"},{L"7.0",L"7e0"},{L"+7e0000",L"7e0"},
        {L"  -0.000E999 ",L"0"},{L".00120",L"12e-4"},{L"1000e-2",L"1e1"},
        {L"1.38E+10",L"138e8"},{L"13800000000.0",L"138e8"},
        {L"1.2345678901235",L"12345678901235e-13"},{L"1e30",L"1e30"},
        {L"10e999999999999999999999",L"1e1000000000000000000000"},
        {L".1e-999999999999999999999",L"1e-1000000000000000000000"},
        {L"NaN",NULL},{L"Infinity",NULL},{L"0x10",NULL},{L"1.2.3",NULL},{L"1e+",NULL}
    };
    for(size_t i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        wchar_t *key=qp_table_numeric_key(cases[i].input);
        if(cases[i].want)assert(key && !wcscmp(key,cases[i].want));else assert(!key);
        free(key);
    }
}
static void long_headers(void)
{
    wchar_t a[304],b[304],csv[1000];
    wmemset(a,L'a',300);a[300]=L'X';a[301]=0;
    wcscpy(b,a);b[300]=L'Y';
    swprintf(csv,1000,L"%ls,%ls\nleft,right\n",a,b);
    QpTable *t=load(csv);assert(t);
    assert(qp_table_col_index(t,b,-1)==1);
    b[QP_LABEL_MAX-1]=0;
    assert(qp_table_col_index(t,b,-1)==-1);
}
int main(int argc,char **argv)
{
    char dir[]="/tmp/qp-key-test-XXXXXX";assert(mkdtemp(dir));
    char file[600];snprintf(file,sizeof(file),"%s/table.csv",dir);qp_utf8_to_wide(file,path,600);
    ref=dtref_create(L"Keys",path);ref->encoding=QP_ENC_UTF8;ref->delimiter=QP_DELIM_COMMA;ref->has_header=1;qp_table_set_refs(ref);
    if(argc==2 && !strcmp(argv[1],"long"))long_keys();else {precision();long_keys();decimal_forms();long_headers();}
    qp_table_invalidate_all();qp_table_set_refs(NULL);dtref_free(ref);
    assert(!unlink(file) && !rmdir(dir));puts("table keys: exact numeric precision/text priority and complete long-key identity");return 0;
}
