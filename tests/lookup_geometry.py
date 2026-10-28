from source_helpers import read_source, INCLUDE_FLAGS, CORE_SOURCES
"""Exercise the 数据表 / 查表匹配 dialog layouts through a Win32 geometry adapter.

This is source-executed evidence that both new dialogs of the lookup feature put
every control inside the client area, never overlap two controls, keep list-view
column widths inside their control, and create every HWND field the context
declares.  The 数据表 dialog is resizable, so it is exercised from its enforced
minimum size upward (a resizable window that never clamps its minimum will let
the user drag the layout apart).

It does not claim native Windows painting, message dispatch, DPI awareness
beyond the DPIX() scale factor, or combo-popup behaviour.  Run with:
  python3 tests/lookup_geometry.py
"""
import pathlib
import re
import subprocess
import sys
import tempfile

repo = pathlib.Path(__file__).resolve().parents[1]
source = read_source("ui_dialogs.c")


def red(msg):
    print("RED: " + msg)
    sys.exit(1)


def function(name):
    match = re.search(r"(?:static )?[^\n]+\b" + name + r"\([^;]+?\)\n\{", source)
    if not match:
        red("missing production function " + name)
    return source[match.start():source.index("\n}", match.end()) + 2] + "\n"


def grab(pattern, what):
    match = re.search(pattern, source)
    if not match:
        red("missing production constant " + what)
    return match.group(0)


def ctx_struct(tag):
    end = source.index("} " + tag + ";") + len("} " + tag + ";")
    return source[source.rfind("typedef struct {", 0, end):end]


def hwnd_fields(ctx):
    decls = re.findall(r"HWND ([^;]+);", ctx)
    fields = [f.strip() for decl in decls for f in decl.split(",")]
    return [f for f in fields if f != "hwnd"]


table_ctx = ctx_struct("TableDlgCtx")
lookup_ctx = ctx_struct("LookupDlgCtx")
table_fields = hwnd_fields(table_ctx)
lookup_fields = hwnd_fields(lookup_ctx)

# ---------------------------------------------------------------- contracts --
# 数据表 窗口可以拉大缩小（WS_THICKFRAME），所以它必须像模板编辑器那样自己定一个
# 最小尺寸；否则用户能把窗口拖到控件互相压住、按钮跑出客户区。
table_proc = function("table_dlg_proc")
if "WM_GETMINMAXINFO" not in table_proc or "ptMinTrackSize" not in table_proc:
    red("数据表 dialog is resizable (WS_THICKFRAME) but never clamps its minimum "
        "size — 模板编辑器用 WM_GETMINMAXINFO + ED_MIN_W/ED_MIN_H，这里缺了同一套")
for token in ("DPIX(DT_MIN_W)", "DPIX(DT_MIN_H)"):
    if token not in table_proc:
        red("数据表 minimum size is not derived from the design minimum " + token)

minw = re.search(r"#define\s+DT_MIN_W\s+(\d+)", source)
minh = re.search(r"#define\s+DT_MIN_H\s+(\d+)", source)
if not minw or not minh:
    red("missing DT_MIN_W/DT_MIN_H design minimums for the 数据表 dialog")
DT_MIN_W, DT_MIN_H = int(minw.group(1)), int(minh.group(1))

# 两个窗口的控件集：少建一个就会在下面「HWND 字段必须被创建」的检查里暴露。
for name in ("hList", "hEnc", "hDelim", "hHeader", "hInfo", "hCols", "hPrev",
             "hImport", "hRename", "hRemove", "hReload", "hOpen",
             "hLblList", "hLblRead", "hLblEnc", "hLblDelim", "hLblPrev"):
    if name not in table_fields:
        red("数据表 context lost control field " + name)
for name in ("hTable", "hTableInfo", "hKeyMode", "hKeyVar", "hKeyFix", "hKeyCol",
             "hHit", "hSep", "hUnique", "hOutList", "hOutCol", "hOutTarget",
             "hTip", "hKeyVarLbl", "hKeyFixLbl", "hSepLbl", "hLblTable",
             "hLblKeyMode", "hLblKeyCol", "hLblHit", "hLblOut", "hLblOutCol",
             "hLblOutTarget", "hBtnManage", "hOutAdd", "hOutDel", "hOutUp",
             "hOutDown"):
    if name not in lookup_fields:
        red("查表匹配 context lost control field " + name)

# ------------------------------------------------------------------ harness --
preamble = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "engine.h"
#include "table.h"
#include "resource.h"

