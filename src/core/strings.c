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
/* 字符串工具                                                          */
/* ================================================================== */

void qp_strlcpy(wchar_t *dst, const wchar_t *src, size_t cap)
{
    size_t i;
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = L'\0'; return; }
    for (i = 0; i + 1 < cap && src[i]; ++i) dst[i] = src[i];
    if (i && dst[i - 1] >= 0xD800 && dst[i - 1] <= 0xDBFF &&
        src[i] >= 0xDC00 && src[i] <= 0xDFFF) --i;
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
                len = qp_utf16_prefix(seg_start,len,cap-1);
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

int qp_between_take(const wchar_t *src, const wchar_t *before,
                    const wchar_t *after, wchar_t *out, size_t cap)
{
    const wchar_t *start, *end;
    size_t len;

    if (out && cap) out[0] = L'\0';
    if (!src || !out || cap == 0) return 0;

    /* 前标记：空 = 从头；非空 = 定位到它之后（大小写不敏感，与 qp_istr 一致）。
       找不到就算没命中 —— 与 split 取不到段一样返回 0，宁可不填不能乱填。 */
    if (before && *before) {
        const wchar_t *p = qp_istr(src, before);
        if (!p) return 0;
        start = p + wcslen(before);
    } else {
        start = src;
    }

    /* 后标记：空 = 到末尾；非空 = 从 start 之后再找一次，找不到同样算没命中。 */
    if (after && *after) {
        const wchar_t *q = qp_istr(start, after);
        if (!q) return 0;
        end = q;
    } else {
        end = start + wcslen(start);
    }

    len = (size_t)(end - start);
    len = qp_utf16_prefix(start,len,cap-1);
    memcpy(out, start, len * sizeof(wchar_t));
    out[len] = L'\0';
    return 1;
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
size_t append_n(wchar_t *out, size_t cap, size_t used,
                       const wchar_t *src, size_t n)
{
    return qp_append_checked(out,cap,used,src,n,NULL);
}

size_t qp_append_checked(wchar_t *out,size_t cap,size_t used,const wchar_t *src,size_t n,unsigned *flags)
{
    size_t room=out && cap>used?cap-used-1:0;
    size_t take=qp_utf16_prefix(src,n,room);
    if(take<n && flags)*flags|=QP_TEXT_TRUNCATED;
    if(!out || cap<=used)return used;
    memcpy(out+used,src,take*sizeof(*out));used+=take;
    out[used] = L'\0';
    return used;
}

/* 追加一段普通文本，途中做两件折叠：
   {{        → 一个字面的 {
   \t \n \\  → 制表符 / 换行 / 反斜杠；其余 \x 原样保留（认不出就照打，
               与「认不出的 {名字} 原样保留」同一态度）。
   只有固定片段的正文会走到这里（占位符展开的必经之路），所以用户自己敲进
   文本框里的 \ 与 { 永远是字面量。 */
static size_t append_unescaped(wchar_t *out, size_t cap, size_t used,
                               const wchar_t *src, size_t n, unsigned *flags)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        wchar_t ch = src[i];
        if (ch == L'{' && i + 1 < n && src[i + 1] == L'{') {
            ++i;                                   /* {{ 折成一个字面 { */
        } else if (ch == L'\\' && i + 1 < n) {
            wchar_t nx = src[i + 1];
            if      (nx == L't')  { ch = L'\t'; ++i; }
            else if (nx == L'n')  { ch = L'\n'; ++i; }
            else if (nx == L'\\') { ch = L'\\'; ++i; }
            /* 其余 \x：ch 仍是 '\\'，原样输出，x 下一轮照常处理 */
            used = qp_append_checked(out, cap, used, &ch, 1, flags);
            continue;
        }
        size_t units=i+1<n && src[i]>=0xD800 && src[i]<=0xDBFF && src[i+1]>=0xDC00 && src[i+1]<=0xDFFF?2:1;
        used = qp_append_checked(out, cap, used, src + i, units, flags);
        i+=units-1;
    }
    return used;
}

/* 带标记的占位符展开（内部用）：值取到空串时把位置记进 marks。
   out_base 会加在每个记录上，便于拼进更大的缓冲后仍能定位。 */
size_t qp_expand_placeholders_status(const wchar_t *src, QpPlaceholderLookup lookup,
                                            void *user, wchar_t *out, size_t cap,
                                            int out_base, int *marks, int mark_cap,
                                            int *mark_count,
                                            int (*missing)(void *, const wchar_t *), unsigned *flags)
{
    size_t used = 0, pos = 0, s = 0, e = 0;
    wchar_t name[QP_LABEL_MAX];
    int n = 0;
    if (mark_count) *mark_count = 0;
    if (!out || cap == 0) return 0;
    out[0] = L'\0';
    if (!src) return 0;
    while (placeholder_next(src, pos, &s, &e, name, QP_LABEL_MAX)) {
        const wchar_t *v = lookup ? lookup(user, name) : NULL;
        used = append_unescaped(out, cap, used, src + pos, s - pos, flags);
        if (v) {
            /* 认得出、但此刻是空 —— 这里本该出现一段内容却没出现 */
            if (!v[0] && marks && n < mark_cap && (!missing || missing(user, name)))
                marks[n++] = out_base + (int)used;
            used = qp_append_checked(out, cap, used, v, wcslen(v), flags);
        } else {
            /* 认不出的名字原样保留（写错了要看得见），不算「缺内容」 */
            used = qp_append_checked(out, cap, used, src + s, e - s + 1, flags);
        }
        pos = e + 1;
    }
    used = append_unescaped(out, cap, used, src + pos, wcslen(src + pos), flags);
    if (mark_count) *mark_count = n;
    return used;
}

size_t qp_expand_placeholders_checked(const wchar_t *src,QpPlaceholderLookup lookup,void *user,
    wchar_t *out,size_t cap,int base,int *marks,int mark_cap,int *mark_count,
    int (*missing)(void *,const wchar_t *))
{
    return qp_expand_placeholders_status(src,lookup,user,out,cap,base,marks,mark_cap,mark_count,missing,NULL);
}

size_t qp_expand_placeholders_marked(const wchar_t *src, QpPlaceholderLookup lookup,
    void *user, wchar_t *out, size_t cap, int base, int *marks, int mark_cap, int *count)
{
    return qp_expand_placeholders_checked(src, lookup, user, out, cap, base, marks, mark_cap, count, NULL);
}

size_t qp_expand_placeholders(const wchar_t *src, QpPlaceholderLookup lookup,
                              void *user, wchar_t *out, size_t cap)
{
    return qp_expand_placeholders_marked(src, lookup, user, out, cap, 0,
                                         NULL, 0, NULL);
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
    L"yyyyMMdd",
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
