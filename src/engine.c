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

int qp_iends(const wchar_t *hay, const wchar_t *suffix)
{
    size_t hlen, slen;
    if (!hay || !suffix || !*suffix) return 0;
    hlen = wcslen(hay);
    slen = wcslen(suffix);
    if (slen > hlen) return 0;
    return qp_icmp(hay + (hlen - slen), suffix) == 0;
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

/* --------------------------- 占位符 ------------------------------- */
/*
 * 固定片段的内容里可以写 {变量名}，生成时换成那个变量此刻的值。这样「一句话里
 * 夹一个变量」不用再拆成「文本 - 变量 - 文本」三段，也就不必为了拼接而造一堆
 * 只为引用而存在的隐藏变量。
 *
 * 约定就三条：
 *   {{           输出一个字面的 {
 *   {没有的名字}  原样保留 —— 预览里一眼看得见写错了，不会悄悄变成空
 *   换进来的内容里若还有 {…}，不再展开（只替换一层，免得互相套成死循环）
 * 名字按「id 优先、其次变量名（不分大小写）」找，与规则里指目标同一口径。
 */

/* 找出 src 从 pos 起的下一个占位符：*start / *end 为 { 与 } 的下标，name 为中间
   那段（已去掉首尾空白）。{{ 不算占位符（转义留给调用方折叠）。名字里出现
   换行、又一个 {、或长过变量名上限，都当它不是占位符 —— 少了个 } 时不至于把
   后面一整段正文都吞进去。 */
static int placeholder_next(const wchar_t *src, size_t pos,
                            size_t *start, size_t *end,
                            wchar_t *name, size_t cap)
{
    size_t i, j, n;
    if (!src || !name || cap == 0) return 0;
    for (i = pos; src[i]; ++i) {
        if (src[i] != L'{') continue;
        if (src[i + 1] == L'{') { ++i; continue; }
        for (j = i + 1; src[j]; ++j) {
            if (src[j] == L'}' || src[j] == L'{' ||
                src[j] == L'\r' || src[j] == L'\n') break;
            if (j - i > QP_LABEL_MAX) break;
        }
        if (src[j] != L'}') continue;
        n = j - i - 1;
        if (n == 0 || n >= cap) continue;
        memcpy(name, src + i + 1, n * sizeof(wchar_t));
        name[n] = L'\0';
        qp_trim(name);
        if (!name[0]) continue;                     /* {   } 只有空白，不算 */
        if (start) *start = i;
        if (end) *end = j;
        return 1;
    }
    return 0;
}

/* 往 out 尾部追加 n 个字符，装不下就截断（结尾 0 始终留位） */
static size_t append_n(wchar_t *out, size_t cap, size_t used,
                       const wchar_t *src, size_t n)
{
    if (used + 1 >= cap) return used;
    if (n > cap - used - 1) n = cap - used - 1;
    memcpy(out + used, src, n * sizeof(wchar_t));
    used += n;
    out[used] = L'\0';
    return used;
}

/* 追加一段普通文本，途中把 {{ 折成一个 { */
static size_t append_unescaped(wchar_t *out, size_t cap, size_t used,
                               const wchar_t *src, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        if (src[i] == L'{' && i + 1 < n && src[i + 1] == L'{') ++i;
        used = append_n(out, cap, used, src + i, 1);
    }
    return used;
}

size_t qp_expand_placeholders(const wchar_t *src, QpPlaceholderLookup lookup,
                              void *user, wchar_t *out, size_t cap)
{
    size_t used = 0, pos = 0, s = 0, e = 0;
    wchar_t name[QP_LABEL_MAX];
    if (!out || cap == 0) return 0;
    out[0] = L'\0';
    if (!src) return 0;
    while (placeholder_next(src, pos, &s, &e, name, QP_LABEL_MAX)) {
        const wchar_t *v = lookup ? lookup(user, name) : NULL;
        used = append_unescaped(out, cap, used, src + pos, s - pos);
        if (v) used = append_n(out, cap, used, v, wcslen(v));
        else   used = append_n(out, cap, used, src + s, e - s + 1);
        pos = e + 1;
    }
    used = append_unescaped(out, cap, used, src + pos, wcslen(src + pos));
    return used;
}

