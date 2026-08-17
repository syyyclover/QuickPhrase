/*
 * engine.c —— 数据模型生命周期管理 + 解析规则/条件规则引擎
 * 纯 C 实现，不含任何窗口 API，便于独立单元测试。
 */
#include "engine.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wctype.h>

/* ================================================================== */
/* 字符串工具                                                          */
/* ================================================================== */

void qp_strlcpy(wchar_t *dst, const wchar_t *src, size_t cap)
{
    size_t i;
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = L'\0'; return; }
    for (i = 0; i + 1 < cap && src[i]; ++i) dst[i] = src[i];
    dst[i] = L'\0';
}

wchar_t *qp_wcsdup(const wchar_t *s)
{
    size_t n;
    wchar_t *p;
    if (!s) s = L"";
    n = wcslen(s) + 1;
    p = (wchar_t *)malloc(n * sizeof(wchar_t));
    if (p) memcpy(p, s, n * sizeof(wchar_t));
    return p;
}

/* 自己实现大小写不敏感比较，避免依赖 _wcsicmp / wcscasecmp 的平台差异 */
int qp_icmp(const wchar_t *a, const wchar_t *b)
{
    if (!a) a = L"";
    if (!b) b = L"";
    for (;;) {
        wint_t ca = towlower((wint_t)*a);
        wint_t cb = towlower((wint_t)*b);
        if (ca != cb) return ca < cb ? -1 : 1;
        if (!*a) return 0;
        ++a; ++b;
    }
}

const wchar_t *qp_istr(const wchar_t *hay, const wchar_t *needle)
{
    size_t nlen;
    if (!hay || !needle) return NULL;
    nlen = wcslen(needle);
    if (nlen == 0) return NULL;
    for (; *hay; ++hay) {
        size_t i = 0;
        while (i < nlen && hay[i] &&
               towlower((wint_t)hay[i]) == towlower((wint_t)needle[i]))
            ++i;
        if (i == nlen) return hay;
    }
    return NULL;
}

int qp_istarts(const wchar_t *hay, const wchar_t *prefix)
{
    if (!hay || !prefix) return 0;
    if (!*prefix) return 0;
    for (; *prefix; ++hay, ++prefix) {
        if (!*hay) return 0;
        if (towlower((wint_t)*hay) != towlower((wint_t)*prefix)) return 0;
    }
    return 1;
}

int qp_is_blank(const wchar_t *s)
{
    if (!s) return 1;
    for (; *s; ++s) if (!iswspace((wint_t)*s)) return 0;
    return 1;
}

void qp_trim(wchar_t *s)
{
    size_t len, start = 0;
    if (!s) return;
    len = wcslen(s);
    while (len > 0 && iswspace((wint_t)s[len - 1])) s[--len] = L'\0';
    while (s[start] && iswspace((wint_t)s[start])) ++start;
    if (start) memmove(s, s + start, (len - start + 1) * sizeof(wchar_t));
}

int qp_split_take(const wchar_t *src, const wchar_t *delim, int index,
                  wchar_t *out, size_t cap)
{
    const wchar_t *p, *seg_start;
    size_t dlen;
    int n = 1;

    if (out && cap) out[0] = L'\0';
    if (!src || !out || cap == 0) return 0;
    if (index < 1) return 0;
    if (!delim || !*delim) {
        /* 无分隔符：整串视为第 1 段 */
        if (index != 1) return 0;
        qp_strlcpy(out, src, cap);
        return 1;
    }

    dlen = wcslen(delim);
    seg_start = src;
    p = src;
    while (*p) {
        if (wcsncmp(p, delim, dlen) == 0) {
            if (n == index) {
                size_t len = (size_t)(p - seg_start);
                if (len >= cap) len = cap - 1;
                memcpy(out, seg_start, len * sizeof(wchar_t));
                out[len] = L'\0';
                return 1;
            }
            ++n;
            p += dlen;
            seg_start = p;
        } else {
            ++p;
        }
    }
    if (n == index) {
        qp_strlcpy(out, seg_start, cap);
        return 1;
    }
    return 0;
}

