/*
 * test_textview.c —— 预览排版内核自测（纯 C，本机 gcc 即可跑）
 */
#include "../src/textview.h"

#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int g_pass = 0, g_fail = 0;

static void check(int cond, const char *what)
{
    if (cond) { ++g_pass; printf("  [ok]   %s\n", what); }
    else      { ++g_fail; printf("  [FAIL] %s\n", what); }
}

static void check_str(const wchar_t *got, const wchar_t *want, const char *what)
{
    if (got && want && wcscmp(got, want) == 0) {
        ++g_pass;
        printf("  [ok]   %s\n", what);
    } else {
        ++g_fail;
        printf("  [FAIL] %s\n         期望: %ls\n         实际: %ls\n",
               what, want ? want : L"(null)", got ? got : L"(null)");
    }
}

#define BUFCAP 512
static wchar_t disp[BUFCAP];
static unsigned char kind[BUFCAP];

/* 显示串里 kind=1 的那几段拼起来（用来断言「只有提示被标出来」） */
static void marked_text(wchar_t *out, int cap)
{
    int i, n = 0;
    for (i = 0; disp[i] && n < cap - 1; ++i)
        if (kind[i]) out[n++] = disp[i];
    out[n] = L'\0';
}

static void test_build_basic(void)
{
    wchar_t mk[BUFCAP];
    int marks[8], n;

    printf("\n== 拼显示串 ==\n");
    n = qp_tv_build(L"经核查由于硬盘异常", NULL, 0, L"失败", disp, BUFCAP, kind);
    check(n == 9, "无提示时长度不变");
    check_str(disp, L"经核查由于硬盘异常", "无提示时正文原样");
    marked_text(mk, BUFCAP);
    check_str(mk, L"", "无提示时没有任何字符被标成提示");

    marks[0] = 5;
    n = qp_tv_build(L"经核查由于异常", marks, 1, L"失败", disp, BUFCAP, kind);
    check_str(disp, L"经核查由于失败异常", "提示插在指定位置");
    marked_text(mk, BUFCAP);
    check_str(mk, L"失败", "只有插进去的提示被标成 kind=1");

    /* 末尾位置 */
    marks[0] = 5;
    qp_tv_build(L"经核查由于", marks, 1, L"失败", disp, BUFCAP, kind);
    check_str(disp, L"经核查由于失败", "提示可以落在文本末尾");

    /* 同一位置多个提示 */
    marks[0] = 2; marks[1] = 2;
    qp_tv_build(L"甲乙丙", marks, 2, L"!", disp, BUFCAP, kind);
    check_str(disp, L"甲乙!!丙", "同一位置可以叠多个提示");

    /* 多个位置 */
    marks[0] = 0; marks[1] = 3;
    qp_tv_build(L"abcdef", marks, 2, L"*", disp, BUFCAP, kind);
    check_str(disp, L"*abc*def", "多个位置各插一个");
}

static void test_build_never_colors_data(void)
{
    wchar_t mk[BUFCAP];
    int marks[4];

    printf("\n== 数据里的「失败」不该被标红 ==\n");
    /* 正文本身含「失败」二字，但没有给任何提示位置 —— 一个字符都不能标 */
    qp_tv_build(L"处理结果：失败，告警未消除", NULL, 0, L"失败",
                disp, BUFCAP, kind);
    check_str(disp, L"处理结果：失败，告警未消除", "正文里的「失败」原样保留");
    marked_text(mk, BUFCAP);
    check_str(mk, L"", "正文里的「失败」不被标成提示");

    /* 只有给定了未匹配位置的那一处才标，别处一律不标 */
    marks[0] = 5;                      /* 插在「：」之后 */
    qp_tv_build(L"处理结果：失败", marks, 1, L"失败", disp, BUFCAP, kind);
    check_str(disp, L"处理结果：失败失败", "提示与正文里的「失败」同时存在");
    marked_text(mk, BUFCAP);
    check_str(mk, L"失败", "只标出插进去的那一个提示");
    check(kind[5] == 1 && kind[6] == 1, "提示两字 kind=1");
    check(kind[7] == 0 && kind[8] == 0, "正文里的「失败」仍是 kind=0");
}

static void test_build_edge(void)
{
    int marks[4];
    wchar_t small[8];

    printf("\n== 拼显示串的边界 ==\n");
    /* 制表符折成空格 */
    qp_tv_build(L"a\tb", NULL, 0, L"!", disp, BUFCAP, kind);
    check_str(disp, L"a    b", "制表符折成四个空格");

    /* 越界位置夹到末尾、负数忽略 */
    marks[0] = 999; marks[1] = -1;
    qp_tv_build(L"abc", marks, 2, L"!", disp, BUFCAP, kind);
    check_str(disp, L"abc!", "越界位置夹到末尾、负数忽略");

    /* 缓冲区不够也不能越界，且始终以 0 结尾 */
    memset(small, 0x7f, sizeof(small));
    qp_tv_build(L"abcdefghij", NULL, 0, L"!", small, 5, NULL);
    check(small[4] == L'\0' && wcslen(small) == 4, "缓冲不够时截断并封口");

    /* kind 允许为 NULL（只要文本时不必分配） */
    check(qp_tv_build(L"abc", NULL, 0, L"!", disp, BUFCAP, NULL) == 3,
          "kind 可以为 NULL");
}