int qp_placeholder_at(const wchar_t *src, int index, wchar_t *name, size_t cap)
{
    wchar_t buf[QP_LABEL_MAX];
    size_t pos = 0, s = 0, e = 0;
    int n = 0;
    if (name && cap) name[0] = L'\0';
    while (placeholder_next(src, pos, &s, &e, buf, QP_LABEL_MAX)) {
        if (n == index) {
            if (name) qp_strlcpy(name, buf, cap);
            return 1;
        }
        ++n;
        pos = e + 1;
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

/* ------------------------------------------------------------------ */
/* 时间格式化                                                          */
/*                                                                    */
/* 自己解析记号而不是转手 strftime：strftime 的 %-记号对不写代码的人不                */
/* 友好（%Y-%m-%d 谁记得住），而 yyyy-MM-dd 一看就懂，还能直接写                 */
/* 「yyyy年M月d日」这种中间夹汉字的格式。                                       */
/* ------------------------------------------------------------------ */
static const wchar_t *const QP_TIME_FORMATS[] = {
    L"yyyy-MM-dd HH:mm:ss",
    L"yyyy-MM-dd HH:mm",
    L"yyyy-MM-dd",
    L"yyyy年M月d日",
    L"yyyy年M月d日 HH:mm",
    L"M月d日 HH:mm",
    L"HH:mm:ss",
    L"HH:mm",
    L"yyyy/MM/dd HH:mm",
    L"yyyyMMddHHmmss"
};

int qp_time_format_count(void)
{
    return (int)(sizeof(QP_TIME_FORMATS) / sizeof(QP_TIME_FORMATS[0]));
}

const wchar_t *qp_time_format_at(int index)
{
    if (index < 0 || index >= qp_time_format_count()) return L"";
    return QP_TIME_FORMATS[index];
}

/* 数出 fmt 从 p 起连续多少个相同字符（yyyy → 4） */
static int run_len(const wchar_t *p)
{
    int n = 1;
    while (p[n] == p[0]) ++n;
    return n;
}

static void put_num(wchar_t *out, size_t cap, size_t *used, int v, int width)
{
    wchar_t buf[16];
    size_t len;
    swprintf(buf, 16, width >= 2 ? L"%02d" : L"%d", v);
    len = wcslen(buf);
    if (*used + len + 1 > cap) return;
    wmemcpy(out + *used, buf, len);
    *used += len;
    out[*used] = L'\0';
}

static void put_text(wchar_t *out, size_t cap, size_t *used, const wchar_t *s)
{
    size_t len = wcslen(s);
    if (*used + len + 1 > cap) return;
    wmemcpy(out + *used, s, len);
    *used += len;
    out[*used] = L'\0';
}

void qp_format_time(const struct tm *tmv, const wchar_t *fmt,
                    wchar_t *out, size_t cap)
{
    static const wchar_t *const week[] = {
        L"周日", L"周一", L"周二", L"周三", L"周四", L"周五", L"周六"
    };
    const wchar_t *p;
    size_t used = 0;

    if (!out || cap == 0) return;
    out[0] = L'\0';
    if (!tmv) return;
    if (!fmt || !*fmt) fmt = QP_TIME_FORMATS[0];

    for (p = fmt; *p; ) {
        int n = run_len(p);
        switch (*p) {
        case L'y':
            /* yy = 两位年，其余一律四位 */
            if (n == 2) put_num(out, cap, &used, (tmv->tm_year + 1900) % 100, 2);
            else        put_num(out, cap, &used, tmv->tm_year + 1900, 4);
            break;
        case L'M': put_num(out, cap, &used, tmv->tm_mon + 1, n); break;
        case L'd':
            /* ddd = 中文星期；d / dd = 日 */
            if (n >= 3) {
                int w = tmv->tm_wday;
                put_text(out, cap, &used, week[(w >= 0 && w <= 6) ? w : 0]);
            } else {
                put_num(out, cap, &used, tmv->tm_mday, n);
            }
            break;
        case L'H': put_num(out, cap, &used, tmv->tm_hour, n); break;
        case L'm': put_num(out, cap, &used, tmv->tm_min, n); break;
        case L's': put_num(out, cap, &used, tmv->tm_sec, n); break;
        default: {
            /* 非记号字符原样搬过去（一次搬完这一串相同字符） */
            int k;
            for (k = 0; k < n; ++k) {
                if (used + 2 > cap) break;
                out[used++] = *p;
                out[used] = L'\0';
            }
            break;
        }
        }
        p += n;
    }
}

void qp_format_time_now(const wchar_t *fmt, wchar_t *out, size_t cap)
{
    time_t now = time(NULL);
    struct tm tmv;
#if defined(_WIN32)
    localtime_s(&tmv, &now);
#else
    {
        struct tm *p = localtime(&now);
        if (p) tmv = *p;
        else memset(&tmv, 0, sizeof(tmv));
    }
#endif
    qp_format_time(&tmv, fmt, out, cap);
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
    c->multiline = s->multiline;
    qp_strlcpy(c->time_format, s->time_format, QP_FORMAT_MAX);
    qp_strlcpy(c->ref_segment_id, s->ref_segment_id, QP_ID_MAX);
    c->ref_mode = s->ref_mode;
    qp_strlcpy(c->ref_delimiter, s->ref_delimiter, QP_SEP_MAX);
    c->ref_index = s->ref_index;
    c->ref_start = s->ref_start;
    c->ref_len = s->ref_len;
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
        take = (s->ref_len > 0) ? (size_t)s->ref_len : (len - from);
        if (take > len - from) take = len - from;     /* 取多了就取到末尾 */
        if (take > cap - 1) take = cap - 1;
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

void cond_term_set_match(CondTerm *t, const wchar_t *raw)
{
    const wchar_t *p, *start;
    wchar_t buf[QP_MATCH_MAX];
    if (!t) return;
    qp_strlcpy(t->match_value, raw ? raw : L"", QP_MATCH_MAX);
    cond_term_free_values(t);
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
                wchar_t **arr = (wchar_t **)realloc(t->match_values,
                        (size_t)(t->match_value_count + 1) * sizeof(wchar_t *));
                if (arr) {
                    t->match_values = arr;
                    t->match_values[t->match_value_count] = qp_wcsdup(buf);
                    t->match_value_count++;
                }
            }
            if (!*p) break;
            start = p + 1;
        }
    }
}