void qp_new_id(wchar_t *out, size_t cap, const wchar_t *prefix)
{
    static unsigned long counter = 0;
    unsigned long t = (unsigned long)time(NULL);
    ++counter;
    if (!prefix) prefix = L"id";
    swprintf(out, cap, L"%ls_%lx_%lx", prefix, t, counter);
}

void qp_now_iso(wchar_t *out, size_t cap)
{
    time_t now = time(NULL);
    struct tm tmv;
    char buf[32];
#if defined(_WIN32)
    localtime_s(&tmv, &now);
#else
    {
        struct tm *p = localtime(&now);
        if (p) tmv = *p;
        else memset(&tmv, 0, sizeof(tmv));
    }
#endif
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    swprintf(out, cap, L"%hs", buf);
}

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

void seg_set_options(Segment *s, const wchar_t *const *opts, int count)
{
    int i;
    if (!s) return;
    seg_free_options(s);
    if (count <= 0 || !opts) return;
    s->options = (wchar_t **)calloc((size_t)count, sizeof(wchar_t *));
    if (!s->options) return;
    for (i = 0; i < count; ++i) s->options[i] = qp_wcsdup(opts[i]);
    s->option_count = count;
}

void seg_add_option(Segment *s, const wchar_t *opt)
{
    wchar_t **arr;
    if (!s || !opt) return;
    arr = (wchar_t **)realloc(s->options, (size_t)(s->option_count + 1) * sizeof(wchar_t *));
    if (!arr) return;
    s->options = arr;
    s->options[s->option_count] = qp_wcsdup(opt);
    s->option_count++;
}

void seg_remove_option(Segment *s, int index)
{
    int i;
    if (!s || index < 0 || index >= s->option_count) return;
    free(s->options[index]);
    for (i = index; i < s->option_count - 1; ++i) s->options[i] = s->options[i + 1];
    s->option_count--;
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
    return 1;
}

Segment *seg_clone(const Segment *s)
{
    Segment *c;
    const ParseRule *pr;
    const ConditionRule *cr;
    int i;
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
    if (s->option_count > 0) {
        c->options = (wchar_t **)calloc((size_t)s->option_count, sizeof(wchar_t *));
        if (c->options) {
            for (i = 0; i < s->option_count; ++i) c->options[i] = qp_wcsdup(s->options[i]);
            c->option_count = s->option_count;
        }
    }
    for (pr = s->parse_rules; pr; pr = pr->next)
        parse_rule_append(&c->parse_rules, parse_rule_clone(pr));
    for (cr = s->condition_rules; cr; cr = cr->next)
        cond_rule_append(&c->condition_rules, cond_rule_clone(cr));
    return c;
}

const wchar_t *seg_display_name(const Segment *s)
{
    if (!s) return L"";
    if (s->type == SEG_STATIC) return s->content;
    return s->label[0] ? s->label : L"(未命名变量)";
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
    return r;
}

void parse_rule_free(ParseRule *r) { free(r); }

ParseRule *parse_rule_clone(const ParseRule *r)
{
    ParseRule *c;
    if (!r) return NULL;
    c = (ParseRule *)calloc(1, sizeof(ParseRule));
    if (!c) return NULL;
    *c = *r;
    c->next = NULL;
    return c;
}

/* ================================================================== */
/* 条件规则                                                            */
/* ================================================================== */

ConditionRule *cond_rule_create(void)
{
    ConditionRule *r = (ConditionRule *)calloc(1, sizeof(ConditionRule));
    if (!r) return NULL;
    qp_new_id(r->id, QP_ID_MAX, L"cr");
    r->op = OP_CONTAINS;
    return r;
}

static void cond_free_values(ConditionRule *r)
{
    int i;
    if (!r || !r->match_values) return;
    for (i = 0; i < r->match_value_count; ++i) free(r->match_values[i]);
    free(r->match_values);
    r->match_values = NULL;
    r->match_value_count = 0;
}

