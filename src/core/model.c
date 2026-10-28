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
/* 片段                                                                */
/* ================================================================== */

Segment *seg_create(SegmentType type)
{
    Segment *s = (Segment *)calloc(1, sizeof(Segment));
    if (!s) return NULL;
    qp_new_id(s->id, QP_ID_MAX, L"seg");
    s->type = type;
    qp_strlcpy(s->separator, L"、", QP_SEP_MAX);
    /* 引用参数的默认值：第 1 段 / 第 1 个字起，取到末尾 */
    s->ref_mode = REF_WHOLE;
    s->ref_index = 1;
    s->ref_start = 1;
    s->ref_len = 0;
    if (type == SEG_DATETIME) qp_strlcpy(s->time_format, qp_time_format_at(0), QP_FORMAT_MAX);
    return s;
}

static void seg_free_options(Segment *s)
{
    int i;
    if (!s || !s->options) return;
    for (i = 0; i < s->option_count; ++i) free(s->options[i]);
    free(s->options);
    s->options = NULL;
    s->option_count = 0;
}

void seg_free(Segment *s)
{
    ParseRule *pr;
    ConditionRule *cr;
    if (!s) return;
    seg_free_options(s);
    pr = s->parse_rules;
    while (pr) { ParseRule *n = pr->next; parse_rule_free(pr); pr = n; }
    cr = s->condition_rules;
    while (cr) { ConditionRule *n = cr->next; cond_rule_free(cr); cr = n; }
    free(s);
}

int qp_seg_set_options(Segment *s, const wchar_t *const *opts, int count)
{
    Segment candidate = {0};
    if (!s || count < 0 || (count > 0 && !opts)) return 0;
    if (count > 0) {
        candidate.options = (wchar_t **)calloc((size_t)count, sizeof(wchar_t *));
        if (!candidate.options) return 0;
        candidate.option_count = count;
        for (int i = 0; i < count; ++i) {
            candidate.options[i] = qp_wcsdup(opts[i]);
            if (!candidate.options[i]) { seg_free_options(&candidate); return 0; }
        }
    }
    seg_free_options(s);
    s->options = candidate.options;
    s->option_count = candidate.option_count;
    if(s->custom_option>count)s->custom_option=0;
    return 1;
}

void seg_set_options(Segment *s, const wchar_t *const *opts, int count)
{ (void)qp_seg_set_options(s, opts, count < 0 ? 0 : count); }

int qp_seg_add_option(Segment *s, const wchar_t *opt)
{
    wchar_t **arr;
    wchar_t *value;
    if (!s || !opt || s->option_count < 0) return 0;
    value = qp_wcsdup(opt);
    if (!value) return 0;
    arr = (wchar_t **)realloc(s->options, (size_t)(s->option_count + 1) * sizeof(wchar_t *));
    if (!arr) { free(value); return 0; }
    s->options = arr;
    s->options[s->option_count++] = value;
    return 1;
}

void seg_add_option(Segment *s, const wchar_t *opt)
{ (void)qp_seg_add_option(s, opt); }

void seg_remove_option(Segment *s, int index)
{
    int i;
    if (!s || index < 0 || index >= s->option_count) return;
    free(s->options[index]);
    for (i = index; i < s->option_count - 1; ++i) s->options[i] = s->options[i + 1];
    s->option_count--;
    if(s->custom_option==index+1)s->custom_option=0;
    else if(s->custom_option>index+1)--s->custom_option;
}

int seg_move_option(Segment *s, int index, int delta)
{
    int target;
    wchar_t *tmp;
    if (!s) return 0;
    target = index + delta;
    if (index < 0 || index >= s->option_count) return 0;
    if (target < 0 || target >= s->option_count) return 0;
    tmp = s->options[index];
    s->options[index] = s->options[target];
    s->options[target] = tmp;
    if(s->custom_option==index+1)s->custom_option=target+1;
    else if(s->custom_option==target+1)s->custom_option=index+1;
    return 1;
}

