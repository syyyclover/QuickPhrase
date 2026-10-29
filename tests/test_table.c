/*
 * test_table.c —— 数据表（名单 / 联系表）自测
 *
 * 覆盖三件事，任何一件出问题都会让「查表」在真实环境里出错：
 *   ① 读表：CSV/TSV 解析（引号、内嵌分隔符与换行、BOM、CRLF、不规则行）、
 *      编码识别、分隔符识别；
 *   ② 查表：键的规范化（大小写 / 空白 / 全角空格 / 数值写法）、键不唯一时的
 *      取第一条 / 最后一条 / 全部拼接、空键与空值的语义；
 *   ③ 规模：数万行下建表与查询的耗时（防止悄悄退化成 O(n²)）。
 *
 * 纯 C，不依赖 Windows API；`make test` 在本机直接跑。
 */
#include "table.h"
#include "storage.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

#ifdef _WIN32
#  include <direct.h>
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <fcntl.h>
#endif

static int g_pass = 0, g_fail = 0;

static void check(int cond, const char *what)
{
    if (cond) { ++g_pass; printf("  [ok]   %s\n", what); }
    else      { ++g_fail; printf("  [FAIL] %s\n", what); }
}

static void check_int(int got, int want, const char *what)
{
    if (got == want) { ++g_pass; printf("  [ok]   %s\n", what); }
    else {
        ++g_fail;
        printf("  [FAIL] %s\n         期望: %d\n         实际: %d\n", what, want, got);
    }
}

static void check_str(const wchar_t *got, const wchar_t *want, const char *what)
{
    if (got && want && wcscmp(got, want) == 0) {
        ++g_pass;
        printf("  [ok]   %s\n", what);
    } else {
        ++g_fail;
        printf("  [FAIL] %s\n         期望: 「%ls」\n         实际: 「%ls」\n",
               what, want ? want : L"(null)", got ? got : L"(null)");
    }
}

/* ------------------------------------------------------------------ */
/* 临时文件                                                            */
/* ------------------------------------------------------------------ */

static wchar_t g_dir[QP_PATH_MAX];
static wchar_t g_path[QP_PATH_MAX];      /* 每次测试都覆盖这同一个文件 */
static DataTableRef *g_refs;

static void make_dir(const wchar_t *p)
{
#ifdef _WIN32
    _wmkdir(p);
#else
    char *u8 = qp_wide_to_utf8_alloc(p);
    if (u8) { mkdir(u8, 0700); free(u8); }
#endif
}

static void init_paths(void)
{
#ifdef _WIN32
    const wchar_t *tmp = _wgetenv(L"TEMP");
    if (!tmp || !*tmp) tmp = L".";
    swprintf(g_dir, QP_PATH_MAX, L"%ls\\qp_table_test", tmp);
#else
    qp_strlcpy(g_dir, L"/tmp/qp_table_test", QP_PATH_MAX);
#endif
    make_dir(g_dir);
    swprintf(g_path, QP_PATH_MAX, L"%ls/case.csv", g_dir);
    g_path[QP_PATH_MAX - 1] = L'\0';
}

static int write_bytes(const wchar_t *path, const void *data, size_t n)
{
    FILE *f = NULL;
#ifdef _WIN32
    f = _wfopen(path, L"wb");
#else
    {
        char *u8 = qp_wide_to_utf8_alloc(path);
        if (u8) { f = fopen(u8, "wb"); free(u8); }
    }
#endif
    if (!f) return 0;
    if (n && data) fwrite(data, 1, n, f);
    fclose(f);
    return 1;
}

static int write_utf8(const wchar_t *text)
{
    char *u8 = qp_wide_to_utf8_alloc(text);
    int ok;
    if (!u8) return 0;
    ok = write_bytes(g_path, u8, strlen(u8));
    free(u8);
    return ok;
}

/* 按 UTF-16LE 落盘（可选 BOM），用来验证 UTF-16 识别 */
static int write_utf16le(const wchar_t *text, int bom)
{
    size_t n = wcslen(text), i;
    unsigned char *b = (unsigned char *)malloc(n * 2 + 2);
    if (!b) return 0;
    for (i = 0; i < n; ++i) {
        b[i * 2] = (unsigned char)(text[i] & 0xFF);
        b[i * 2 + 1] = (unsigned char)((text[i] >> 8) & 0xFF);
    }
    if (bom) {
        unsigned char *b2 = (unsigned char *)malloc(n * 2 + 4);
        int ok;
        if (!b2) { free(b); return 0; }
        b2[0] = 0xFF; b2[1] = 0xFE;
        memcpy(b2 + 2, b, n * 2);
        ok = write_bytes(g_path, b2, n * 2 + 2);
        free(b2);
        free(b);
        return ok;
    }
    {
        int ok = write_bytes(g_path, b, n * 2);
        free(b);
        return ok;
    }
}

/* 用给定内容重建一张表：每次都换掉 ref 与缓存，保证读到的是新内容 */
static QpTable *build_from(const wchar_t *csv, int enc, int delim, int has_header)
{
    wchar_t err[256];
    QpTable *t;

    qp_table_set_refs(NULL);
    qp_table_invalidate_all();
    if (g_refs) { dtref_list_free(g_refs); g_refs = NULL; }
    if (!write_utf8(csv)) { printf("  ... 写临时文件失败\n"); return NULL; }
    g_refs = dtref_create(L"测试表", g_path);
    if (!g_refs) return NULL;
    g_refs->encoding = enc;
    g_refs->delimiter = delim;
    g_refs->has_header = has_header;
    qp_table_set_refs(g_refs);

    err[0] = L'\0';
    t = qp_table_get(g_refs->id, err, 256);
    if (!t) printf("  ... 读表失败: %ls\n", err[0] ? err : L"(无错误信息)");
    return t;
}

static void teardown(void)
{
    char *u8;
    qp_table_set_refs(NULL);
    qp_table_invalidate_all();
    if (g_refs) { dtref_list_free(g_refs); g_refs = NULL; }
    u8 = qp_wide_to_utf8_alloc(g_path);
    if (u8) { remove(u8); free(u8); }
    u8 = qp_wide_to_utf8_alloc(g_dir);
    if (u8) { remove(u8); free(u8); }
}

