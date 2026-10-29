from source_helpers import read_source, INCLUDE_FLAGS, CORE_SOURCES
"""Exercise the table preview's column-width computation through a fake GDI layer.

Regression evidence for: 表格预览每刷新一次，每一列就再宽一个 pv_cellpad()*2。

原因：p->colw 是 realloc 出来的，里面还留着上一次算完（已经加过一遍内边距）
的宽度；测量循环只「取更大值」，最后又 += pv_cellpad() * 2，于是同一条内容
连刷两次，列宽就从「内容宽 + 14」变成「内容宽 + 28」，越刷越宽，几次以后撑到
列宽上限。用户看到的就是「预览文字后面莫名多出一段空格」。

这里把 qp_preview_set_table 原文抽出来，配一套假的 GDI（按字符估宽），
连刷三次同一条内容，断言列宽、总宽、行高、行列数都不再变化。

Run with:  python3 tests/preview_table_width.py
"""
import pathlib
import re
import subprocess
import sys
import tempfile

repo = pathlib.Path(__file__).resolve().parents[1]
source = read_source("preview.c")
engine = read_source("engine.c")


def red(msg):
    print("RED: " + msg)
    sys.exit(1)


def function(name):
    # 单行形式：static int pv_cellpad(void) { return DPIX(7); }
    one = re.search(r"^static [^\n]*\b" + name + r"\([^\n]*\{[^\n]*\}\n", source, re.M)
    if one:
        return one.group(0)
    match = re.search(r"(?:static )?[^\n]+\b" + name + r"\([^;]+?\)\n\{", source)
    if not match:
        red("missing production function " + name)
    return source[match.start():source.index("\n}", match.end()) + 2] + "\n"


def engine_function(name):
    one = re.search(r"^static [^\n]*\b" + name + r"\([^\n]*\{[^\n]*\}\n", engine, re.M)
    if one:
        return one.group(0)
    match = re.search(r"(?:static )?(?:void|int|size_t|const [^\n]+) \*?" + name +
                      r"\([^;]+?\)\n\{", engine)
    if not match:
        red("missing engine function " + name)
    return engine[match.start():engine.index("\n}", match.end()) + 2] + "\n"


def public_function(name):
    match = re.search(r"(?:void|int) " + name + r"\([^;]+?\)\n\{", source)
    if not match:
        red("missing production function " + name)
    return source[match.start():source.index("\n}", match.end()) + 2] + "\n"


def grab(pattern, what):
    match = re.search(pattern, source)
    if not match:
        red("missing production constant " + what)
    return match.group(0) + "\n"


# 生产代码里被 set_table 直接或间接用到的部分
structs = (source[source.index("typedef struct { int off; int len; int missing; } PvCell;"):
                 source.index("} Preview;") + len("} Preview;")] + "\n")
structs = "#define PV_MAX_MARKS 512\n" + structs
consts = (grab(r"#define PV_MAX_COLS[^\n]+", "PV_MAX_COLS") +
          grab(r"#define PV_MAX_MARKS[^\n]+", "PV_MAX_MARKS") +
          grab(r'static const wchar_t \*PV_TABLE_MARK[^\n]+', "PV_TABLE_MARK") +
          grab(r'static const wchar_t \*PV_TEXT_MARK[^\n]+', "PV_TEXT_MARK"))
helpers = (function("pv_of") + function("pv_cellpad") + function("pv_maxcolw") +
           function("pv_scan_rows") + function("pv_fill_cells") +
           function("pv_build_header"))
# 表头那一行要用「从格式串里取第 i 个 {占位符}」的真实实现，连同它依赖的
# 小工具一起从 engine.c 抽出来，避免测试自己写一份可能走样的版本。
engine_bits = (engine_function("qp_strlcpy") + engine_function("qp_trim") +
               engine_function("placeholder_next") +
               engine_function("qp_placeholder_at"))
target = public_function("qp_preview_set_table_marked")

preamble = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include "engine.h"

typedef void *HDC;
typedef void *HFONT;
typedef void *HWND;
typedef struct { int cx, cy; } SIZE;
typedef struct { int tmHeight; } TEXTMETRICW;

static int g_dpi = 96;
static HFONT g_fontMono;
static int failures,measurements,fault,attempts;
static void *allocate(size_t n){return ++attempts==fault?NULL:malloc(n);}
static void *allocate_zero(size_t n,size_t s){return ++attempts==fault?NULL:calloc(n,s);}
#define malloc allocate
#define calloc allocate_zero

#define DPIX(v) ((v) * g_dpi / 96)
#define FALSE 0
#define GWLP_USERDATA (-21)
static long GetWindowLongPtrW(HWND h, int i) { (void)i; return (long)(size_t)h; }

static HDC GetDC(HWND h) { (void)h; return (HDC)1; }
static void ReleaseDC(HWND h, HDC d) { (void)h; (void)d; }
static void *SelectObject(HDC d, HFONT f) { (void)d; (void)f; return NULL; }
static int GetTextMetricsW(HDC d, TEXTMETRICW *tm) {
    (void)d; tm->tmHeight = 15; return 1;
}
/* 按「半角 7px、全角 14px」估宽，与生产代码里 wAscii/wWide 的口径一致 */
static int GetTextExtentPoint32W(HDC d, const wchar_t *s, int n, SIZE *sz) {
    int i;
    (void)d;
    ++measurements;sz->cx = 0;
    for (i = 0; i < n; ++i) sz->cx += (s[i] > 0x2E80) ? 14 : 7;
    return 1;
}
static void pv_update_scroll(HWND h) { (void)h; }
static void InvalidateRect(HWND h, const void *r, int e) { (void)h; (void)r; (void)e; }
'''

driver = r'''
static void require(int yes, const char *what) {
    if (!yes) { printf("FAIL: %s\n", what); ++failures; }
}

