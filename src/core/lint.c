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

/* ================================================================== */
/* 模板体检                                                            */
/* ================================================================== */
/*
 * 保存模板时跑一遍，把「配好了、但永远不会生效」的规则挑出来。规则一多就没人
 * 能在脑子里把它们全跑一遍，写错了的表现又只是「少了一段」，很难往回找。
 *
 * 只报能证明的：判定用的是引擎自己的 cond_term_hit()，取值清单也只在真的能
 * 穷举时才算。穷举不了就闭嘴 —— 宁可漏报，不能冤枉一条好规则。
 */

/* 往取值清单里加一条，重复的不加；满了返回 0（调用方据此放弃穷举） */
static int lint_vals_add(const wchar_t **vals, int *n, int cap, const wchar_t *v)
{
    int i;
    if (!v) v = L"";
    for (i = 0; i < *n; ++i)
        if (wcscmp(vals[i], v) == 0) return 1;
    if (*n >= cap) return 0;
    vals[(*n)++] = v;
    return 1;
}

/*
 * 这个变量的取值能不能穷举？能就把清单写进 vals（指向模板里的原串，不拷贝），
 * 返回条数；穷举不了返回 -1。
 *
 * 穷举不了的情形一律不猜：引用来的、被别人的解析规则填的、能手打的、多选的
 * 组合、时间串、内容里带占位符的固定片段。
 * 能穷举时，清单里除了它自己的选项/默认值，还必须算上：
 *   - 空串（一条规则都没命中、也未填写过时它就是空）
 *   - 它自己每条条件规则的结果值（隐藏变量常常正是靠这个拿到值的）
 */
#define LINT_VALS_MAX 24

static int lint_value_set(const Template *t, const Segment *s,
                          const wchar_t **vals, int cap)
{
    const Segment *o;
    const ConditionRule *r;
    int n = 0, i;

    if (!t || !s || !vals || cap <= 0) return -1;
    if (s->ref_segment_id[0]) return -1;
    for (o = t->segments; o; o = o->next) {
        const ParseRule *p;
        for (p = o->parse_rules; p; p = p->next) {
            if (tmpl_find_segment(t, p->target_segment_id) == s) return -1;
            /* 查表规则的目标写在 lk_outs 里，它填的同样是「推导值」。
               漏掉这一段，一个「只由查表填值」的隐藏变量就会被当成只有
               默认值和空串，于是引用它的条件被冤枉成「永远不成立」——
               宁可漏报也不能冤枉好规则，所以这里一律不猜。 */
            {
                const LookupOut *lo;
                for (lo = p->lk_outs; lo; lo = lo->next)
                    if (tmpl_find_segment(t, lo->target_segment_id) == s) return -1;
            }
        }
    }

    switch (s->type) {
    case SEG_STATIC:
        if (wcschr(s->content, L'{')) return -1;   /* 占位符 → 是别人的值 */
        if (!lint_vals_add(vals, &n, cap, s->content)) return -1;
        break;
    case SEG_SELECT:
        for (i = 0; i < s->option_count; ++i)
            if (!lint_vals_add(vals, &n, cap,
                               s->options && s->options[i] ? s->options[i] : L""))
                return -1;
        if (!lint_vals_add(vals, &n, cap, s->default_value)) return -1;
        break;
    case SEG_TEXT:
        if (!s->hidden) return -1;                 /* 能手打 → 什么都可能 */
        if (!lint_vals_add(vals, &n, cap, s->default_value)) return -1;
        break;
    default:
        return -1;                                 /* 多选的组合、时间串 */
    }
    if (!lint_vals_add(vals, &n, cap, L"")) return -1;
    for (r = s->condition_rules; r; r = r->next)
        if (!lint_vals_add(vals, &n, cap, r->result_value)) return -1;
    return n;
}

