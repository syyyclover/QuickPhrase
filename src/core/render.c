/*
 * engine.c —— 数据模型生命周期管理 + 解析规则/条件规则引擎
 * 纯 C 实现，不含任何窗口 API，便于独立单元测试。
 */
#include "engine_internal.h"
#include "regex.h"
#include "table.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wctype.h>

static size_t render_text_core(EvalContext *,wchar_t *,size_t,int *,int,int *,unsigned *);
static size_t render_mode_core(EvalContext *,OutputMode,wchar_t *,size_t,int *,int,int *,unsigned *);

size_t eval_render_mode(EvalContext *ctx,OutputMode mode,wchar_t *out,size_t cap)
{
    return render_mode_core(ctx,mode,out,cap,NULL,0,NULL,NULL);
}
size_t eval_render(const EvalContext *ctx,wchar_t *out,size_t cap)
{
    return eval_render_mode((EvalContext *)ctx,OUTPUT_TEXT,out,cap);
}

/* 该字段是不是某条解析规则的目标 —— 是的话，取值为空就说明规则没命中。
   查表匹配的目标写在 lk_outs 里（一条规则能填好几个目标），也得算进来：
   不然「本该由查表填、却没查到」的那一处不会在预览里标出来，与其它解析
   规则的表现不一致。 */
static int field_is_parse_target(const EvalContext *ctx, int idx)
{
    const wchar_t *want;
    int i;
    if (!ctx || idx < 0 || idx >= ctx->count) return 0;
    want = ctx->fields[idx].seg ? ctx->fields[idx].seg->id : NULL;
    if (!want || !want[0]) return 0;
    for (i = 0; i < ctx->count; ++i) {
        const Segment *s = ctx->fields[i].seg;
        const ParseRule *r;
        if (!s || s->type != SEG_TEXT) continue;
        for (r = s->parse_rules; r; r = r->next) {
            const LookupOut *lo;
            if (wcscmp(r->target_segment_id, want) == 0) return 1;
            for (lo = r->lk_outs; lo; lo = lo->next)
                if (wcscmp(lo->target_segment_id, want) == 0) return 1;
        }
    }
    return 0;
}

static int table_missing(void *user, const wchar_t *name)
{
    const PlaceholderCtx *p = (const PlaceholderCtx *)user;
    int idx = eval_index_of_id(p->ctx, name);
    if (idx < 0) idx = eval_index_of_label(p->ctx, name);
    return idx >= 0 && !p->ctx->fields[idx].rule_driven && field_is_parse_target(p->ctx, idx);
}

static size_t render_mode_core(EvalContext *ctx, OutputMode mode, wchar_t *out, size_t cap,
                               int *marks, int mark_cap, int *count, unsigned *flags)
{
    PlaceholderCtx pc = {ctx, NULL, -1, NULL};
    if (mode == OUTPUT_TEXT) return render_text_core(ctx,out,cap,marks,mark_cap,count,flags);
    if (count) *count = 0;
    if (out && cap) out[0] = 0;
    if (!ctx || !ctx->tmpl || !ctx->tmpl->table_output_enabled) return 0;
    return qp_expand_placeholders_status(ctx->tmpl->table_output_format, eval_placeholder_lookup,
        &pc,out,cap,0,marks,mark_cap,count,table_missing,flags);
}

static size_t render_text_core(EvalContext *ctx, wchar_t *out, size_t cap,
                               int *marks, int mark_cap, int *mark_count, unsigned *flags)
{
    size_t used = 0;
    int i, nm = 0;

    if (mark_count) *mark_count = 0;
    if (!out || cap == 0) return 0;
    out[0] = L'\0';
    if (!ctx) return 0;

    for (i = 0; i < ctx->count; ++i) {
        const EvalField *f = &ctx->fields[i];
        const Segment *s = f->seg;
        const wchar_t *v = f->value;
        int k;

        /* 「不计入输出」的变量不拼进普通文本，与 eval_render_mode 同一口径 */
        if (s && s->no_output) continue;

        /* ① 固定内容里「{变量} 展开成空」的位置。eval_run 记下的正是产出这一版
              value 的那一次展开，所以下标与 v 严格对得上，不需要再展开一遍。 */
        for (k = 0; k < f->empty_count && k < QP_EMPTY_MARK_MAX; ++k) {
            if (!marks || nm >= mark_cap) break;
            marks[nm++] = (int)used + f->empty_marks[k];
        }
        /* ② 值是空的、而它正是某条解析规则的目标 → 解析规则没命中 */
        if (s && s->type != SEG_STATIC && !f->rule_driven && (!v || !*v) &&
            field_is_parse_target(ctx, i)) {
            if (marks && nm < mark_cap) marks[nm++] = (int)used;
        }
        if (v && *v) used = qp_append_checked(out,cap,used,v,wcslen(v),flags);
    }
    if (mark_count) *mark_count = nm;
    return used;
}