int main(void) {
    static const wchar_t *row1 = L"张三\t13800138000";
    static const wchar_t *row2 = L"李四\t13900139000";
    wchar_t raw[512], fmt[64];
    Preview p;
    int round, first_total = 0, first_col0 = 0, first_col1 = 0;
    int first_rows = 0, first_cols = 0;

    swprintf(raw, 512, L"%ls\n%ls", row1, row2);
    wcscpy(fmt, L"{姓名}\t{电话}");

    memset(&p, 0, sizeof(p));
    p.mode = 1;                                   /* QP_PREVIEW_TABLE */
    p.hdr = (wchar_t *)calloc(PV_MAX_COLS * QP_LABEL_MAX, sizeof(wchar_t));
    require(p.hdr != NULL, "表头缓冲申请成功");

    for (round = 0; round < 3; ++round) {
        int measured=measurements;
        if(round>0){p.scrollX=5;p.scrollY=23;p.selAnchor=1;p.selEnd=2;}
        qp_preview_set_table_marked(&p, raw, fmt, NULL, 0);
        if(round>0)require(p.scrollX==5 && p.scrollY==23 && p.selEnd==2 && measurements==measured,
                          "相同表格保持阅读位置和度量结果");
        printf("  第 %d 次刷新: rows=%d cols=%d colw=[%d,%d] totalW=%d rowH=%d\n",
               round + 1, p.rows, p.cols, p.colw[0], p.colw[1], p.totalW, p.rowH);
        if (round == 0) {
            first_total = p.totalW;
            first_col0 = p.colw[0];
            first_col1 = p.colw[1];
            first_rows = p.rows;
            first_cols = p.cols;
            continue;
        }
        require(p.colw[0] == first_col0, "第 1 列列宽不随刷新变化");
        require(p.colw[1] == first_col1, "第 2 列列宽不随刷新变化");
        require(p.totalW == first_total, "表格总宽不随刷新变化");
        require(p.rows == first_rows && p.cols == first_cols, "行列数稳定");
    }

    /* 内容变短时列宽要跟着收回来（不能只增不减） */
    qp_preview_set_table_marked(&p, L"甲\t乙", fmt, NULL, 0);
    printf("  换成短内容: colw=[%d,%d] totalW=%d\n", p.colw[0], p.colw[1], p.totalW);
    require(p.colw[1] < first_col1, "内容变短后第 2 列跟着收窄（不是只增不减）");
    require(p.colw[0] <= first_col0, "内容变短后第 1 列不变宽");
    require(p.colw[0] >= DPIX(60), "列宽不低于设计下限");
    require(p.colw[0] <= pv_maxcolw(), "列宽不超过上限");

    /* 再刷两次同样要稳住 */
    {
        int w0 = p.colw[0], w1 = p.colw[1], tw = p.totalW;
        qp_preview_set_table_marked(&p, L"甲\t乙", fmt, NULL, 0);
        qp_preview_set_table_marked(&p, L"甲\t乙", fmt, NULL, 0);
        require(p.colw[0] == w0 && p.colw[1] == w1 && p.totalW == tw,
                "短内容连刷也不再变宽");
    }

    {
        int marks[] = {1};
        qp_preview_set_table_marked(&p, L"\t", L"", marks, 1);
        require(!p.cells[0].missing && p.cells[1].missing, "合法空白不标错，仅失配单元格标错");
        marks[0]=0;
        qp_preview_set_table_marked(&p, L"", L"{变量}", marks, 1);
        require(p.rows==1 && p.cols==1 && p.cells[0].missing, "单个空的失配单元格仍可见");
        qp_preview_set_table_marked(&p, L"甲\t乙\r\n丙\t丁", fmt, NULL, 0);
        require(p.cells[1].len==1, "CRLF 的回车不会进入单元格正文");
    }
    for(fault=1;fault<=5;++fault) {
        attempts=0;wchar_t *original=p.raw;int rows=p.rows,cols=p.cols;
        qp_preview_set_table_marked(&p,L"new\tcontent\tthree",L"{a}\t{b}\t{c}",NULL,0);
        require(p.raw==original && p.rows==rows && p.cols==cols,"表格构造分配失败保留原预览及原文");
    }
    fault=0;
    free(p.raw); free(p.cells); free(p.colw); free(p.hdr);free(p.modelFormat);
    if (!failures) printf("表格预览列宽：连刷 3 次宽度稳定，无累加内边距\n");
    return failures ? 1 : 0;
}
'''

if __name__ == '__main__':
    code = preamble + structs + consts + engine_bits + helpers + target + driver
    with tempfile.TemporaryDirectory(prefix="qp-preview-width-") as tmp:
        exe = pathlib.Path(tmp) / "width"
        result = subprocess.run(
            ["cc", "-std=gnu11", "-O0", *INCLUDE_FLAGS, "-x", "c", "-",
             "-o", str(exe)],
            input=code, text=True)
        if result.returncode:
            sys.exit(result.returncode)
        sys.exit(subprocess.run([str(exe)]).returncode)