/* ================================================================== */
/* ① 读表                                                              */
/* ================================================================== */

static const wchar_t *PHONE =
    L"设备编号,联系人,电话,部门\n"
    L"A001,张三,13800138000,运维\n"
    L"a002,李四,13900139000,运维\n"
    L" A003 ,王五,13700137000,网络\n"
    L"1.3800138E+10,科学,100,测试\n"
    L"13800138001,数值,200,测试\n"
    L"A001,赵六,13600136000,运维\n"
    L"  ,空键,300,测试\n"
    L"B001,,400,测试\n";

static void test_basic(void)
{
    QpTable *t = build_from(PHONE, QP_ENC_AUTO, QP_DELIM_AUTO, 1);

    printf("\n== 基本读表 ==\n");
    check(t != NULL, "普通 CSV 能读出来");
    if (!t) return;
    check_int(qp_table_rows(t), 8, "数据行数（不含表头）");
    check_int(qp_table_cols(t), 4, "列数");
    check_int(qp_table_has_header(t), 1, "认出了表头");
    check_str(qp_table_col_name(t, 0), L"设备编号", "第 1 列列名");
    check_str(qp_table_col_name(t, 3), L"部门", "第 4 列列名");
    check(qp_table_col_name(t, 9) == NULL, "列名越界返回 NULL");
    check_str(qp_table_cell(t, 0, 0), L"A001", "第 1 行第 1 列");
    check_str(qp_table_cell(t, 0, 1), L"张三", "第 1 行第 2 列");
    check_str(qp_table_cell(t, 6, 1), L"空键", "带空键的那一行取得到");
    check_str(qp_table_cell(t, 99, 0), L"", "行号越界返回空串");
    check_str(qp_table_cell(t, 0, 99), L"", "列号越界返回空串");
}

static void test_quotes(void)
{
    QpTable *t = build_from(
        L"编号,备注\n"
        L"A1,\"含,逗号\"\n"
        L"A2,\"含\n换行\"\n"
        L"A3,\"他说\"\"你好\"\"\"\n"
        L"A4,\"只有引号\"\"\"\n"
        L"A5,\"尾巴\"x\n", QP_ENC_AUTO, QP_DELIM_AUTO, 1);

    printf("\n== 引号与特殊字符 ==\n");
    check(t != NULL, "带引号的 CSV 能读出来");
    if (!t) return;
    check_int(qp_table_rows(t), 5, "内嵌换行的字段没有把行数算错");
    check_int(qp_table_cols(t), 2, "引号里的逗号没有多切出列");
    check_str(qp_table_cell(t, 0, 0), L"A1", "第 1 行键");
    check_str(qp_table_cell(t, 0, 1), L"含,逗号", "引号里的逗号原样保留");
    check_str(qp_table_cell(t, 1, 1), L"含\n换行", "引号里的换行原样保留");
    check_str(qp_table_cell(t, 2, 1), L"他说\"你好\"", "两个双引号 = 一个字面引号");
    check_str(qp_table_cell(t, 3, 1), L"只有引号\"", "字段就是一个引号");
    check_str(qp_table_cell(t, 4, 1), L"尾巴x", "引号后的尾巴宽容并进来");
}

static void test_lines(void)
{
    QpTable *t;
    wchar_t err[256];
    QpTablePreview *pv;

    printf("\n== 行尾与不规则行 ==\n");
    t = build_from(L"a,b\r\n1,2\r\n", QP_ENC_AUTO, QP_DELIM_AUTO, 1);
    check(t != NULL, "CRLF 行尾能读");
    if (t) {
        check_int(qp_table_rows(t), 1, "CRLF 下数据行数正确");
        check_str(qp_table_cell(t, 0, 1), L"2", "CRLF 下单元格没带上 \\r");
    }

    t = build_from(L"a,b\n1,2", QP_ENC_AUTO, QP_DELIM_AUTO, 1);
    check(t != NULL, "末行没有换行也能读");
    if (t) check_int(qp_table_rows(t), 1, "末行没有换行时行数正确");

    t = build_from(L"a,b\n1,2\n\n\n", QP_ENC_AUTO, QP_DELIM_AUTO, 1);
    check(t != NULL, "文件末尾的空行被忽略（不报错）");
    if (t) check_int(qp_table_rows(t), 1, "末尾空行不算数据行");

    t = build_from(L"a,b,c\n1\n2,3\n", QP_ENC_AUTO, QP_DELIM_AUTO, 1);
    check(t != NULL, "有的行少几列也能读");
    if (t) {
        check_int(qp_table_cols(t), 3, "列数取最长的一行");
        check_str(qp_table_cell(t, 0, 0), L"1", "短行的第 1 列");
        check_str(qp_table_cell(t, 0, 2), L"", "短行缺的列是空");
        check_str(qp_table_cell(t, 1, 1), L"3", "短行后面的行不受影响");
    }

    t = build_from(L"a,b\n1,2,3\n", QP_ENC_AUTO, QP_DELIM_AUTO, 1);
    check(t != NULL, "有的行多几列也能读");
    if (t) {
        check_int(qp_table_cols(t), 3, "多出来的列也算一列");
        check_str(qp_table_cell(t, 0, 2), L"3", "多出来的列取得到");
    }

    /* 只有一行：没有表头可言，这一行就是数据 */
    t = build_from(L"只有一行,没有表头\n", QP_ENC_AUTO, QP_DELIM_AUTO, 1);
    check(t != NULL, "只有一行的文件能读");
    if (t) {
        check_int(qp_table_has_header(t), 0, "只有一行时不把这一行当表头");
        check_int(qp_table_rows(t), 1, "只有一行时它就是 1 条数据");
        check_str(qp_table_cell(t, 0, 0), L"只有一行", "这一行的内容取得到");
    }

    /* 空文件：明确报错，不能悄悄变成「0 行」让规则永远不命中还说不出原因 */
    err[0] = L'\0';
    write_utf8(L"");
    qp_table_set_refs(NULL);
    qp_table_invalidate_all();
    if (g_refs) { dtref_list_free(g_refs); g_refs = NULL; }
    g_refs = dtref_create(L"空表", g_path);
    qp_table_set_refs(g_refs);
    check(qp_table_get(g_refs->id, err, 256) == NULL, "空文件读不出来（返回 NULL）");
    check(err[0] != L'\0', "空文件给出了错误说明");

    /* 文件不存在 */
    err[0] = L'\0';
    qp_table_set_refs(NULL);
    qp_table_invalidate_all();
    if (g_refs) { dtref_list_free(g_refs); g_refs = NULL; }
    g_refs = dtref_create(L"没这文件", L"/tmp/qp_table_test/绝对不存在的文件.csv");
    qp_table_set_refs(g_refs);
    check(qp_table_get(g_refs->id, err, 256) == NULL, "文件不存在时返回 NULL");
    check(err[0] != L'\0', "文件不存在时给出错误说明");

    /* 导入预览：能读到行数、列名，并自动识别参数 */
    pv = qp_table_preview(g_path, NULL, 5, err, 256);
    check(pv == NULL, "预览一个不存在的文件也返回 NULL");

    write_utf8(PHONE);
    pv = qp_table_preview(g_path, NULL, 3, err, 256);
    check(pv != NULL, "预览读得通");
    if (pv) {
        check_int(pv->total_rows, 8, "预览报出的总行数");
        check_int(pv->cols, 4, "预览报出的列数");
        check_int(pv->rows, 3, "预览只取前 3 行");
        check_int(pv->has_header, 1, "预览认出表头");
        check_int(pv->encoding, QP_ENC_UTF8, "预览识别出 UTF-8");
        check_int(pv->delimiter, QP_DELIM_COMMA, "预览识别出逗号分隔");
        check_str(pv->col_name[1], L"联系人", "预览的列名");
        check_str(pv->preview[1 * 4 + 1], L"李四", "预览第 2 行第 2 列");
        check_int(pv->col_dup[0], 1, "列名不重名时计数为 1");
        qp_table_preview_free(pv);
    }
}