Segment *seg_clone(const Segment *s)
{
    Segment *c;
    const ParseRule *pr;
    const ConditionRule *cr;
    if (!s) return NULL;
    c = (Segment *)calloc(1, sizeof(Segment));
    if (!c) return NULL;
    qp_strlcpy(c->id, s->id, QP_ID_MAX);
    c->type = s->type;
    qp_strlcpy(c->label, s->label, QP_LABEL_MAX);
    qp_strlcpy(c->content, s->content, QP_CONTENT_MAX);
    qp_strlcpy(c->placeholder, s->placeholder, QP_PLACEHOLD_MAX);
    qp_strlcpy(c->separator, s->separator, QP_SEP_MAX);
    qp_strlcpy(c->default_value, s->default_value, QP_VALUE_MAX);
    c->hidden = s->hidden;
    c->no_output = s->no_output;
    c->multiline = s->multiline;
    qp_strlcpy(c->time_format, s->time_format, QP_FORMAT_MAX);
    qp_strlcpy(c->ref_segment_id, s->ref_segment_id, QP_ID_MAX);
    c->ref_mode = s->ref_mode;
    qp_strlcpy(c->ref_delimiter, s->ref_delimiter, QP_SEP_MAX);
    c->ref_index = s->ref_index;
    c->ref_start = s->ref_start;
    c->ref_len = s->ref_len;
    if (!qp_seg_set_options(c, (const wchar_t *const *)s->options, s->option_count)) {
        seg_free(c); return NULL;
    }
    c->custom_option=s->custom_option;
    for (pr = s->parse_rules; pr; pr = pr->next) {
        ParseRule *copy = parse_rule_clone(pr);
        if (!copy) { seg_free(c); return NULL; }
        parse_rule_append(&c->parse_rules, copy);
    }
    for (cr = s->condition_rules; cr; cr = cr->next) {
        ConditionRule *copy = cond_rule_clone(cr);
        if (!copy) { seg_free(c); return NULL; }
        cond_rule_append(&c->condition_rules, copy);
    }
    return c;
}

const wchar_t *seg_display_name(const Segment *s)
{
    if (!s) return L"";
    if (s->type == SEG_STATIC) return s->content;
    return s->label[0] ? s->label : L"(未命名变量)";
}

/* ------------------------------------------------------------------ */
/* 引用                                                                */
/* ------------------------------------------------------------------ */

void seg_ref_extract(const Segment *s, const wchar_t *src,
                     wchar_t *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = L'\0';
    if (!s || !src) return;

    if (s->ref_mode == REF_SPLIT) {
        /* 与解析规则的 split 用同一个函数，两处行为天生一致 */
        if (!qp_split_take(src, s->ref_delimiter, s->ref_index, out, cap))
            out[0] = L'\0';
        return;
    }
    if (s->ref_mode == REF_SUBSTR) {
        size_t len = wcslen(src);
        size_t from = (s->ref_start > 1) ? (size_t)(s->ref_start - 1) : 0;
        size_t take;
        if (from >= len) return;                      /* 起点越界 → 空 */
        /* Keep legacy UTF-16 unit indexing, snapping a low-half start back to
           its complete character rather than emitting an isolated surrogate. */
        if(from && src[from]>=0xDC00 && src[from]<=0xDFFF &&
           src[from-1]>=0xD800 && src[from-1]<=0xDBFF)--from;
        take = (s->ref_len > 0) ? (size_t)s->ref_len : (len - from);
        if (take > len - from) take = len - from;     /* 取多了就取到末尾 */
        take = qp_utf16_prefix(src+from,take,cap-1);
        if(take && from+take<len && src[from+take-1]>=0xD800 &&
           src[from+take-1]<=0xDBFF && src[from+take]>=0xDC00 && src[from+take]<=0xDFFF)--take;
        wmemcpy(out, src + from, take);
        out[take] = L'\0';
        return;
    }
    qp_strlcpy(out, src, cap);                        /* REF_WHOLE */
}

void seg_ref_summary(const Segment *s, const Template *t,
                     wchar_t *out, size_t cap)
{
    const Segment *src;
    const wchar_t *name;
    if (!out || cap == 0) return;
    out[0] = L'\0';
    if (!s || !s->ref_segment_id[0]) return;
    src = tmpl_find_segment(t, s->ref_segment_id);
    name = src ? seg_display_name(src) : L"(已删除的变量)";
    switch (s->ref_mode) {
    case REF_SPLIT:
        swprintf(out, cap, L"引用[%ls] 按“%ls”取第 %d 段",
                 name, s->ref_delimiter, s->ref_index);
        break;
    case REF_SUBSTR:
        if (s->ref_len > 0)
            swprintf(out, cap, L"引用[%ls] 第 %d 个字起取 %d 个",
                     name, s->ref_start > 0 ? s->ref_start : 1, s->ref_len);
        else
            swprintf(out, cap, L"引用[%ls] 第 %d 个字起到末尾",
                     name, s->ref_start > 0 ? s->ref_start : 1);
        break;
    default:
        swprintf(out, cap, L"引用[%ls]", name);
        break;
    }
}

