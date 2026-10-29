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
#include <errno.h>
#include <stdint.h>

/* ================================================================== */
/* 规则引擎                                                            */
/* ================================================================== */

static int eval_reset_checked(EvalContext *ctx);

struct QpEvalWorkspace {
    wchar_t (*work)[QP_VALUE_MAX], (*derived)[QP_VALUE_MAX];
    int *derived_set, *parse_set, *condition_set, *ref_cycle, *empty_slots, *empty_n, *needed;
    unsigned *flags;
    unsigned char data[];
};

static struct QpEvalWorkspace *workspace_create(int count)
{
    size_t per = 2 * QP_VALUE_MAX * sizeof(wchar_t) +
                 (6 + QP_EMPTY_MARK_MAX) * sizeof(int) + sizeof(unsigned);
    if (count < 0 || (size_t)count > (SIZE_MAX-sizeof(struct QpEvalWorkspace))/per)
        return NULL;
    struct QpEvalWorkspace *w = calloc(1, sizeof(*w) + (size_t)count * per);
    if (!w) return NULL;
    w->work = (void *)w->data;
    w->derived = w->work + count;
    w->derived_set = (void *)(w->derived + count);
    w->parse_set = w->derived_set + count;
    w->condition_set = w->parse_set + count;
    w->ref_cycle = w->condition_set + count;
    w->empty_slots = w->ref_cycle + count;
    w->empty_n = w->empty_slots + (size_t)count * QP_EMPTY_MARK_MAX;
    w->needed = w->empty_n + count;
    w->flags = (void *)(w->needed + count);
    return w;
}

EvalContext *eval_create(const Template *t)
{
    EvalContext *ctx;
    Segment *s;
    int i = 0, n;
    if (!t) return NULL;
    ctx = (EvalContext *)calloc(1, sizeof(EvalContext));
    if (!ctx) return NULL;
    ctx->tmpl = t;
    n = seg_list_count(t->segments);
    ctx->count = n;
    if (n > 0) {
        ctx->fields = (EvalField *)calloc((size_t)n, sizeof(EvalField));
        if (!ctx->fields) { free(ctx); return NULL; }
        for (s = t->segments; s; s = s->next, ++i) ctx->fields[i].seg = s;
        ctx->workspace = workspace_create(n);
        if (!ctx->workspace) { eval_destroy(ctx); return NULL; }
    }
    if (!eval_reset_checked(ctx)) { eval_destroy(ctx); return NULL; }
    return ctx;
}

void eval_destroy(EvalContext *ctx)
{
    if (!ctx) return;
    free(ctx->fields);
    free(ctx->workspace);
    free(ctx);
}

/* 片段的「原始默认值」 */
static const wchar_t *seg_default_of(const Segment *s)
{
    if (!s) return L"";
    if (s->type == SEG_STATIC) return s->content;
    if (s->default_value[0]) return s->default_value;
    return L"";
}

/* 片段的「兜底值」：手填值优先，否则只取显式默认值。 */
static const wchar_t *field_fallback_of(const EvalField *f)
{
    if (!f) return L"";
    if (f->user_touched) return f->user_value;
    return seg_default_of(f->seg);
}

static int eval_reset_checked(EvalContext *ctx)
{
    int i;
    if (!ctx) return 0;
    for (i = 0; i < ctx->count; ++i) {
        EvalField *f = &ctx->fields[i];
        f->user_touched = 0;
        f->input_flags = 0;
        f->rule_driven = 0;
        f->user_value[0] = L'\0';
        f->choice_tracked=f->custom_selected=0;
        f->custom_value[0]=f->choice_base[0]=L'\0';
        qp_strlcpy(f->value, seg_default_of(f->seg), QP_VALUE_MAX);
        /* 时间变量：开这份填写（新建 / 切模板 / 重置）时就按当下拍一张快照，
           写进「手动填写」那一层。放在这里而不是每次求值都取 time()，是为了
           让这个值稳定——否则在别处敲一个字，眼前的时间就自己往前跳一秒，
           而人根本没动它。要刷新成此刻，界面上有「现在」按钮。 */
        if (f->seg && f->seg->type == SEG_DATETIME) {
            qp_format_time_now(f->seg->time_format, f->user_value, QP_VALUE_MAX);
            f->user_touched = 1;
            qp_strlcpy(f->value, f->user_value, QP_VALUE_MAX);
        }
    }
    return qp_eval_run_checked(ctx);
}

