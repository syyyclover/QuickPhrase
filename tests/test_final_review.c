#include "engine.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Catch editor-default previews ignoring the batch toggle/source selection. */
static void test_batch_defaults(void)
{
    Template *t = tmpl_create(L"Default preview");
    Segment *src = seg_create(SEG_TEXT);
    Segment *other = seg_create(SEG_TEXT);
    Segment *value = seg_create(SEG_TEXT);
    Segment *row = seg_create(SEG_STATIC);
    ParseRule *rule = parse_rule_create();
    wchar_t out[256];
    size_t n;

    qp_strlcpy(src->label, L"source", QP_LABEL_MAX);
    qp_strlcpy(src->default_value, L"[A]\r\n\r\n  \n[B]", QP_VALUE_MAX);
    qp_strlcpy(other->default_value, L"one\ntwo\nthree", QP_VALUE_MAX);
    qp_strlcpy(value->label, L"value", QP_LABEL_MAX);
    src->no_output = other->no_output = value->no_output = 1;
    qp_strlcpy(row->content, L"{value}\\tconstant", QP_CONTENT_MAX);
    rule->type = PARSE_BETWEEN;
    qp_strlcpy(rule->keyword, L"[", QP_KEYWORD_MAX);
    qp_strlcpy(rule->marker_after, L"]", QP_KEYWORD_MAX);
    qp_strlcpy(rule->target_segment_id, value->id, QP_ID_MAX);
    parse_rule_append(&src->parse_rules, rule);
    seg_list_append(&t->segments, src);
    seg_list_append(&t->segments, other);
    seg_list_append(&t->segments, value);
    seg_list_append(&t->segments, row);
    qp_strlcpy(t->batch_source_id, src->id, QP_ID_MAX);

    tmpl_render_defaults(t, out, 256);
    assert(wcscmp(out, L"A\tconstant") == 0);
    t->batch_mode = 1;
    n = tmpl_render_defaults(t, out, 256);
    assert(wcscmp(out, L"A\tconstant\nB\tconstant") == 0);
    assert(n == wcslen(L"A\tconstant\nB\tconstant"));
    assert(wcscmp(src->default_value, L"[A]\r\n\r\n  \n[B]") == 0);

    /* Changing the source changes which rows drive repeated evaluation. */
    qp_strlcpy(t->batch_source_id, other->id, QP_ID_MAX);
    tmpl_render_defaults(t, out, 256);
    assert(wcscmp(out, L"A\tconstant\nA\tconstant\nA\tconstant") == 0);
    t->batch_source_id[0] = L'\0';
    tmpl_render_defaults(t, out, 256);
    assert(wcscmp(out, L"A\tconstant") == 0);
    qp_strlcpy(t->batch_source_id, L"deleted-source", QP_ID_MAX);
    tmpl_render_defaults(t, out, 256);
    assert(wcscmp(out, L"A\tconstant") == 0);
    qp_strlcpy(t->batch_source_id, src->id, QP_ID_MAX);
    t->batch_mode = 0;
    tmpl_render_defaults(t, out, 256);
    assert(wcscmp(out, L"A\tconstant") == 0);
    t->batch_mode = 1;
    qp_strlcpy(src->default_value, L"[C]", QP_VALUE_MAX);
    tmpl_render_defaults(t, out, 256);
    assert(wcscmp(out, L"C\tconstant") == 0);
    src->default_value[0] = L'\0';
    tmpl_render_defaults(t, out, 256);
    assert(wcscmp(out, L"\tconstant") == 0);
    tmpl_free(t);
    puts("batch default preview: passed");
}

/* Exact heap allocations expose truncated swprintf + unbounded wcslen to ASan. */
static void test_summary_bounds(void)
{
    ParseRule *r = parse_rule_create();
    const size_t capacities[] = {1, 2, 3, 4, 5, 6, 16, 32, 256, 512, 2048, 4096};
    const wchar_t suffix[] = L"（逐段）";
    wchar_t untouched = L'!';
    size_t c;
    int variant, per;

    wmemset(r->fill_value, L'X', QP_VALUE_MAX - 1);
    r->fill_value[QP_VALUE_MAX - 1] = L'\0';
    qp_strlcpy(r->delimiter, L"-", QP_SEP_MAX);
    qp_strlcpy(r->marker_after, L"]", QP_KEYWORD_MAX);
    parse_rule_summary(r, NULL, &untouched, 0);
    assert(untouched == L'!');
    parse_rule_summary(r, NULL, NULL, 10);
    for (variant = 0; variant < 4; ++variant) {
        r->type = variant < 2 ? PARSE_SPLIT :
                  variant == 2 ? PARSE_BETWEEN : PARSE_CONTAINS;
        qp_strlcpy(r->keyword, variant == 0 ? L"" : L"[", QP_KEYWORD_MAX);
        for (per = 1; per >= 0; --per) {
            r->per_paragraph = per;
            for (c = 0; c < sizeof(capacities) / sizeof(capacities[0]); ++c) {
                size_t cap = capacities[c], len;
                wchar_t *out = malloc(cap * sizeof(*out));
                assert(out);
                wmemset(out, L'!', cap);
                parse_rule_summary(r, NULL, out, cap);
                assert(wmemchr(out, L'\0', cap) != NULL);
                len = wcslen(out);
                assert(len < cap);
                if (per && cap >= 5) {
                    assert(len >= 4);
                    assert(wcscmp(out + len - 4, suffix) == 0);
                } else if (per) {
                    assert(len == cap - 1);
                    assert(wcsncmp(out, suffix, len) == 0);
                }
                if (cap == 4096) {
                    wchar_t *fill = wcschr(out, L'X');
                    assert(fill && wcsspn(fill, L"X") == QP_VALUE_MAX - 1);
                }
                free(out);
            }
        }
    }
    parse_rule_free(r);
    puts("parse summary bounds: passed");
}