void cond_rule_free(ConditionRule *r)
{
    if (!r) return;
    cond_free_values(r);
    free(r);
}

void cond_rule_set_match(ConditionRule *r, const wchar_t *raw)
{
    const wchar_t *p, *start;
    wchar_t buf[QP_MATCH_MAX];
    if (!r) return;
    qp_strlcpy(r->match_value, raw ? raw : L"", QP_MATCH_MAX);
    cond_free_values(r);
    if (!raw || !*raw) return;

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
                wchar_t **arr = (wchar_t **)realloc(r->match_values,
                        (size_t)(r->match_value_count + 1) * sizeof(wchar_t *));
                if (arr) {
                    r->match_values = arr;
                    r->match_values[r->match_value_count] = qp_wcsdup(buf);
                    r->match_value_count++;
                }
            }
            if (!*p) break;
            start = p + 1;
        }
    }
}

ConditionRule *cond_rule_clone(const ConditionRule *r)
{
    ConditionRule *c;
    if (!r) return NULL;
    c = (ConditionRule *)calloc(1, sizeof(ConditionRule));
    if (!c) return NULL;
    qp_strlcpy(c->id, r->id, QP_ID_MAX);
    qp_strlcpy(c->trigger_segment_id, r->trigger_segment_id, QP_ID_MAX);
    c->op = r->op;
    qp_strlcpy(c->result_value, r->result_value, QP_RESULT_MAX);
    cond_rule_set_match(c, r->match_value);
    return c;
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
    qp_strlcpy(c->created_at, t->created_at, QP_TIME_MAX);
    qp_strlcpy(c->updated_at, t->updated_at, QP_TIME_MAX);
    for (s = t->segments; s; s = s->next)
        seg_list_append(&c->segments, seg_clone(s));
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

/* ================================================================== */
/* 规则引擎                                                            */
/* ================================================================== */

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
    }
    eval_reset(ctx);
    return ctx;
}

void eval_destroy(EvalContext *ctx)
{
    if (!ctx) return;
    free(ctx->fields);
    free(ctx);
}

/* 片段的「原始默认值」 */
static const wchar_t *seg_default_of(const Segment *s)
{
    if (!s) return L"";
    if (s->type == SEG_STATIC) return s->content;
    if (s->default_value[0]) return s->default_value;
    /* 单选未设默认值时取第一个选项，避免输出空洞 */
    if (s->type == SEG_SELECT && s->option_count > 0 && s->options && s->options[0])
        return s->options[0];
    return L"";
}

/* 片段的「兜底值」：默认值优先；默认值为空（只靠变量匹配填充的变量）时，
   保留上一次规则推导出的值，避免规则某一刻不再命中就在输出里留下空洞。
   「重置」会清掉 sticky，回到真正的空。 */
static const wchar_t *field_fallback_of(const EvalField *f)
{
    const wchar_t *d;
    if (!f) return L"";
    d = seg_default_of(f->seg);
    if (d && *d) return d;
    if (f->has_sticky && f->sticky[0]) return f->sticky;
    return L"";
}

void eval_reset(EvalContext *ctx)
{
    int i;
    if (!ctx) return;
    for (i = 0; i < ctx->count; ++i) {
        EvalField *f = &ctx->fields[i];
        f->user_touched = 0;
        f->user_override = 0;
        f->has_last_derived = 0;
        f->has_sticky = 0;
        f->rule_driven = 0;
        f->user_value[0] = L'\0';
        f->last_derived[0] = L'\0';
        f->sticky[0] = L'\0';
        qp_strlcpy(f->value, seg_default_of(f->seg), QP_VALUE_MAX);
    }
    eval_run(ctx);
}

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
    qp_strlcpy(f->user_value, v ? v : L"", QP_VALUE_MAX);
    f->user_touched = 1;
    /* 若该字段正被规则驱动，且用户改成了别的值 → 记为「手动覆盖」，
       直到触发源变化（推导值改变）为止都尊重用户选择。 */
    if (f->has_last_derived)
        f->user_override = (wcscmp(f->user_value, f->last_derived) != 0);
    else
        f->user_override = 0;
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
    /* PARSE_CONTAINS */
    if (!r->keyword[0]) return 0;
    if (!qp_istr(input, r->keyword)) return 0;
    if (extracted) qp_strlcpy(extracted, r->keyword, cap);
    return 1;
}

