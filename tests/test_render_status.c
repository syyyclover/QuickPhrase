/* Legacy output append must never leave the high half of a valid pair. */
#include "engine.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "test_fail_alloc.h"

static void checked_statuses(void)
{
    wchar_t out[QP_OUTPUT_MAX];
    Template *t=tmpl_create(L"Checked");Segment *source=seg_create(SEG_TEXT),*fixed=seg_create(SEG_STATIC);
    wcscpy(source->label,L"X");source->no_output=1;wcscpy(fixed->content,L"[{X}]");
    seg_list_append(&t->segments,source);seg_list_append(&t->segments,fixed);
    EvalContext *ctx=eval_create(t);eval_set_user_value(ctx,0,L"A\nB");
    t->batch_mode=1;wcscpy(t->batch_source_id,source->id);
    QpRenderResult result=qp_eval_render_checked(ctx,OUTPUT_TEXT,1,out,8,NULL,0);
    assert(result.status==QP_RENDER_OK && !wcscmp(out,L"[A]\n[B]"));
    result=qp_eval_render_checked(ctx,OUTPUT_TEXT,1,out,7,NULL,0);
    assert(result.status==QP_RENDER_TRUNCATED);
    fa_inject(1);result=qp_eval_render_checked(ctx,OUTPUT_TEXT,1,out,QP_OUTPUT_MAX,NULL,0);fa_disable();
    assert(fa_hit && result.status==QP_RENDER_NOMEM && result.written==0 && !out[0]);
    assert(!wcscmp(ctx->fields[0].user_value,L"A\nB"));
    t->batch_mode=0;wcscpy(fixed->content,L"{X}{X}");
    wchar_t long_value[1501];wmemset(long_value,L'a',1500);long_value[1500]=0;
    eval_set_user_value(ctx,0,long_value);result=qp_eval_render_checked(ctx,OUTPUT_TEXT,0,out,QP_OUTPUT_MAX,NULL,0);
    assert(result.status==QP_RENDER_TRUNCATED);
    /* An unrelated hidden source does not make a complete output incomplete. */
    wcscpy(fixed->content,L"complete");
    wchar_t too_long[QP_VALUE_MAX+2];wmemset(too_long,L'z',QP_VALUE_MAX+1);too_long[QP_VALUE_MAX+1]=0;
    eval_set_user_value(ctx,0,too_long);
    result=qp_eval_render_checked(ctx,OUTPUT_TEXT,0,out,QP_OUTPUT_MAX,NULL,0);
    assert(result.status==QP_RENDER_OK && !wcscmp(out,L"complete"));
    wcscpy(fixed->content,L"{X}");
    result=qp_eval_render_checked(ctx,OUTPUT_TEXT,0,out,QP_OUTPUT_MAX,NULL,0);
    assert(result.status==QP_RENDER_TRUNCATED);
    /* Paragraph extraction can overflow a target while the source still fits. */
    wcscpy(fixed->content,L"{X}");source->no_output=0;
    ParseRule *rule=parse_rule_create();assert(rule);rule->type=PARSE_CONTAINS;rule->per_paragraph=1;
    wcscpy(rule->keyword,L"z");wcscpy(rule->target_segment_id,source->id);
    wmemset(rule->fill_value,L'a',100);rule->fill_value[100]=0;
    source->parse_rules=rule;
    wchar_t lines[200];for(int i=0;i<80;++i){lines[i*2]=L'z';lines[i*2+1]=L'\n';}lines[160]=0;
    eval_set_user_value(ctx,0,lines);
    result=qp_eval_render_checked(ctx,OUTPUT_TEXT,0,out,QP_OUTPUT_MAX,NULL,0);
    assert(result.status==QP_RENDER_TRUNCATED);
    eval_destroy(ctx);tmpl_free(t);assert(!fa_live);
}

int main(void)
{
    wchar_t source[]={L'A',0xD83D,0xDE00,0},out[4];
    Template *t=tmpl_create(L"Render");Segment *s=seg_create(SEG_TEXT);seg_list_append(&t->segments,s);
    EvalContext *ctx=eval_create(t);eval_set_user_value(ctx,0,source);eval_run(ctx);
    eval_render_mode(ctx,OUTPUT_TEXT,out,3);
    assert(out[0]==L'A' && out[1]==0);
    qp_expand_placeholders(source,NULL,NULL,out,3);
    assert(out[0]==L'A' && out[1]==0);
    qp_split_take(source,L"|",1,out,3);
    assert(out[0]==L'A' && out[1]==0);
    qp_between_take(source,L"",L"",out,3);
    assert(out[0]==L'A' && out[1]==0);
    eval_render_text_marked(ctx,out,3,NULL,0,NULL);
    assert(out[0]==L'A' && out[1]==0);
    s->ref_mode=REF_SUBSTR;s->ref_start=1;s->ref_len=2;
    seg_ref_extract(s,source,out,4);assert(out[0]==L'A' && out[1]==0);
    s->ref_start=3;s->ref_len=2;
    seg_ref_extract(s,source,out,4);assert(out[0]==0xD83D && out[1]==0xDE00 && !out[2]);
    eval_destroy(ctx);tmpl_free(t);checked_statuses();
    puts("render: UTF-16, full/truncated/failed batch and intermediate expansion states passed");return 0;
}
