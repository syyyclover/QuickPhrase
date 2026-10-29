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
/* 显示名称 / 摘要                                                     */
/* ================================================================== */

const wchar_t *seg_type_name(SegmentType t)
{
    switch (t) {
    case SEG_STATIC:       return L"固定";
    case SEG_TEXT:         return L"文本";
    case SEG_SELECT:       return L"单选";
    case SEG_MULTI_SELECT: return L"多选";
    case SEG_DATETIME:     return L"时间";
    default:               return L"?";
    }
}

const wchar_t *parse_type_name(ParseRuleType t)
{
    switch (t) {
    case PARSE_SPLIT:    return L"分割取段";
    case PARSE_CONTAINS: return L"包含关键词";
    case PARSE_BETWEEN:  return L"取两标记之间";
    case PARSE_REGEX:    return L"正则匹配";
    case PARSE_LOOKUP:   return L"查表匹配";
    default:             return L"?";
    }
}

const wchar_t *cond_op_name(CondOperator op)
{
    switch (op) {
    case OP_CONTAINS:    return L"包含";
    case OP_EQUALS:      return L"等于";
    case OP_STARTS_WITH: return L"开头是";
    case OP_ENDS_WITH:   return L"结尾是";
    case OP_IN:          return L"属于列表";
    case OP_EMPTY:       return L"为空";
    default:             return L"?";
    }
}

/* 摘要里用的自然说法：取反时直接说「不包含」而不是「不满足 包含」 */
const wchar_t *cond_op_phrase(CondOperator op, int negate)
{
    if (!negate) return cond_op_name(op);
    switch (op) {
    case OP_CONTAINS:    return L"不包含";
    case OP_EQUALS:      return L"不等于";
    case OP_STARTS_WITH: return L"开头不是";
    case OP_ENDS_WITH:   return L"结尾不是";
    case OP_IN:          return L"不属于列表";
    case OP_EMPTY:       return L"不为空";
    default:             return L"?";
    }
}

int cond_op_needs_value(CondOperator op)
{
    return op != OP_EMPTY;
}

static const wchar_t *seg_name_by_id(const Template *t, const wchar_t *id)
{
    const Segment *s;
    /* 「没选」和「选的那个已经被删了」是两回事：后者的规则/引用一律不生效，
       摘要里必须说清楚，否则界面上看着配好了、实际永远不命中。 */
    if (!id || !id[0]) return L"(未指定)";
    s = tmpl_find_segment(t, id);
    if (!s) return L"(已删除的变量)";
    return seg_display_name(s);
}

/* 查表规则的摘要：查表[值班表] 键=设备编号 → 联系人、电话 */
void lookup_rule_summary(const ParseRule *r, const Template *t,
                         wchar_t *out, size_t cap)
{
    const wchar_t *tname;
    const LookupOut *o;
    size_t used = 0;
    int n = 0;

    if (!out || cap == 0) return;
    out[0] = L'\0';
    if (!r) return;
    tname = qp_table_ref_name(r->lk_table_id);
    if (!r->lk_table_id[0]) tname = L"（未选择表）";
    else if (!tname) tname = L"(表已删除)";

    if (r->lk_key_mode == LOOKUP_KEY_FIX)
        swprintf(out, cap, L"查表[%ls] 键=固定值「%ls」", tname, r->lk_key_value);
    else if (r->lk_key_mode == LOOKUP_KEY_VAR)
        swprintf(out, cap, L"查表[%ls] 键=%ls", tname,
                 seg_name_by_id(t, r->lk_key_var_id));
    else
        swprintf(out, cap, L"查表[%ls] 键=本变量", tname);
    out[cap - 1] = L'\0';
    used = wcslen(out);

    for (o = r->lk_outs; o; o = o->next) {
        wchar_t one[QP_LABEL_MAX + QP_LABEL_MAX + 8];
        size_t one_cap = QP_LABEL_MAX + QP_LABEL_MAX + 8;
        used = wcslen(out);
        if (used + 4 >= cap) break;
        if (o->col[0])
            swprintf(one, one_cap, L"%ls「%ls」", n == 0 ? L" → " : L"、", o->col);
        else
            swprintf(one, one_cap, L"%ls第 %d 列", n == 0 ? L" → " : L"、",
                     o->col_index + 1);
        one[one_cap - 1] = L'\0';
        qp_strlcpy(out + used, one, cap - used);
        ++n;
    }
    used = wcslen(out);
    if (n == 0) qp_strlcpy(out + used, L" → （未配置输出映射）", cap - used);
    if (r->lk_hit == LOOKUP_LAST) qp_strlcpy(out + wcslen(out), L"（取最后一条）",
                                             cap - wcslen(out));
    else if (r->lk_hit == LOOKUP_ALL) qp_strlcpy(out + wcslen(out), L"（全部拼接）",
                                                 cap - wcslen(out));
}