CondTerm *cond_term_clone(const CondTerm *t)
{
    CondTerm *c;
    if (!t) return NULL;
    c = (CondTerm *)calloc(1, sizeof(CondTerm));
    if (!c) return NULL;
    qp_strlcpy(c->trigger_segment_id, t->trigger_segment_id, QP_ID_MAX);
    c->op = t->op;
    c->negate = t->negate;
    cond_term_set_match(c, t->match_value);
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
    for (t = r->terms; t; t = t->next)
        cond_term_append(&c->terms, cond_term_clone(t));
    return c;
}

void cond_rule_assign(ConditionRule *dst, const ConditionRule *src)
{
    const CondTerm *t;
    if (!dst || !src || dst == src) return;
    cond_term_list_free(dst->terms);
    dst->terms = NULL;
    dst->join = src->join;
    qp_strlcpy(dst->result_value, src->result_value, QP_RESULT_MAX);
    for (t = src->terms; t; t = t->next)
        cond_term_append(&dst->terms, cond_term_clone(t));
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
        f->has_sticky = 0;
        f->rule_driven = 0;
        f->user_value[0] = L'\0';
        f->sticky[0] = L'\0';
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
    /* 注意：手动输入不会压过变量匹配。该字段一旦被规则推导出值，
       eval_run 里就以规则值为准（详见 eval_run 第 3 步）。 */
}

/* 恢复兜底值。并行填写切回来时用：一个「只靠匹配填、此刻已不再命中」的变量，
   靠的就是这个值才不会在输出里空一段，所以它跟手填值一样得随快照一起回来。 */