void eval_reset(EvalContext *ctx)
{ (void)eval_reset_checked(ctx); }

int eval_index_of_id(const EvalContext *ctx, const wchar_t *id)
{
    int i;
    if (!ctx || !id || !*id) return -1;
    for (i = 0; i < ctx->count; ++i)
        if (ctx->fields[i].seg && wcscmp(ctx->fields[i].seg->id, id) == 0) return i;
    /* 兼容按变量名引用 */
    for (i = 0; i < ctx->count; ++i)
        if (ctx->fields[i].seg && ctx->fields[i].seg->label[0] &&
            qp_icmp(ctx->fields[i].seg->label, id) == 0) return i;
    return -1;
}

int eval_index_of_label(const EvalContext *ctx, const wchar_t *label)
{
    int i;
    if (!ctx || !label || !*label) return -1;
    for (i = 0; i < ctx->count; ++i)
        if (ctx->fields[i].seg && qp_icmp(ctx->fields[i].seg->label, label) == 0) return i;
    return -1;
}

void eval_set_user_value(EvalContext *ctx, int idx, const wchar_t *v)
{
    EvalField *f;
    if (!ctx || idx < 0 || idx >= ctx->count) return;
    f = &ctx->fields[idx];
    f->input_flags = v && wcslen(v)>=QP_VALUE_MAX ? QP_TEXT_TRUNCATED : 0;
    qp_strlcpy(f->user_value, v ? v : L"", QP_VALUE_MAX);
    f->user_touched = 1;
    /* 注意：手动输入不会压过变量匹配。该字段一旦被规则推导出值，
       eval_run 里就以规则值为准（详见 eval_run 第 3 步）。 */
}

/* 解析规则是否命中；命中时 extracted 返回提取到的内容 */
static int parse_rule_hit(const ParseRule *r, const wchar_t *input,
                          wchar_t *extracted, size_t cap)
{
    if (extracted && cap) extracted[0] = L'\0';
    if (!r || !input) return 0;
    if (r->type == PARSE_SPLIT) {
        wchar_t part[QP_VALUE_MAX];
        if (!qp_split_take(input, r->delimiter, r->segment_index, part, QP_VALUE_MAX))
            return 0;
        qp_trim(part);
        if (r->keyword[0]) {
            if (qp_icmp(part, r->keyword) != 0) return 0;
        } else if (!part[0]) {
            return 0;   /* 无关键词时，提取到空串视为未命中 */
        }
        if (extracted) qp_strlcpy(extracted, part, cap);
        return 1;
    }
    if (r->type == PARSE_BETWEEN) {
        /* 复用 keyword 作前标记、marker_after 作后标记 */
        wchar_t part[QP_VALUE_MAX];
        if (!qp_between_take(input, r->keyword, r->marker_after, part, QP_VALUE_MAX))
            return 0;
        qp_trim(part);
        if (!part[0]) return 0;   /* 取到空视为未命中，与 split 一致 */
        if (extracted) qp_strlcpy(extracted, part, cap);
        return 1;
    }
    if (r->type == PARSE_REGEX) {
        /* 正则：在源里搜索第一次匹配，取整体（capture_group = 0）或第 N 个
           捕获组。取到空串视为未命中，与 split / between 同一口径。 */
        QpRegexSpan sp[QP_REGEX_MAX_GROUPS];
        int g = r->capture_group;
        if (!r->pattern[0]) return 0;
        if (g < 0 || g >= QP_REGEX_MAX_GROUPS) return 0;
        if (!qp_regex_search(r->pattern, input, sp, QP_REGEX_MAX_GROUPS)) return 0;
        if (!sp[g].begin || sp[g].len == 0) return 0;
        if (extracted && cap) {
            size_t n = sp[g].len;
            n = qp_utf16_prefix(sp[g].begin,n,cap-1);
            wmemcpy(extracted, sp[g].begin, n);
            extracted[n] = L'\0';
        }
        return 1;
    }
    /* PARSE_CONTAINS */
    if (!r->keyword[0]) return 0;
    if (!qp_istr(input, r->keyword)) return 0;
    if (extracted) qp_strlcpy(extracted, r->keyword, cap);
    return 1;
}

/* 应用一条解析规则，命中则把「要填进目标的值」写进 out、返回 1。
   fill_value 非空则用它，否则用提取到的值 —— 与旧 step-1 的语义一致。
   per_paragraph 打开时：把输入按换行拆成多段，逐段抽取，命中的结果按换行
   合并（未命中的段跳过）；关闭时就是单段，行为与从前完全一样。 */