static void test_encoding(void)
{
    static const char utf8_bom[] = "\xEF\xBB\xBF设备编号,姓名\nA1,张三\n";
    wchar_t err[256];
    QpTable *t;
    QpTablePreview *pv;

    printf("\n== 编码 ==\n");
    t = build_from(L"编号,姓名\nA1,张三\n", QP_ENC_AUTO, QP_DELIM_AUTO, 1);
    check(t != NULL, "UTF-8（无 BOM）能读");
    if (t) check_str(qp_table_cell(t, 0, 1), L"张三", "UTF-8 中文正确");

    /* 带 BOM 的 UTF-8：BOM 不能混进第一列的列名 */
    qp_table_set_refs(NULL);
    qp_table_invalidate_all();
    if (g_refs) { dtref_list_free(g_refs); g_refs = NULL; }
    write_bytes(g_path, utf8_bom, sizeof(utf8_bom) - 1);
    g_refs = dtref_create(L"BOM 表", g_path);
    qp_table_set_refs(g_refs);
    t = qp_table_get(g_refs->id, err, 256);
    check(t != NULL, "带 BOM 的 UTF-8 能读");
    if (t) {
        check_int(qp_table_rows(t), 1, "带 BOM 时行数正确");
        check_str(qp_table_col_name(t, 0), L"设备编号", "BOM 没有混进列名");
        check_int(qp_table_col_index(t, L"设备编号", -1), 0, "按列名找得到（说明 BOM 已去掉）");
    }
    pv = qp_table_preview(g_path, NULL, 5, err, 256);
    if (pv) {
        check_int(pv->encoding, QP_ENC_UTF8, "BOM 表识别为 UTF-8");
        qp_table_preview_free(pv);
    }

    /* UTF-16LE + BOM（Windows 记事本「Unicode」另存） */
    qp_table_set_refs(NULL);
    qp_table_invalidate_all();
    if (g_refs) { dtref_list_free(g_refs); g_refs = NULL; }
    write_utf16le(L"编号,姓名\nA1,张三\n", 1);
    g_refs = dtref_create(L"UTF16 表", g_path);
    qp_table_set_refs(g_refs);
    t = qp_table_get(g_refs->id, err, 256);
    check(t != NULL, "UTF-16LE 能读");
    if (t) {
        check_int(qp_table_rows(t), 1, "UTF-16 行数正确");
        check_str(qp_table_col_name(t, 0), L"编号", "UTF-16 列名正确");
        check_str(qp_table_cell(t, 0, 1), L"张三", "UTF-16 中文正确");
    }
    pv = qp_table_preview(g_path, NULL, 5, err, 256);
    if (pv) {
        check_int(pv->encoding, QP_ENC_UTF16, "识别为 UTF-16");
        qp_table_preview_free(pv);
    }

#ifndef _WIN32
    /* 本机（Linux）没有 GBK 码表：必须明确告诉用户怎么办，而不是乱码或静默失败 */
    {
        static const char gbk[] = "\xC9\xE8\xB1\xB8\xB1\xE0\xBA\xC5,\xD0\xD5\xC3\xFB\n"
                                  "A1,\xD5\xC5\xC8\xFD\n";
        qp_table_set_refs(NULL);
        qp_table_invalidate_all();
        if (g_refs) { dtref_list_free(g_refs); g_refs = NULL; }
        write_bytes(g_path, gbk, sizeof(gbk) - 1);
        g_refs = dtref_create(L"GBK 表", g_path);
        qp_table_set_refs(g_refs);
        err[0] = L'\0';
        t = qp_table_get(g_refs->id, err, 256);
        check(t == NULL, "本机读不了 GBK（识别出来了但没码表）");
        check(err[0] != L'\0', "给出了「改用 UTF-8」这类可操作的提示");
        printf("         （提示原文：%ls）\n", err);
    }
#endif
}