typedef unsigned UINT;
typedef unsigned long WPARAM;
typedef long LPARAM;
typedef long LRESULT;
typedef unsigned long DWORD;
typedef wchar_t *LPWSTR;
typedef const wchar_t *LPCWSTR;
typedef struct { int left, top, right, bottom; } RECT;
typedef struct { UINT mask; int fmt, cx; LPWSTR pszText; int iSubItem; } LVCOLUMNW;

typedef struct Ctl {
    int id, placed, visible, enabled, checked, count, sel;
    int x, y, w, h;
    int cols, ncolw, colw[8];
    DWORD style;
    struct Ctl *parent;
    const char *name;
} Ctl;
typedef Ctl *HWND;

#define TRUE 1
#define ES_MULTILINE 4
#define ES_READONLY 2048
#define ES_AUTOVSCROLL 64
#define WS_VSCROLL 0x200000
#define WS_TABSTOP 0x10000
#define WS_EX_CLIENTEDGE 512
#define FALSE 0
#define CB_ERR (-1)
#define CB_RESETCONTENT 0x014B
#define CB_ADDSTRING    0x0143
#define CB_GETITEMDATA  0x0150
#define CB_SETITEMDATA  0x0151
#define CB_SETCURSEL    0x014E
#define CB_GETCURSEL    0x0147
#define BM_SETCHECK     0x00F1
#define BM_GETCHECK     0x00F0
#define BM_SETSTYLE     0x00F4
#define EM_SETCUEBANNER 0x1501
#define BST_CHECKED     1
#define BST_UNCHECKED   0
#define LVCF_TEXT       0x0004
#define LVCF_WIDTH      0x0002
#define LVCF_SUBITEM    0x0008
#define BS_DEFPUSHBUTTON 0x00000001L
#define WM_SETREDRAW    0x000B
#define IDOK            1
#define IDCANCEL        2
#define _snwprintf swprintf

static Ctl controls[160];
static int ncontrols, dpi = 96, failures, cases;
static int setcolw_calls;          /* ListView_SetColumnWidth 被调了几次 */
static Ctl root;

static int DPIX(int x) { return (x * dpi + 48) / 96; }

static HWND newctl(int id, HWND parent, DWORD style) {
    Ctl *c = &controls[ncontrols++];
    memset(c, 0, sizeof(*c));
    c->name = "(未命名)"; c->id = id; c->parent = parent; c->style = style;
    c->visible = 1; c->enabled = 1; c->sel = CB_ERR;
    return c;
}
static HWND qp_create(const wchar_t *cls, const wchar_t *text, DWORD style, DWORD ex,
                      int x, int y, int w, int h, HWND parent, int id) {
    (void)cls; (void)text; (void)ex; (void)x; (void)y; (void)w; (void)h;
    return newctl(id, parent, style);
}
static HWND mk_label(HWND p, const wchar_t *t, int id) {
    return qp_create(L"Static", t, 0, 0, 0, 0, 10, 10, p, id);
}
static HWND mk_wrapped_label(HWND p,const wchar_t *t,int id) { return mk_label(p,t,id); }
static int qp_wrapped_text_height(HWND h,int width) { (void)h;(void)width;return DPIX(90); }
static HWND mk_label_r(HWND p, const wchar_t *t, int id) {
    return qp_create(L"Static", t, 0, 0, 0, 0, 10, 10, p, id);
}
static HWND mk_edit(HWND p, int id, DWORD extra) {
    (void)extra; return qp_create(L"Edit", L"", 0, 0, 0, 0, 10, 10, p, id);
}
static HWND mk_btn(HWND p, const wchar_t *t, int id) {
    return qp_create(L"Button", t, 0, 0, 0, 0, 10, 10, p, id);
}
static HWND mk_check(HWND p, const wchar_t *t, int id) {
    return qp_create(L"Button", t, 0, 0, 0, 0, 10, 10, p, id);
}
static HWND mk_combo(HWND p, int id) {
    return qp_create(L"ComboBox", L"", 0, 0, 0, 0, 10, DPIX(260), p, id);
}
static HWND mk_list(HWND p, int id, DWORD extra) {
    (void)extra; return qp_create(L"SysListView32", L"", 0, 0, 0, 0, 10, 10, p, id);
}