static int cond_rule_hit(const ConditionRule *r, const wchar_t *trigger_value)
{
    int i;
    if (!r) return 0;
    if (!trigger_value) trigger_value = L"";
    switch (r->op) {
    case OP_CONTAINS:
        return r->match_value[0] && qp_istr(trigger_value, r->match_value) != NULL;
    case OP_EQUALS:
        /* 允许匹配「等于空」 */
        return qp_icmp(trigger_value, r->match_value) == 0;
    case OP_STARTS_WITH:
        return r->match_value[0] && qp_istarts(trigger_value, r->match_value);
    case OP_IN:
        for (i = 0; i < r->match_value_count; ++i)
            if (qp_icmp(trigger_value, r->match_values[i]) == 0) return 1;
        return 0;
    default:
        return 0;
    }
}

void eval_run(EvalContext *ctx)
{
    wchar_t (*work)[QP_VALUE_MAX];
    wchar_t (*derived)[QP_VALUE_MAX];
    int *derived_set;
    int i, pass;

    if (!ctx || ctx->count <= 0) return;

    work = (wchar_t (*)[QP_VALUE_MAX])calloc((size_t)ctx->count, sizeof(*work));
    derived = (wchar_t (*)[QP_VALUE_MAX])calloc((size_t)ctx->count, sizeof(*derived));
    derived_set = (int *)calloc((size_t)ctx->count, sizeof(int));
    if (!work || !derived || !derived_set) {
        free(work); free(derived); free(derived_set);
        return;
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

        for (i = 0; i < ctx->count; ++i) { derived_set[i] = 0; derived[i][0] = L'\0'; }

        /* 1) 解析规则：只有 text 变量参与，按顺序，每个目标取第一条命中的 */
        for (i = 0; i < ctx->count; ++i) {
            const Segment *s = ctx->fields[i].seg;
            const ParseRule *r;
            if (!s || s->type != SEG_TEXT) continue;
            for (r = s->parse_rules; r; r = r->next) {
                wchar_t extracted[QP_VALUE_MAX];
                int target = eval_index_of_id(ctx, r->target_segment_id);
                if (target < 0 || target >= ctx->count) continue;
                if (derived_set[target]) continue;          /* 第一个命中的生效 */
                if (!parse_rule_hit(r, work[i], extracted, QP_VALUE_MAX)) continue;
                qp_strlcpy(derived[target],
                           r->fill_value[0] ? r->fill_value : extracted,
                           QP_VALUE_MAX);
                derived_set[target] = 1;
            }
        }

        /* 2) 条件规则：挂在片段自身上，按顺序，命中即停止 */
        for (i = 0; i < ctx->count; ++i) {
            const Segment *s = ctx->fields[i].seg;
            const ConditionRule *r;
            if (!s) continue;
            for (r = s->condition_rules; r; r = r->next) {
                int trig = eval_index_of_id(ctx, r->trigger_segment_id);
                if (trig < 0 || trig >= ctx->count) continue;
                if (!cond_rule_hit(r, work[trig])) continue;
                qp_strlcpy(derived[i], r->result_value, QP_VALUE_MAX);
                derived_set[i] = 1;
                break;
            }
        }

        /* 3) 应用：规则值 > 用户输入 > 默认值；但用户手动覆盖时尊重用户 */
        for (i = 0; i < ctx->count; ++i) {
            EvalField *f = &ctx->fields[i];
            wchar_t next[QP_VALUE_MAX];
            if (derived_set[i]) {
                int override_active = f->user_override && f->has_last_derived &&
                                      wcscmp(derived[i], f->last_derived) == 0;
                qp_strlcpy(next, override_active ? f->user_value : derived[i], QP_VALUE_MAX);
            } else if (f->user_touched) {
                qp_strlcpy(next, f->user_value, QP_VALUE_MAX);
            } else {
                qp_strlcpy(next, field_fallback_of(f), QP_VALUE_MAX);
            }
            if (wcscmp(next, work[i]) != 0) {
                qp_strlcpy(work[i], next, QP_VALUE_MAX);
                changed = 1;
            }
        }

        if (!changed) break;
    }

    /* 提交：记录本轮推导值，供下次判断「用户覆盖是否仍然有效」 */
    for (i = 0; i < ctx->count; ++i) {
        EvalField *f = &ctx->fields[i];
        f->rule_driven = derived_set[i];
        if (derived_set[i]) {
            int override_active = f->user_override && f->has_last_derived &&
                                  wcscmp(derived[i], f->last_derived) == 0;
            qp_strlcpy(f->last_derived, derived[i], QP_VALUE_MAX);
            f->has_last_derived = 1;
            if (derived[i][0]) {          /* 记住推导过的值，作为后续兜底 */
                qp_strlcpy(f->sticky, derived[i], QP_VALUE_MAX);
                f->has_sticky = 1;
            }
            if (!override_active) f->user_override = 0;
        } else {
            f->has_last_derived = 0;
            f->user_override = 0;
        }
        qp_strlcpy(f->value, work[i], QP_VALUE_MAX);
    }

    free(work);
    free(derived);
    free(derived_set);
}