static void test_delim(void)
{
    QpTable *t;

    printf("\n== 分隔符识别 ==\n");
    t = build_from(L"设备编号\t姓名\nA1\t张三\n", QP_ENC_AUTO, QP_DELIM_AUTO, 1);
    check(t != NULL, "制表符分隔能读");
    if (t) {
        check_int(qp_table_cols(t), 2, "制表符自动识别为 2 列");
        check_str(qp_table_cell(t, 0, 1), L"张三", "制表符表的内容正确");
    }

    t = build_from(L"设备编号;姓名\nA1;张三\n", QP_ENC_AUTO, QP_DELIM_AUTO, 1);
    check(t != NULL, "分号分隔能读");
    if (t) check_int(qp_table_cols(t), 2, "分号自动识别为 2 列");

    t = build_from(L"设备编号,姓名\nA1,张三\n", QP_ENC_AUTO, QP_DELIM_TAB, 1);
    check(t != NULL, "显式指定错的分隔符也不崩");
    if (t) check_int(qp_table_cols(t), 1, "显式指定制表符时整行就是 1 列");

    t = build_from(L"a,\"x,y,z\"\n1,2\n", QP_ENC_AUTO, QP_DELIM_AUTO, 1);
    check(t != NULL, "引号里的逗号不影响识别");
    if (t) {
        check_int(qp_table_cols(t), 2, "识别成 2 列（引号里的逗号不算）");
        check_str(qp_table_col_name(t, 1), L"x,y,z", "表头里的引号内容正确");
        check_str(qp_table_cell(t, 0, 1), L"2", "第二行第 2 列");
    }

    t = build_from(L"a\tb\tc\n1\t2\t3\n", QP_ENC_UTF8, QP_DELIM_AUTO, 0);
    check(t != NULL, "没有表头也能读");
    if (t) {
        check_int(qp_table_has_header(t), 0, "按设置当成没有表头");
        check_int(qp_table_rows(t), 2, "没有表头时第一行也算数据");
        check_int(qp_table_cols(t), 3, "3 列");
        check_str(qp_table_cell(t, 0, 2), L"c", "第一行第 3 列");
    }
}

/* ================================================================== */
/* ② 查表                                                              */
/* ================================================================== */

static wchar_t g_out[QP_MATCH_MAX];

static void test_match(void)
{
    QpTable *t = build_from(PHONE, QP_ENC_AUTO, QP_DELIM_AUTO, 1);

    printf("\n== 键的匹配 ==\n");
    if (!t) { check(0, "表没读出来，后续匹配用例无法验证"); return; }

    check_int(qp_table_lookup(t, 0, L"A001", 1, LOOKUP_FIRST, NULL, 0, g_out, QP_MATCH_MAX), 1,
              "键 A001 命中");
    check_str(g_out, L"张三", "取到第一行的联系人");
    check_int(qp_table_lookup(t, 0, L"a001", 1, LOOKUP_FIRST, NULL, 0, g_out, QP_MATCH_MAX), 1,
              "键 a001 也命中（大小写不敏感）");
    check_str(g_out, L"张三", "大小写不同取到同一个人");
    check_int(qp_table_lookup(t, 0, L"A002", 1, LOOKUP_FIRST, NULL, 0, g_out, QP_MATCH_MAX), 1,
              "表里是小写 a002，用大写查也命中");
    check_str(g_out, L"李四", "取到李四");

    check_int(qp_table_lookup(t, 0, L"A003", 1, LOOKUP_FIRST, NULL, 0, g_out, QP_MATCH_MAX), 1,
              "表里的键带空格，用干净的键能命中");
    check_str(g_out, L"王五", "取到王五");
    check_int(qp_table_lookup(t, 0, L"  A003  ", 1, LOOKUP_FIRST, NULL, 0, g_out, QP_MATCH_MAX), 1,
              "键自己带空格也命中");
    check_str(g_out, L"王五", "两边空格都去掉后相同");
    check_int(qp_table_lookup(t, 0, L"\u3000A003\u3000", 1, LOOKUP_FIRST, NULL, 0,
                              g_out, QP_MATCH_MAX), 1,
              "全角空格也当空白去掉");
    check_str(g_out, L"王五", "全角空格不影响匹配");
    check_int(qp_table_lookup(t, 0, L"\u00A0A003", 1, LOOKUP_FIRST, NULL, 0,
                              g_out, QP_MATCH_MAX), 1,
              "不换行空格（Excel 里常见）也当空白去掉");

    /* 电话在表里是科学计数法，手输纯数字也要对得上 */
    check_int(qp_table_lookup(t, 0, L"13800138000", 1, LOOKUP_FIRST, NULL, 0,
                              g_out, QP_MATCH_MAX), 1,
              "表里写 1.3800138E+10，用 13800138000 查得到");
    check_str(g_out, L"科学", "数值规范化后命中同一行");
    check_int(qp_table_lookup(t, 0, L"1.3800138E+10", 1, LOOKUP_FIRST, NULL, 0,
                              g_out, QP_MATCH_MAX), 1,
              "原样照抄表里的写法也能命中");
    check_str(g_out, L"科学", "两种写法结果一致");
    check_int(qp_table_lookup(t, 0, L"13800138000.0", 1, LOOKUP_FIRST, NULL, 0,
                              g_out, QP_MATCH_MAX), 1,
              "多写一个小数 .0 也算同一个键");
    check_int(qp_table_lookup(t, 0, L"1380013800", 1, LOOKUP_FIRST, NULL, 0,
                              g_out, QP_MATCH_MAX), 0,
              "少一位数字不会被数值近似误命中");

    check_int(qp_table_lookup(t, 0, L"没有这个键", 1, LOOKUP_FIRST, NULL, 0,
                              g_out, QP_MATCH_MAX), 0, "键不存在时不命中");
    check_str(g_out, L"", "不命中时输出被清空");

    check_int(qp_table_lookup(t, 0, L"", 1, LOOKUP_FIRST, NULL, 0,
                              g_out, QP_MATCH_MAX), 0, "空键不命中");
    check_int(qp_table_lookup(t, 0, L"   ", 1, LOOKUP_FIRST, NULL, 0,
                              g_out, QP_MATCH_MAX), 0, "只有空白的键不命中");
    check_int(qp_table_count(t, 0, L"  "), 0, "表的空键行没有被当成命中目标");

    /* 命中行但取值列是空的：算命中（预览会把它标出来），不是「没查到」 */
    check_int(qp_table_lookup(t, 0, L"B001", 1, LOOKUP_FIRST, NULL, 0,
                              g_out, QP_MATCH_MAX), 1, "命中了行但值为空，仍算命中");
    check_str(g_out, L"", "命中行的空值输出为空串");

    check_int(qp_table_lookup(t, 0, L"A001", 9, LOOKUP_FIRST, NULL, 0,
                              g_out, QP_MATCH_MAX), 0, "取值列越界时不命中");
    check_int(qp_table_lookup(t, 9, L"A001", 1, LOOKUP_FIRST, NULL, 0,
                              g_out, QP_MATCH_MAX), 0, "键列越界时不命中");
    check_int(qp_table_lookup(t, 0, L"A001", 1, LOOKUP_FIRST, NULL, 0, NULL, 0), 0,
              "输出缓冲为 0 时不崩且返回不命中");
}