int tmpl_ref_would_cycle(const Template *t, const Segment *target,
                         const wchar_t *source_id)
{
    const Segment *p;
    int depth;
    if (!target || !source_id || !source_id[0]) return 0;
    /* 自己引用自己 */
    if (wcscmp(source_id, target->id) == 0) return 1;
    /* 顺着来源自己的引用链往上走，看会不会绕回 target */
    p = tmpl_find_segment(t, source_id);
    for (depth = 0; p && depth < QP_REF_MAX_DEPTH; ++depth) {
        if (!p->ref_segment_id[0]) return 0;
        if (wcscmp(p->ref_segment_id, target->id) == 0) return 1;
        p = tmpl_find_segment(t, p->ref_segment_id);
    }
    /* 走满深度还没走完：链上本来就有环，别再往里加 */
    return p != NULL;
}

/* ================================================================== */
/* 解析规则                                                            */
/* ================================================================== */

ParseRule *parse_rule_create(void)
{
    ParseRule *r = (ParseRule *)calloc(1, sizeof(ParseRule));
    if (!r) return NULL;
    qp_new_id(r->id, QP_ID_MAX, L"pr");
    r->type = PARSE_SPLIT;
    r->segment_index = 1;
    qp_strlcpy(r->delimiter, L"-", QP_SEP_MAX);
    /* 查表默认值：键取本变量的值、取第一条、全部模式的分隔符与多选一致 */
    r->lk_key_mode = LOOKUP_KEY_HOST;
    r->lk_hit = LOOKUP_FIRST;
    qp_strlcpy(r->lk_sep, L"、", QP_SEP_MAX);
    return r;
}

void lookup_out_list_free(LookupOut *head)
{
    while (head) {
        LookupOut *n = head->next;
        free(head);
        head = n;
    }
}

void parse_rule_free(ParseRule *r)
{
    if (!r) return;
    lookup_out_list_free(r->lk_outs);
    r->lk_outs = NULL;
    free(r);
}

LookupOut *lookup_out_create(void)
{
    return (LookupOut *)calloc(1, sizeof(LookupOut));
}

void lookup_out_free(LookupOut *o) { free(o); }

int lookup_out_count(const LookupOut *head)
{
    int n = 0;
    for (; head; head = head->next) ++n;
    return n;
}

LookupOut *lookup_out_at(LookupOut *head, int index)
{
    int i = 0;
    if (index < 0) return NULL;
    for (; head; head = head->next, ++i)
        if (i == index) return head;
    return NULL;
}

void lookup_out_append(LookupOut **head, LookupOut *o)
{
    if (!head || !o) return;
    o->next = NULL;
    if (!*head) { *head = o; return; }
    {
        LookupOut *cur = *head;
        while (cur->next) cur = cur->next;
        cur->next = o;
    }
}

int lookup_out_remove_at(LookupOut **head, int index)
{
    LookupOut *cur, *prev = NULL;
    int i = 0;
    if (!head || index < 0) return 0;
    for (cur = *head; cur; prev = cur, cur = cur->next, ++i) {
        if (i != index) continue;
        if (prev) prev->next = cur->next;
        else *head = cur->next;
        free(cur);
        return 1;
    }
    return 0;
}

LookupOut *lookup_out_clone_list(const LookupOut *head)
{
    LookupOut *nh = NULL, *tail = NULL;
    for (; head; head = head->next) {
        LookupOut *c = (LookupOut *)calloc(1, sizeof(LookupOut));
        if (!c) { lookup_out_list_free(nh); return NULL; }
        *c = *head;
        c->next = NULL;
        if (tail) tail->next = c;
        else nh = c;
        tail = c;
    }
    return nh;
}

ParseRule *parse_rule_clone(const ParseRule *r)
{
    ParseRule *c;
    if (!r) return NULL;
    c = (ParseRule *)calloc(1, sizeof(ParseRule));
    if (!c) return NULL;
    *c = *r;
    c->next = NULL;
    c->lk_outs = lookup_out_clone_list(r->lk_outs);   /* 输出对要深拷贝 */
    if (r->lk_outs && !c->lk_outs) { free(c); return NULL; }
    return c;
}

/* ================================================================== */
/* 条件规则                                                            */
/* ================================================================== */

