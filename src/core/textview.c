/*
 * textview.c —— 预览用纯文本排版的实现（纯 C，无 Windows 依赖）
 */
#include "textview.h"

#include <string.h>

int qp_tv_char_wide(wchar_t c)
{
    unsigned int cp = (unsigned int)c;
    return (c >= 0x1100 && c <= 0x115F) ||      /* 谚文字母 */
           (c >= 0x2E80 && c <= 0xA4CF) ||      /* CJK / 假名 / 注音 */
           (c >= 0xAC00 && c <= 0xD7A3) ||      /* 谚文音节 */
           (c >= 0xF900 && c <= 0xFAFF) ||      /* CJK 兼容 */
           (c >= 0xFE30 && c <= 0xFE6F) ||      /* CJK 标点 */
           (c >= 0xFF00 && c <= 0xFF60) ||      /* 全角 */
           (c >= 0xFFE0 && c <= 0xFFE6) ||
           (cp >= 0x1F300 && cp <= 0x1FAFF) ||
           (cp >= 0x20000 && cp <= 0x3FFFF);
}

static int tv_units(const wchar_t *s, int at, int end)
{
    return at + 1 < end && s[at] >= 0xD800 && s[at] <= 0xDBFF &&
           s[at + 1] >= 0xDC00 && s[at + 1] <= 0xDFFF ? 2 : 1;
}

static int tv_width(const wchar_t *s, int at, int units, int narrow, int wide)
{
    return units == 2 ? (wide > narrow ? wide : narrow) :
                       qp_tv_char_w(s[at], narrow, wide);
}

int qp_tv_char_w(wchar_t c, int w_ascii, int w_wide)
{
    if (w_ascii < 1) w_ascii = 1;
    if (w_wide < w_ascii) w_wide = w_ascii;
    return qp_tv_char_wide(c) ? w_wide : w_ascii;
}

size_t qp_tv_capacity(const wchar_t *text, int nmarks, const wchar_t *mark)
{
    size_t n = 1;
    const wchar_t *p;
    for (p = text; p && *p; ++p) n += *p == L'\t' ? QP_TV_TAB_COLS : 1;
    if (nmarks > 0 && mark) n += (size_t)nmarks * wcslen(mark);
    return n;
}

int qp_tv_build_mapped(const wchar_t *text, const int *marks, int nmarks,
                const wchar_t *mark, wchar_t *disp, int disp_cap,
                unsigned char *kind, int *source_map)
{
    size_t mlen = wcslen(mark ? mark : L"");
    int tlen = 0, n = 0, i, k = 0;

    if (!disp || disp_cap <= 0) return 0;
    disp[0] = L'\0';
    if (text) tlen = (int)wcslen(text);
    if (nmarks < 0) nmarks = 0;
    if (!marks) nmarks = 0;

    for (i = 0; i < tlen; ++i) {
        /* 落在本字符之前的提示先插进去（同一位置可有多个） */
        while (k < nmarks && marks[k] <= i) {
            size_t j;
            if (marks[k] < 0) { ++k; continue; }
            for (j = 0; j < mlen && n < disp_cap - 1; ++j) {
                disp[n] = mark[j];
                if (kind) kind[n] = 1;
                if (source_map) source_map[n] = -1;
                ++n;
            }
            ++k;
        }
        if (n < disp_cap - 1) {
            wchar_t c = text[i];
            if (c == L'\t') {
                /* 制表符折成固定几个空格，免得等宽字体里画成一个方块 */
                int t;
                for (t = 0; t < QP_TV_TAB_COLS && n < disp_cap - 1; ++t) {
                    disp[n] = L' ';
                    if (kind) kind[n] = 0;
                    if (source_map) source_map[n] = i;
                    ++n;
                }
            } else {
                disp[n] = c;
                if (kind) kind[n] = 0;
                if (source_map) source_map[n] = i;
                ++n;
            }
        }
    }
    /* 落在文本末尾（或越界）的提示 */
    while (k < nmarks) {
        size_t j;
        if (marks[k] >= 0) {
            for (j = 0; j < mlen && n < disp_cap - 1; ++j) {
                disp[n] = mark[j];
                if (kind) kind[n] = 1;
                if (source_map) source_map[n] = -1;
                ++n;
            }
        }
        ++k;
    }
    disp[n] = L'\0';
    return n;
}

int qp_tv_build(const wchar_t *text, const int *marks, int nmarks,
                const wchar_t *mark, wchar_t *disp, int disp_cap,
                unsigned char *kind)
{
    return qp_tv_build_mapped(text, marks, nmarks, mark, disp, disp_cap, kind, NULL);
}