static void test_dupes(void)
{
    QpTable *t = build_from(PHONE, QP_ENC_AUTO, QP_DELIM_AUTO, 1);

    printf("\n== 键不唯一 ==\n");
    if (!t) { check(0, "表没读出来，后续用例无法验证"); return; }
    check_int(qp_table_count(t, 0, L"A001"), 2, "A001 在表里有 2 行");
    check_int(qp_table_count(t, 0, L"a002"), 1, "a002 只有 1 行");

    qp_table_lookup(t, 0, L"A001", 1, LOOKUP_FIRST, NULL, 0, g_out, QP_MATCH_MAX);
    check_str(g_out, L"张三", "取第一条 = 文件里靠前的那条");
    qp_table_lookup(t, 0, L"A001", 1, LOOKUP_LAST, NULL, 0, g_out, QP_MATCH_MAX);
    check_str(g_out, L"赵六", "取最后一条 = 文件里靠后的那条");

    qp_table_lookup(t, 0, L"A001", 1, LOOKUP_ALL, NULL, 0, g_out, QP_MATCH_MAX);
    check_str(g_out, L"张三、赵六", "全部取出的默认分隔符是「、」");
    qp_table_lookup(t, 0, L"A001", 1, LOOKUP_ALL, L"|", 0, g_out, QP_MATCH_MAX);
    check_str(g_out, L"张三|赵六", "全部取出可以用自定义分隔符");
    qp_table_lookup(t, 0, L"A001", 1, LOOKUP_ALL, L"", 0, g_out, QP_MATCH_MAX);
    check_str(g_out, L"张三、赵六", "分隔符给空串时回落成「、」");

    qp_table_lookup(t, 0, L"A001", 3, LOOKUP_ALL, NULL, 0, g_out, QP_MATCH_MAX);
    check_str(g_out, L"运维、运维", "不做去重时重复值原样列出来");
    qp_table_lookup(t, 0, L"A001", 3, LOOKUP_ALL, NULL, 1, g_out, QP_MATCH_MAX);
    check_str(g_out, L"运维", "打开「重复值只留一个」后只剩一份");
    check_int(qp_table_lookup(t, 0, L"B001", 3, LOOKUP_ALL, NULL, 0,
                              g_out, QP_MATCH_MAX), 1, "全部取出时空值行也算命中");
    check_str(g_out, L"测试", "空值不占位，取到的是非空的那一列");

    qp_table_lookup(t, 0, L"a002", 1, LOOKUP_LAST, NULL, 0, g_out, QP_MATCH_MAX);
    check_str(g_out, L"李四", "只有一行时「最后一条」就是它自己");
    qp_table_lookup(t, 0, L"a002", 1, LOOKUP_ALL, NULL, 0, g_out, QP_MATCH_MAX);
    check_str(g_out, L"李四", "只有一行时「全部」也不加分隔符");
}

/* 造一张 n 行、键全一样的表，值在 distinct 个取值之间轮转（形如 V0000）。
   键重复上千行是名单的常态（同一个部门、同一个班组），而旧实现只收前 512 行，
   「取第一条」会悄悄取到第 489 行 —— 这一节就是钉住那个行为不再回来。 */
static wchar_t *build_dup_text(int n, int distinct)
{
    size_t cap = (size_t)n * 40 + 64, used = 0;
    wchar_t *s = (wchar_t *)malloc(cap * sizeof(wchar_t));
    int i;
    if (!s) return NULL;
    used += (size_t)swprintf(s + used, cap - used, L"key,val\n");
    for (i = 0; i < n; ++i)
        used += (size_t)swprintf(s + used, cap - used, L"K,V%04d\n", i % distinct);
    return s;
}

static void test_many_dupes(void)
{
    enum { N = 1000, CYCLE = 300, DISTINCT = 40 };
    wchar_t *text;
    QpTable *t;

    printf("\n== 一个键命中上千行 ==\n");

    text = build_dup_text(N, N);
    check(text != NULL, "造得出 1000 行同键的表");
    if (!text) return;
    t = build_from(text, QP_ENC_UTF8, QP_DELIM_COMMA, 1);
    free(text);
    if (!t) { check(0, "1000 行同键表没读出来"); return; }

    check_int(qp_table_rows(t), N, "1000 行都在");
    check_int(qp_table_count(t, 0, L"K"), N, "计数不封顶：命中 1000 行就报 1000");

    check_int(qp_table_lookup(t, 0, L"K", 1, LOOKUP_FIRST, NULL, 0,
                              g_out, QP_MATCH_MAX), 1, "「取第一条」能命中");
    check_str(g_out, L"V0000", "命中上千行时「取第一条」仍是文件里第一条");
    check_int(qp_table_lookup(t, 0, L"K", 1, LOOKUP_LAST, NULL, 0,
                              g_out, QP_MATCH_MAX), 1, "「取最后一条」能命中");
    check_str(g_out, L"V0999", "命中上千行时「取最后一条」仍是文件里最后一条");

    /* 结果装不下时，必须停在完整的一项上，不能写半个值 */
    {
        wchar_t small[64];
        const wchar_t *p;
        int all_ok = 1;

        check_int(qp_table_lookup(t, 0, L"K", 1, LOOKUP_ALL, L",", 0,
                                  small, 64), 1, "结果装不下时仍然算命中");
        check_int((int)wcslen(small), 59, "装不下就停在完整项上（10 项共 59 字符）");
        for (p = small; *p; ) {
            const wchar_t *comma = wcschr(p, L',');
            size_t len = comma ? (size_t)(comma - p) : wcslen(p);
            if (len != 5 || p[0] != L'V' || p[1] < L'0' || p[1] > L'9' ||
                p[2] < L'0' || p[2] > L'9' || p[3] < L'0' || p[3] > L'9' ||
                p[4] < L'0' || p[4] > L'9')
                all_ok = 0;
            if (!comma) break;
            p = comma + 1;
        }
        check(all_ok, "每一项都是完整的 V+4 位数字，没有被截断的半个值");
    }

    /* 去重不能只记住前 32 个值：名单里几十个不同的取值很常见 */
    text = build_dup_text(CYCLE, DISTINCT);
    check(text != NULL, "造得出循环取值的表");
    if (text) {
        wchar_t big[QP_VALUE_MAX];
        QpTable *t2 = build_from(text, QP_ENC_UTF8, QP_DELIM_COMMA, 1);
        const wchar_t *p;
        int items = 1;

        free(text);
        if (!t2) { check(0, "循环取值表没读出来"); return; }
        check_int(qp_table_count(t2, 0, L"K"), CYCLE, "循环取值表命中 300 行");
        check_int(qp_table_lookup(t2, 0, L"K", 1, LOOKUP_ALL, L",", 1,
                                  big, QP_VALUE_MAX), 1, "去重模式下能命中");
        for (p = big; *p; ++p) if (*p == L',') ++items;
        check_int(items, DISTINCT,
                  "40 个不同的值去重后仍是 40 项（没有从第 33 项起失效）");
    }
}

