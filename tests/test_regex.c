/*
 * test_regex.c —— 内置正则引擎自测（纯 C，本机 gcc 即可运行）
 */
#include "regex.h"

#include <locale.h>
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

static void check(int cond, const char *what)
{
    if (cond) { ++g_pass; printf("  [ok]   %s\n", what); }
    else      { ++g_fail; printf("  [FAIL] %s\n", what); }
}

/* 匹配成功且整体命中 == want（want 为空则要求不命中） */
static void expect(const wchar_t *pat, const wchar_t *text, const wchar_t *want,
                   const char *what)
{
    QpRegexSpan sp[QP_REGEX_MAX_GROUPS];
    int hit = qp_regex_search(pat, text, sp, QP_REGEX_MAX_GROUPS);
    wchar_t got[256];
    if (hit && sp[0].begin) {
        size_t n = sp[0].len < 255 ? sp[0].len : 255;
        wmemcpy(got, sp[0].begin, n); got[n] = L'\0';
    } else {
        got[0] = L'\0';
    }
    if (want && *want) {
        if (hit && wcscmp(got, want) == 0) check(1, what);
        else {
            ++g_fail;
            printf("  [FAIL] %s\n         期望命中: %ls\n         实际: %s (%ls)\n",
                   what, want, hit ? "命中" : "未命中", got);
        }
    } else {
        if (!hit) check(1, what);
        else {
            ++g_fail;
            printf("  [FAIL] %s\n         期望不命中，实际命中: %ls\n", what, got);
        }
    }
}

/* 校验第 g 组内容 */
static void expect_group(const wchar_t *pat, const wchar_t *text, int g,
                         const wchar_t *want, const char *what)
{
    QpRegexSpan sp[QP_REGEX_MAX_GROUPS];
    int hit = qp_regex_search(pat, text, sp, QP_REGEX_MAX_GROUPS);
    wchar_t got[256];
    if (hit && g < QP_REGEX_MAX_GROUPS && sp[g].begin) {
        size_t n = sp[g].len < 255 ? sp[g].len : 255;
        wmemcpy(got, sp[g].begin, n); got[n] = L'\0';
    } else {
        got[0] = L'\0';
    }
    if (want && hit && wcscmp(got, want) == 0) check(1, what);
    else if (!want && (!hit || !sp[g].begin)) check(1, what);
    else {
        ++g_fail;
        printf("  [FAIL] %s\n         期望组%d=%ls 实际=%s%ls\n",
               what, g, want ? want : L"(未参与)", hit ? "" : "(未命中)", got);
    }
}

static void test_valid(void)
{
    printf("\n== 语法校验 ==\n");
    check(qp_regex_valid(L"abc", NULL) == 1, "字面量合法");
    check(qp_regex_valid(L"a(b|c)*", NULL) == 1, "组合合法");
    check(qp_regex_valid(L"a\\d+", NULL) == 1, "转义合法");
    check(qp_regex_valid(L"(abc", NULL) == 0, "缺右括号非法");
    check(qp_regex_valid(L"a[", NULL) == 0, "缺右方括号非法");
    check(qp_regex_valid(L"a)", NULL) == 0, "多余的右括号非法");
    check(qp_regex_valid(L"a{2,1}", NULL) == 1, "{2,1} 当作字面量而非错误");
}

static void test_literal_any(void)
{
    printf("\n== 字面量与 . ==\n");
    expect(L"abc", L"xxabcxx", L"abc", "字面量命中");
    expect(L"abc", L"abd", L"", "字面量不命中");
    expect(L"a.c", L"xxaXc", L"aXc", "点号匹配任意字符");
    expect(L"a.c", L"ac", L"", "点号需要至少一个字符");
    expect(L"abc", L"ABC", L"ABC", "字面量大小写不敏感");
}

static void test_class(void)
{
    printf("\n== 字符类 ==\n");
    expect(L"[abc]+", L"xxcabxx", L"cab", "字符类命中");
    expect(L"[^abc]+", L"xxabcdef", L"xx", "取反字符类命中");
    expect(L"[a-z]+", L"123abcXYZ", L"abcXYZ", "范围类大小写不敏感（含大写）");
    expect(L"[a-z]+", L"123ABC", L"ABC", "范围类大小写不敏感（含小写）");
    expect(L"[0-9]{4}", L"ab2026cd", L"2026", "范围+量词命中");
    expect(L"[^0-9]+", L"123abc", L"abc", "取反范围命中");
    expect(L"[\\]a]", L"x]y", L"]", "类内 ] 作首元素");
    expect(L"[-a]", L"x-y", L"-", "类内 - 作字面量");
    expect(L"[]a]", L"x]y", L"]", "空类首字符 ] 字面量");
}