static int parse_rule_apply(const ParseRule *r, const wchar_t *input,
                            wchar_t *out, size_t cap, unsigned *flags)
{
    wchar_t extracted[QP_VALUE_MAX];
    if (out && cap) out[0] = L'\0';
    if (!r || !input || !out || cap == 0) return 0;

    if (!r->per_paragraph) {
        if (!parse_rule_hit(r, input, extracted, QP_VALUE_MAX)) return 0;
        const wchar_t *value=r->fill_value[0] ? r->fill_value : extracted;
        qp_append_checked(out,cap,0,value,wcslen(value),flags);
        return 1;
    }

    {
        const wchar_t *p = input, *nl;
        size_t used = 0;
        int any = 0;
        wchar_t line[QP_VALUE_MAX];
        for (;;) {
            size_t llen;
            nl = wcschr(p, L'\n');
            llen = nl ? (size_t)(nl - p) : wcslen(p);
            if (llen >= QP_VALUE_MAX) llen = QP_VALUE_MAX - 1;
            wmemcpy(line, p, llen);
            line[llen] = L'\0';
            if (llen && line[llen - 1] == L'\r') line[llen - 1] = L'\0';  /* CRLF */
            if (!qp_is_blank(line) &&
                parse_rule_hit(r, line, extracted, QP_VALUE_MAX)) {
                const wchar_t *val = r->fill_value[0] ? r->fill_value : extracted;
                const wchar_t *sep = r->paragraph_inline ? r->paragraph_separator : L"\n";
                if (any) used = qp_append_checked(out, cap, used, sep, wcslen(sep), flags);
                used = qp_append_checked(out, cap, used, val, wcslen(val), flags);
                any = 1;
            }
            if (!nl) break;
            p = nl + 1;
        }
        return any;
    }
}

/* 单个条件项是否成立（已计入 negate） */
int cond_term_hit(const CondTerm *t, const wchar_t *trigger_value)
{
    int hit = 0, i;
    if (!t) return 0;
    if (!trigger_value) trigger_value = L"";
    if (t->op != OP_EMPTY && qp_is_blank(trigger_value)) return 0;
    switch (t->op) {
    case OP_CONTAINS:
        hit = t->match_value[0] && qp_istr(trigger_value, t->match_value) != NULL;
        break;
    case OP_EQUALS:
        /* 允许匹配「等于空」 */
        hit = qp_icmp(trigger_value, t->match_value) == 0;
        break;
    case OP_STARTS_WITH:
        hit = t->match_value[0] && qp_istarts(trigger_value, t->match_value);
        break;
    case OP_ENDS_WITH:
        hit = t->match_value[0] && qp_iends(trigger_value, t->match_value);
        break;
    case OP_IN:
        for (i = 0; i < t->match_value_count; ++i)
            if (qp_icmp(trigger_value, t->match_values[i]) == 0) { hit = 1; break; }
        break;
    case OP_EMPTY:
        hit = qp_is_blank(trigger_value);
        break;
    default:
        hit = 0;
        break;
    }
    return t->negate ? !hit : hit;
}

/*
 * 整条规则是否命中：条件项按 join 组合。
 * 没有条件项 → 不命中（避免半成品规则无条件改写片段）。
 * 条件项指向不存在的变量 → 该项一律不成立（连 negate 也不生效），
 * 这样悬空规则永远不会凭空命中。
 */
static int cond_rule_hit(const EvalContext *ctx, const ConditionRule *r,
                         wchar_t (*work)[QP_VALUE_MAX])
{
    const CondTerm *t;
    if (!ctx || !r || !r->terms) return 0;
    for (t = r->terms; t; t = t->next) {
        int trig = eval_index_of_id(ctx, t->trigger_segment_id);
        int hit = (trig >= 0 && trig < ctx->count)
                    ? cond_term_hit(t, work[trig]) : 0;
        if (r->join == COND_JOIN_ANY) {
            if (hit) return 1;
        } else if (!hit) {
            return 0;
        }
    }
    return r->join == COND_JOIN_ANY ? 0 : 1;
}

/* 占位符取值：本轮的工作值，名字按「id 优先、其次变量名」找（与规则同一口径）。
   指向自己（固定片段用自己的 id 当占位符名）算认不出，免得自我展开。 */
