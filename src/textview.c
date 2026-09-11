/*
 * textview.c —— 预览用纯文本排版的实现（纯 C，无 Windows 依赖）
 */
#include "textview.h"

#include <string.h>

int qp_tv_char_wide(wchar_t c)
{
    return (c >= 0x1100 && c <= 0x115F) ||      /* 谚文字母 */
           (c >= 0x2E80 && c <= 0xA4CF) ||      /* CJK / 假名 / 注音 */
           (c >= 0xAC00 && c <= 0xD7A3) ||      /* 谚文音节 */
           (c >= 0xF900 && c <= 0xFAFF) ||      /* CJK 兼容 */
           (c >= 0xFE30 && c <= 0xFE6F) ||      /* CJK 标点 */
           (c >= 0xFF00 && c <= 0xFF60) ||      /* 全角 */
           (c >= 0xFFE0 && c <= 0xFFE6);
}

int qp_tv_char_w(wchar_t c, int w_ascii, int w_wide)
{
    if (w_ascii < 1) w_ascii = 1;
    if (w_wide < w_ascii) w_wide = w_ascii;
    return qp_tv_char_wide(c) ? w_wide : w_ascii;
}

int qp_tv_build(const wchar_t *text, const int *marks, int nmarks,
                const wchar_t *mark, wchar_t *disp, int disp_cap,
                unsigned char *kind)
{
    size_t mlen = wcslen(mark ? mark : L"");
    int tlen = 0, n = 0, i, k = 0;

    if (!disp || disp_cap <= 0) return 0;
    disp[0] = L'\0';
    if (text) tlen = (int)wcslen(text);
    if (nmarks < 0) nmarks = 0;

    for (i = 0; i < tlen; ++i) {
        /* 落在本字符之前的提示先插进去（同一位置可有多个） */
        while (k < nmarks && marks[k] <= i) {
            size_t j;
            if (marks[k] < 0) { ++k; continue; }
            for (j = 0; j < mlen && n < disp_cap - 1; ++j) {
                disp[n] = mark[j];
                if (kind) kind[n] = 1;
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
                    ++n;
                }
            } else {
                disp[n] = c;
                if (kind) kind[n] = 0;
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
                ++n;
            }
        }
        ++k;
    }
    disp[n] = L'\0';
    return n;
}

int qp_tv_wrap(const wchar_t *disp, int disp_len, int w_ascii, int w_wide,
               int avail, int *line_off, int *line_len, int line_cap)
{
    int i = 0, start = 0, lastSpace = -1, w = 0, n = 0;

    if (!line_off || !line_len || line_cap <= 0) return 0;
    if (avail < 1) avail = 1;
    if (!disp || disp_len <= 0) {
        line_off[0] = 0;
        line_len[0] = 0;
        return 1;
    }
    while (i < disp_len && n < line_cap) {
        wchar_t c = disp[i];
        int cw;
        if (c == L'\n') {
            line_off[n] = start;
            line_len[n] = i - start;
            ++n;
            ++i;
            start = i;
            w = 0;
            lastSpace = -1;
            continue;
        }
        cw = qp_tv_char_w(c, w_ascii, w_wide);
        if (w + cw > avail && i > start) {
            int brk = (lastSpace >= start) ? lastSpace : i;
            line_off[n] = start;
            line_len[n] = brk - start;
            ++n;
            start = (lastSpace >= start) ? brk + 1 : brk;
            w = 0;
            lastSpace = -1;
            continue;                    /* 当前字符留到新行重新处理 */
        }
        if (c == L' ') lastSpace = i;
        w += cw;
        ++i;
    }
    if (n < line_cap) {
        line_off[n] = start;
        line_len[n] = disp_len - start;
        ++n;
    }
    return n;
}

int qp_tv_x_of(const wchar_t *disp, const int *line_off, int line, int idx,
               int w_ascii, int w_wide)
{
    int s, e, i, x = 0;
    if (!disp || !line_off || line < 0) return 0;
    s = line_off[line];
    e = (idx > s) ? idx : s;
    for (i = s; i < e && disp[i]; ++i) x += qp_tv_char_w(disp[i], w_ascii, w_wide);
    return x;
}

int qp_tv_index_at(const wchar_t *disp, const int *line_off, const int *line_len,
                   int line, int x, int w_ascii, int w_wide)
{
    int s, len, i, acc = 0;
    if (!disp || !line_off || !line_len || line < 0) return 0;
    s = line_off[line];
    len = line_len[line];
    if (x <= 0) return s;
    for (i = 0; i < len; ++i) {
        int cw = qp_tv_char_w(disp[s + i], w_ascii, w_wide);
        if (acc + cw > x) return s + i;          /* 落在本字符前半段 */
        if (acc + cw * 2 > x) return s + i + 1;  /* 后半段算下一个 */
        acc += cw;
    }
    return s + len;
}

void qp_tv_sel_range(int a, int b, int *lo, int *hi)
{
    if (a <= b) { if (lo) *lo = a; if (hi) *hi = b; }
    else        { if (lo) *lo = b; if (hi) *hi = a; }
}