/* 把取值清单写成人话：「计算服务器 / 存储服务器 / 空」，长了就省略 */
static void lint_vals_text(const wchar_t **vals, int n, wchar_t *out, size_t cap)
{
    size_t used = 0;
    int i;
    if (!out || cap == 0) return;
    out[0] = L'\0';
    for (i = 0; i < n; ++i) {
        const wchar_t *v = (vals[i] && vals[i][0]) ? vals[i] : L"空";
        if (used + wcslen(v) + 8 >= cap) {
            qp_strlcpy(out + used, L" …", cap - used);
            return;
        }
        if (used) {
            qp_strlcpy(out + used, L" / ", cap - used);
            used += wcslen(out + used);
        }
        qp_strlcpy(out + used, v, cap - used);
        used += wcslen(out + used);
    }
}

static void lint_add(LintIssue *out, int *n, int cap, LintKind kind,
                     int seg_index, int rule_index, const wchar_t *text)
{
    LintIssue *it;
    if (!out || !n || *n >= cap) return;
    it = &out[*n];
    it->kind = kind;
    it->seg_index = seg_index;
    it->rule_index = rule_index;
    qp_strlcpy(it->text, text, QP_RESULT_MAX);
    ++(*n);
}

static int lint_label_match_count(const Template *t, const wchar_t *label)
{
    const Segment *s;
    int count = 0;
    if (!t || !label || !label[0]) return 0;
    for (s = t->segments; s; s = s->next)
        if (s->label[0] && qp_icmp(s->label, label) == 0)
            ++count;
    return count;
}

/* 查表规则的体检：配没配齐、表在不在、列名对不对、键唯不唯一、目标还在不在。
   一律「只提醒不拦人」，判据保守：读不通表就报读不通，不猜结果。 */