const wchar_t *eval_placeholder_lookup(void *user, const wchar_t *name)
{
    const PlaceholderCtx *p = (const PlaceholderCtx *)user;
    int at;
    if (!p) return NULL;
    at = eval_index_of_id(p->ctx, name);
    if (at < 0 || at == p->self) return NULL;
    return p->work ? p->work[at] : p->ctx->fields[at].value;
}

/* A rule may deliberately produce an empty value: it is not a missing input. */
int eval_placeholder_missing(void *user, const wchar_t *name)
{
    const PlaceholderCtx *p = (const PlaceholderCtx *)user;
    int at = eval_index_of_id(p->ctx, name);
    if (at < 0 || at == p->self) return 0;
    return !(p->driven ? p->driven[at] : p->ctx->fields[at].rule_driven);
}

/*
 * 查表匹配：一次查表可以填好几个目标变量。
 *
 * 与其它解析规则的关系完全一致：一样是「推导值」，压过手填；一样按目标各自
 * 遵守「第一个命中的规则生效」（已经被前面的规则占下的目标就跳过）；查不到
 * 就整条不命中，目标回到手填 / 显式默认值，界面那一处会标出来。
 *
 * 键的来源三选一：本变量的值（默认）/ 另一个变量的值 / 一个固定值。
 * 空键一律不命中 —— 与解析规则「空来源不命中」同一个态度，不凭空填值。
 *
 * per_paragraph 打开时与其它解析规则同一口径：把键的来源按换行拆成多段，
 * 逐段各查一次，命中段的结果按换行合并写入各自的目标（未命中的段跳过）。
 * 一个变量里粘进多段编号、每段各查一行，就能一次填出多行结果。
 */