void eval_set_sticky(EvalContext *ctx, int idx, const wchar_t *v)
{
    EvalField *f;
    if (!ctx || idx < 0 || idx >= ctx->count) return;
    f = &ctx->fields[idx];
    if (!v || !*v) { f->sticky[0] = L'\0'; f->has_sticky = 0; return; }
    qp_strlcpy(f->sticky, v, QP_VALUE_MAX);
    f->has_sticky = 1;
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

/* 单个条件项是否成立（已计入 negate） */
static int cond_term_hit(const CondTerm *t, const wchar_t *trigger_value)
{
    int hit = 0, i;
    if (!t) return 0;
    if (!trigger_value) trigger_value = L"";
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
typedef struct {
    const EvalContext *ctx;
    wchar_t (*work)[QP_VALUE_MAX];
    int self;
} PlaceholderCtx;

static const wchar_t *eval_placeholder_lookup(void *user, const wchar_t *name)
{
    const PlaceholderCtx *p = (const PlaceholderCtx *)user;
    int at;
    if (!p) return NULL;
    at = eval_index_of_id(p->ctx, name);
    if (at < 0 || at == p->self) return NULL;
    return p->work[at];
}

void eval_run(EvalContext *ctx)
{
    wchar_t (*work)[QP_VALUE_MAX];
    wchar_t (*derived)[QP_VALUE_MAX];
    int *derived_set;
    int *parse_set;      /* 本轮该目标已被解析规则占下 */
    int *ref_cycle;      /* 该字段的引用链上有环 → 整条不生效 */
    int i, pass, rp;

    if (!ctx || ctx->count <= 0) return;

    work = (wchar_t (*)[QP_VALUE_MAX])calloc((size_t)ctx->count, sizeof(*work));
    derived = (wchar_t (*)[QP_VALUE_MAX])calloc((size_t)ctx->count, sizeof(*derived));
    derived_set = (int *)calloc((size_t)ctx->count, sizeof(int));
    parse_set = (int *)calloc((size_t)ctx->count, sizeof(int));
    ref_cycle = (int *)calloc((size_t)ctx->count, sizeof(int));
    if (!work || !derived || !derived_set || !parse_set || !ref_cycle) {
        free(work); free(derived); free(derived_set); free(parse_set); free(ref_cycle);
        return;
    }
    ctx->unstable = 0;

    /* 引用成环检测只跟模板结构有关，与填了什么无关，所以在迭代外面做一次。
       A→B→A 这种环没有「正确答案」，整条链一律当没配引用，退回手填/默认值，
       与「条件项指向不存在的变量一律不成立」同一个态度：宁可不生效，不能乱生效。 */
    for (i = 0; i < ctx->count; ++i) {
        const Segment *s = ctx->fields[i].seg;
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
            derived[i][0] = L'\0';
        }

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
                break;
            }
        }

        /* 4) 应用：推导值 > 用户输入 > 默认值。
           变量匹配（条件规则 / 解析规则 / 引用）推导出的值一律压过手动输入与
           手动选择，这样「既能手填又有匹配」的变量不会因为手滑而与口径脱节。 */
        for (i = 0; i < ctx->count; ++i) {
            EvalField *f = &ctx->fields[i];
            wchar_t next[QP_VALUE_MAX];
            if (derived_set[i]) {
                qp_strlcpy(next, derived[i], QP_VALUE_MAX);
            } else if (f->user_touched) {
                qp_strlcpy(next, f->user_value, QP_VALUE_MAX);
            } else {
                qp_strlcpy(next, field_fallback_of(f), QP_VALUE_MAX);
            }
            /* 固定片段里的 {变量名} 换成那个变量此刻的值。放在这里而不是渲染时，
               是为了让它跟着不动点一起收敛：被引用的变量本轮才刚被规则推出值，
               下一轮这里就取得到了。每轮都是从模板原文重新展开一次，所以不会
               越展开越长。只有固定片段做替换 —— 用户自己敲进文本框的 { 永远
               是字面量，不会被当成占位符解释。 */
            if (f->seg && f->seg->type == SEG_STATIC && wcschr(next, L'{')) {
                wchar_t expanded[QP_VALUE_MAX];
                PlaceholderCtx pc;
                pc.ctx = ctx;
                pc.work = work;
                pc.self = i;
                qp_expand_placeholders(next, eval_placeholder_lookup, &pc,
                                       expanded, QP_VALUE_MAX);
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

    /* 提交 */
    for (i = 0; i < ctx->count; ++i) {
        EvalField *f = &ctx->fields[i];
        f->rule_driven = derived_set[i];
        if (derived_set[i] && derived[i][0]) {
            qp_strlcpy(f->sticky, derived[i], QP_VALUE_MAX);  /* 推导过的值留作兜底 */
            f->has_sticky = 1;
        }
        qp_strlcpy(f->value, work[i], QP_VALUE_MAX);
    }

    free(work);
    free(derived);
    free(derived_set);
    free(parse_set);
    free(ref_cycle);
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
    case SEG_DATETIME:     return L"时间";
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
        qp_strlcpy(out, L"（还没有设置条件）", cap);
        used = wcslen(out);
    }
    if (used + 16 < cap) {
        wchar_t tail[QP_RESULT_MAX + 32];
        swprintf(tail, sizeof(tail) / sizeof(tail[0]), L" → 内容 = 「%ls」", r->result_value);
        qp_strlcpy(out + used, tail, cap - used);
    }
}

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
 *   - 空串（一条规则都没命中、也没填过时它就是空）
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
        for (p = o->parse_rules; p; p = p->next)
            if (tmpl_find_segment(t, p->target_segment_id) == s) return -1;
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

static void lint_add(LintIssue *out, int *n, LintKind kind,
                     int seg_index, int rule_index, const wchar_t *text)
{
    LintIssue *it = &out[*n];
    it->kind = kind;
    it->seg_index = seg_index;
    it->rule_index = rule_index;
    qp_strlcpy(it->text, text, QP_RESULT_MAX);
    ++(*n);
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
                         L"第 %d 段（%ls）：占位符「{%ls}」找不到对应的变量，"
                         L"生成时会原样输出这几个字。", si + 1, name, ph);
                lint_add(out, &n, LINT_PLACEHOLDER, si, -1, msg);
            }
        }

        /* 解析规则的目标变量已被删除 */
        ri = 0;
        for (p = s->parse_rules; p && n < cap; p = p->next, ++ri) {
            if (tmpl_find_segment(t, p->target_segment_id)) continue;
            swprintf(msg, QP_RESULT_MAX,
                     L"第 %d 段（%ls）第 %d 条解析规则：目标变量已被删除，"
                     L"这条规则不会生效。", si + 1, name, ri + 1);
            lint_add(out, &n, LINT_PARSE_DANGLING, si, ri, msg);
        }

        ri = 0;
        for (r = s->condition_rules; r && n < cap; r = r->next, ++ri) {
            const CondTerm *term;
            int ti = 0;

            if (!r->terms) {
                swprintf(msg, QP_RESULT_MAX,
                         L"第 %d 段（%ls）第 %d 条条件规则一个条件都没有，"
                         L"永远不会命中。", si + 1, name, ri + 1);
                lint_add(out, &n, LINT_COND_NOTERM, si, ri, msg);
                continue;
            }
            if (!r->result_value[0] && n < cap) {
                swprintf(msg, QP_RESULT_MAX,
                         L"第 %d 段（%ls）第 %d 条条件规则命中后会把这一段变成空，"
                         L"确认是有意的吗？", si + 1, name, ri + 1);
                lint_add(out, &n, LINT_COND_EMPTY, si, ri, msg);
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
                                 term->trigger_segment_id[0] ? L"已被删除" : L"还没选");
                        lint_add(out, &n, LINT_COND_DANGLING, si, ri, msg);
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
                                 L"任意满足即可，所以规则还能命中，但这一项白写了。",
                                 si + 1, name, ri + 1, dead);
                    lint_add(out, &n, LINT_COND_NEVER, si, ri, msg);
                }
            }
        }
    }
    return n;
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
    cond_rule_set_simple(r, trigger ? trigger->id : L"", op, 0, match);
    qp_strlcpy(r->result_value, result, QP_RESULT_MAX);
    cond_rule_append(&host->condition_rules, r);
}