CondTerm *cond_term_create(void)
{
    CondTerm *t = (CondTerm *)calloc(1, sizeof(CondTerm));
    if (!t) return NULL;
    t->op = OP_CONTAINS;
    return t;
}

static void cond_term_free_values(CondTerm *t)
{
    int i;
    if (!t || !t->match_values) return;
    for (i = 0; i < t->match_value_count; ++i) free(t->match_values[i]);
    free(t->match_values);
    t->match_values = NULL;
    t->match_value_count = 0;
}

void cond_term_free(CondTerm *t)
{
    if (!t) return;
    cond_term_free_values(t);
    free(t);
}

static void cond_term_list_free(CondTerm *head)
{
    while (head) {
        CondTerm *n = head->next;
        cond_term_free(head);
        head = n;
    }
}

int qp_cond_term_set_match(CondTerm *t, const wchar_t *raw)
{
    const wchar_t *p, *start;
    wchar_t buf[QP_MATCH_MAX];
    CondTerm candidate = {0};
    if (!t) return 0;
    if (!raw) raw = L"";
    qp_strlcpy(candidate.match_value, raw, QP_MATCH_MAX);

    /* 逗号 / 中文逗号 / 顿号 / 分号 均视为分隔符 */
    start = raw;
    for (p = raw;; ++p) {
        if (*p == L',' || *p == L'，' || *p == L'、' || *p == L';' || *p == L'；' || *p == L'\0') {
            size_t len = (size_t)(p - start);
            if (len >= QP_MATCH_MAX) len = QP_MATCH_MAX - 1;
            memcpy(buf, start, len * sizeof(wchar_t));
            buf[len] = L'\0';
            qp_trim(buf);
            if (buf[0]) {
                wchar_t *value = qp_wcsdup(buf);
                if (!value) { cond_term_free_values(&candidate); return 0; }
                wchar_t **arr = (wchar_t **)realloc(candidate.match_values,
                        ((size_t)candidate.match_value_count + 1) * sizeof(wchar_t *));
                if (!arr) { free(value); cond_term_free_values(&candidate); return 0; }
                candidate.match_values = arr;
                candidate.match_values[candidate.match_value_count++] = value;
            }
            if (!*p) break;
            start = p + 1;
        }
    }
    cond_term_free_values(t);
    qp_strlcpy(t->match_value, candidate.match_value, QP_MATCH_MAX);
    t->match_values = candidate.match_values;
    t->match_value_count = candidate.match_value_count;
    return 1;
}

void cond_term_set_match(CondTerm *t, const wchar_t *raw)
{ (void)qp_cond_term_set_match(t, raw); }

CondTerm *cond_term_clone(const CondTerm *t)
{
    CondTerm *c;
    if (!t) return NULL;
    c = (CondTerm *)calloc(1, sizeof(CondTerm));
    if (!c) return NULL;
    qp_strlcpy(c->trigger_segment_id, t->trigger_segment_id, QP_ID_MAX);
    c->op = t->op;
    c->negate = t->negate;
    qp_strlcpy(c->match_value, t->match_value, QP_MATCH_MAX);
    if (t->match_value_count > 0) {
        c->match_values = (wchar_t **)calloc((size_t)t->match_value_count, sizeof(wchar_t *));
        if (!c->match_values) { free(c); return NULL; }
        c->match_value_count = t->match_value_count;
        for (int i = 0; i < t->match_value_count; ++i) {
            c->match_values[i] = qp_wcsdup(t->match_values[i]);
            if (!c->match_values[i]) { cond_term_free(c); return NULL; }
        }
    }
    return c;
}

int cond_term_count(const CondTerm *head)
{
    int n = 0;
    for (; head; head = head->next) ++n;
    return n;
}

CondTerm *cond_term_at(CondTerm *head, int index)
{
    int i = 0;
    if (index < 0) return NULL;
    for (; head; head = head->next, ++i)
        if (i == index) return head;
    return NULL;
}

void cond_term_append(CondTerm **head, CondTerm *t)
{
    if (!head || !t) return;
    t->next = NULL;
    if (!*head) { *head = t; return; }
    {
        CondTerm *cur = *head;
        while (cur->next) cur = cur->next;
        cur->next = t;
    }
}

int cond_term_remove_at(CondTerm **head, int index)
{
    CondTerm *cur, *prev = NULL;
    int i = 0;
    if (!head || index < 0) return 0;
    cur = *head;
    while (cur && i < index) { prev = cur; cur = cur->next; ++i; }
    if (!cur) return 0;
    if (prev) prev->next = cur->next;
    else *head = cur->next;
    cond_term_free(cur);
    return 1;
}