static int MoveWindow(HWND h, int x, int y, int w, int hh, int repaint) {
    (void)repaint;
    h->x = x; h->y = y; h->w = w; h->h = hh; h->placed = 1;
    return 1;
}
static void GetClientRect(HWND h, RECT *r) {
    r->left = 0; r->top = 0; r->right = h->w; r->bottom = h->h;
}
typedef void *HDWP;
static void qp_dialog_content_rect(HWND h,RECT *r){GetClientRect(h,r);}
static HDWP qp_layout_begin(int n){(void)n;return (void *)1;}
static HDWP qp_dialog_defer(HDWP q,HWND h,int x,int y,int w,int height){MoveWindow(h,x,y,w,height,1);return q;}
static void qp_layout_end(HDWP q){(void)q;}
static void qp_dialog_update_view(HWND h){(void)h;}
static HWND GetDlgItem(HWND parent, int id) {
    int i;
    for (i = 0; i < ncontrols; ++i)
        if (controls[i].parent == parent && controls[i].id == id) return &controls[i];
    return NULL;
}
static LRESULT SendMessageW(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    switch (msg) {
    case CB_RESETCONTENT: h->count = 0; h->sel = CB_ERR; return 0;
    case CB_ADDSTRING:    return h->count++;
    case CB_SETITEMDATA:  return 0;
    case CB_GETITEMDATA:  return 0;
    case CB_SETCURSEL:    h->sel = (int)wp; return h->sel;
    case CB_GETCURSEL:    return h->sel;
    case BM_SETCHECK:     h->checked = (int)wp; return 0;
    case BM_GETCHECK:     return h->checked;
    default:              return 0;
    }
}
static void SetWindowTextW(HWND h, const wchar_t *t) { (void)h; (void)t; }
static void ListView_InsertColumn(HWND h, int i, const LVCOLUMNW *c) {
    h->cols++;
    if (i >= h->ncolw) h->ncolw = i + 1;
    if (i >= 0 && i < 8) h->colw[i] = c->cx;
}
static void ListView_SetColumnWidth(HWND h, int n, int w) {
    ++setcolw_calls;
    if (n >= h->ncolw) h->ncolw = n + 1;
    if (n >= 0 && n < 8) h->colw[n] = w;
}
static int ListView_GetColumnWidth(HWND h, int n) {
    return (n >= 0 && n < 8) ? h->colw[n] : 0;
}
static void InvalidateRect(HWND h, const RECT *r, int erase) {
    (void)h; (void)r; (void)erase;
}

/* 表模块里被布局间接引用的三个名字函数（只取名字，几何无关） */
const wchar_t *qp_table_encoding_name(int enc) { (void)enc; return L"UTF-8"; }
const wchar_t *qp_table_delim_name(int delim) { (void)delim; return L"逗号"; }
const wchar_t *qp_table_hit_name(int hit) { (void)hit; return L"第一条"; }

static void fill_var_combo(HWND combo, const Template *t, const wchar_t *sel_id,
                           const Segment *exclude, int include_static) {
    (void)combo; (void)t; (void)sel_id; (void)exclude; (void)include_static;
}
'''

consts = "\n".join([
    "#define DT_MIN_W %d\n#define DT_MIN_H %d" % (DT_MIN_W, DT_MIN_H),
    grab(r"(?:static )?const int kTableEncs\[\][^;]+;", "kTableEncs"),
    grab(r"#define QP_TABLE_ENC_COUNT[^\n]+", "QP_TABLE_ENC_COUNT"),
    grab(r"(?:static )?const int kTableDelims\[\][^;]+;", "kTableDelims"),
    grab(r"#define QP_TABLE_DELIM_COUNT[^\n]+", "QP_TABLE_DELIM_COUNT"),
    grab(r"#define QP_DT_PREVIEW_ROWS[^\n]+", "QP_DT_PREVIEW_ROWS"),
    grab(r"(?:static )?const int kLookupKeyModes\[\][^;]+;", "kLookupKeyModes"),
    grab(r"#define QP_LK_KEYMODE_COUNT[^\n]+", "QP_LK_KEYMODE_COUNT"),
    grab(r"(?:static )?const int kLookupHits\[\][^;]+;", "kLookupHits"),
    grab(r"#define QP_LK_HIT_COUNT[^\n]+", "QP_LK_HIT_COUNT"),
]) + "\n"

helpers = (function("lv_add_col") + function("lookup_keymode_index") +
           function("lookup_hit_index") + function("lookup_keymode_hint"))

layouts = (function("table_dlg_fit_cols") + function("table_dlg_layout") +
           function("lookup_dlg_layout"))

table_names = "\n".join('    label_ctl(c.%s, "%s");' % (f, f) for f in table_fields)
lookup_names = "\n".join('    label_ctl(c.%s, "%s");' % (f, f) for f in lookup_fields)
table_field_asserts = "\n".join(
    '    if (!c.%s) { printf("FAIL %s: %%s never created\\n", tag); ++failures; }'
    % (f, f) for f in table_fields)
lookup_field_asserts = "\n".join(
    '    if (!c.%s) { printf("FAIL %s: %%s never created\\n", tag); ++failures; }'
    % (f, f) for f in lookup_fields)

driver = r'''
static void label_ctl(HWND h, const char *name) { if (h) h->name = name; }