void parse_rule_summary(const ParseRule *r, const Template *t, wchar_t *out, size_t cap)
{
    const wchar_t suffix[] = L"（逐段）";
    size_t body_cap = cap;
    if (!out || cap == 0) return;
    out[0] = L'\0';
    if (!r) return;
    /* 为逐段标记预留空间；极小缓冲仍保留终止符，标记按可用空间截断。 */
    if (r->per_paragraph) {
        size_t suffix_len = sizeof(suffix) / sizeof(suffix[0]) - 1;
        body_cap = cap > suffix_len ? cap - suffix_len : 1;
    }
    if (r->type == PARSE_SPLIT) {
        if (r->keyword[0])
            swprintf(out, body_cap, L"按「%ls」分割取第 %d 段，命中「%ls」→ %ls = 「%ls」",
                     r->delimiter, r->segment_index, r->keyword,
                     seg_name_by_id(t, r->target_segment_id),
                     r->fill_value[0] ? r->fill_value : L"(提取值)");
        else
            swprintf(out, body_cap, L"按「%ls」分割取第 %d 段 → %ls = 「%ls」",
                     r->delimiter, r->segment_index,
                     seg_name_by_id(t, r->target_segment_id),
                     r->fill_value[0] ? r->fill_value : L"(提取值)");
    } else if (r->type == PARSE_BETWEEN) {
        swprintf(out, body_cap, L"取「%ls」与「%ls」之间 → %ls = 「%ls」",
                 r->keyword[0] ? r->keyword : L"开头",
                 r->marker_after[0] ? r->marker_after : L"末尾",
                 seg_name_by_id(t, r->target_segment_id),
                 r->fill_value[0] ? r->fill_value : L"(提取值)");
    } else if (r->type == PARSE_REGEX) {
        if (r->capture_group > 0)
            swprintf(out, body_cap, L"正则「%ls」取第 %d 个捕获组 → %ls = 「%ls」",
                     r->pattern, r->capture_group,
                     seg_name_by_id(t, r->target_segment_id),
                     r->fill_value[0] ? r->fill_value : L"(捕获内容)");
        else
            swprintf(out, body_cap, L"正则「%ls」取整体匹配 → %ls = 「%ls」",
                     r->pattern, seg_name_by_id(t, r->target_segment_id),
                     r->fill_value[0] ? r->fill_value : L"(整体匹配)");
    } else if (r->type == PARSE_LOOKUP) {
        lookup_rule_summary(r, t, out, body_cap);
    } else {
        swprintf(out, body_cap, L"包含「%ls」→ %ls = 「%ls」",
                 r->keyword, seg_name_by_id(t, r->target_segment_id),
                 r->fill_value[0] ? r->fill_value : L"(关键词)");
    }
    /* swprintf 截断时不保证写入终止符，任何后续字符串读取前都显式封口。 */
    out[body_cap - 1] = L'\0';
    if (r->per_paragraph) {
        size_t u = wcslen(out);
        qp_strlcpy(out + u, suffix, cap - u);
    }
}

void cond_term_summary(const CondTerm *term, const Template *t, wchar_t *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = L'\0';
    if (!term) return;
    if (cond_op_needs_value(term->op))
        swprintf(out, cap, L"%ls %ls「%ls」",
                 seg_name_by_id(t, term->trigger_segment_id),
                 cond_op_phrase(term->op, term->negate), term->match_value);
    else
        swprintf(out, cap, L"%ls %ls",
                 seg_name_by_id(t, term->trigger_segment_id),
                 cond_op_phrase(term->op, term->negate));
}

void cond_rule_summary(const ConditionRule *r, const Template *t, wchar_t *out, size_t cap)
{
    const wchar_t *joiner;
    const CondTerm *term;
    size_t used;
    int n = 0;

    if (!out || cap == 0) return;
    out[0] = L'\0';
    if (!r) return;

    joiner = (r->join == COND_JOIN_ANY) ? L" 或 " : L" 且 ";
    qp_strlcpy(out, L"如果 ", cap);
    used = wcslen(out);
    for (term = r->terms; term; term = term->next, ++n) {
        wchar_t one[QP_MATCH_MAX + QP_LABEL_MAX + 32];
        if (n > 0) {
            qp_strlcpy(out + used, joiner, cap - used);
            used += wcslen(out + used);
        }
        cond_term_summary(term, t, one, sizeof(one) / sizeof(one[0]));
        qp_strlcpy(out + used, one, cap - used);
        used += wcslen(out + used);
        if (used + 8 >= cap) break;
    }
    if (n == 0) {
        qp_strlcpy(out, L"（未配置条件）", cap);
        used = wcslen(out);
    }
    if (used + 16 < cap) {
        wchar_t tail[QP_RESULT_MAX + 32];
        swprintf(tail, sizeof(tail) / sizeof(tail[0]), L" → 内容 = 「%ls」", r->result_value);
        qp_strlcpy(out + used, tail, cap - used);
    }
}