static void test_predef(void)
{
    printf("\n== 预定义类 ==\n");
    expect(L"\\d+", L"编号123号", L"123", "\\d 命中数字");
    expect(L"\\d+", L"编号", L"", "\\d 无数字不命中");
    expect(L"\\D+", L"abc123", L"abc", "\\D 命中非数字");
    expect(L"\\w+", L"  hello_世界  ", L"hello_世界", "\\w 命中单词字符");
    expect(L"\\s+", L"a\t b", L"\t ", "\\s 命中空白");
    expect(L"[\\d]+", L"v3.1", L"3", "类内 \\d");
    expect(L"[^\\d]+", L"v3.1", L"v", "类内取反 \\d");
}

static void test_anchor(void)
{
    printf("\n== 锚点 ==\n");
    expect(L"^abc", L"abcdef", L"abc", "^ 开头命中");
    expect(L"^abc", L"xabcdef", L"", "^ 非开头不命中");
    expect(L"abc$", L"xxabc", L"abc", "$ 结尾命中");
    expect(L"abc$", L"abcxx", L"", "$ 非结尾不命中");
    expect(L"^abc$", L"abc", L"abc", "^...$ 整串相等");
    expect(L"^abc$", L"xabc", L"", "^...$ 不整串则不命中");
}

static void test_quant(void)
{
    printf("\n== 量词 ==\n");
    expect(L"ab*c", L"ac", L"ac", "* 零次");
    expect(L"ab*c", L"abbbc", L"abbbc", "* 多次");
    expect(L"ab+c", L"ac", L"", "+ 需要至少一次");
    expect(L"ab+c", L"abbc", L"abbc", "+ 多次");
    expect(L"ab?c", L"ac", L"ac", "? 零次");
    expect(L"ab?c", L"abc", L"abc", "? 一次");
    expect(L"a{3}", L"aaaa", L"aaa", "{m} 恰好");
    expect(L"a{2,4}", L"aaaaa", L"aaaa", "{m,n} 贪婪取上限");
    expect(L"a{2,}", L"aaaa", L"aaaa", "{m,} 无限贪婪");
    expect(L"a{2,4}?", L"aaaaa", L"aa", "{m,n}? 懒惰取下限");
    expect(L"<.+>", L"<a><b>", L"<a><b>", "贪婪 .+ 吃满");
    expect(L"<.+?>", L"<a><b>", L"<a>", "懒惰 .+? 最少");
}

static void test_alt_group(void)
{
    printf("\n== 分支与捕获组 ==\n");
    expect(L"cat|dog", L"xxdogxx", L"dog", "分支命中后者");
    expect(L"cat|dog", L"cat", L"cat", "分支命中前者");
    expect(L"(\\d+)-(\\w+)", L"id=42-x7", L"42-x7", "捕获组整体");
    expect_group(L"(\\d+)-(\\w+)", L"id=42-x7", 1, L"42", "捕获组1");
    expect_group(L"(\\d+)-(\\w+)", L"id=42-x7", 2, L"x7", "捕获组2");
    expect(L"(?:\\d+)", L"n=123", L"123", "非捕获组整体");
    expect_group(L"(a|(b))", L"b", 2, L"b", "嵌套捕获组");
}

static void test_backref(void)
{
    printf("\n== 反向引用 ==\n");
    expect(L"(\\w+)@\\1", L"xx ab@ab yy", L"ab@ab", "反向引用命中");
    expect(L"(\\w+)@\\1", L"ab@cd", L"", "反向引用不命中");
    expect(L"<(\\w+)>.*</\\1>", L"<b>hi</b>", L"<b>hi</b>", "成对标签");
    expect(L"<(\\w+)>.*</\\1>", L"<b>hi</i>", L"", "标签不匹配");
}