static void apply_lookup_rule(EvalContext *ctx, const ParseRule *r, int host,
                              const wchar_t (*work)[QP_VALUE_MAX],
                              wchar_t (*derived)[QP_VALUE_MAX],
                              int *derived_set, int *parse_set)
{
    const wchar_t *key = NULL;
    QpTable *tbl;
    int key_col;
    const LookupOut *o;

    (void)host;
    if (r->lk_key_mode == LOOKUP_KEY_FIX) {
        key = r->lk_key_value;
    } else if (r->lk_key_mode == LOOKUP_KEY_VAR) {
        int ki = eval_index_of_id(ctx, r->lk_key_var_id);
        if (ki < 0 || ki >= ctx->count) return;      /* 悬空：不命中 */
        key = work[ki];
    } else {
        key = work[host];
    }
    if (!key || qp_is_blank(key)) return;            /* 空键不命中 */

    errno=0;
    tbl = qp_table_get(r->lk_table_id, NULL, 0);
    if (!tbl) { if(errno==ENOMEM)ctx->output_flags|=QP_TEXT_NOMEM; return; }
    key_col = qp_table_col_index(tbl, r->lk_key_col, r->lk_key_col_index);
    if (key_col < 0) return;                         /* 键列找不到：不命中 */

    if (!r->per_paragraph) {
        QpTableMatch *match=qp_table_match(tbl,key_col,key);
        if(!match){ctx->output_flags|=QP_TEXT_NOMEM;return;}
        for (o = r->lk_outs; o; o = o->next) {
            wchar_t val[QP_VALUE_MAX];
            int target = eval_index_of_id(ctx, o->target_segment_id);
            int val_col;
            if (target < 0 || target >= ctx->count) continue;
            if (derived_set[target]) continue;           /* 更靠前的规则先占 */
            val_col = qp_table_col_index(tbl, o->col, o->col_index);
            if (val_col < 0) continue;                   /* 取值列找不到：这一项跳过 */
            QpTableLookupResult result=qp_table_match_value(match,val_col,r->lk_hit,r->lk_sep,r->lk_unique,val,QP_VALUE_MAX);
            if(result.failed)ctx->output_flags|=QP_TEXT_NOMEM;
            if(result.truncated)ctx->workspace->flags[target]|=QP_TEXT_TRUNCATED;
            if(!result.found || result.failed)continue;
            qp_strlcpy(derived[target], val, QP_VALUE_MAX);
            derived_set[target] = 1;
            parse_set[target] = 1;
        }
        qp_table_match_free(match);
        return;
    }

    /* ---- 逐段处理 ---- */
    {
        int nout = lookup_out_count(r->lk_outs), i, any = 0;
        int *tgt, *used, *matched;
        wchar_t *acc;
        const wchar_t *p, *nl;
        wchar_t line[QP_VALUE_MAX];

        if (nout <= 0) return;
        tgt  = (int *)malloc((size_t)nout * sizeof(int));
        used = (int *)calloc((size_t)nout, sizeof(int));
        matched = (int *)calloc((size_t)nout, sizeof(int));
        acc  = (wchar_t *)calloc((size_t)nout * QP_VALUE_MAX, sizeof(wchar_t));
        if (!tgt || !used || !matched || !acc) { free(tgt); free(used); free(matched); free(acc); ctx->output_flags|=QP_TEXT_NOMEM; return; }

        /* 先定下这条规则能碰哪些目标：目标悬空、取值列找不到、更靠前的规则
           已经占下、或同一个目标被本规则里更靠前的一项认领过的，一律不碰。 */
        i = 0;
        for (o = r->lk_outs; o; o = o->next, ++i) {
            int t = eval_index_of_id(ctx, o->target_segment_id);
            int j;
            tgt[i] = -1;
            if (t < 0 || t >= ctx->count) continue;
            if (derived_set[t]) continue;                /* 更靠前的规则先占 */
            if (qp_table_col_index(tbl, o->col, o->col_index) < 0) continue;
            for (j = 0; j < i; ++j) if (tgt[j] == t) break;
            if (j < i) continue;                         /* 同一目标只认第一项 */
            tgt[i] = t;
        }

        for (p = key; ; ) {
            size_t llen;
            nl = wcschr(p, L'\n');
            llen = nl ? (size_t)(nl - p) : wcslen(p);
            if (llen >= QP_VALUE_MAX) llen = QP_VALUE_MAX - 1;
            wmemcpy(line, p, llen);
            line[llen] = L'\0';
            if (llen && line[llen - 1] == L'\r') line[llen - 1] = L'\0';   /* CRLF */
            if (!qp_is_blank(line)) {
                QpTableMatch *match=qp_table_match(tbl,key_col,line);
                if(!match){ctx->output_flags|=QP_TEXT_NOMEM;break;}
                i = 0;
                for (o = r->lk_outs; o; o = o->next, ++i) {
                    wchar_t val[QP_VALUE_MAX];
                    int val_col;
                    size_t vlen;
                    if (tgt[i] < 0) continue;
                    val_col = qp_table_col_index(tbl, o->col, o->col_index);
                    if (val_col < 0) continue;
                    QpTableLookupResult result=qp_table_match_value(match,val_col,r->lk_hit,r->lk_sep,r->lk_unique,val,QP_VALUE_MAX);
                    if(result.failed)ctx->output_flags|=QP_TEXT_NOMEM;
                    if(result.truncated)ctx->workspace->flags[tgt[i]]|=QP_TEXT_TRUNCATED;
                    if(!result.found || result.failed)continue;
                    vlen = wcslen(val);
                    wchar_t *value=acc+(size_t)i*QP_VALUE_MAX;
                    const wchar_t *sep=r->paragraph_inline?r->paragraph_separator:L"\n";
                    if(matched[i]>0)used[i]=(int)qp_append_checked(value,QP_VALUE_MAX,(size_t)used[i],sep,wcslen(sep),&ctx->workspace->flags[tgt[i]]);
                    used[i]=(int)qp_append_checked(value,QP_VALUE_MAX,(size_t)used[i],val,vlen,&ctx->workspace->flags[tgt[i]]);
                    ++matched[i];
                    any = 1;
                }
                qp_table_match_free(match);
            }
            if (!nl) break;
            p = nl + 1;
        }

        if (any) {
            i = 0;
            for (o = r->lk_outs; o; o = o->next, ++i) {
                if (tgt[i] < 0 || matched[i] <= 0) continue;
                qp_strlcpy(derived[tgt[i]], acc + (size_t)i * QP_VALUE_MAX,
                           QP_VALUE_MAX);
                derived_set[tgt[i]] = 1;
                parse_set[tgt[i]] = 1;
            }
        }
        free(tgt); free(used); free(matched); free(acc);
    }
}