/* 「表里同时有 007 和 7」时不能把两者当成同一个键 */
static void test_numeric_not_overeager(void)
{
    QpTable *t = build_from(L"编号,名称\n007,零零七\n7,七\n0007,四个零\n",
                            QP_ENC_AUTO, QP_DELIM_AUTO, 1);

    printf("\n== 数值归一化不能过头 ==\n");
    check(t != NULL, "带前导零的表能读");
    if (!t) return;
    qp_table_lookup(t, 0, L"007", 1, LOOKUP_FIRST, NULL, 0, g_out, QP_MATCH_MAX);
    check_str(g_out, L"零零七", "查 007 取到 007 那一行");
    qp_table_lookup(t, 0, L"7", 1, LOOKUP_FIRST, NULL, 0, g_out, QP_MATCH_MAX);
    check_str(g_out, L"七", "查 7 取到 7 那一行");
    qp_table_lookup(t, 0, L"0007", 1, LOOKUP_FIRST, NULL, 0, g_out, QP_MATCH_MAX);
    check_str(g_out, L"四个零", "查 0007 取到 0007 那一行");
    check_int(qp_table_lookup(t, 0, L"7.0", 1, LOOKUP_FIRST, NULL, 0,
                              g_out, QP_MATCH_MAX), 1,
              "表里没有 7.0 时才退到数值匹配");
    check_str(g_out, L"零零七", "数值相等的几行按文件顺序取第一条");

    /* 表里只有科学计数法时，纯数字写法要能命中（这是归一化的目的） */
    t = build_from(L"编号,名称\n1.3800138E+10,唯一一行\n", QP_ENC_AUTO, QP_DELIM_AUTO, 1);
    if (t) {
        check_int(qp_table_lookup(t, 0, L"13800138000", 1, LOOKUP_FIRST, NULL, 0,
                                  g_out, QP_MATCH_MAX), 1, "只剩科学计数法时纯数字也能命中");
        check_str(g_out, L"唯一一行", "取到那一行");
    }
}

static void test_cols(void)
{
    QpTable *t = build_from(L"Name,Tel\nA1,123\n", QP_ENC_AUTO, QP_DELIM_AUTO, 1);

    printf("\n== 按列名找列 ==\n");
    if (t) {
        check_int(qp_table_col_index(t, L"Name", -1), 0, "列名精确匹配");
        check_int(qp_table_col_index(t, L"name", -1), 0, "列名大小写不敏感");
        check_int(qp_table_col_index(t, L"  name  ", -1), 0, "列名两边空白忽略");
        check_int(qp_table_col_index(t, L"Tel", -1), 1, "第二列按名找得到");
        check_int(qp_table_col_index(t, L"没有这列", 1), -1,
                  "有表头却找不到列名时宁可返回 -1，也不猜一个列号");
        check_int(qp_table_col_index(t, L"", 1), 1, "列名留空时按列号");
        check_int(qp_table_col_index(t, L"", 99), -1, "列号越界时返回 -1");
    }

    t = build_from(L"A1,123\nA2,456\n", QP_ENC_AUTO, QP_DELIM_AUTO, 0);
    if (t) {
        check_int(qp_table_col_index(t, L"Tel", 1), 1, "没有表头时按存下来的列号");
        check_int(qp_table_col_index(t, L"Tel", 99), -1, "没有表头且列号越界时返回 -1");
        check_int(qp_table_col_index(t, L"", 0), 0, "没有表头时列名留空按列号");
    }
}

/* ================================================================== */
/* ③ 规模                                                              */
/* ================================================================== */

#define BIG_ROWS 50000

static wchar_t *build_big_text(int n)
{
    size_t cap = (size_t)n * 48 + 128, used = 0;
    wchar_t *buf = (wchar_t *)malloc(cap * sizeof(wchar_t));
    int i, w;
    if (!buf) return NULL;
    w = swprintf(buf, cap, L"设备编号,联系人,电话,部门\n");
    if (w < 0) { free(buf); return NULL; }
    used = (size_t)w;
    for (i = 0; i < n; ++i) {
        w = swprintf(buf + used, cap - used, L"K%05d,姓名%d,138%08d,部门%d\n",
                     i, i, i, i % 7);
        if (w < 0) break;
        used += (size_t)w;
    }
    return buf;
}