static void reset(void) {
    ncontrols = 0;
    memset(&root, 0, sizeof(root));
    root.placed = 1; root.visible = 1; root.enabled = 1; root.name = "dialog";
}

/* 每个控件都要：被摆过位、尺寸非负、整个落在客户区里；任意两个控件不得叠在
   一起；列表控件的列宽之和不得超过列表自身宽度。 */
static void verify(const char *tag) {
    int i, j;
    ++cases;
    if (ncontrols < 12) {
        printf("FAIL %s: 只建了 %d 个控件，布局根本没跑起来\n", tag, ncontrols);
        ++failures;
    }
    for (i = 0; i < ncontrols; ++i) {
        Ctl *a = &controls[i];
        if (!a->placed) {
            printf("FAIL %s: 控件 %s 建了却从没摆位\n", tag, a->name);
            ++failures; continue;
        }
        if (a->w < 0 || a->h < 0) {
            printf("FAIL %s: 控件 %s 尺寸为负 %dx%d\n", tag, a->name, a->w, a->h);
            ++failures;
        }
        if (a->x < 0 || a->y < 0 || a->x + a->w > root.w || a->y + a->h > root.h) {
            printf("FAIL %s: 控件 %s 越出客户区 (%d,%d,%d,%d) vs %dx%d\n",
                   tag, a->name, a->x, a->y, a->w, a->h, root.w, root.h);
            ++failures;
        }
        if (a->cols > 0) {
            int k, sum = 0;
            for (k = 0; k < a->ncolw; ++k) sum += a->colw[k];
            if (sum > a->w) {
                printf("FAIL %s: 列表 %s 的列宽之和 %d 超过自身宽度 %d\n",
                       tag, a->name, sum, a->w);
                ++failures;
            }
        }
    }
    for (i = 0; i < ncontrols; ++i) {
        for (j = i + 1; j < ncontrols; ++j) {
            Ctl *a = &controls[i], *b = &controls[j];
            if (!a->placed || !b->placed) continue;
            if (a->x < b->x + b->w && b->x < a->x + a->w &&
                a->y < b->y + b->h && b->y < a->y + a->h) {
                printf("FAIL %s: 控件 %s 和 %s 叠在一起\n", tag, a->name, b->name);
                ++failures;
            }
        }
    }
}

/* 数据表窗口：先摆一次（建控件），再摆一次（模拟 WM_SIZE 的重排） */
static void run_table(int cw, int ch) {
    TableDlgCtx c;
    char tag[160];
    int round;
    reset();
    memset(&c, 0, sizeof(c));
    c.hwnd = &root;
    for (round = 0; round < 2; ++round) {
        root.w = cw; root.h = ch;
        table_dlg_layout(&c);
        if (round == 0) {
TABLE_NAMES
            label_ctl(GetDlgItem(&root, IDC_DT_OK), "IDC_DT_OK");
        }
TABLE_ASSERTS
        snprintf(tag, sizeof(tag), "数据表 %dx%d dpi=%d round=%d", cw, ch, dpi, round);
        verify(tag);
    }
}

/* 查表匹配窗口：固定设计尺寸，把三种键来源 × 三种命中模式都走一遍 */
static void run_lookup(int key_mode, int hit, const char *mode_name) {
    LookupDlgCtx c;
    ParseRule r;
    Template t;
    char tag[160];
    reset();
    memset(&c, 0, sizeof(c));
    memset(&r, 0, sizeof(r));
    memset(&t, 0, sizeof(t));
    c.hwnd = &root; c.rule = &r; c.tmpl = &t;
    r.lk_key_mode = key_mode;
    r.lk_hit = hit;
    root.w = DPIX(700); root.h = DPIX(620);
    lookup_dlg_layout(&c);
LOOKUP_NAMES
    label_ctl(GetDlgItem(&root, IDOK), "IDOK");
    label_ctl(GetDlgItem(&root, IDCANCEL), "IDCANCEL");
LOOKUP_ASSERTS
    snprintf(tag, sizeof(tag), "查表匹配 %s dpi=%d", mode_name, dpi);
    verify(tag);
}

/* 预览列宽：改一次列宽就是一次整块重排 + 重画，所以「什么时候不该改」和
   「改成多少」一样重要——拖边框时一秒来上百条 WM_SIZE，每条都重铺十几列
   就是用户说的那种卡。 */
static void check(int cond, const char *what) {
    if (!cond) { printf("FAIL fit_cols: %s\n", what); ++failures; }
}