size_t eval_render(const EvalContext *ctx, wchar_t *out, size_t cap)
{
    size_t used = 0;
    int i;
    if (!out || cap == 0) return 0;
    out[0] = L'\0';
    if (!ctx) return 0;
    for (i = 0; i < ctx->count; ++i) {
        const wchar_t *v = ctx->fields[i].value;
        size_t len;
        if (!v || !*v) continue;
        len = wcslen(v);
        if (used + len + 1 > cap) len = (cap > used + 1) ? (cap - used - 1) : 0;
        if (len == 0) break;
        memcpy(out + used, v, len * sizeof(wchar_t));
        used += len;
        out[used] = L'\0';
    }
    return used;
}

size_t tmpl_render_defaults(const Template *t, wchar_t *out, size_t cap)
{
    EvalContext *ctx;
    size_t n;
    if (out && cap) out[0] = L'\0';
    if (!t) return 0;
    ctx = eval_create(t);
    if (!ctx) return 0;
    n = eval_render(ctx, out, cap);
    eval_destroy(ctx);
    return n;
}

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
    default:               return L"?";
    }
}

const wchar_t *parse_type_name(ParseRuleType t)
{
    return t == PARSE_SPLIT ? L"分割取段" : L"包含关键词";
}

const wchar_t *cond_op_name(CondOperator op)
{
    switch (op) {
    case OP_CONTAINS:    return L"包含";
    case OP_EQUALS:      return L"等于";
    case OP_STARTS_WITH: return L"开头是";
    case OP_IN:          return L"属于列表";
    default:             return L"?";
    }
}

static const wchar_t *seg_name_by_id(const Template *t, const wchar_t *id)
{
    const Segment *s = tmpl_find_segment(t, id);
    if (!s) return L"(未指定)";
    return seg_display_name(s);
}

void parse_rule_summary(const ParseRule *r, const Template *t, wchar_t *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = L'\0';
    if (!r) return;
    if (r->type == PARSE_SPLIT) {
        if (r->keyword[0])
            swprintf(out, cap, L"按「%ls」分割取第 %d 段，命中「%ls」→ %ls = 「%ls」",
                     r->delimiter, r->segment_index, r->keyword,
                     seg_name_by_id(t, r->target_segment_id),
                     r->fill_value[0] ? r->fill_value : L"(提取值)");
        else
            swprintf(out, cap, L"按「%ls」分割取第 %d 段 → %ls = 「%ls」",
                     r->delimiter, r->segment_index,
                     seg_name_by_id(t, r->target_segment_id),
                     r->fill_value[0] ? r->fill_value : L"(提取值)");
    } else {
        swprintf(out, cap, L"包含「%ls」→ %ls = 「%ls」",
                 r->keyword, seg_name_by_id(t, r->target_segment_id),
                 r->fill_value[0] ? r->fill_value : L"(关键词)");
    }
}