void cond_term_list_clear(CondTerm **head)
{
    if (!head) return;
    cond_term_list_free(*head);
    *head = NULL;
}

ConditionRule *cond_rule_create(void)
{
    ConditionRule *r = (ConditionRule *)calloc(1, sizeof(ConditionRule));
    if (!r) return NULL;
    qp_new_id(r->id, QP_ID_MAX, L"cr");
    r->join = COND_JOIN_ALL;
    /* 新规则默认带一个空白条件项：界面上立刻有一行可填，且未填完不会命中 */
    cond_term_append(&r->terms, cond_term_create());
    return r;
}

void cond_rule_free(ConditionRule *r)
{
    if (!r) return;
    cond_term_list_free(r->terms);
    free(r);
}

void cond_rule_set_simple(ConditionRule *r, const wchar_t *trigger_id,
                          CondOperator op, int negate, const wchar_t *match)
{
    CondTerm *t;
    if (!r) return;
    cond_term_list_free(r->terms);
    r->terms = NULL;
    r->join = COND_JOIN_ALL;
    t = cond_term_create();
    if (!t) return;
    qp_strlcpy(t->trigger_segment_id, trigger_id ? trigger_id : L"", QP_ID_MAX);
    t->op = op;
    t->negate = negate ? 1 : 0;
    cond_term_set_match(t, match);
    cond_term_append(&r->terms, t);
}

ConditionRule *cond_rule_clone(const ConditionRule *r)
{
    ConditionRule *c;
    const CondTerm *t;
    if (!r) return NULL;
    c = (ConditionRule *)calloc(1, sizeof(ConditionRule));
    if (!c) return NULL;
    qp_strlcpy(c->id, r->id, QP_ID_MAX);
    c->join = r->join;
    qp_strlcpy(c->result_value, r->result_value, QP_RESULT_MAX);
    for (t = r->terms; t; t = t->next) {
        CondTerm *copy = cond_term_clone(t);
        if (!copy) { cond_rule_free(c); return NULL; }
        cond_term_append(&c->terms, copy);
    }
    return c;
}

void cond_rule_assign(ConditionRule *dst, const ConditionRule *src)
{
    ConditionRule *copy;
    if (!dst || !src || dst == src) return;
    copy = cond_rule_clone(src);
    if (copy) qp_cond_rule_take(dst, copy);
}

void qp_parse_rule_take(ParseRule *dst, ParseRule *owned)
{
    ParseRule *next;
    wchar_t id[QP_ID_MAX];
    if (!dst || !owned || dst == owned) return;
    next = dst->next;
    qp_strlcpy(id, dst->id, QP_ID_MAX);
    lookup_out_list_free(dst->lk_outs);
    *dst = *owned;
    dst->next = next;
    qp_strlcpy(dst->id, id, QP_ID_MAX);
    free(owned);
}

void qp_cond_rule_take(ConditionRule *dst, ConditionRule *owned)
{
    ConditionRule *next;
    wchar_t id[QP_ID_MAX];
    if (!dst || !owned || dst == owned) return;
    next = dst->next;
    qp_strlcpy(id, dst->id, QP_ID_MAX);
    cond_term_list_free(dst->terms);
    *dst = *owned;
    dst->next = next;
    qp_strlcpy(dst->id, id, QP_ID_MAX);
    free(owned);
}

/* ================================================================== */
/* 模板 / 分组 / AppData                                               */
/* ================================================================== */

Template *tmpl_create(const wchar_t *name)
{
    Template *t = (Template *)calloc(1, sizeof(Template));
    if (!t) return NULL;
    qp_new_id(t->id, QP_ID_MAX, L"tpl");
    qp_strlcpy(t->name, name ? name : L"新模板", QP_NAME_MAX);
    qp_now_iso(t->created_at, QP_TIME_MAX);
    qp_strlcpy(t->updated_at, t->created_at, QP_TIME_MAX);
    return t;
}

void tmpl_free(Template *t)
{
    Segment *s;
    if (!t) return;
    s = t->segments;
    while (s) { Segment *n = s->next; seg_free(s); s = n; }
    free(t);
}