/* 一行批量渲染最多记这么多处失败；够用且不必按行长分配 */
#define QP_ROW_MARK_MAX 64

/* 批量渲染的统一实现：mode 决定普通文本还是表格；marks 只在普通文本下有意义。
   eval_render_batch_mode / eval_render_batch_text_marked 都走这里，保证两条
   调用路径的逐行语义不会各自演化。 */
static size_t render_batch_core(EvalContext *ctx, OutputMode mode,
                                wchar_t *out, size_t cap,
                                int *marks, int mark_cap, int *mark_count, unsigned *flags)
{
    int src, first = 1;
    const wchar_t *srcval, *p, *nl;
    wchar_t whole[QP_VALUE_MAX], line[QP_VALUE_MAX];
    wchar_t *row;
    size_t used = 0;

    if (mark_count) *mark_count = 0;
    if (!out || cap == 0) return 0;
    out[0] = L'\0';
    if (!ctx) return 0;

    /* 没开批量 / 没模板 / 没选源变量 / 源已删除 / 源为空或只有一行
       → 就按普通单次渲染 */
#define QP_BATCH_FALLBACK()                                                   \
    do {                                                                      \
        if (mode == OUTPUT_TEXT)                                              \
            return render_text_core(ctx,out,cap,marks,mark_cap,mark_count,flags);                       \
        return render_mode_core(ctx,mode,out,cap,marks,mark_cap,mark_count,flags);                          \
    } while (0)

    if (!ctx->tmpl || !ctx->tmpl->batch_mode || !ctx->tmpl->batch_source_id[0])
        QP_BATCH_FALLBACK();
    src = eval_index_of_id(ctx, ctx->tmpl->batch_source_id);
    if (src < 0) QP_BATCH_FALLBACK();

    /* 源变量此刻的值：优先取用户手填的整段，否则取生效值 */
    srcval = ctx->fields[src].user_touched ? ctx->fields[src].user_value
                                           : ctx->fields[src].value;
    /* 源为空或只有一行 → 退化为单次渲染，免得边打字边预览时闪空 */
    if(flags)*flags|=ctx->fields[src].user_touched?ctx->fields[src].input_flags:ctx->fields[src].value_flags;
    if (qp_is_blank(srcval) || !wcschr(srcval, L'\n')) QP_BATCH_FALLBACK();

    /* row 有 QP_OUTPUT_MAX 那么大，放堆上，别撑爆栈 */
    row = (wchar_t *)malloc(QP_OUTPUT_MAX * sizeof(wchar_t));
    if(!row){if(flags)*flags|=QP_TEXT_NOMEM;return 0;}

    /* 渲染使用独立字段快照，不在用户正在填写的上下文里注入行数据。 */
    qp_strlcpy(whole, srcval, QP_VALUE_MAX);
    {
        EvalContext *copy = eval_create(ctx->tmpl);
        if(!copy){free(row);if(flags)*flags|=QP_TEXT_NOMEM;return 0;}
        memcpy(copy->fields, ctx->fields, (size_t)ctx->count * sizeof(EvalField));
        copy->unstable = ctx->unstable;
        ctx = copy;
    }

    for (p = whole; ; ) {
        size_t llen;
        nl = wcschr(p, L'\n');
        llen = nl ? (size_t)(nl - p) : wcslen(p);
        if (llen >= QP_VALUE_MAX) llen = QP_VALUE_MAX - 1;
        wmemcpy(line, p, llen);
        line[llen] = L'\0';
        if (llen && line[llen - 1] == L'\r') line[llen - 1] = L'\0';   /* CRLF */
        if (!qp_is_blank(line)) {
            eval_set_user_value(ctx, src, line);
            if(!qp_eval_run_checked(ctx)){if(flags)*flags|=QP_TEXT_NOMEM;break;}
            if(flags)*flags|=qp_eval_output_flags(ctx,mode);
            if (mode == OUTPUT_TEXT) {
                int row_marks[QP_ROW_MARK_MAX];
                size_t base;
                int rn = 0, k;
                render_text_core(ctx,row,QP_OUTPUT_MAX,row_marks,QP_ROW_MARK_MAX,&rn,flags);
                /* 普通文本沿原行为：本行即使是空的也照样占一行 */
                if (!first) used = qp_append_checked(out,cap,used,L"\n", 1,flags);
                base = used;
                used = qp_append_checked(out,cap,used,row, wcslen(row),flags);
                /* 行内位置折算成整段输出里的位置 */
                for (k = 0; k < rn && marks && mark_count &&
                            *mark_count < mark_cap; ++k)
                    marks[(*mark_count)++] = (int)base + row_marks[k];
                first = 0;
                if (!nl) break;
                p = nl + 1;
                continue;
            }
            {
            int row_marks[QP_ROW_MARK_MAX], rn = 0, k;
            size_t base;
            render_mode_core(ctx,mode,row,QP_OUTPUT_MAX,row_marks,QP_ROW_MARK_MAX,&rn,flags);
            /* 表格格式未启用、格式为空或本行展开为空时，不把它算成一行，
               否则第二个空行会凭空留下一个换行符。普通文本保持原有行为。 */
            if (row[0] || rn) {
                if (!first) used = qp_append_checked(out,cap,used,L"\n", 1,flags);
                base = used;
                used = qp_append_checked(out,cap,used,row, wcslen(row),flags);
                for (k = 0; k < rn && marks && mark_count && *mark_count < mark_cap; ++k)
                    marks[(*mark_count)++] = (int)base + row_marks[k];
                first = 0;
            }
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
#undef QP_BATCH_FALLBACK

    eval_destroy(ctx);
    free(row);
    return used;
}

size_t eval_render_batch_mode_marked(EvalContext *ctx, OutputMode mode, wchar_t *out, size_t cap,
                                     int *marks, int mark_cap, int *count)
{ return render_batch_core(ctx,mode,out,cap,marks,mark_cap,count,NULL); }

size_t eval_render_batch_mode(EvalContext *ctx, OutputMode mode,
                              wchar_t *out, size_t cap)
{
    return render_batch_core(ctx,mode,out,cap,NULL,0,NULL,NULL);
}

size_t eval_render_batch_text_marked(EvalContext *ctx, wchar_t *out, size_t cap,
                                     int *marks, int mark_cap, int *mark_count)
{
    return render_batch_core(ctx,OUTPUT_TEXT,out,cap,marks,mark_cap,mark_count,NULL);
}

size_t eval_render_batch(EvalContext *ctx, wchar_t *out, size_t cap)
{
    return eval_render_batch_mode(ctx, OUTPUT_TEXT, out, cap);
}

size_t tmpl_render_defaults(const Template *t, wchar_t *out, size_t cap)
{
    EvalContext *ctx;
    size_t n;
    if (out && cap) out[0] = L'\0';
    if (!t) return 0;
    ctx = eval_create(t);
    if (!ctx) return 0;
    n = eval_render_batch(ctx, out, cap);
    eval_destroy(ctx);
    return n;
}

size_t eval_render_mode_marked(EvalContext *ctx,OutputMode mode,wchar_t *out,size_t cap,int *marks,int mark_cap,int *count)
{
    return render_mode_core(ctx,mode,out,cap,marks,mark_cap,count,NULL);
}
size_t eval_render_text_marked(EvalContext *ctx,wchar_t *out,size_t cap,int *marks,int mark_cap,int *count)
{
    return render_text_core(ctx,out,cap,marks,mark_cap,count,NULL);
}
QpRenderResult qp_eval_render_checked(EvalContext *ctx,OutputMode mode,int batch,wchar_t *out,size_t cap,int *marks,int mark_cap)
{
    QpRenderResult result={QP_RENDER_INVALID,0,0};
    if(out && cap)out[0]=0;
    if(!ctx || !out || !cap || (mode!=OUTPUT_TEXT && mode!=OUTPUT_TABLE))return result;
    if(!qp_eval_run_checked(ctx)){result.status=QP_RENDER_NOMEM;return result;}
    ctx->output_flags=qp_eval_output_flags(ctx,mode);
    result.written=batch?render_batch_core(ctx,mode,out,cap,marks,mark_cap,&result.mark_count,&ctx->output_flags):
        render_mode_core(ctx,mode,out,cap,marks,mark_cap,&result.mark_count,&ctx->output_flags);
    result.status=ctx->output_flags&QP_TEXT_NOMEM?QP_RENDER_NOMEM:
        ctx->output_flags&QP_TEXT_TRUNCATED?QP_RENDER_TRUNCATED:QP_RENDER_OK;
    if(result.status==QP_RENDER_NOMEM){out[0]=0;result.written=0;result.mark_count=0;}
    return result;
}