void cond_rule_summary(const ConditionRule *r, const Template *t, wchar_t *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = L'\0';
    if (!r) return;
    swprintf(out, cap, L"如果 %ls %ls「%ls」→ 内容 = 「%ls」",
             seg_name_by_id(t, r->trigger_segment_id),
             cond_op_name(r->op), r->match_value, r->result_value);
}

/* ================================================================== */
/* 首次运行的示例数据（一条与业务无关的复杂测试口径，覆盖全部功能）    */
/* ================================================================== */

static Segment *seed_static(const wchar_t *text)
{
    Segment *s = seg_create(SEG_STATIC);
    if (s) qp_strlcpy(s->content, text, QP_CONTENT_MAX);
    return s;
}

static Segment *seed_var(SegmentType type, const wchar_t *label,
                         const wchar_t *placeholder, const wchar_t *defval)
{
    Segment *s = seg_create(type);
    if (!s) return NULL;
    qp_strlcpy(s->label, label, QP_LABEL_MAX);
    qp_strlcpy(s->placeholder, placeholder ? placeholder : L"", QP_PLACEHOLD_MAX);
    qp_strlcpy(s->default_value, defval ? defval : L"", QP_VALUE_MAX);
    return s;
}

static void seed_add_parse(Segment *host, const wchar_t *delim, int index,
                           const wchar_t *keyword, const Segment *target,
                           const wchar_t *fill)
{
    ParseRule *r = parse_rule_create();
    if (!r || !host) { parse_rule_free(r); return; }
    r->type = PARSE_SPLIT;
    qp_strlcpy(r->delimiter, delim, QP_SEP_MAX);
    r->segment_index = index;
    qp_strlcpy(r->keyword, keyword, QP_KEYWORD_MAX);
    if (target) qp_strlcpy(r->target_segment_id, target->id, QP_ID_MAX);
    qp_strlcpy(r->fill_value, fill, QP_VALUE_MAX);
    parse_rule_append(&host->parse_rules, r);
}

static void seed_add_parse_contains(Segment *host, const wchar_t *keyword,
                                    const Segment *target, const wchar_t *fill)
{
    ParseRule *r = parse_rule_create();
    if (!r || !host) { parse_rule_free(r); return; }
    r->type = PARSE_CONTAINS;
    qp_strlcpy(r->keyword, keyword, QP_KEYWORD_MAX);
    if (target) qp_strlcpy(r->target_segment_id, target->id, QP_ID_MAX);
    qp_strlcpy(r->fill_value, fill, QP_VALUE_MAX);
    parse_rule_append(&host->parse_rules, r);
}

static void seed_add_cond(Segment *host, const Segment *trigger, CondOperator op,
                          const wchar_t *match, const wchar_t *result)
{
    ConditionRule *r = cond_rule_create();
    if (!r || !host) { cond_rule_free(r); return; }
    if (trigger) qp_strlcpy(r->trigger_segment_id, trigger->id, QP_ID_MAX);
    r->op = op;
    cond_rule_set_match(r, match);
    qp_strlcpy(r->result_value, result, QP_RESULT_MAX);
    cond_rule_append(&host->condition_rules, r);
}