Template *tmpl_clone(const Template *t)
{
    Template *c;
    const Segment *s;
    if (!t) return NULL;
    c = (Template *)calloc(1, sizeof(Template));
    if (!c) return NULL;
    qp_strlcpy(c->id, t->id, QP_ID_MAX);
    qp_strlcpy(c->name, t->name, QP_NAME_MAX);
    qp_strlcpy(c->group_id, t->group_id, QP_ID_MAX);
    c->batch_mode = t->batch_mode;
    qp_strlcpy(c->batch_source_id, t->batch_source_id, QP_ID_MAX);
    c->table_output_enabled = t->table_output_enabled;
    qp_strlcpy(c->table_output_format, t->table_output_format, QP_OUTPUT_MAX);
    qp_strlcpy(c->created_at, t->created_at, QP_TIME_MAX);
    qp_strlcpy(c->updated_at, t->updated_at, QP_TIME_MAX);
    for (s = t->segments; s; s = s->next) {
        Segment *copy = seg_clone(s);
        if (!copy) { tmpl_free(c); return NULL; }
        seg_list_append(&c->segments, copy);
    }
    return c;
}

Group *group_create(const wchar_t *name)
{
    Group *g = (Group *)calloc(1, sizeof(Group));
    if (!g) return NULL;
    qp_new_id(g->id, QP_ID_MAX, L"grp");
    qp_strlcpy(g->name, name ? name : L"新分组", QP_NAME_MAX);
    return g;
}

void group_free(Group *g) { free(g); }

AppData *appdata_create(void)
{
    return (AppData *)calloc(1, sizeof(AppData));
}

void appdata_free(AppData *d)
{
    Group *g;
    Template *t;
    if (!d) return;
    g = d->groups;
    while (g) { Group *n = g->next; group_free(g); g = n; }
    t = d->templates;
    while (t) { Template *n = t->next; tmpl_free(t); t = n; }
    qp_table_forget_refs(d->tables);    /* 别让表模块拿着已释放的清单 */
    dtref_list_free(d->tables);
    d->tables = NULL;
    free(d);
}

const Segment *tmpl_find_segment(const Template *t, const wchar_t *id)
{
    const Segment *s;
    if (!t || !id || !*id) return NULL;
    for (s = t->segments; s; s = s->next)
        if (wcscmp(s->id, id) == 0) return s;
    /* 兼容：按变量名匹配（手工编辑 JSON / 导入时可能用名字） */
    for (s = t->segments; s; s = s->next)
        if (s->label[0] && qp_icmp(s->label, id) == 0) return s;
    return NULL;
}

/* ================================================================== */
/* 链表操作                                                            */
/* ================================================================== */

int seg_list_count(const Segment *head)
{
    int n = 0;
    for (; head; head = head->next) ++n;
    return n;
}

Segment *seg_list_at(Segment *head, int index)
{
    if (index < 0) return NULL;
    for (; head && index > 0; head = head->next) --index;
    return index == 0 ? head : NULL;
}

int seg_list_index_of(Segment *head, const Segment *s)
{
    int i = 0;
    for (; head; head = head->next, ++i)
        if (head == s) return i;
    return -1;
}

void seg_list_append(Segment **head, Segment *s)
{
    Segment *p;
    if (!head || !s) return;
    s->next = NULL;
    if (!*head) { *head = s; return; }
    for (p = *head; p->next; p = p->next) {}
    p->next = s;
}

void seg_list_insert_at(Segment **head, Segment *s, int index)
{
    Segment *prev;
    int i;
    if (!head || !s) return;
    if (index <= 0 || !*head) {
        s->next = *head;
        *head = s;
        return;
    }
    prev = *head;
    for (i = 1; i < index && prev->next; ++i) prev = prev->next;
    s->next = prev->next;
    prev->next = s;
}

Segment *seg_list_remove_at(Segment **head, int index)
{
    Segment *cur, *prev = NULL;
    int i = 0;
    if (!head || !*head || index < 0) return NULL;
    cur = *head;
    while (cur && i < index) { prev = cur; cur = cur->next; ++i; }
    if (!cur) return NULL;
    if (prev) prev->next = cur->next; else *head = cur->next;
    cur->next = NULL;
    return cur;
}

int seg_list_move(Segment **head, int from, int to)
{
    Segment *node;
    int count;
    if (!head) return 0;
    count = seg_list_count(*head);
    if (from < 0 || from >= count) return 0;
    if (to < 0) to = 0;
    if (to >= count) to = count - 1;
    if (from == to) return 0;
    node = seg_list_remove_at(head, from);
    if (!node) return 0;
    seg_list_insert_at(head, node, to);
    return 1;
}