int qp_eval_run_checked(EvalContext *ctx)
{
    wchar_t (*work)[QP_VALUE_MAX];
    wchar_t (*derived)[QP_VALUE_MAX];
    int *derived_set;
    int *parse_set;      /* 本轮该目标已被解析规则占下 */
    int *condition_set;  /* 本轮该字段由条件规则给值 */
    int *ref_cycle;      /* 该字段的引用链上有环 → 整条不生效 */
    int *empty_slots;    /* 本轮各字段「占位符展开成空」的位置（count × 上限） */
    int *empty_n;        /* 本轮各字段记了几处 */
    int i, pass, rp;

    if (!ctx || ctx->count < 0) return 0;
    ctx->output_flags=0;
    if (ctx->count == 0) return 1;

    if (!ctx->workspace) ctx->workspace = workspace_create(ctx->count);
    if (!ctx->workspace) { ctx->output_flags=QP_TEXT_NOMEM; return 0; }
    work=ctx->workspace->work; derived=ctx->workspace->derived;
    derived_set=ctx->workspace->derived_set; parse_set=ctx->workspace->parse_set;
    condition_set=ctx->workspace->condition_set; ref_cycle=ctx->workspace->ref_cycle;
    empty_slots=ctx->workspace->empty_slots; empty_n=ctx->workspace->empty_n;
    if (!work || !derived || !derived_set || !parse_set || !condition_set || !ref_cycle ||
        !empty_slots || !empty_n) {
        ctx->output_flags|=QP_TEXT_NOMEM;
        return 0;
    }
    ctx->unstable = 0;
    if (!qp_table_read_begin()) {
        ctx->output_flags|=QP_TEXT_NOMEM;
        return 0;
    }

    /* 引用成环检测只跟模板结构有关，与填了什么无关，所以在迭代外面做一次。
       A→B→A 这种环没有「正确答案」，整条链一律当没配引用，退回手填/默认值，
       与「条件项指向不存在的变量一律不成立」同一个态度：宁可不生效，不能乱生效。 */
    for (i = 0; i < ctx->count; ++i) {
        const Segment *s = ctx->fields[i].seg;
        ref_cycle[i]=0;
        ctx->workspace->flags[i]=ctx->fields[i].input_flags;
        int at, depth;
        if (!s || !s->ref_segment_id[0]) continue;
        at = eval_index_of_id(ctx, s->ref_segment_id);
        for (depth = 0; at >= 0 && depth < QP_REF_MAX_DEPTH; ++depth) {
            const Segment *p = ctx->fields[at].seg;
            if (at == i) { ref_cycle[i] = 1; break; }
            if (!p || !p->ref_segment_id[0]) break;
            at = eval_index_of_id(ctx, p->ref_segment_id);
        }
        if (depth >= QP_REF_MAX_DEPTH) ref_cycle[i] = 1;   /* 走不完：链上有环 */
    }

    /* 初值：用户输入优先于默认值 */
    for (i = 0; i < ctx->count; ++i) {
        EvalField *f = &ctx->fields[i];
        if (f->user_touched)
            qp_strlcpy(work[i], f->user_value, QP_VALUE_MAX);
        else
            qp_strlcpy(work[i], field_fallback_of(f), QP_VALUE_MAX);
    }

    /* 迭代求不动点：规则之间可能互相依赖 */
    for (pass = 0; pass < QP_EVAL_MAX_PASS; ++pass) {
        int changed = 0;

        for (i = 0; i < ctx->count; ++i) {
            derived_set[i] = 0;
            parse_set[i] = 0;
            condition_set[i] = 0;
            derived[i][0] = L'\0';
        }

        /* 1) 解析规则：只有 text 变量参与，按顺序，每个目标取第一条命中的。
              查表规则一次能填多个目标，所以单独走一条分支（见 apply_lookup_rule）。 */
        for (i = 0; i < ctx->count; ++i) {
            const Segment *s = ctx->fields[i].seg;
            const ParseRule *r;
            if (!s || s->type != SEG_TEXT) continue;
            for (r = s->parse_rules; r; r = r->next) {
                wchar_t produced[QP_VALUE_MAX];
                int target;
                if (r->type == PARSE_LOOKUP) {
                    apply_lookup_rule(ctx, r, i, work, derived, derived_set,
                                      parse_set);
                    continue;
                }
                target = eval_index_of_id(ctx, r->target_segment_id);
                if (target < 0 || target >= ctx->count) continue;
                if (derived_set[target]) continue;          /* 第一个命中的生效 */
                if (!parse_rule_apply(r, work[i], produced, QP_VALUE_MAX, &ctx->workspace->flags[target])) continue;
                qp_strlcpy(derived[target], produced, QP_VALUE_MAX);
                derived_set[target] = 1;
                parse_set[target] = 1;
            }
        }

        /* 2) 引用：本片段的值直接取自另一个变量（整体或其中一部分）。
              解析规则已经推导出的目标不再被引用改写（解析规则更具体，优先），
              而下面第 3 步的条件规则仍可覆盖引用结果 —— 三者叠加时只有一个说法。

              内部再迭代一次是为了链式引用（A→B→C→D）：取源值时优先读本轮已经
              推导出的 derived，一轮之内就能传到链尾，不用靠外层那 8 轮预算去
              一级一级挪（链长超过 7 就会挪不完）。 */
        for (rp = 0; rp < QP_REF_MAX_DEPTH; ++rp) {
            int ref_changed = 0;
            for (i = 0; i < ctx->count; ++i) {
                const Segment *s = ctx->fields[i].seg;
                wchar_t picked[QP_VALUE_MAX];
                int src;
                if (!s || s->type == SEG_STATIC || !s->ref_segment_id[0]) continue;
                if (parse_set[i]) continue;              /* 解析规则占了这个目标 */
                src = eval_index_of_id(ctx, s->ref_segment_id);
                if (src < 0 || src == i) continue;       /* 悬空 / 自引用 → 视为没配 */
                if (ref_cycle[i]) continue;              /* 成环的那条链整条不生效 */
                seg_ref_extract(s, derived_set[src] ? derived[src] : work[src],
                                picked, QP_VALUE_MAX);
                if (derived_set[i] && wcscmp(derived[i], picked) == 0) continue;
                qp_strlcpy(derived[i], picked, QP_VALUE_MAX);
                derived_set[i] = 1;
                ref_changed = 1;
            }
            if (!ref_changed) break;
        }

        /* 3) 条件规则：挂在片段自身上，按顺序，命中即停止 */
        for (i = 0; i < ctx->count; ++i) {
            const Segment *s = ctx->fields[i].seg;
            const ConditionRule *r;
            if (!s) continue;
            for (r = s->condition_rules; r; r = r->next) {
                if (!cond_rule_hit(ctx, r, work)) continue;
                qp_strlcpy(derived[i], r->result_value, QP_VALUE_MAX);
                derived_set[i] = 1;
                condition_set[i] = 1;
                break;
            }
        }

        /* 4) 应用：推导值 > 用户输入 > 默认值。
           变量匹配（条件规则 / 解析规则 / 引用）推导出的值一律压过手动输入与
           手动选择，这样「既能手填又有匹配」的变量不会因为手滑而与口径脱节。 */
        for (i = 0; i < ctx->count; ++i) {
            EvalField *f = &ctx->fields[i];
            wchar_t next[QP_VALUE_MAX];
            if (empty_n) empty_n[i] = 0;
            if (derived_set[i]) {
                qp_strlcpy(next, derived[i], QP_VALUE_MAX);
            } else if (f->user_touched) {
                qp_strlcpy(next, f->user_value, QP_VALUE_MAX);
            } else {
                qp_strlcpy(next, field_fallback_of(f), QP_VALUE_MAX);
            }
            /* 固定片段或本轮命中的条件结果里的 {变量名} 换成那个变量此刻的值，
               同时把 \t \n \\ 这些转义折出来。放在这里是为了让它跟着不动点一起
               收敛；每轮都从模板原文 / 规则结果重新展开，所以只替换一层，不会
               越展开越长。手填、默认、解析与引用拿到的文本仍保持字面量。
               顺手把「展开成空串」的位置记下来（预览要据此标红），记的正是产出
               这一版值的那一次展开，所以位置与值永远对得上。 */
            if (f->seg &&
                ((f->seg->type == SEG_STATIC &&
                  (wcschr(next, L'{') || wcschr(next, L'\\'))) ||
                 (condition_set[i] && wcschr(next, L'{')))) {
                wchar_t expanded[QP_VALUE_MAX];
                PlaceholderCtx pc;
                pc.ctx = ctx;
                pc.work = work;
                pc.self = i;
                pc.driven = derived_set;
                qp_expand_placeholders_status(
                    next, eval_placeholder_lookup, &pc, expanded, QP_VALUE_MAX, 0,
                    empty_slots ? empty_slots + (size_t)i * QP_EMPTY_MARK_MAX : NULL,
                    QP_EMPTY_MARK_MAX,
                    empty_n ? &empty_n[i] : NULL,eval_placeholder_missing,&ctx->workspace->flags[i]);
                qp_strlcpy(next, expanded, QP_VALUE_MAX);
            }
            if (wcscmp(next, work[i]) != 0) {
                qp_strlcpy(work[i], next, QP_VALUE_MAX);
                changed = 1;
            }
        }

        if (!changed) break;
        /* 跑满还在变 = 规则互相覆盖，收不敛。以最后一轮为准，同时立个旗子，
           界面提示一句，别让人对着一个来回跳的预览猜自己哪里配错了。 */
        if (pass == QP_EVAL_MAX_PASS - 1) ctx->unstable = 1;
    }

    /* Allocation failure must not publish a partially evaluated candidate. */
    if(ctx->output_flags & QP_TEXT_NOMEM){
        qp_table_read_end();return 0;
    }
    /* 提交 */
    for (i = 0; i < ctx->count; ++i) {
        EvalField *f = &ctx->fields[i];
        int k, n = empty_n ? empty_n[i] : 0;
        if (n > QP_EMPTY_MARK_MAX) n = QP_EMPTY_MARK_MAX;
        f->rule_driven = derived_set[i];
        f->value_flags = ctx->workspace->flags[i];
        qp_strlcpy(f->value, work[i], QP_VALUE_MAX);
        f->empty_count = n;
        for (k = 0; k < n; ++k)
            f->empty_marks[k] = empty_slots[(size_t)i * QP_EMPTY_MARK_MAX + k];
    }

    qp_table_read_end();
    return 1;
}