static void test_wrap(void)
{
    int off[64], len[64], n;

    printf("\n== 折行 ==\n");
    /* 窄字符宽 10、宽字符宽 20；可用宽 30 → 每行 3 个窄字符 */
    n = qp_tv_wrap(L"abcdefgh", 8, 10, 20, 30, off, len, 64);
    check(n == 3, "8 个窄字符在 30px 下折成 3 行");
    check(off[0] == 0 && len[0] == 3, "第 1 行取前 3 个");
    check(off[1] == 3 && len[1] == 3, "第 2 行接着 3 个");
    check(off[2] == 6 && len[2] == 2, "第 3 行剩 2 个");

    /* 宽字符按两倍宽：30px 装不下两个（40px），所以一行一个 */
    n = qp_tv_wrap(L"中文测试", 4, 10, 20, 30, off, len, 64);
    check(n == 4, "4 个汉字在 30px 下折成 4 行（每行只放得下 1 个）");
    check(len[0] == 1 && len[1] == 1 && len[2] == 1 && len[3] == 1,
          "宽字符一行一个");

    /* 优先在空格处断：35px 放不下 "ab cd"，断在空格并跳过它 */
    n = qp_tv_wrap(L"ab cd", 5, 10, 20, 35, off, len, 64);
    check(n == 2 && off[0] == 0 && len[0] == 2 && off[1] == 3,
          "宽度不够时在空格处断，并跳过断点空格");
    /* 宽度够时不该断 */
    n = qp_tv_wrap(L"ab cd", 5, 10, 20, 50, off, len, 64);
    check(n == 1 && len[0] == 5, "宽度刚好够时不折行");

    /* 换行符强制断行 */
    n = qp_tv_wrap(L"ab\ncd", 5, 10, 20, 1000, off, len, 64);
    check(n == 2 && len[0] == 2 && off[1] == 3 && len[1] == 2, "换行符强制断行");

    /* 单个字符比可用宽还宽：不能死循环，也不能漏字 */
    n = qp_tv_wrap(L"中中", 2, 10, 20, 5, off, len, 64);
    check(n == 2 && len[0] == 1 && len[1] == 1, "超宽的字符各占一行，不会丢");

    /* 空文本也有一行（光标/选区要落得下） */
    n = qp_tv_wrap(L"", 0, 10, 20, 30, off, len, 64);
    check(n == 1 && len[0] == 0, "空文本返回一个空行");
}

static void test_hit_test(void)
{
    int off[64], len[64], n, i;

    printf("\n== 命中与反算 ==\n");
    n = qp_tv_wrap(L"abcd", 4, 10, 20, 1000, off, len, 64);
    check(n >= 1, "一行放得下");

    /* 行内像素 → 下标 */
    check(qp_tv_index_at(L"abcd", off, len, 0, -5, 10, 20) == 0, "x<0 归到行首");
    check(qp_tv_index_at(L"abcd", off, len, 0, 0, 10, 20) == 0, "x=0 是第一个字符");
    check(qp_tv_index_at(L"abcd", off, len, 0, 25, 10, 20) == 2, "x=25 落在第三个字符");
    check(qp_tv_index_at(L"abcd", off, len, 0, 999, 10, 20) == 4, "x 超出行宽归到行尾");

    /* 下标 → 像素，与上面互为反函数 */
    for (i = 0; i <= 4; ++i)
        check(qp_tv_index_at(L"abcd", off, len, 0, qp_tv_x_of(L"abcd", off, 0, i, 10, 20),
                             10, 20) == i,
              (i == 0) ? "下标 0 ↔ 像素 0 往返一致" : "下标与像素互算往返一致");

    /* 宽字符 */
    n = qp_tv_wrap(L"中a", 2, 10, 20, 1000, off, len, 64);
    check(qp_tv_x_of(L"中a", off, 0, 1, 10, 20) == 20, "宽字符占两格宽");
    check(qp_tv_x_of(L"中a", off, 0, 2, 10, 20) == 30, "宽字符之后的偏移正确");
}

static void test_sel_range(void)
{
    int lo = -1, hi = -1;
    printf("\n== 选区排序 ==\n");
    qp_tv_sel_range(7, 3, &lo, &hi);
    check(lo == 3 && hi == 7, "反向拖选也能得到有序区间");
    qp_tv_sel_range(2, 2, &lo, &hi);
    check(lo == 2 && hi == 2, "同一位置得到空选区");
}

static void test_wide(void)
{
    printf("\n== 宽窄判定 ==\n");
    check(qp_tv_char_wide(L'a') == 0, "半角字母是窄的");
    check(qp_tv_char_wide(L'中') == 1, "汉字是宽的");
    check(qp_tv_char_wide(L'，') == 1, "全角逗号是宽的");
    check(qp_tv_char_wide(L'。') == 1, "全角句号是宽的");
    check(qp_tv_char_wide(L' ') == 0, "空格是窄的");
}

int main(void)
{
    setlocale(LC_ALL, "");
    printf("== 预览排版内核自测 ==\n");
    test_build_basic();
    test_build_never_colors_data();
    test_build_edge();
    test_wrap();
    test_hit_test();
    test_sel_range();
    test_wide();
    printf("\n=========================\n通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