AppData *appdata_create_seed(void)
{
    AppData *d = appdata_create();
    Group *g;
    Template *t;
    Segment *s_prod, *s_ver, *s_vertype, *s_impact, *s_rollback, *s_change;
    static const wchar_t *prod_opts[] = { L"订单系统", L"库存系统", L"财务系统", L"报表系统" };
    static const wchar_t *vertype_opts[] = { L"正式版", L"测试版", L"内测版" };
    static const wchar_t *impact_opts[] = { L"全部用户", L"部分用户", L"内部用户" };
    static const wchar_t *rollback_opts[] = { L"已准备一键回滚", L"灰度观察暂不回滚", L"按需手工回滚" };
    static const wchar_t *change_opts[] = { L"功能新增", L"缺陷修复", L"性能优化", L"安全加固" };
    if (!d) return NULL;

    g = group_create(L"示例");
    if (g) { g->order = 0; group_list_append(&d->groups, g); }

    t = tmpl_create(L"软件版本发布说明口径");
    if (!t) return d;
    if (g) qp_strlcpy(t->group_id, g->id, QP_ID_MAX);

    seg_list_append(&t->segments, seed_static(L"本次发布「"));

    s_prod = seed_var(SEG_SELECT, L"产品名称", L"选择产品", L"订单系统");
    seg_set_options(s_prod, prod_opts, (int)(sizeof(prod_opts) / sizeof(prod_opts[0])));
    seg_list_append(&t->segments, s_prod);

    seg_list_append(&t->segments, seed_static(L"」"));

    s_ver = seed_var(SEG_TEXT, L"版本号", L"如 V2.3.1-20240816-rel", L"V2.3.1-20240816-rel");
    seg_list_append(&t->segments, s_ver);

    seg_list_append(&t->segments, seed_static(L"（"));

    s_vertype = seed_var(SEG_SELECT, L"版本性质", L"", L"正式版");
    seg_set_options(s_vertype, vertype_opts, (int)(sizeof(vertype_opts) / sizeof(vertype_opts[0])));
    seg_list_append(&t->segments, s_vertype);

    seg_list_append(&t->segments, seed_static(L"），主要变更："));

    s_change = seed_var(SEG_MULTI_SELECT, L"变更类型", L"可多选", L"功能新增、缺陷修复");
    seg_set_options(s_change, change_opts, (int)(sizeof(change_opts) / sizeof(change_opts[0])));
    seg_list_append(&t->segments, s_change);

    seg_list_append(&t->segments, seed_static(L"；影响范围："));

    s_impact = seed_var(SEG_SELECT, L"影响范围", L"", L"全部用户");
    seg_set_options(s_impact, impact_opts, (int)(sizeof(impact_opts) / sizeof(impact_opts[0])));
    seg_list_append(&t->segments, s_impact);

    seg_list_append(&t->segments, seed_static(L"；回滚方案："));

    s_rollback = seed_var(SEG_SELECT, L"回滚方案", L"", L"已准备一键回滚");
    seg_set_options(s_rollback, rollback_opts, (int)(sizeof(rollback_opts) / sizeof(rollback_opts[0])));
    seg_list_append(&t->segments, s_rollback);

    seg_list_append(&t->segments, seed_static(L"。"));

    /* 解析规则（挂在版本号上）：按 "-" 取第 3 段判断发布性质，或按关键词 */
    seed_add_parse(s_ver, L"-", 3, L"rel",  s_vertype, L"正式版");
    seed_add_parse(s_ver, L"-", 3, L"beta", s_vertype, L"测试版");
    seed_add_parse_contains(s_ver, L"alpha", s_vertype, L"内测版");

    /* 条件规则（挂在影响范围上）：
       变更类型属于「安全加固」→ 全部用户强制升级；版本号以 V2 开头 → 分批灰度 */
    seed_add_cond(s_impact, s_change, OP_IN, L"安全加固", L"全部用户（安全加固强制升级）");
    seed_add_cond(s_impact, s_ver, OP_STARTS_WITH, L"V2", L"部分用户（分批灰度）");

    /* 条件规则（挂在回滚方案上）：
       版本性质等于「测试版」→ 灰度观察；变更类型包含「缺陷修复」→ 升级前已备份 */
    seed_add_cond(s_rollback, s_vertype, OP_EQUALS, L"测试版", L"灰度观察暂不回滚");
    seed_add_cond(s_rollback, s_change, OP_CONTAINS, L"缺陷修复", L"升级前已备份，按需手工回滚");

    tmpl_list_append(&d->templates, t);

    return d;
}