int qp_tv_copy_raw(const wchar_t *raw, const int *map, int length,
                   int a, int b, wchar_t *out, int cap)
{
    int lo, hi, i, first = -1, last = -1, n, rawlen;
    if (!out || cap <= 0) return 0;
    out[0] = 0;
    if (!raw || !map || length <= 0) return 0;
    rawlen = (int)wcslen(raw);
    qp_tv_sel_range(a, b, &lo, &hi);
    if (lo < 0) lo = 0;
    if (hi > length) hi = length;
    for (i = lo; i < hi; ++i) if (map[i] >= 0 && map[i] < rawlen) {
        if (first < 0) first = map[i];
        last = map[i];
    }
    if (first < 0 || last < first) return 0;
    if (first > 0 && raw[first] >= 0xDC00 && raw[first] <= 0xDFFF &&
        raw[first - 1] >= 0xD800 && raw[first - 1] <= 0xDBFF) --first;
    n = last - first + tv_units(raw, last, rawlen);
    if (n >= cap) n = cap - 1;
    if (n > 0 && raw[first + n - 1] >= 0xD800 && raw[first + n - 1] <= 0xDBFF) --n;
    wmemcpy(out, raw + first, (size_t)n);
    out[n] = 0;
    return n;
}

static int tv_wrap(const wchar_t *disp, int disp_len, int w_ascii, int w_wide,
               int avail, int *line_off, int *line_len, int line_cap, const int *widths)
{
    int start = 0, n = 0;

    if (!line_off || !line_len || line_cap <= 0) return 0;
    if (avail < 1) avail = 1;
    if (!disp || disp_len <= 0) {
        line_off[0] = 0;
        line_len[0] = 0;
        return 1;
    }
    while (start <= disp_len && n < line_cap) {
        int i = start, width = 0, space = -1, next;
        while (i < disp_len && disp[i] != L'\n' && disp[i] != L'\r') {
            int units = tv_units(disp, i, disp_len);
            int cw = widths ? widths[i] : tv_width(disp, i, units, w_ascii, w_wide);
            if (cw < 1) cw = 1;
            if (width + cw > avail && i > start) break;
            if (disp[i] == L' ') space = i;
            width += cw;
            i += units;
        }
        next = i;
        if (i < disp_len && (disp[i] == L'\n' || disp[i] == L'\r')) {
            next = i + 1;
            if (disp[i] == L'\r' && next < disp_len && disp[next] == L'\n') ++next;
        } else if (i < disp_len && space >= start) {
            i = space;
            next = space + 1;
        }
        line_off[n] = start;
        line_len[n] = i - start;
        ++n;
        if (i == disp_len) break;
        start = next; /* 重新扫描回退后的字符，不能沿用上一行的宽度。 */
    }
    return n;
}

static int tv_x(const wchar_t *disp, const int *line_off, int line, int idx,
               int w_ascii, int w_wide, const int *widths)
{
    int s, e, i, x = 0, total;
    if (!disp || !line_off || line < 0) return 0;
    s = line_off[line];
    total = (int)wcslen(disp);
    e = (idx > s) ? idx : s;
    for (i = s; i < e && disp[i]; ) {
        int units = tv_units(disp, i, total);
        if (i + units > e) break;
        x += widths ? widths[i] : tv_width(disp, i, units, w_ascii, w_wide);
        i += units;
    }
    return x;
}

static int tv_index(const wchar_t *disp, const int *line_off, const int *line_len,
                   int line, int x, int w_ascii, int w_wide, const int *widths)
{
    int s, len, i, acc = 0;
    if (!disp || !line_off || !line_len || line < 0) return 0;
    s = line_off[line];
    len = line_len[line];
    if (x <= 0) return s;
    for (i = 0; i < len; ) {
        int units = tv_units(disp, s + i, s + len);
        int cw = widths ? widths[s + i] : tv_width(disp, s + i, units, w_ascii, w_wide);
        if (cw < 1) cw = 1;
        if (x < acc + cw) return x - acc < (cw + 1) / 2 ? s + i : s + i + units;
        acc += cw;
        i += units;
    }
    return s + len;
}

int qp_tv_wrap(const wchar_t *s, int length, int narrow, int wide, int avail,
                int *off, int *len, int cap)
{ return tv_wrap(s, length, narrow, wide, avail, off, len, cap, NULL); }
int qp_tv_wrap_measured(const wchar_t *s, int length, const int *widths, int avail,
                        int *off, int *len, int cap)
{ return tv_wrap(s, length, 1, 1, avail, off, len, cap, widths); }
int qp_tv_x_of(const wchar_t *s, const int *off, int line, int idx, int narrow, int wide)
{ return tv_x(s, off, line, idx, narrow, wide, NULL); }
int qp_tv_x_measured(const wchar_t *s, const int *off, int line, int idx, const int *widths)
{ return tv_x(s, off, line, idx, 1, 1, widths); }
int qp_tv_index_at(const wchar_t *s, const int *off, const int *len, int line, int x, int narrow, int wide)
{ return tv_index(s, off, len, line, x, narrow, wide, NULL); }
int qp_tv_index_measured(const wchar_t *s, const int *off, const int *len, int line, int x, const int *widths)
{ return tv_index(s, off, len, line, x, 1, 1, widths); }

void qp_tv_sel_range(int a, int b, int *lo, int *hi)
{
    if (a <= b) { if (lo) *lo = a; if (hi) *hi = b; }
    else        { if (lo) *lo = b; if (hi) *hi = a; }
}