static void test_scale(void)
{
    wchar_t *text = build_big_text(BIG_ROWS);
    QpTable *t;
    clock_t t0, t1;
    double ms_build, ms_query;
    int i, cap_check = 0, bad = 0;
    wchar_t key[64];

    printf("\n== 数万行 ==\n");
    check(text != NULL, "造得出 5 万行的测试表");
    if (!text) return;
    check(write_utf8(text) == 1, "5 万行写盘");

    t0 = clock();
    {
        wchar_t err[256];
        qp_table_set_refs(NULL);
        qp_table_invalidate_all();
        if (g_refs) { dtref_list_free(g_refs); g_refs = NULL; }
        g_refs = dtref_create(L"大表", g_path);
        qp_table_set_refs(g_refs);
        err[0] = L'\0';
        t = qp_table_get(g_refs->id, err, 256);
        if (!t) printf("  ... 读大表失败: %ls\n", err);
    }
    t1 = clock();
    ms_build = (double)(t1 - t0) * 1000.0 / (double)CLOCKS_PER_SEC;
    check(t != NULL, "5 万行能读出来");
    if (!t) { free(text); return; }
    check_int(qp_table_rows(t), BIG_ROWS, "5 万行一行不多一行不少");
    check_int(qp_table_cols(t), 4, "5 万行表是 4 列");

    /* 抽查：每一行都查得到，且取到的是自己那一行（索引没串行） */
    for (i = 0; i < BIG_ROWS; i += 977) {
        wchar_t want[64];
        swprintf(key, 64, L"K%05d", i);
        swprintf(want, 64, L"姓名%d", i);
        if (qp_table_lookup(t, 0, key, 1, LOOKUP_FIRST, NULL, 0, g_out, QP_MATCH_MAX) != 1 ||
            wcscmp(g_out, want) != 0) {
            ++bad;
            if (bad <= 3) printf("         %ls 期望 %ls 实际 %ls\n", key, want, g_out);
        }
        if (qp_table_count(t, 0, key) == 1) ++cap_check;
    }
    check_int(bad, 0, "抽查每一行都取到自己那一行");
    check(cap_check > 40, "抽查的每个键计数都是 1");

    check_int(qp_table_lookup(t, 0, L"K99999", 1, LOOKUP_FIRST, NULL, 0,
                              g_out, QP_MATCH_MAX), 0, "不存在的键在大表里也不命中");
    check_int(qp_table_lookup(t, 2, L"13800049999", 1, LOOKUP_FIRST, NULL, 0,
                              g_out, QP_MATCH_MAX), 1, "大表里按电话列也查得到");
    check_str(g_out, L"姓名49999", "按电话查到的是最后一行");

    t0 = clock();
    for (i = 0; i < 20000; ++i) {
        swprintf(key, 64, L"K%05d", (i * 7919) % BIG_ROWS);
        qp_table_lookup(t, 0, key, 1, LOOKUP_FIRST, NULL, 0, g_out, QP_MATCH_MAX);
    }
    t1 = clock();
    ms_query = (double)(t1 - t0) * 1000.0 / (double)CLOCKS_PER_SEC;
    printf("         建表（读盘+解析+建索引）%.1f ms；2 万次查询共 %.1f ms（单次 %.2f µs）\n",
           ms_build, ms_query, ms_query * 1000.0 / 20000.0);

    check(ms_build < 3000.0, "5 万行建表在 3 秒以内（本机实测见上一行）");
    check(ms_query * 1000.0 / 20000.0 < 50.0,
          "单次查询平均在 50 µs 以内（远低于「卡顿」的门槛）");

    free(text);
}

/* 文件被覆盖（每周换名单）后要能自动用上新内容 */
static void test_reload(void)
{
    QpTable *t;
    int i, seen = 0;

    printf("\n== 文件被覆盖后自动重读 ==\n");
    t = build_from(L"编号,姓名\nA1,旧名单\n", QP_ENC_AUTO, QP_DELIM_AUTO, 1);
    check(t != NULL, "先读一份旧名单");
    if (!t) return;
    check_int(qp_table_rows(t), 1, "旧名单 1 行");

    write_utf8(L"编号,姓名\nA1,新名单\nA2,新同事\nA3,又一位\n");
    for (i = 0; i < 64; ++i) {
        t = qp_table_get(g_refs->id, NULL, 0);
        if (t && qp_table_rows(t) == 3) { seen = 1; break; }
    }
    check(seen, "覆盖同一个文件后，查询会自动用上新内容");
    if (seen) {
        qp_table_lookup(t, 0, L"A1", 1, LOOKUP_FIRST, NULL, 0, g_out, QP_MATCH_MAX);
        check_str(g_out, L"新名单", "取到的是新名单里的值");
        check_int(qp_table_rows(t), 3, "新名单 3 行");
    }

    /* 显式失效也必须生效（界面上的「重新加载」走这条） */
    check(build_from(L"编号,姓名\nA1,X\n", QP_ENC_AUTO, QP_DELIM_AUTO, 1) != NULL,
          "重建一份表");
    write_utf8(L"编号,姓名\nA1,Y\nA2,Z\n");
    qp_table_invalidate(g_refs->id);
    t = qp_table_get(g_refs->id, NULL, 0);
    check(t && qp_table_rows(t) == 2, "invalidate 之后立刻读到新内容");
}

/* ================================================================== */
/* ④ 引用登记与预览的其他分支                                          */
/* ================================================================== */

static void test_refs(void)
{
    DataTableRef *r;
    wchar_t err[256];

    printf("\n== 表引用登记 ==\n");
    build_from(L"a,b\n1,2\n", QP_ENC_AUTO, QP_DELIM_AUTO, 1);
    check(g_refs != NULL, "登记了一张表");
    if (!g_refs) return;
    r = qp_table_find_ref(g_refs->id);
    check(r == g_refs, "按 id 找得回这张表");
    check_str(qp_table_ref_name(g_refs->id), L"测试表", "取得到表名");
    check(qp_table_find_ref(L"tbl_不存在") == NULL, "不存在的 id 返回 NULL");
    check(qp_table_ref_name(L"tbl_不存在") == NULL, "不存在的 id 取不到表名");

    qp_table_forget_refs(NULL);                 /* 不是同一份清单：不能误伤 */
    check(qp_table_find_ref(g_refs->id) == g_refs, "forget(NULL) 不会误清已登记的表");

    err[0] = L'\0';
    check(qp_table_get(L"", err, 256) == NULL, "id 为空时取不到表");
    check(err[0] != L'\0', "id 为空时给出说明");

    qp_table_forget_refs(g_refs);
    err[0] = L'\0';
    check(qp_table_get(g_refs->id, err, 256) == NULL, "注销引用后取不到表");
    check(err[0] != L'\0', "注销后给出「表不存在」的说明");
    qp_table_set_refs(g_refs);

    check_int(qp_table_probe(g_refs->id, NULL, NULL, err, 256), 1,
              "probe 能问到「表读得通」");
}