static void lint_lookup(const Template *t, const Segment *host, int si,
                        const wchar_t *name, int ri, const ParseRule *p,
                        LintIssue *out, int *n, int cap, wchar_t *msg)
{
    DataTableRef *ref;
    QpTable *tbl = NULL;
    wchar_t err[256];
    int key_col = -1, k;

    (void)host;
    if (!p) return;

    /* ① 配齐了没有：没配的地方一次说清，别让用户改一条试一次 */
    if (!p->lk_outs) {
        swprintf(msg, QP_RESULT_MAX,
                 L"第 %d 段（%ls）第 %d 条查表规则：没有配置任何输出列，"
                 L"即使查到数据也无处填写。", si + 1, name, ri + 1);
        lint_add(out, n, cap, LINT_LOOKUP_SETUP, si, ri, msg);
    } else {
        /* 配了输出列、却一个目标变量都没选：引擎里这些项全部跳过，
           整条规则查到了也无法填写目标变量 —— 与「没配输出列」一样是死规则。 */
        const LookupOut *lo;
        int total = 0, dangling = 0;
        for (lo = p->lk_outs; lo; lo = lo->next) {
            ++total;
            if (!tmpl_find_segment(t, lo->target_segment_id)) ++dangling;
        }
        if (total > 0 && dangling == total) {
            swprintf(msg, QP_RESULT_MAX,
                     L"第 %d 段（%ls）第 %d 条查表规则：%d 个输出列都没有选择"
                     L"目标变量，这条规则查到数据也无法填写目标变量。",
                     si + 1, name, ri + 1, total);
            lint_add(out, n, cap, LINT_LOOKUP_SETUP, si, ri, msg);
        }
    }
    if (p->lk_key_mode == LOOKUP_KEY_VAR &&
        !tmpl_find_segment(t, p->lk_key_var_id)) {
        swprintf(msg, QP_RESULT_MAX,
                 L"第 %d 段（%ls）第 %d 条查表规则：作为键来源的变量%ls，"
                 L"这条规则不会生效。", si + 1, name, ri + 1,
                 p->lk_key_var_id[0] ? L"已被删除" : L"尚未选择");
        lint_add(out, n, cap, LINT_LOOKUP_SETUP, si, ri, msg);
    }
    if (p->lk_key_mode == LOOKUP_KEY_FIX && qp_is_blank(p->lk_key_value)) {
        /* 固定值当键、值却是空的：引擎里空键一律不命中，这条规则不会生效 */
        swprintf(msg, QP_RESULT_MAX,
                 L"第 %d 段（%ls）第 %d 条查表规则：键来源选的是「固定值」，"
                 L"但固定值是空的，空键不会命中，这条规则不会生效。",
                 si + 1, name, ri + 1);
        lint_add(out, n, cap, LINT_LOOKUP_SETUP, si, ri, msg);
    }
    if (!p->lk_table_id[0]) {
        swprintf(msg, QP_RESULT_MAX,
                 L"第 %d 段（%ls）第 %d 条查表规则：尚未选择数据表，"
                 L"这条规则不会生效。", si + 1, name, ri + 1);
        lint_add(out, n, cap, LINT_LOOKUP_SETUP, si, ri, msg);
        return;
    }

    /* ② 表在不在、读不读得通 */
    ref = qp_table_find_ref(p->lk_table_id);
    if (!ref) {
        swprintf(msg, QP_RESULT_MAX,
                 L"第 %d 段（%ls）第 %d 条查表规则：引用的数据表已被删除，"
                 L"这条规则不会生效。", si + 1, name, ri + 1);
        lint_add(out, n, cap, LINT_LOOKUP_TABLE, si, ri, msg);
        return;
    }
    err[0] = L'\0';
    tbl = qp_table_get(p->lk_table_id, err, 256);
    if (!tbl) {
        swprintf(msg, QP_RESULT_MAX,
                 L"第 %d 段（%ls）第 %d 条查表规则：数据表「%ls」无法读取"
                 L"（%ls）。规则不会生效。", si + 1, name, ri + 1, ref->name,
                 err[0] ? err : L"原因未知");
        lint_add(out, n, cap, LINT_LOOKUP_TABLE, si, ri, msg);
        return;
    }

    /* ③ 列名在表里找不找得到 */
    if (p->lk_key_col[0] || p->lk_key_col_index > 0) {
        key_col = qp_table_col_index(tbl, p->lk_key_col, p->lk_key_col_index);
        if (key_col < 0) {
            swprintf(msg, QP_RESULT_MAX,
                     L"第 %d 段（%ls）第 %d 条查表规则：键列「%ls」在表「%ls」里"
                     L"不存在（表头可能已改名），规则不会生效。",
                     si + 1, name, ri + 1, p->lk_key_col, ref->name);
            lint_add(out, n, cap, LINT_LOOKUP_COL, si, ri, msg);
        }
    }
    {
        const LookupOut *o;
        int oi = 0;
        for (o = p->lk_outs; o; o = o->next, ++oi) {
            if (*n >= cap) break;
            if (!tmpl_find_segment(t, o->target_segment_id)) {
                swprintf(msg, QP_RESULT_MAX,
                         L"第 %d 段（%ls）第 %d 条查表规则的第 %d 个输出："
                         L"目标变量%ls，这一项不会被填写。", si + 1, name, ri + 1, oi + 1,
                         o->target_segment_id[0] ? L"已被删除" : L"尚未选择");
                lint_add(out, n, cap, LINT_LOOKUP_TARGET, si, ri, msg);
            }
            if (!o->col[0] && o->col_index < 0) {
                /* 取值列没选：引擎里这一项直接跳过，等于白配 */
                swprintf(msg, QP_RESULT_MAX,
                         L"第 %d 段（%ls）第 %d 条查表规则的第 %d 个输出："
                         L"尚未选择取值列，这一项不会被填写。",
                         si + 1, name, ri + 1, oi + 1);
                lint_add(out, n, cap, LINT_LOOKUP_COL, si, ri, msg);
            } else if (o->col[0] && qp_table_col_index(tbl, o->col, -1) < 0) {
                swprintf(msg, QP_RESULT_MAX,
                         L"第 %d 段（%ls）第 %d 条查表规则的第 %d 个输出："
                         L"取值列「%ls」在表「%ls」中不存在，这一项不会被填写。",
                         si + 1, name, ri + 1, oi + 1, o->col, ref->name);
                lint_add(out, n, cap, LINT_LOOKUP_COL, si, ri, msg);
            }
        }
    }

    /* ④ 键唯不唯一：键重复时，取第一条 / 最后一条 的差别很大，值得提醒一句 */
    if (key_col >= 0) {
        int rows = qp_table_rows(tbl);
        int scan = rows > 20000 ? 20000 : rows;
        int dup = 0;
        for (k = 0; k < scan && dup < 5; ++k) {
            const wchar_t *kv = qp_table_cell(tbl, k, key_col);
            if (!kv || !*kv) continue;
            if (qp_table_count(tbl, key_col, kv) > 1) ++dup;
        }
        if (dup > 0) {
            swprintf(msg, QP_RESULT_MAX,
                     L"第 %d 段（%ls）第 %d 条查表规则：键列「%ls」不是唯一键"
                     L"（前 %d 行里就有重复），当前命中策略是「%ls」——"
                     L"请确认当前命中策略是否符合预期。",
                     si + 1, name, ri + 1,
                     p->lk_key_col[0] ? p->lk_key_col : L"(按列号)",
                     scan, qp_table_hit_name(p->lk_hit));
            lint_add(out, n, cap, LINT_LOOKUP_DUPKEY, si, ri, msg);
        }
    }
}