static void test_search_semantics(void)
{
    QpRegexSpan sp[QP_REGEX_MAX_GROUPS];
    printf("\n== 搜索语义 ==\n");
    expect(L"\\d+", L"2026-08-18 09:05", L"2026", "左起第一个匹配");
    check(qp_regex_search(L"", L"anything", sp, QP_REGEX_MAX_GROUPS) == 1 &&
          sp[0].begin != NULL && sp[0].len == 0,
          "空模式匹配空串（长度为 0）");
    expect(L"存储(服务器|设备)", L"经核查由于存储服务器一硬盘",
           L"存储服务器", "中文分组命中");
    expect(L"第(\\d+)段", L"取第8段", L"第8段", "中文+捕获整体");
    expect_group(L"第(\\d+)段", L"取第8段", 1, L"8", "中文捕获组");
}

static void test_edge_cases(void)
{
    QpRegexSpan sp[QP_REGEX_MAX_GROUPS];
    printf("\n== 边界与回溯 ==\n");
    expect(L"(a|b)+", L"abab", L"abab", "分组+量词重复");
    expect_group(L"(a+)(b+)", L"aaabbb", 1, L"aaa", "贪婪捕获组 1");
    expect_group(L"(a+)(b+)", L"aaabbb", 2, L"bbb", "贪婪捕获组 2");
    expect(L"(a*)*", L"aaa", L"aaa", "零宽内层不会死循环");
    expect(L"(a?)*b", L"aab", L"aab", "可空量词后仍能匹配后续");
    expect(L"(ab){2}", L"ababab", L"abab", "分组整体量词");
    expect(L"a{2,3}", L"aaaa", L"aaa", "分组外的 {m,n} 贪婪");
    check(qp_regex_search(L"x*", L"yyy", sp, QP_REGEX_MAX_GROUPS) == 1 &&
          sp[0].len == 0, "x* 在全不匹配的串上命中零宽");
    expect(L"[^a-z]+", L"abc123", L"123", "取反类吃掉非字母");
    expect(L"a|ab", L"ab", L"a", "分支取左起第一个（a 先于 ab）");
    check(qp_regex_search(L"(a|)", L"b", sp, QP_REGEX_MAX_GROUPS) == 1 &&
          sp[0].len == 0, "分支可含空分支并命中零宽");
    expect(L"((((a))))", L"xa", L"a", "多层嵌套整体");
    expect_group(L"((((a))))", L"xa", 4, L"a", "多层嵌套最内层捕获");
    expect(L"(?:\\d{2}:){2}\\d{2}", L"t 09:05:07 x", L"09:05:07", "非捕获组重复+花括号");
    /* \w 不含点，所以 \w+ 只能从 b 起匹配 —— 这是正确行为 */
    expect(L"\\w+@\\w+\\.(com|cn)", L"mailto a.b@x.cn!", L"b@x.cn", "邮箱式组合");
    expect(L"[\\w.]+@[\\w.]+", L"mailto a.b@x.cn!", L"a.b@x.cn", "带点的用户名字符类");

    /* 灾难性回溯：不能挂住，宁可判不命中 */
    expect(L"(a+)+b", L"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaac", L"", "病态回溯有预算兜底");

    /* 量词套在可空分组上：最容易写错的一类，必须能正确回退到「少循环几次」 */
    expect(L"(A*)*A", L"A", L"A", "(X*)*Y 需要回退到 0 次循环");
    expect(L"(A*){0,}A", L"A", L"A", "(X*){0,}Y 需要回退到 0 次循环");
    expect(L"(A?)*A", L"A", L"A", "(X?)*Y 需要回退");
    expect(L"(A|)*A", L"A", L"A", "空分支分组加量词需要回退");
    expect(L"(A*)+A", L"A", L"A", "(X*)+Y 需要回退");
    expect(L"(ab*)*c", L"ababc", L"ababc", "可空嵌套量词的正常贪婪路径");

    /* 字符类的大小写折叠：区间里是大写时，小写输入也要认 */
    expect(L"[@-_]", L"b", L"b", "[@-_] 认得小写 b（区间含 A-Z）");
    expect(L"[@-_]+", L"ab", L"ab", "[@-_]+ 连续命中大小写");
    expect(L"[A-Z]+", L"abc", L"abc", "[A-Z]+ 认得小写");
    expect(L"[^@-_]+", L"ab", L"", "[^@-_]+ 不认大小写字母");
}

int main(void)
{
    setlocale(LC_ALL, "");
    printf("== 正则引擎自测 ==\n");
    test_valid();
    test_literal_any();
    test_class();
    test_predef();
    test_anchor();
    test_quant();
    test_alt_group();
    test_backref();
    test_search_semantics();
    test_edge_cases();
    printf("\n=========================\n通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
