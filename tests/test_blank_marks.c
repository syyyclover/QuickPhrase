#include "engine.h"
#include "storage.h"
#include "table.h"
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

int main(void)
{
    char dir[]="/tmp/qp-blank-marks-XXXXXX",file[600];wchar_t path[600],out[200];int marks[10];
    assert(mkdtemp(dir));snprintf(file,sizeof(file),"%s/table.csv",dir);qp_utf8_to_wide(file,path,600);
    assert(storage_write_text(path,"key,value\nA,\n"));
    DataTableRef *ref=dtref_create(L"Blanks",path);assert(ref);
    ref->encoding=QP_ENC_UTF8;ref->delimiter=QP_DELIM_COMMA;ref->has_header=1;qp_table_set_refs(ref);
    Template *t=tmpl_create(L"Blank marks");assert(t);
    Segment *key=seg_create(SEG_TEXT),*value=seg_create(SEG_TEXT),*fixed=seg_create(SEG_STATIC);
    assert(key && value && fixed);wcscpy(value->label,L"V");wcscpy(fixed->content,L"[{V}]");key->no_output=1;
    seg_list_append(&t->segments,key);seg_list_append(&t->segments,value);seg_list_append(&t->segments,fixed);
    ParseRule *r=parse_rule_create();LookupOut *lo=lookup_out_create();assert(r && lo);
    r->type=PARSE_LOOKUP;wcscpy(r->lk_table_id,ref->id);wcscpy(r->lk_key_col,L"key");r->lk_key_col_index=-1;
    wcscpy(lo->col,L"value");lo->col_index=-1;wcscpy(lo->target_segment_id,value->id);r->lk_outs=lo;key->parse_rules=r;
    t->table_output_enabled=1;wcscpy(t->table_output_format,L"[{V}]");
    EvalContext *ctx=eval_create(t);assert(ctx);eval_set_user_value(ctx,0,L"A");
    QpRenderResult result=qp_eval_render_checked(ctx,OUTPUT_TEXT,0,out,200,marks,10);
    assert(result.status==QP_RENDER_OK && !wcscmp(out,L"[]") && ctx->fields[1].rule_driven);
    assert(result.mark_count==0 && ctx->fields[2].empty_count==0);
    result=qp_eval_render_checked(ctx,OUTPUT_TABLE,0,out,200,marks,10);
    assert(result.mark_count==0 && !wcscmp(out,L"[]"));
    t->batch_mode=1;wcscpy(t->batch_source_id,key->id);eval_set_user_value(ctx,0,L"A\nA");
    result=qp_eval_render_checked(ctx,OUTPUT_TEXT,1,out,200,marks,10);
    assert(result.mark_count==0 && !wcscmp(out,L"[]\n[]"));
    t->batch_mode=0;eval_set_user_value(ctx,0,L"Z");
    result=qp_eval_render_checked(ctx,OUTPUT_TEXT,0,out,200,marks,10);
    assert(result.mark_count==2 && ctx->fields[2].empty_count==1 && !ctx->fields[1].rule_driven);
    eval_destroy(ctx);tmpl_free(t);qp_table_invalidate_all();qp_table_set_refs(NULL);dtref_free(ref);
    assert(!unlink(file) && !rmdir(dir));puts("blank lookup values: text/placeholders/batch not marked as failures; misses remain marked");return 0;
}