int tmpl_list_count(const Template *head)
{
    int n = 0;
    for (; head; head = head->next) ++n;
    return n;
}

Template *tmpl_list_at(Template *head, int index)
{
    if (index < 0) return NULL;
    for (; head && index > 0; head = head->next) --index;
    return index == 0 ? head : NULL;
}

void tmpl_list_append(Template **head, Template *t)
{
    Template *p;
    if (!head || !t) return;
    t->next = NULL;
    if (!*head) { *head = t; return; }
    for (p = *head; p->next; p = p->next) {}
    p->next = t;
}

Template *tmpl_list_find(Template *head, const wchar_t *id)
{
    if (!id) return NULL;
    for (; head; head = head->next)
        if (wcscmp(head->id, id) == 0) return head;
    return NULL;
}

int tmpl_list_remove(Template **head, const wchar_t *id)
{
    Template *cur, *prev = NULL;
    if (!head || !id) return 0;
    cur = *head;
    while (cur) {
        if (wcscmp(cur->id, id) == 0) {
            if (prev) prev->next = cur->next; else *head = cur->next;
            tmpl_free(cur);
            return 1;
        }
        prev = cur;
        cur = cur->next;
    }
    return 0;
}

int tmpl_list_replace(Template **head, Template *newer)
{
    Template *cur, *prev = NULL;
    if (!head || !newer) return 0;
    cur = *head;
    while (cur) {
        if (wcscmp(cur->id, newer->id) == 0) {
            newer->next = cur->next;
            if (prev) prev->next = newer; else *head = newer;
            tmpl_free(cur);
            return 1;
        }
        prev = cur;
        cur = cur->next;
    }
    return 0;
}

int group_list_count(const Group *head)
{
    int n = 0;
    for (; head; head = head->next) ++n;
    return n;
}

Group *group_list_at(Group *head, int index)
{
    if (index < 0) return NULL;
    for (; head && index > 0; head = head->next) --index;
    return index == 0 ? head : NULL;
}

void group_list_append(Group **head, Group *g)
{
    Group *p;
    if (!head || !g) return;
    g->next = NULL;
    if (!*head) { *head = g; return; }
    for (p = *head; p->next; p = p->next) {}
    p->next = g;
}

Group *group_list_find(Group *head, const wchar_t *id)
{
    if (!id || !*id) return NULL;
    for (; head; head = head->next)
        if (wcscmp(head->id, id) == 0) return head;
    return NULL;
}

int group_list_remove(Group **head, const wchar_t *id)
{
    Group *cur, *prev = NULL;
    if (!head || !id) return 0;
    cur = *head;
    while (cur) {
        if (wcscmp(cur->id, id) == 0) {
            if (prev) prev->next = cur->next; else *head = cur->next;
            group_free(cur);
            return 1;
        }
        prev = cur;
        cur = cur->next;
    }
    return 0;
}

/* --------------------------- 数据表引用 --------------------------- */

DataTableRef *dtref_create(const wchar_t *name, const wchar_t *path)
{
    DataTableRef *r = (DataTableRef *)calloc(1, sizeof(DataTableRef));
    if (!r) return NULL;
    qp_new_id(r->id, QP_ID_MAX, L"tbl");
    qp_strlcpy(r->name, name ? name : L"", QP_NAME_MAX);
    qp_strlcpy(r->path, path ? path : L"", QP_PATH_MAX);
    r->encoding = QP_ENC_AUTO;
    r->delimiter = QP_DELIM_AUTO;
    r->has_header = 1;
    return r;
}

void dtref_free(DataTableRef *r) { free(r); }

int dtref_list_count(const DataTableRef *head)
{
    int n = 0;
    for (; head; head = head->next) ++n;
    return n;
}

DataTableRef *dtref_list_at(DataTableRef *head, int index)
{
    if (index < 0) return NULL;
    for (; head && index > 0; head = head->next) --index;
    return index == 0 ? head : NULL;
}

DataTableRef *dtref_list_find(DataTableRef *head, const wchar_t *id)
{
    if (!id || !*id) return NULL;
    for (; head; head = head->next)
        if (wcscmp(head->id, id) == 0) return head;
    return NULL;
}

void dtref_list_append(DataTableRef **head, DataTableRef *r)
{
    DataTableRef *p;
    if (!head || !r) return;
    r->next = NULL;
    if (!*head) { *head = r; return; }
    for (p = *head; p->next; p = p->next) {}
    p->next = r;
}