static void test_fit_cols(void) {
    Ctl list;
    TableDlgCtx c;
    int k, sum;

    reset();
    memset(&list, 0, sizeof(list));
    memset(&c, 0, sizeof(c));
    list.w = DPIX(500); list.h = DPIX(200);
    c.hwnd = &root; c.hPrev = &list;

    /* 还没铺过列（prev_cols = 0）：一列都不许动 */
    setcolw_calls = 0;
    table_dlg_fit_cols(&c, 1);
    check(setcolw_calls == 0, "没有列的时候不该去动列宽");

    c.prev_cols = 4;
    setcolw_calls = 0;
    table_dlg_fit_cols(&c, 1);
    check(setcolw_calls == 4, "强制铺一次：4 列各改一次");
    check(list.colw[0] == DPIX(40), "第 1 列固定给行号");

    /* 宽度没变：再铺就该一次都不动（拖高度时就是这种常态） */
    setcolw_calls = 0;
    table_dlg_fit_cols(&c, 0);
    check(setcolw_calls == 0, "控件宽度没变时不该重复改列宽");

    /* 宽度变了：要重铺 */
    list.w = DPIX(600);
    setcolw_calls = 0;
    table_dlg_fit_cols(&c, 0);
    check(setcolw_calls == 4, "宽度变了就该重铺");

    /* 列数变了（预览刚重建过）：即使宽度没变也要重铺 */
    c.prev_cols = 6;
    setcolw_calls = 0;
    table_dlg_fit_cols(&c, 0);
    check(setcolw_calls == 6, "列数变了也要重铺");

    sum = 0;
    for (k = 0; k < list.ncolw; ++k) sum += list.colw[k];
    check(sum <= list.w, "列宽之和不得超过列表自身宽度");

    /* 只有行号一列时也不能除零 */
    c.prev_cols = 1;
    setcolw_calls = 0;
    table_dlg_fit_cols(&c, 1);
    check(setcolw_calls == 1, "只有行号列时也只改那一列");

    /* 控件宽度为 0（还没摆位）时什么都不做 */
    c.prev_cols = 4;
    list.w = 0;
    setcolw_calls = 0;
    table_dlg_fit_cols(&c, 1);
    check(setcolw_calls == 0, "控件还没有宽度时不该动列宽");
}

int main(void) {
    static const int dpi_list[] = { 96, 120, 144, 192 };
    static const int table_sizes[][2] = {
        { DT_MIN_W, DT_MIN_H },      /* 允许的最小尺寸 */
        { 760, 560 },                /* 创建时的设计尺寸 */
        { 1280, 820 },
        { 1600, 1000 },
    };
    int d, s, m, h;
    for (d = 0; d < (int)(sizeof(dpi_list) / sizeof(dpi_list[0])); ++d) {
        dpi = dpi_list[d];
        for (s = 0; s < (int)(sizeof(table_sizes) / sizeof(table_sizes[0])); ++s)
            run_table(DPIX(table_sizes[s][0]), DPIX(table_sizes[s][1]));
        for (m = 0; m < QP_LK_KEYMODE_COUNT; ++m) {
            for (h = 0; h < QP_LK_HIT_COUNT; ++h) {
                char name[64];
                snprintf(name, sizeof(name), "键模式%d/命中%d", m, h);
                run_lookup(kLookupKeyModes[m], kLookupHits[h], name);
            }
        }
    }
    printf("%d lookup dialog geometry cases, %d failures\n", cases, failures);
    test_fit_cols();
    if (failures) printf("fit_cols 有失败项（见上）\n");
    return failures ? 1 : 0;
}
'''.replace("TABLE_NAMES", table_names) \
   .replace("TABLE_ASSERTS", table_field_asserts) \
   .replace("LOOKUP_NAMES", lookup_names) \
   .replace("LOOKUP_ASSERTS", lookup_field_asserts)

code = preamble + consts + table_ctx + "\n" + lookup_ctx + "\n" + helpers + layouts + driver
with tempfile.TemporaryDirectory(prefix="qp-lookup-geometry-") as tmp:
    exe = pathlib.Path(tmp) / "geometry"
    result = subprocess.run(
        ["cc", "-std=gnu11", "-O0", "-Wall", "-Wextra", *INCLUDE_FLAGS,
         "-x", "c", "-", "-o", str(exe)],
        input=code, text=True, capture_output=True)
    if result.stderr.strip():
        sys.stderr.write(result.stderr)
    if result.returncode:
        sys.exit(result.returncode)
    sys.exit(subprocess.run([str(exe)]).returncode)