static void test_dup_header(void)
{
    QpTablePreview *pv;
    wchar_t err[256];

    printf("\n== 表头重名 ==\n");
    write_utf8(L"编号,编号,备注\nA1,A2,x\n");
    pv = qp_table_preview(g_path, NULL, 5, err, 256);
    check(pv != NULL, "重名表头的表能读");
    if (!pv) return;
    check_int(pv->col_dup[0], 2, "第一个「编号」出现 2 次");
    check_int(pv->col_dup[1], 2, "第二个「编号」也数到 2 次");
    check_int(pv->col_dup[2], 1, "「备注」只出现 1 次");
    qp_table_preview_free(pv);

    /* 没有表头时列名用「列 N」占位 */
    write_utf8(L"a,b\n1,2\n");
    {
        QpTableOpts opts;
        opts.encoding = QP_ENC_AUTO;
        opts.delimiter = QP_DELIM_AUTO;
        opts.has_header = 0;
        pv = qp_table_preview(g_path, &opts, 5, err, 256);
        check(pv != NULL, "按「没有表头」预览");
        if (pv) {
            check_str(pv->col_name[0], L"列 1", "没有表头时第一列叫「列 1」");
            check_str(pv->col_name[1], L"列 2", "没有表头时第二列叫「列 2」");
            check_int(pv->total_rows, 2, "没有表头时两行都是数据");
            check_str(pv->preview[0], L"a", "没有表头时预览第一行是文件第一行");
            qp_table_preview_free(pv);
        }
    }

    check(qp_table_preview(NULL, NULL, 5, err, 256) == NULL, "路径为空时预览失败");
}

static void test_enum_keys(void)
{
    printf("\n== 存档键名 ==\n");
    check(strcmp(qp_table_encoding_key(QP_ENC_UTF8), "utf8") == 0, "编码键名 UTF-8");
    check(strcmp(qp_table_encoding_key(QP_ENC_AUTO), "auto") == 0, "编码键名 自动");
    check(strcmp(qp_table_delim_key(QP_DELIM_TAB), "tab") == 0, "分隔符键名 制表符");
    check(strcmp(qp_table_hit_key(LOOKUP_ALL), "all") == 0, "命中键名 全部");
    check(qp_table_encoding_from_key("gbk") == QP_ENC_GBK, "键名能还原成编码");
    check(qp_table_encoding_from_key("乱写的") == QP_ENC_AUTO, "认不出的编码回落成自动");
    check(qp_table_delim_from_key("semi") == QP_DELIM_SEMI, "键名能还原成分隔符");
    check(qp_table_delim_from_key(NULL) == QP_DELIM_AUTO, "空键名回落成自动");
    check(qp_table_hit_from_key("last") == LOOKUP_LAST, "键名能还原成命中策略");
    check(qp_table_hit_from_key("乱写的") == LOOKUP_FIRST, "认不出的命中策略回落成第一条");
    check(wcscmp(qp_table_hit_name(LOOKUP_ALL), L"全部取出") == 0, "命中策略的中文名");
    check(wcscmp(qp_table_encoding_name(QP_ENC_AUTO), L"自动识别") == 0, "编码的中文名");
    check(wcscmp(qp_table_delim_name(QP_DELIM_SEMI), L"分号") == 0, "分隔符的中文名");
}

static void test_cache_refresh(void)
{
    wchar_t err[256], value[64];
    QpTable *t=build_from(L"key,value\nA,old\n",QP_ENC_UTF8,QP_DELIM_COMMA,1);
    check(t!=NULL,"缓存测试初始加载成功");
    write_utf8(L"key,value\nA,updated-value\n");
    t=qp_table_get(g_refs->id,err,256);
    check(t && qp_table_lookup(t,0,L"A",1,LOOKUP_FIRST,L"",0,value,64),"更新文件后的第一次查询命中");
    check_str(value,L"updated-value","文件更新后第一次查询就使用新值");
    qp_table_invalidate_all();
    qp_strlcpy(g_refs->path,L"missing-quickphrase-cache-test.csv",QP_PATH_MAX);
    check(qp_table_get(g_refs->id,err,256)==NULL,"缺失文件产生缓存错误");
    qp_strlcpy(g_refs->path,g_path,QP_PATH_MAX);
    check(qp_table_get(g_refs->id,err,256)!=NULL,"文件恢复后不被旧错误缓存永久阻塞");
#ifndef _WIN32
    {
        char *path=qp_wide_to_utf8_alloc(g_path);
        struct timespec stamp[2]={{1700000000,111},{1700000000,111}};
        write_utf8(L"key,value\nA,old\n");
        check(utimensat(AT_FDCWD,path,stamp,0)==0,"设置亚秒缓存测试时间戳");
        qp_table_invalidate_all();
        t=qp_table_get(g_refs->id,err,256);
        check(t!=NULL,"固定时间戳数据加载成功");
        write_utf8(L"key,value\nA,new\n");
        stamp[1].tv_nsec=222;
        check(utimensat(AT_FDCWD,path,stamp,0)==0,"同秒内等长度更新时间戳");
        t=qp_table_get(g_refs->id,err,256);
        check(t && qp_table_lookup(t,0,L"A",1,LOOKUP_FIRST,L"",0,value,64),"同秒内更新后命中");
        check_str(value,L"new","等长度文件在同一秒内更新也立即生效");
        free(path);
    }
#endif
}

int main(void)
{
    setlocale(LC_ALL, "");
    printf("== 数据表（名单 / 联系表）自测 ==\n");
    init_paths();
    test_cache_refresh();

    test_basic();
    test_quotes();
    test_lines();
    test_encoding();
    test_delim();
    test_match();
    test_dupes();
    test_many_dupes();
    test_numeric_not_overeager();
    test_cols();
    test_scale();
    test_reload();
    test_refs();
    test_dup_header();
    test_enum_keys();

    teardown();
    printf("\n=========================\n通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