int dtref_list_remove(DataTableRef **head, const wchar_t *id)
{
    DataTableRef *cur, *prev = NULL;
    if (!head || !id) return 0;
    cur = *head;
    while (cur) {
        if (wcscmp(cur->id, id) == 0) {
            if (prev) prev->next = cur->next; else *head = cur->next;
            dtref_free(cur);
            return 1;
        }
        prev = cur;
        cur = cur->next;
    }
    return 0;
}

void dtref_list_free(DataTableRef *head)
{
    while (head) {
        DataTableRef *n = head->next;
        free(head);
        head = n;
    }
}

int parse_rule_count(const ParseRule *head)
{
    int n = 0;
    for (; head; head = head->next) ++n;
    return n;
}

ParseRule *parse_rule_at(ParseRule *head, int index)
{
    if (index < 0) return NULL;
    for (; head && index > 0; head = head->next) --index;
    return index == 0 ? head : NULL;
}

void parse_rule_append(ParseRule **head, ParseRule *r)
{
    ParseRule *p;
    if (!head || !r) return;
    r->next = NULL;
    if (!*head) { *head = r; return; }
    for (p = *head; p->next; p = p->next) {}
    p->next = r;
}

int parse_rule_remove_at(ParseRule **head, int index)
{
    ParseRule *cur, *prev = NULL;
    int i = 0;
    if (!head || !*head || index < 0) return 0;
    cur = *head;
    while (cur && i < index) { prev = cur; cur = cur->next; ++i; }
    if (!cur) return 0;
    if (prev) prev->next = cur->next; else *head = cur->next;
    parse_rule_free(cur);
    return 1;
}

int parse_rule_move(ParseRule **head, int from, int to)
{
    ParseRule *cur, *prev = NULL, *node;
    int i = 0, count;
    if (!head) return 0;
    count = parse_rule_count(*head);
    if (from < 0 || from >= count || from == to) return 0;
    if (to < 0) to = 0;
    if (to >= count) to = count - 1;
    cur = *head;
    while (cur && i < from) { prev = cur; cur = cur->next; ++i; }
    if (!cur) return 0;
    node = cur;
    if (prev) prev->next = cur->next; else *head = cur->next;
    node->next = NULL;
    if (to <= 0) { node->next = *head; *head = node; return 1; }
    prev = *head;
    for (i = 1; i < to && prev->next; ++i) prev = prev->next;
    node->next = prev->next;
    prev->next = node;
    return 1;
}

int cond_rule_count(const ConditionRule *head)
{
    int n = 0;
    for (; head; head = head->next) ++n;
    return n;
}

ConditionRule *cond_rule_at(ConditionRule *head, int index)
{
    if (index < 0) return NULL;
    for (; head && index > 0; head = head->next) --index;
    return index == 0 ? head : NULL;
}

void cond_rule_append(ConditionRule **head, ConditionRule *r)
{
    ConditionRule *p;
    if (!head || !r) return;
    r->next = NULL;
    if (!*head) { *head = r; return; }
    for (p = *head; p->next; p = p->next) {}
    p->next = r;
}

int cond_rule_remove_at(ConditionRule **head, int index)
{
    ConditionRule *cur, *prev = NULL;
    int i = 0;
    if (!head || !*head || index < 0) return 0;
    cur = *head;
    while (cur && i < index) { prev = cur; cur = cur->next; ++i; }
    if (!cur) return 0;
    if (prev) prev->next = cur->next; else *head = cur->next;
    cond_rule_free(cur);
    return 1;
}

int cond_rule_move(ConditionRule **head, int from, int to)
{
    ConditionRule *cur, *prev = NULL, *node;
    int i = 0, count;
    if (!head) return 0;
    count = cond_rule_count(*head);
    if (from < 0 || from >= count || from == to) return 0;
    if (to < 0) to = 0;
    if (to >= count) to = count - 1;
    cur = *head;
    while (cur && i < from) { prev = cur; cur = cur->next; ++i; }
    if (!cur) return 0;
    node = cur;
    if (prev) prev->next = cur->next; else *head = cur->next;
    node->next = NULL;
    if (to <= 0) { node->next = *head; *head = node; return 1; }
    prev = *head;
    for (i = 1; i < to && prev->next; ++i) prev = prev->next;
    node->next = prev->next;
    prev->next = node;
    return 1;
}