void eval_run(EvalContext *ctx)
{ (void)qp_eval_run_checked(ctx); }

/* Truncation is relevant only if the rendered output depends on that field.
   Conservatively include declared rule inputs, even for a currently missed
   condition: clipping may itself have changed whether the rule matched. */
static int need_id(EvalContext *ctx, const wchar_t *id)
{
    int i=eval_index_of_id(ctx,id);
    if(i<0 || ctx->workspace->needed[i]) return 0;
    ctx->workspace->needed[i]=1;
    return 1;
}

static int need_placeholders(EvalContext *ctx, const wchar_t *text)
{
    wchar_t name[QP_LABEL_MAX];
    int changed=0;
    for(int n=0;qp_placeholder_at(text,n,name,QP_LABEL_MAX);++n)
        changed|=need_id(ctx,name);
    return changed;
}

unsigned qp_eval_output_flags(EvalContext *ctx, OutputMode mode)
{
    if(!ctx || !ctx->workspace) return ctx ? ctx->output_flags : QP_TEXT_NOMEM;
    int *needed=ctx->workspace->needed, changed;
    memset(needed,0,(size_t)ctx->count*sizeof(*needed));
    if(mode==OUTPUT_TABLE && ctx->tmpl->table_output_enabled)
        need_placeholders(ctx,ctx->tmpl->table_output_format);
    else if(mode==OUTPUT_TEXT)
        for(int i=0;i<ctx->count;++i)needed[i]=ctx->fields[i].seg && !ctx->fields[i].seg->no_output;
    do {
        changed=0;
        for(int i=0;i<ctx->count;++i) {
            const Segment *s=ctx->fields[i].seg;
            if(!s)continue;
            if(needed[i]) {
                changed|=need_id(ctx,s->ref_segment_id);
                if(s->type==SEG_STATIC)changed|=need_placeholders(ctx,s->content);
                for(const ConditionRule *r=s->condition_rules;r;r=r->next) {
                    for(const CondTerm *t=r->terms;t;t=t->next)changed|=need_id(ctx,t->trigger_segment_id);
                    changed|=need_placeholders(ctx,r->result_value);
                }
            }
            if(s->type!=SEG_TEXT)continue;
            for(const ParseRule *r=s->parse_rules;r;r=r->next) {
                int target=eval_index_of_id(ctx,r->target_segment_id), affects=target>=0 && needed[target];
                if(r->type==PARSE_LOOKUP) {
                    affects=0;
                    for(const LookupOut *o=r->lk_outs;o;o=o->next) {
                        target=eval_index_of_id(ctx,o->target_segment_id);
                        if(target>=0 && needed[target])affects=1;
                    }
                    if(affects && r->lk_key_mode==LOOKUP_KEY_VAR)changed|=need_id(ctx,r->lk_key_var_id);
                    if(r->lk_key_mode==LOOKUP_KEY_FIX || r->lk_key_mode==LOOKUP_KEY_VAR)continue;
                }
                if(affects && !needed[i]){needed[i]=1;changed=1;}
            }
        }
    }while(changed);
    unsigned flags=ctx->output_flags & QP_TEXT_NOMEM;
    for(int i=0;i<ctx->count;++i)if(needed[i])flags|=ctx->fields[i].value_flags;
    return flags;
}