/* 多条件项的规则：先 begin 拿到规则，再逐个 seed_cond_term 加条件项 */
static ConditionRule *seed_cond_begin(Segment *host, CondJoin join,
                                      const wchar_t *result)
{
    ConditionRule *r = cond_rule_create();
    if (!r || !host) { cond_rule_free(r); return NULL; }
    cond_term_list_clear(&r->terms);      /* 丢掉自带的空白项，下面自己加 */
    r->join = join;
    qp_strlcpy(r->result_value, result, QP_RESULT_MAX);
    cond_rule_append(&host->condition_rules, r);
    return r;
}

static void seed_cond_term(ConditionRule *r, const Segment *trigger,
                           CondOperator op, int negate, const wchar_t *match)
{
    CondTerm *t;
    if (!r) return;
    t = cond_term_create();
    if (!t) return;
    qp_strlcpy(t->trigger_segment_id, trigger ? trigger->id : L"", QP_ID_MAX);
    t->op = op;
    t->negate = negate;
    cond_term_set_match(t, match ? match : L"");
    cond_term_append(&r->terms, t);
}

static Segment *seed_flag(Segment *s, int hidden, int multiline)
{
    if (s) { s->hidden = hidden; s->multiline = multiline; }
    return s;
}

/* ------------------------------------------------------------------ */
/* 复杂示例：一条把所有功能都用上的通报口径，装完就能照着它试            */
/*                                                                    */
/* 覆盖：固定片段（含换行）、文本输入、下拉单选、下拉多选、隐藏变量、    */
/*       多行大文本框、按分隔符取段、按关键词命中、多条件项（全部满足 /  */
/*       任意满足 / 不满足取反）、以及「变量匹配的结果压过手动选择」。   */
/* ------------------------------------------------------------------ */
Template *tmpl_create_demo(void)
{
    Template *t = tmpl_create(L"复杂示例 · 违规处置通报");
    Segment *s_ticket, *s_date, *s_plat, *s_level, *s_kind, *s_action;
    Segment *s_detail, *s_evid, *s_follow, *s_reviewer, *s_review;
    ConditionRule *r;
    static const wchar_t *plat_opts[]   = { L"微信群", L"QQ 群", L"直播间", L"短视频评论区" };
    static const wchar_t *level_opts[]  = { L"严重", L"一般", L"轻微" };
    static const wchar_t *kind_opts[]   = { L"辱骂他人", L"刷屏广告", L"发布外链",
                                            L"诱导私加", L"违规内容" };
    static const wchar_t *action_opts[] = { L"口头警告", L"禁言 24 小时", L"禁言 7 天",
                                            L"永久封禁", L"移出并拉黑" };
    if (!t) return NULL;

    seg_list_append(&t->segments, seed_static(L"【处置通报】\r\n· 工单编号："));

    /* 一个变量喂饱下面一串：日期、等级、处置方式全从工单编号里拆出来 */
    s_ticket = seed_var(SEG_TEXT, L"工单编号", L"如 GK-20260817-A2-严重-封禁",
                        L"GK-20260817-A2-严重-封禁");
    seg_list_append(&t->segments, s_ticket);

    seg_list_append(&t->segments, seed_static(L"\r\n· 发生日期："));
    s_date = seed_flag(seed_var(SEG_TEXT, L"发生日期", L"由工单编号自动拆出", L""), 1, 0);
    seg_list_append(&t->segments, s_date);

    seg_list_append(&t->segments, seed_static(L"\r\n· 处置平台："));
    s_plat = seed_var(SEG_SELECT, L"处置平台", L"", L"微信群");
    seg_set_options(s_plat, plat_opts, (int)(sizeof(plat_opts) / sizeof(plat_opts[0])));
    seg_list_append(&t->segments, s_plat);

    seg_list_append(&t->segments, seed_static(L"　违规等级："));
    s_level = seed_var(SEG_SELECT, L"违规等级", L"", L"一般");
    seg_set_options(s_level, level_opts, (int)(sizeof(level_opts) / sizeof(level_opts[0])));
    seg_list_append(&t->segments, s_level);

    seg_list_append(&t->segments, seed_static(L"\r\n· 违规类型："));
    s_kind = seed_var(SEG_MULTI_SELECT, L"违规类型", L"可多选", L"刷屏广告");
    seg_set_options(s_kind, kind_opts, (int)(sizeof(kind_opts) / sizeof(kind_opts[0])));
    seg_list_append(&t->segments, s_kind);

    seg_list_append(&t->segments, seed_static(L"\r\n· 处置方式："));
    s_action = seed_var(SEG_SELECT, L"处置方式", L"", L"口头警告");
    seg_set_options(s_action, action_opts,
                    (int)(sizeof(action_opts) / sizeof(action_opts[0])));
    seg_list_append(&t->segments, s_action);

    seg_list_append(&t->segments, seed_static(L"\r\n· 处置说明：\r\n"));
    s_detail = seed_flag(seed_var(SEG_TEXT, L"处置说明",
                                  L"逐行写清取证时间、截图编号、当事人回应…", L""), 0, 1);
    seg_list_append(&t->segments, s_detail);

    seg_list_append(&t->segments, seed_static(L"\r\n· 取证情况："));
    s_evid = seed_flag(seed_var(SEG_TEXT, L"取证情况", L"", L"暂无截图，待补"), 1, 0);
    seg_list_append(&t->segments, s_evid);

    seg_list_append(&t->segments, seed_static(L"\r\n· 后续要求："));
    s_follow = seed_flag(seed_var(SEG_TEXT, L"后续要求", L"", L"按常规流程归档即可"), 1, 0);
    seg_list_append(&t->segments, s_follow);

    seg_list_append(&t->segments, seed_static(L"\r\n· 复核人："));
    s_reviewer = seed_var(SEG_TEXT, L"复核人", L"留空表示尚未复核", L"");
    seg_list_append(&t->segments, s_reviewer);

    s_review = seed_flag(seed_var(SEG_TEXT, L"复核状态", L"", L"（待复核）"), 1, 0);
    seg_list_append(&t->segments, s_review);

    /* --- 解析规则：挂在「工单编号」上，一次输入带出三个变量 --- */
    /* 按 "-" 取第 2 段 = 日期；不填充值，直接用取到的那一段 */
    seed_add_parse(s_ticket, L"-", 2, L"", s_date, L"");
    /* 按 "-" 取第 4 段，命中哪个词就填对应等级 */
    seed_add_parse(s_ticket, L"-", 4, L"严重", s_level, L"严重");
    seed_add_parse(s_ticket, L"-", 4, L"一般", s_level, L"一般");
    seed_add_parse(s_ticket, L"-", 4, L"轻微", s_level, L"轻微");
    /* 按关键词命中：整串里出现就算，顺序在前的先生效 */
    seed_add_parse_contains(s_ticket, L"封禁", s_action, L"永久封禁");
    seed_add_parse_contains(s_ticket, L"拉黑", s_action, L"移出并拉黑");
    seed_add_parse_contains(s_ticket, L"禁言", s_action, L"禁言 7 天");
    seed_add_parse_contains(s_ticket, L"警告", s_action, L"口头警告");
    /* 多行文本里写了「截图」就自动标注附件情况 */
    seed_add_parse_contains(s_detail, L"截图", s_evid, L"已附截图取证");

    /* --- 条件规则：挂在「后续要求」上，按顺序取第一条命中的 --- */
    /* 全部满足：等级严重 且 处置方式属于封禁/拉黑 */
    r = seed_cond_begin(s_follow, COND_JOIN_ALL,
                        L"2 小时内同步风控组，纳入平台黑名单，并在群内公示");
    seed_cond_term(r, s_level, OP_EQUALS, 0, L"严重");
    seed_cond_term(r, s_action, OP_IN, 0, L"永久封禁,移出并拉黑");
    /* 任意满足：平台在直播间/评论区，或违规类型里有「违规内容」 */
    r = seed_cond_begin(s_follow, COND_JOIN_ANY,
                        L"立即截图取证并上报内容安全组，24 小时内回执");
    seed_cond_term(r, s_plat, OP_IN, 0, L"直播间,短视频评论区");
    seed_cond_term(r, s_kind, OP_CONTAINS, 0, L"违规内容");
    /* 带取反：等级「不是」轻微 且 处置说明「不为空」 */
    r = seed_cond_begin(s_follow, COND_JOIN_ALL, L"抄送组长复核后归档");
    seed_cond_term(r, s_level, OP_EQUALS, 1, L"轻微");
    seed_cond_term(r, s_detail, OP_EMPTY, 1, L"");

    /* --- 条件规则：复核人填了就算已复核（同样是「不为空」的写法） --- */
    r = seed_cond_begin(s_review, COND_JOIN_ALL, L"（已复核）");
    seed_cond_term(r, s_reviewer, OP_EMPTY, 1, L"");

    return t;
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

    /* 再放一份把全部功能都用上的复杂示例，方便照着它试规则 */
    t = tmpl_create_demo();
    if (t) {
        if (g) qp_strlcpy(t->group_id, g->id, QP_ID_MAX);
        tmpl_list_append(&d->templates, t);
    }

    return d;
}