int tmpl_lint(const Template *t, LintIssue *out, int cap)
{
    const Segment *s;
    int n = 0, si = 0;

    if (!t || !out || cap <= 0) return 0;

    for (s = t->segments; s && n < cap; s = s->next, ++si) {
        const ConditionRule *r;
        const ParseRule *p;
        wchar_t msg[QP_RESULT_MAX];
        const wchar_t *name = seg_display_name(s);
        int ri, k;

        /* 固定内容里的占位符名字认不出来 → 生成时会把 {名字} 原样打出去 */
        if (s->type == SEG_STATIC) {
            wchar_t ph[QP_LABEL_MAX];
            for (k = 0; n < cap &&
                        qp_placeholder_at(s->content, k, ph, QP_LABEL_MAX); ++k) {
                if (tmpl_find_segment(t, ph)) continue;
                swprintf(msg, QP_RESULT_MAX,
                         L"第 %d 段（%ls）：占位符「{%ls}」没有对应的变量，"
                         L"生成时将按原样输出。", si + 1, name, ph);
                lint_add(out, &n, cap, LINT_PLACEHOLDER, si, -1, msg);
            }
        }

        /* 解析规则的目标变量已被删除 / 正则模式写坏了 / 查表规则配错了 */
        ri = 0;
        for (p = s->parse_rules; p && n < cap; p = p->next, ++ri) {
            /* 解析规则只对文本变量跑（见 eval_run 里那圈循环）。编辑器里这一栏
               也只对文本变量显示，所以正常配不出这种组合 —— 但先把变量配成文本、
               再改成单选/多选/时间就会留下它：规则还在数据里、永远不会运行，
               而编辑器中那一栏已经藏起来，用户既看不见也删不掉。体检得说一句。 */
            if (s->type != SEG_TEXT) {
                swprintf(msg, QP_RESULT_MAX,
                         L"第 %d 段（%ls）第 %d 条解析规则：这个变量现在的类型是"
                         L"「%ls」，解析规则只对文本变量生效，这条规则不会运行。"
                         L"如需保留该规则，请将类型改回「文本」；不再需要时可删除该规则。",
                         si + 1, name, ri + 1, seg_type_name(s->type));
                lint_add(out, &n, cap, LINT_PARSE_NONTEXT, si, ri, msg);
                continue;
            }
            if (p->type != PARSE_LOOKUP &&
                !tmpl_find_segment(t, p->target_segment_id)) {
                swprintf(msg, QP_RESULT_MAX,
                         L"第 %d 段（%ls）第 %d 条解析规则：目标变量%ls，"
                         L"这条规则不会生效。", si + 1, name, ri + 1,
                         p->target_segment_id[0] ? L"已被删除" : L"尚未选择");
                lint_add(out, &n, cap, LINT_PARSE_DANGLING, si, ri, msg);
            }
            if (p->type == PARSE_REGEX && n < cap) {
                int epos = -1;
                if (!p->pattern[0]) {
                    swprintf(msg, QP_RESULT_MAX,
                             L"第 %d 段（%ls）第 %d 条解析规则是正则匹配，但未填写"
                             L"正则表达式，这条规则永远不会命中。", si + 1, name, ri + 1);
                    lint_add(out, &n, cap, LINT_PARSE_REGEX, si, ri, msg);
                } else if (!qp_regex_valid(p->pattern, &epos)) {
                    swprintf(msg, QP_RESULT_MAX,
                             L"第 %d 段（%ls）第 %d 条解析规则的正则表达式在位置 %d 附近"
                             L"无法解析，这条规则永远不会命中。",
                             si + 1, name, ri + 1, epos + 1);
                    lint_add(out, &n, cap, LINT_PARSE_REGEX, si, ri, msg);
                }
            }
            if (p->type == PARSE_LOOKUP) lint_lookup(t, s, si, name, ri, p,
                                                     out, &n, cap, msg);
        }

        ri = 0;
        for (r = s->condition_rules; r && n < cap; r = r->next, ++ri) {
            const CondTerm *term;
            int ti = 0;

            if (!r->terms) {
                swprintf(msg, QP_RESULT_MAX,
                         L"第 %d 段（%ls）第 %d 条条件规则尚未配置条件，"
                         L"永远不会命中。", si + 1, name, ri + 1);
                lint_add(out, &n, cap, LINT_COND_NOTERM, si, ri, msg);
                continue;
            }
            if (!r->result_value[0] && n < cap) {
                swprintf(msg, QP_RESULT_MAX,
                         L"第 %d 段（%ls）第 %d 条条件规则命中后会把这一段变成空，"
                         L"请确认是否符合预期。", si + 1, name, ri + 1);
                lint_add(out, &n, cap, LINT_COND_EMPTY, si, ri, msg);
            }

            /*
             * 逐个条件项看。能证明「永远不成立」的记下来，但规则整体死不死要看
             * 组合方式：全部满足时一项死就够了，任意满足时得全都死。所以这里
             * 只攒证据，出不出这一条留到循环之后判 —— 一条规则最多报一句。
             */
            {
                int total = 0, never = 0;
                wchar_t dead[QP_RESULT_MAX];
                dead[0] = L'\0';

                for (term = r->terms; term && n < cap; term = term->next, ++ti) {
                    const Segment *trig = tmpl_find_segment(t, term->trigger_segment_id);
                    const wchar_t *vals[LINT_VALS_MAX];
                    wchar_t one[QP_MATCH_MAX + QP_LABEL_MAX + 32];
                    wchar_t list[256];
                    int nv, j, hit = 0;

                    ++total;
                    if (!trig) {
                        swprintf(msg, QP_RESULT_MAX,
                                 L"第 %d 段（%ls）第 %d 条条件规则的第 %d 个条件："
                                 L"所指的变量%ls，这个条件永远不成立。",
                                 si + 1, name, ri + 1, ti + 1,
                                 term->trigger_segment_id[0] ? L"已被删除" : L"尚未选择");
                        lint_add(out, &n, cap, LINT_COND_DANGLING, si, ri, msg);
                        ++never;      /* 引擎里悬空项一律不成立，算进证据 */
                        continue;
                    }
                    /* 取反的条件、「为空」这种不带值的，不做穷举判断：前者一穷举
                       就容易反过来冤枉人，后者的取值清单里永远有空串，本来也报不出。 */
                    if (term->negate || !cond_op_needs_value(term->op)) continue;

                    nv = lint_value_set(t, trig, vals, LINT_VALS_MAX);
                    if (nv <= 0) continue;
                    for (j = 0; j < nv && !hit; ++j)
                        hit = cond_term_hit(term, vals[j]);
                    if (hit) continue;

                    ++never;
                    if (!dead[0]) {   /* 只留第一条，说明里举一个例子就够 */
                        cond_term_summary(term, t, one, sizeof(one) / sizeof(one[0]));
                        lint_vals_text(vals, nv, list, sizeof(list) / sizeof(list[0]));
                        swprintf(dead, QP_RESULT_MAX,
                                 L"条件「%ls」永远不成立（%ls 只可能是：%ls）",
                                 one, seg_display_name(trig), list);
                    }
                }

                if (dead[0] && n < cap) {
                    if (r->join != COND_JOIN_ANY)
                        swprintf(msg, QP_RESULT_MAX,
                                 L"第 %d 段（%ls）第 %d 条条件规则：%ls，"
                                 L"要求全部满足，这条规则不会生效。",
                                 si + 1, name, ri + 1, dead);
                    else if (never >= total)
                        swprintf(msg, QP_RESULT_MAX,
                                 L"第 %d 段（%ls）第 %d 条条件规则：每个条件都永远"
                                 L"不成立（例如%ls），这条规则不会生效。",
                                 si + 1, name, ri + 1, dead);
                    else
                        swprintf(msg, QP_RESULT_MAX,
                                 L"第 %d 段（%ls）第 %d 条条件规则：%ls，"
                                 L"任意满足即可，因此规则仍可命中，但这一项永远不会成立。",
                                 si + 1, name, ri + 1, dead);
                    lint_add(out, &n, cap, LINT_COND_NEVER, si, ri, msg);
                }
            }
        }
    }

    /* 批量模式的源变量：没选 / 已删除 / 不是文本变量 → 这份批量出不来，提醒一句。
       与其它体检项一样只提醒不拦人。 */
    if (t->batch_mode && n < cap) {
        wchar_t msg[QP_RESULT_MAX];
        const Segment *bs = t->batch_source_id[0]
                          ? tmpl_find_segment(t, t->batch_source_id) : NULL;
        if (!bs) {
            swprintf(msg, QP_RESULT_MAX,
                     L"已开启「批量」，但源变量%ls，无法按行重复输出。",
                     t->batch_source_id[0] ? L"已被删除" : L"尚未选择");
            lint_add(out, &n, cap, LINT_BATCH_SOURCE, -1, -1, msg);
        } else if (bs->type != SEG_TEXT) {
            swprintf(msg, QP_RESULT_MAX,
                     L"已开启「批量」，但源变量「%ls」不是文本变量，"
                     L"请选择文本输入变量作为批量源。", seg_display_name(bs));
            lint_add(out, &n, cap, LINT_BATCH_SOURCE,
                     seg_list_index_of(t->segments, bs), -1, msg);
        }
    }

    /* 表格格式按变量名取值，重名时不像其它引用能用 id 消歧，必须提醒。 */
    if (t->table_output_enabled && n < cap) {
        wchar_t msg[QP_RESULT_MAX];
        if (!t->table_output_format[0]) {
            swprintf(msg, QP_RESULT_MAX,
                     L"已启用「表格输出」，但表格格式为空，无法生成表格。");
            lint_add(out, &n, cap, LINT_PLACEHOLDER, -1, -1, msg);
        } else {
            wchar_t ph[QP_LABEL_MAX];
            int k;
            for (k = 0; n < cap &&
                        qp_placeholder_at(t->table_output_format, k, ph, QP_LABEL_MAX); ++k) {
                int matches = lint_label_match_count(t, ph);
                if (matches == 1) continue;
                if (matches == 0) {
                    swprintf(msg, QP_RESULT_MAX,
                             L"表格格式中的占位符「{%ls}」没有对应的变量，"
                             L"生成时将按原样输出。", ph);
                } else {
                    swprintf(msg, QP_RESULT_MAX,
                             L"表格格式中的占位符「{%ls}」匹配到 %d 个同名变量，"
                             L"名称不唯一，无法确定取值。", ph, matches);
                }
                lint_add(out, &n, cap, LINT_PLACEHOLDER, -1, -1, msg);
            }
            if (n < cap && !wcsstr(t->table_output_format, L"\\t") &&
                !wcschr(t->table_output_format, L'\t')) {
                swprintf(msg, QP_RESULT_MAX,
                         L"已启用「表格输出」，但表格格式没有分列符（\\t 或实际 Tab），"
                         L"不会形成多列。");
                lint_add(out, &n, cap, LINT_PLACEHOLDER, -1, -1, msg);
            }
        }
    }

    return n;
}