int main(int argc, char **argv)
{
    {
        Template *t=tmpl_create(L"table diagnostics");
        Segment *src=seg_create(SEG_TEXT), *optional=seg_create(SEG_TEXT), *value=seg_create(SEG_TEXT);
        ParseRule *p=parse_rule_create();
        EvalContext *ctx;
        wchar_t out[128]; int marks[8], n=0;
        qp_strlcpy(optional->label,L"optional",QP_LABEL_MAX);
        qp_strlcpy(value->label,L"derived",QP_LABEL_MAX);
        seg_list_append(&t->segments,src); seg_list_append(&t->segments,optional); seg_list_append(&t->segments,value);
        p->type=PARSE_CONTAINS; qp_strlcpy(p->keyword,L"X",QP_KEYWORD_MAX);
        qp_strlcpy(p->target_segment_id,value->id,QP_ID_MAX); parse_rule_append(&src->parse_rules,p);
        t->table_output_enabled=1;
        qp_strlcpy(t->table_output_format,L"\\t{optional}\\t{derived}",QP_OUTPUT_MAX);
        ctx=eval_create(t); eval_run(ctx);
        eval_render_mode_marked(ctx,OUTPUT_TABLE,out,128,marks,8,&n);
        assert(wcscmp(out,L"\t\t")==0 && n==1 && marks[0]==2);
        eval_set_user_value(ctx,0,L"X"); eval_run(ctx);
        eval_render_mode_marked(ctx,OUTPUT_TABLE,out,128,marks,8,&n);
        assert(wcscmp(out,L"\t\tX")==0 && n==0);
        t->batch_mode=1; qp_strlcpy(t->batch_source_id,src->id,QP_ID_MAX);
        eval_set_user_value(ctx,0,L"A\nX"); eval_run(ctx);
        eval_render_batch_mode_marked(ctx,OUTPUT_TABLE,out,128,marks,8,&n);
        assert(wcscmp(out,L"\t\t\n\t\tX")==0 && n==1 && marks[0]==2);
        eval_destroy(ctx); tmpl_free(t);
        puts("table diagnostics distinguish optional blanks and failed rules: passed");
    }
    {
        Template *t=tmpl_create(L"lint bounds");
        Segment *s=seg_create(SEG_TEXT);
        ParseRule *p=parse_rule_create();
        LintIssue issues[4], guard;
        int n;
        memset(issues,0x5A,sizeof(issues)); guard=issues[1];
        p->type=PARSE_LOOKUP; p->lk_key_mode=LOOKUP_KEY_VAR;
        parse_rule_append(&s->parse_rules,p); seg_list_append(&t->segments,s);
        n=tmpl_lint(t,issues,1);
        assert(n<=1 && memcmp(&issues[1],&guard,sizeof(guard))==0);
        tmpl_free(t);
        puts("lookup lint respects report capacity: passed");
    }
    {
        Template *t = tmpl_create(L"batch isolation");
        Segment *s = seg_create(SEG_TEXT);
        EvalContext *ctx;
        EvalField before;
        wchar_t out[64];
        seg_list_append(&t->segments, s);
        t->batch_mode = 1;
        qp_strlcpy(t->batch_source_id, s->id, QP_ID_MAX);
        ctx = eval_create(t);
        eval_set_user_value(ctx, 0, L"A\nB");
        before = ctx->fields[0];
        ctx->unstable = 1;
        eval_render_batch(ctx, out, 64);
        assert(wcscmp(out, L"A\nB") == 0);
        assert(memcmp(&before, &ctx->fields[0], sizeof(before)) == 0);
        assert(ctx->unstable == 1);
        eval_destroy(ctx); tmpl_free(t);
        puts("batch render preserves original context: passed");
    }
    if (argc == 1 || strcmp(argv[1], "defaults") == 0) test_batch_defaults();
    if (argc == 1 || strcmp(argv[1], "summary") == 0) test_summary_bounds();
    return 0;
}
