/*
 * regex_probe.c —— 差分验证用的探针
 *
 * 从标准输入逐条读入 (模式, 文本)：每两行为一条，行内是空格分隔的十六进制码点
 * （这样模式与文本里有什么字符都不用转义）。对每条跑一次 qp_regex_search，
 * 输出：序号 \t 是否命中 \t 第0组 \t 第1组 \t 第2组 \t 第3组
 * 组内容按码点十六进制连写；未命中或该组没参与则输出 "-"。
 *
 * 与 tests/regex_diff.py 配对使用：那边用 Python 的 re 算出期望值再逐行比对。
 */
#include "../src/regex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROBE_MAX 8192
#define PROBE_GROUPS 4

static int parse_hex_line(const char *line, wchar_t *out, int cap)
{
    const char *p = line;
    int n = 0;
    while (*p && n < cap - 1) {
        char buf[16];
        int k = 0;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        if (!*p) break;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' &&
               k < (int)sizeof(buf) - 1)
            buf[k++] = *p++;
        buf[k] = '\0';
        out[n++] = (wchar_t)strtoul(buf, NULL, 16);
    }
    out[n] = L'\0';
    return n;
}

static void print_hex(const wchar_t *s, size_t len)
{
    size_t i;
    if (!s) { fputs("-", stdout); return; }
    if (len == 0) { fputs("~", stdout); return; }   /* 命中但长度 0 */
    for (i = 0; i < len; ++i) printf("%04x", (unsigned)s[i]);
}

int main(void)
{
    char line[131072];
    wchar_t pat[PROBE_MAX], txt[PROBE_MAX];
    long idx = 0;

    while (fgets(line, (int)sizeof(line), stdin)) {
        QpRegexSpan sp[QP_REGEX_MAX_GROUPS];
        int hit, g;
        parse_hex_line(line, pat, PROBE_MAX);
        if (!fgets(line, (int)sizeof(line), stdin)) break;
        parse_hex_line(line, txt, PROBE_MAX);

        hit = qp_regex_search(pat, txt, sp, QP_REGEX_MAX_GROUPS);
        printf("%ld\t%d", idx, hit ? 1 : 0);
        for (g = 0; g < PROBE_GROUPS; ++g) {
            putchar('\t');
            if (hit && g < QP_REGEX_MAX_GROUPS && sp[g].begin)
                print_hex(sp[g].begin, sp[g].len);
            else
                fputs("-", stdout);
        }
        putchar('\n');
        ++idx;
    }
    return 0;
}
