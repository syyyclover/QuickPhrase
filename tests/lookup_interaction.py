from source_helpers import read_source, INCLUDE_FLAGS, CORE_SOURCES
"""Exercise the 查表匹配 dialog's state machine, output reorder, and the
「重建列表时不许反过来写」这条规矩。

Source-executed evidence for the parts of the lookup dialog that carry real
branching:
  * lookup_sync_enable  — 「用谁去查」三种来源只放行对应的那一行，
                          「多行命中时」只有「全部」才放行分隔符与去重开关；
  * lookup_load_out     — 输出对列表选中哪一行，决定删除/上移/下移能不能点；
  * lookup_out_move     — 上移/下移之后链的顺序与 lookup_out_at 的下标必须一致；
  * lookup_list_changed — 换行只读不写：ListView 重建时的 LVN_ITEMCHANGED 必须
                          整条忽略（那时下拉框里还是上一行的值），
                          用户换行也只把新行读进编辑区，绝不覆盖它；
  * lookup_fill_outs    — 整段重建都压在 loading 里，且用完恢复调用方的原值。

It does not claim native Windows message dispatch, combo popups, or painting.
Run with:
  python3 tests/lookup_interaction.py
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


end = source.index("} LookupDlgCtx;") + len("} LookupDlgCtx;")
lookup_ctx = source[source.rfind("typedef struct {", 0, end):end]

preamble = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "engine.h"
#include "table.h"
#include "storage.h"
#include "resource.h"

#if defined(_WIN32)
#  define QP_TMPDIR L"."
#else
#  define QP_TMPDIR L"/tmp"
#endif

/* 造一个能读的表文件（宽路径 → 窄路径，与 storage 里的做法一致） */
static int write_text_file(const wchar_t *path, const char *utf8) {
    char *u8 = qp_wide_to_utf8_alloc(path);
    FILE *f;
    size_t n = strlen(utf8);
    int ok = 0;
    if (!u8) return 0;
    f = fopen(u8, "wb");
    if (f) { ok = (fwrite(utf8, 1, n, f) == n); fclose(f); }
    free(u8);
    return ok;
}

typedef unsigned UINT;
typedef unsigned long WPARAM;
typedef long LPARAM;
typedef long LRESULT;

typedef struct Ctl {
    int id, enabled, sel, count;
    const void *items[8];          /* 每个下拉项的 item data */
    struct Ctl *parent;
    const char *name;
} Ctl;
typedef Ctl *HWND;

/* 列表控件发回来的通知（这里只要那两个状态位） */
typedef struct { unsigned uNewState, uOldState; } NMLISTVIEW;
#define LVIS_SELECTED 0x0002

#define CB_ERR (-1)
#define CB_RESETCONTENT 0x014B
#define CB_ADDSTRING    0x0143
#define CB_GETCOUNT     0x0146
#define CB_GETITEMDATA  0x0150
#define CB_SETITEMDATA  0x0151
#define CB_SETCURSEL    0x014E
#define CB_GETCURSEL    0x0147
#define _snwprintf swprintf

static Ctl controls[32];
static int ncontrols, failures;
static Ctl root;

/* 记账：这几个数就是「对话框有没有反过来动列表」的证据 */
static int lv_edits;               /* lv_set_row 次数 */
static int lv_deletes;             /* ListView_DeleteAllItems 次数 */
static int lv_selects;             /* lv_select 次数 */
static int load_col_calls;         /* lookup_fill_col_combo 次数 */
static int loading_during_edit = -1;   /* 重建过程中看到的 c->loading */
static void *g_ctx;                /* 指向被测的 LookupDlgCtx */

static HWND mkctl(int id, const char *name) {
    Ctl *c = &controls[ncontrols++];
    memset(c, 0, sizeof(*c));
    c->id = id; c->name = name; c->parent = &root; c->enabled = 1; c->sel = CB_ERR;
    return c;
}
static HWND GetDlgItem(HWND parent, int id) {
    int i;
    for (i = 0; i < ncontrols; ++i)
        if (controls[i].parent == parent && controls[i].id == id) return &controls[i];
    return NULL;
}
static int EnableWindow(HWND h, int enabled) {
    int was = h->enabled;
    h->enabled = enabled;
    return was;
}
static LRESULT SendMessageW(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    switch (msg) {
    case CB_RESETCONTENT: h->count = 0; h->sel = CB_ERR;
                          memset(h->items, 0, sizeof(h->items)); return 0;
    case CB_ADDSTRING:    return h->count++;
    case CB_GETCOUNT:     return h->count;
    case CB_SETITEMDATA:  if ((int)wp >= 0 && (int)wp < 8)
                              h->items[wp] = (const void *)(long)lp;
                          return 0;
    case CB_GETITEMDATA:  return ((int)wp >= 0 && (int)wp < 8)
                              ? (LRESULT)(long)h->items[wp] : 0;
    case CB_SETCURSEL:    h->sel = (int)wp; return h->sel;
    case CB_GETCURSEL:    return h->sel;
    default:              return 0;
    }
}
/* 列表控件：sel 就是「当前选中的行」，-1 表示没选。
   真实的 ListView 在删除/插入/改选中状态时都会同步发 LVN_ITEMCHANGED，
   所以这几个动作都要记下来，供「重建期间有没有反过来动列表」的断言用。 */
static int lv_sel(HWND h) { return h->sel; }
'''

# 下面这几个桩要引 LookupDlgCtx，所以得排在从源码里抽出来的结构体之后
preamble_ui = r'''
static void qp_set_text_if_diff(HWND h,const wchar_t *s){(void)h;(void)s;}
static void ListView_DeleteAllItems(HWND h) {
    ++lv_deletes;
    if (g_ctx) loading_during_edit = ((LookupDlgCtx *)g_ctx)->loading;
    h->count = 0; h->sel = CB_ERR;
}
static void lv_set_row(HWND h, int row, int col, const wchar_t *text) {
    (void)col; (void)text;
    ++lv_edits;
    /* 第 0 列是行头：真实现用它插行，所以行数也跟着长 */
    if (col == 0 && row >= h->count) h->count = row + 1;
    if (g_ctx) loading_during_edit = ((LookupDlgCtx *)g_ctx)->loading;
}
static void lv_select(HWND h, int i) {
    int n = h->count;
    ++lv_selects;
    if (g_ctx) loading_during_edit = ((LookupDlgCtx *)g_ctx)->loading;
    if (n == 0) return;
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    h->sel = i;
}
/* lookup_store_col 用真的（见 tested），所以「提交有没有发生」直接看模型字段。 */
static void lookup_fill_col_combo(HWND combo, const QpTable *t, const wchar_t *want_name,
                                  int want_index) {
    (void)combo; (void)t; (void)want_name; (void)want_index;
    ++load_col_calls;
}

static void require(int cond, const char *what) {
    if (!cond) { printf("FAIL: %s\n", what); ++failures; }
}
static void require_on(Ctl *c, int want, const char *what) {
    if (c->enabled != want) {
        printf("FAIL: %s（期望 %s，实际 %s）\n", what,
               want ? "可用" : "禁用", c->enabled ? "可用" : "禁用");
        ++failures;
    }
}
'''

consts = "\n".join([
    grab(r"(?:static )?const int kLookupKeyModes\[\][^;]+;", "kLookupKeyModes"),
    grab(r"#define QP_LK_KEYMODE_COUNT[^\n]+", "QP_LK_KEYMODE_COUNT"),
    grab(r"(?:static )?const int kLookupHits\[\][^;]+;", "kLookupHits"),
    grab(r"#define QP_LK_HIT_COUNT[^\n]+", "QP_LK_HIT_COUNT"),
]) + "\n"

helpers = function("lookup_keymode_index") + function("lookup_hit_index")
# 顺序按「先用后定义」排：C 里函数得先声明。
tested = (function("lookup_table") +
          function("lookup_store_col") +
          function("lookup_reject_column") +
          function("combo_selected_seg") +
          function("fill_var_combo") +
          function("lookup_row_text") +
          function("lookup_load_out") +
          function("lookup_commit_out") +
          function("lookup_fill_outs") +
          function("lookup_list_changed") +
          function("lookup_out_add") +
          function("lookup_out_move") +
          function("lookup_sync_enable"))

driver = r'''
/* ---------------------------------------------------------------- 输出对重排 */
/* 把链走一遍，同时要求 lookup_out_at(i) 和链上第 i 个是同一个节点 —— 上下移动
   只改链不改下标的话，这里就会露出来。 */
static void require_order(LookupOut *head, const int *want, int n, const char *what) {
    LookupOut *p = head;
    int i, got[8];
    for (i = 0; i < n; ++i) {
        if (!p) { printf("FAIL: %s（链在第 %d 个就断了）\n", what, i); ++failures; return; }
        got[i] = p->col_index;
        p = p->next;
    }
    if (p) { printf("FAIL: %s（链比期望长）\n", what); ++failures; return; }
    for (i = 0; i < n; ++i)
        if (got[i] != want[i]) {
            printf("FAIL: %s（链第 %d 个是 %d，期望 %d）\n", what, i, got[i], want[i]);
            ++failures; return;
        }
    for (i = 0; i < n; ++i) {
        LookupOut *o = lookup_out_at(head, i);
        if (!o || o->col_index != want[i]) {
            printf("FAIL: %s（lookup_out_at(%d) 和链不一致）\n", what, i);
            ++failures; return;
        }
    }
}

static LookupOut *out_with_index(int idx) {
    LookupOut *o = lookup_out_create();
    if (o) o->col_index = idx;
    return o;
}

static void test_move(void) {
    /* lookup_out_move 是「交换两个位置」；调用方只传相邻下标（row, row±1），
       所以等价于上移/下移一格。 */
    static const int a3[] = { 0, 1, 2 };
    static const int b3[] = { 1, 0, 2 };
    static const int c3[] = { 2, 0, 1 };
    static const int d3[] = { 1, 0, 2 };
    LookupOut *head = NULL, *solo;
    int r;

    require(lookup_out_move(&head, 0, 0) == 0, "空链上移动必须原地不动（返回 0）");
    require(lookup_out_count(head) == 0, "空链移动后还是空链");

    lookup_out_append(&head, out_with_index(0));
    lookup_out_append(&head, out_with_index(1));
    lookup_out_append(&head, out_with_index(2));
    require(lookup_out_count(head) == 3, "三个输出对");
    require_order(head, a3, 3, "初始顺序 0,1,2");

    require(lookup_out_move(&head, 0, 1) == 1, "把第 1 项下移一格应当成功");
    require_order(head, b3, 3, "下移一格后 1,0,2");

    require(lookup_out_move(&head, 0, 2) == 1, "首尾互换应当成功");
    require_order(head, c3, 3, "首尾互换后 2,0,1");

    require(lookup_out_move(&head, 2, 0) == 1, "再换回来应当成功");
    require_order(head, d3, 3, "换回来以后 1,0,2");

    require(lookup_out_move(&head, 1, 1) == 0, "自己移到自己必须原地不动");
    require_order(head, d3, 3, "自己移到自己后顺序不变");

    r = lookup_out_move(&head, -1, 0);
    require(r == 0, "下标越界（负）必须原地不动");
    r = lookup_out_move(&head, 0, 3);
    require(r == 0, "下标越界（超出长度）必须原地不动");
    require_order(head, d3, 3, "越界移动后顺序不变");

    /* 只有一项时上移/下移都不该动 */
    solo = NULL;
    lookup_out_append(&solo, out_with_index(7));
    require(lookup_out_move(&solo, 0, 0) == 0, "单项链上移动必须原地不动");
    require(lookup_out_move(&solo, 0, 1) == 0, "单项链越界移动必须原地不动");
    require(lookup_out_count(solo) == 1 && solo->col_index == 7, "单项链保持不变");
    lookup_out_free(solo);

    lookup_out_free(head);
}

/* ------------------------------------------------------- 输出对列表的可用状态 */
static void test_load_out(void) {
    LookupDlgCtx c;
    ParseRule r;
    Template t;
    Ctl list, col, tgt, del, up, down;

    memset(&c, 0, sizeof(c));
    memset(&r, 0, sizeof(r));
    memset(&t, 0, sizeof(t));
    memset(&list, 0, sizeof(list));
    memset(&col, 0, sizeof(col));
    memset(&tgt, 0, sizeof(tgt));
    ncontrols = 0;
    c.hwnd = &root; c.rule = &r; c.tmpl = &t;
    c.hOutList = &list; c.hOutCol = &col; c.hOutTarget = &tgt;
    del = *mkctl(IDC_LK_OUT_DEL, "删除");
    up = *mkctl(IDC_LK_OUT_UP, "上移");
    down = *mkctl(IDC_LK_OUT_DOWN, "下移");
    (void)del; (void)up; (void)down;

    /* 一项都没配：三个按钮和两个下拉都该是灰的 */
    list.sel = -1;
    lookup_load_out(&c);
    require_on(&col, 0, "没有输出对时「取表里的」应当禁用");
    require_on(&tgt, 0, "没有输出对时「填到变量」应当禁用");
    require_on(GetDlgItem(&root, IDC_LK_OUT_DEL), 0, "没有输出对时删除应当禁用");
    require_on(GetDlgItem(&root, IDC_LK_OUT_UP), 0, "没有输出对时上移应当禁用");
    require_on(GetDlgItem(&root, IDC_LK_OUT_DOWN), 0, "没有输出对时下移应当禁用");

    lookup_out_append(&r.lk_outs, out_with_index(0));
    lookup_out_append(&r.lk_outs, out_with_index(1));
    lookup_out_append(&r.lk_outs, out_with_index(2));

    list.sel = 0;                       /* 第一行：只能往下 */
    lookup_load_out(&c);
    require_on(&col, 1, "选中行时「取表里的」应当可用");
    require_on(&tgt, 1, "选中行时「填到变量」应当可用");
    require_on(GetDlgItem(&root, IDC_LK_OUT_DEL), 1, "第一行删除应当可用");
    require_on(GetDlgItem(&root, IDC_LK_OUT_UP), 0, "第一行上移应当禁用");
    require_on(GetDlgItem(&root, IDC_LK_OUT_DOWN), 1, "第一行下移应当可用");

    list.sel = 1;                       /* 中间行：上下都能 */
    lookup_load_out(&c);
    require_on(GetDlgItem(&root, IDC_LK_OUT_UP), 1, "中间行上移应当可用");
    require_on(GetDlgItem(&root, IDC_LK_OUT_DOWN), 1, "中间行下移应当可用");

    list.sel = 2;                       /* 最后一行：只能往上 */
    lookup_load_out(&c);
    require_on(GetDlgItem(&root, IDC_LK_OUT_DEL), 1, "最后一行删除应当可用");
    require_on(GetDlgItem(&root, IDC_LK_OUT_UP), 1, "最后一行上移应当可用");
    require_on(GetDlgItem(&root, IDC_LK_OUT_DOWN), 0, "最后一行下移应当禁用");

    lookup_out_free(r.lk_outs);
}

/* --------------------------------------------------- 键来源 / 命中模式的门控 */
static void test_sync_enable(void) {
    LookupDlgCtx c;
    ParseRule r;
    Template t;
    Ctl kvLbl, kv, kfLbl, kf, sepLbl, sep, uniq, hit;
    int m, h;

    memset(&c, 0, sizeof(c));
    memset(&r, 0, sizeof(r));
    memset(&t, 0, sizeof(t));
    memset(&kvLbl, 0, sizeof(kvLbl));
    memset(&kv, 0, sizeof(kv));
    memset(&kfLbl, 0, sizeof(kfLbl));
    memset(&kf, 0, sizeof(kf));
    memset(&sepLbl, 0, sizeof(sepLbl));
    memset(&sep, 0, sizeof(sep));
    memset(&uniq, 0, sizeof(uniq));
    memset(&hit, 0, sizeof(hit));
    c.rule = &r; c.tmpl = &t;
    c.hKeyVarLbl = &kvLbl; c.hKeyVar = &kv;
    c.hKeyFixLbl = &kfLbl; c.hKeyFix = &kf;
    c.hSepLbl = &sepLbl; c.hSep = &sep; c.hUnique = &uniq; c.hHit = &hit;

    for (m = 0; m < QP_LK_KEYMODE_COUNT; ++m) {
        for (h = 0; h < QP_LK_HIT_COUNT; ++h) {
            int want_var, want_fix, want_all;
            r.lk_key_mode = kLookupKeyModes[m];
            hit.sel = lookup_hit_index(kLookupHits[h]);
            lookup_sync_enable(&c);

            want_var = (kLookupKeyModes[m] == LOOKUP_KEY_VAR);
            want_fix = (kLookupKeyModes[m] == LOOKUP_KEY_FIX);
            want_all = (kLookupHits[h] == LOOKUP_ALL);

            require_on(&kvLbl, want_var, "「键的变量」标签跟着键来源走");
            require_on(&kv, want_var, "「键的变量」下拉跟着键来源走");
            require_on(&kfLbl, want_fix, "「固定值」标签跟着键来源走");
            require_on(&kf, want_fix, "「固定值」输入框跟着键来源走");
            require_on(&sepLbl, want_all, "「拼接用」标签只在「全部」时可用");
            require_on(&sep, want_all, "「拼接用」输入框只在「全部」时可用");
            require_on(&uniq, want_all, "「重复值只留一个」只在「全部」时可用");
        }
    }
}

/* --------------------------------------------------------- 「填到变量」下拉框 */
static void test_fill_var_combo(void) {
    Ctl combo;
    Template t;
    Segment s1, s2;

    memset(&combo, 0, sizeof(combo));
    combo.sel = CB_ERR;

    /* 模板为空 / 连模板都没有：不许炸，也不许往框里塞东西 */
    fill_var_combo(&combo, NULL, L"", NULL, 0);
    require(combo.count == 0, "模板为空时「填到变量」下拉应当留空");
    fill_var_combo(NULL, NULL, L"", NULL, 0);
    require(1, "下拉框句柄为空时也不许炸");

    memset(&t, 0, sizeof(t));
    memset(&s1, 0, sizeof(s1));
    memset(&s2, 0, sizeof(s2));
    s1.type = SEG_TEXT;
    qp_strlcpy(s1.id, L"s1", QP_ID_MAX);
    qp_strlcpy(s1.label, L"甲", QP_LABEL_MAX);
    s2.type = SEG_STATIC;                 /* 静态片段默认不列出来 */
    qp_strlcpy(s2.id, L"s2", QP_ID_MAX);
    qp_strlcpy(s2.content, L"固定文字", QP_CONTENT_MAX);
    s1.next = &s2;
    s2.next = NULL;
    t.segments = &s1;

    fill_var_combo(&combo, &t, L"s1", NULL, 0);
    require(combo.count == 1, "静态片段默认不进「填到变量」下拉");
    require(combo.sel == 0, "按 id 选中应当落在第 1 项");
    require(combo_selected_seg(&combo) == &s1, "选中项的 item data 应当指回片段");

    fill_var_combo(&combo, &t, L"s2", NULL, 1);
    require(combo.count == 2, "include_static 为 1 时静态片段也要列出来");
    require(combo.sel == 1, "按 id 选中静态片段时应当落在它那一项");
    require(combo_selected_seg(&combo) == &s2, "静态片段也要能取回来");

    /* id 对不上：退到第 1 项，但绝不能是「没选」 */
    fill_var_combo(&combo, &t, L"没有这个变量", NULL, 1);
    require(combo.sel == 0, "id 对不上时退到第 1 项");
}

/* ------------------------------------------------- 换行只读不写 / 重建期间闭嘴 */
static void make_lookup_shell(LookupDlgCtx *c, ParseRule *r, Template *t,
                              Ctl *list, Ctl *col, Ctl *tgt) {
    memset(c, 0, sizeof(*c));
    memset(r, 0, sizeof(*r));
    memset(t, 0, sizeof(*t));
    memset(list, 0, sizeof(*list));
    memset(col, 0, sizeof(*col));
    memset(tgt, 0, sizeof(*tgt));
    ncontrols = 0;
    lv_edits = lv_deletes = lv_selects = 0;
    load_col_calls = 0;
    loading_during_edit = -1;
    list->sel = col->sel = tgt->sel = CB_ERR;
    c->hwnd = &root;
    c->rule = r;
    c->tmpl = t;
    c->hOutList = list;
    c->hOutCol = col;
    c->hOutTarget = tgt;
    mkctl(IDC_LK_OUT_DEL, "删除");
    mkctl(IDC_LK_OUT_UP, "上移");
    mkctl(IDC_LK_OUT_DOWN, "下移");
    g_ctx = c;
}

static LookupOut *out_with(int idx, const wchar_t *col, const wchar_t *tgt) {
    LookupOut *o = lookup_out_create();
    if (!o) return NULL;
    if (col) qp_strlcpy(o->col, col, QP_LABEL_MAX);
    o->col_index = idx;
    if (tgt) qp_strlcpy(o->target_segment_id, tgt, QP_ID_MAX);
    return o;
}

/* 用户点了列表里的另一行：只能把新行读进编辑区，绝不能拿编辑区里
   「上一行留下的值」去覆盖新行 —— 以前 WM_NOTIFY 里顺手提交，就干过这事。 */
static void test_select_row_only_reads(void) {
    LookupDlgCtx c;
    ParseRule r;
    Template t;
    Segment sx;
    Ctl list, col, tgt;
    LookupOut *a, *b;
    NMLISTVIEW nv;

    make_lookup_shell(&c, &r, &t, &list, &col, &tgt);
    memset(&sx, 0, sizeof(sx));
    sx.type = SEG_TEXT;
    qp_strlcpy(sx.id, L"sX", QP_ID_MAX);
    qp_strlcpy(sx.label, L"变量X", QP_LABEL_MAX);
    t.segments = &sx;

    a = out_with(-1, L"列A", L"sA");
    b = out_with(7, L"列B", L"sKEEP");
    lookup_out_append(&r.lk_outs, a);
    lookup_out_append(&r.lk_outs, b);

    /* 第 1 行选中着，编辑区里是它的值（列下拉停在某一列、变量下拉停在 sX） */
    list.sel = 0;
    lookup_load_out(&c);
    col.sel = 3;
    require(tgt.count == 1 && tgt.sel == 0, "编辑区应当停在模板里那个变量上");

    /* 现在点第 2 行 */
    load_col_calls = 0;
    list.sel = 1;
    memset(&nv, 0, sizeof(nv));
    nv.uNewState = LVIS_SELECTED;
    lookup_list_changed(&c, &nv);

    /* 提交的痕迹只会留在模型里：这两项一动就说明「上一行的值被写进这一行」 */
    require(wcscmp(b->target_segment_id, L"sKEEP") == 0,
            "换行不能把上一行的目标变量写进这一行");
    require(wcscmp(b->col, L"列B") == 0 && b->col_index == 7,
            "换行不能把上一行的列写进这一行");
    require(wcscmp(a->target_segment_id, L"sA") == 0 && a->col_index == -1,
            "换行不能动没选中的那一行");
    require(load_col_calls == 1, "换行要把新行的列读进下拉框");

    /* 用户真的改了「填到变量」：提交要写，而且只写选中的那一行 */
    lookup_commit_out(&c);
    require(wcscmp(b->target_segment_id, L"sX") == 0, "提交要写进选中的那一行");
    require(b->col_index == 3 && b->col[0] == L'\0',
            "提交要把下拉框里选的列写进这一行（没有表时按列号存）");
    require(wcscmp(a->target_segment_id, L"sA") == 0 && a->col_index == -1,
            "提交不许碰没选中的那一行");

    /* loading 期间提交必须原地不动 */
    qp_strlcpy(b->target_segment_id, L"sKEEP2", QP_ID_MAX);
    c.loading = 1;
    lookup_commit_out(&c);
    require(wcscmp(b->target_segment_id, L"sKEEP2") == 0, "loading 期间不许提交");

    lookup_out_free(r.lk_outs);
}

/* 重建输出对列表：整段都压在 loading 里，且用完恢复调用方原值 */
static void test_rebuild_holds_loading(void) {
    LookupDlgCtx c;
    ParseRule r;
    Template t;
    Segment sx;
    Ctl list, col, tgt;
    NMLISTVIEW nv;

    make_lookup_shell(&c, &r, &t, &list, &col, &tgt);
    memset(&sx, 0, sizeof(sx));
    sx.type = SEG_TEXT;
    qp_strlcpy(sx.id, L"sX", QP_ID_MAX);
    t.segments = &sx;
    lookup_out_append(&r.lk_outs, out_with(-1, L"列A", L"sA"));
    lookup_out_append(&r.lk_outs, out_with(-1, L"列B", L"sB"));

    lv_deletes = lv_selects = lv_edits = 0;
    loading_during_edit = -1;
    lookup_fill_outs(&c, 1);

    require(lv_deletes == 1, "重建列表要先清空");
    require(lv_selects == 1, "重建列表要选中指定行");
    require(lv_edits == 4, "两行 × 两列都要写一遍");
    require(loading_during_edit == 1,
            "重建过程中 c->loading 必须是 1，否则列表的通知会被当成用户操作");
    require(wcscmp(r.lk_outs->col, L"列A") == 0 &&
            wcscmp(r.lk_outs->next->col, L"列B") == 0 &&
            wcscmp(r.lk_outs->next->target_segment_id, L"sB") == 0,
            "重建列表不能顺手提交（编辑区里的值不许写回模型）");
    require(c.loading == 0, "用完要把 loading 恢复成调用方原值（这里本来是 0）");
    require(list.sel == 1, "重建后应当选中指定的那一行");

    /* 被外层包着时（loading 本来就是 1）要恢复成 1，不能抹成 0 */
    c.loading = 1;
    lookup_fill_outs(&c, 0);
    require(c.loading == 1, "嵌套调用后 loading 必须恢复外层原值，不能被写成 0");

    /* loading 期间的通知：既不许读也不许写 */
    c.loading = 1;
    load_col_calls = 0;
    list.sel = 0;
    memset(&nv, 0, sizeof(nv));
    nv.uNewState = LVIS_SELECTED;
    lookup_list_changed(&c, &nv);
    require(load_col_calls == 0 &&
            wcscmp(r.lk_outs->target_segment_id, L"sA") == 0,
            "loading 期间的通知必须整条忽略（不能在列表自己的通知里反过来动列表）");

    /* 不是「变成选中」的通知也不算换行 */
    c.loading = 0;
    load_col_calls = 0;
    memset(&nv, 0, sizeof(nv));
    nv.uOldState = LVIS_SELECTED;
    lookup_list_changed(&c, &nv);
    require(load_col_calls == 0, "取消选中不是换行，不该刷新编辑区");

    memset(&nv, 0, sizeof(nv));
    nv.uNewState = LVIS_SELECTED;
    nv.uOldState = LVIS_SELECTED;
    lookup_list_changed(&c, &nv);
    require(load_col_calls == 0, "本来就选着的那一行再发通知，不该重复刷新");

    /* 真正换行时才读一次 */
    list.sel = 0;
    memset(&nv, 0, sizeof(nv));
    nv.uNewState = LVIS_SELECTED;
    lookup_list_changed(&c, &nv);
    require(load_col_calls == 1, "换行要把新行读进编辑区");

    lookup_out_free(r.lk_outs);
}

/* 表句柄是「借用」的：缓存一共 4 个槽，别处一取别的表就可能把这一张换掉。
   对话框必须「用前重新取」，不能抱着一个借来的指针用一整场——点击时用的是
   已释放的内存，就是那种「点一下直接闪退」。 */
static void test_borrow_table(void) {
    LookupDlgCtx c;
    ParseRule r;
    Template t;
    Ctl list, col, tgt;
    DataTableRef *ref;
    LookupOut *o;
    wchar_t pa[QP_PATH_MAX], pb[QP_PATH_MAX];
    QpTable *first;

    swprintf(pa, QP_PATH_MAX, L"%ls/qp_lookup_borrow_a.csv", QP_TMPDIR);
    swprintf(pb, QP_PATH_MAX, L"%ls/qp_lookup_borrow_b.csv", QP_TMPDIR);
    pa[QP_PATH_MAX - 1] = L'\0';
    pb[QP_PATH_MAX - 1] = L'\0';
    require(write_text_file(pa, "\xE7\x94\xB2,\xE4\xB9\x99,\xE4\xB8\x99\n1,2,3\n"),
            "写出第一张表（表头：甲,乙,丙）");
    require(write_text_file(pb, "\xE4\xB8\x80,\xE4\xBA\x8C,\xE4\xB8\x89\n7,8,9\n"),
            "写出第二张表（表头：一,二,三）");

    ref = dtref_create(L"借用测试", pa);
    if (!ref) { printf("FAIL: 建不出数据表引用\n"); ++failures; return; }
    qp_table_set_refs(ref);

    make_lookup_shell(&c, &r, &t, &list, &col, &tgt);
    qp_strlcpy(r.lk_table_id, ref->id, QP_ID_MAX);

    first = qp_table_get(ref->id, NULL, 0);
    require(first != NULL && qp_table_cols(first) == 3, "第一张表能读出来");
    c.tbl = first;                     /* 对话框手里那个「借来的」指针 */

    /* 别处把这张表的文件换掉、并让缓存作废：c.tbl 现在是已释放的指针 */
    qp_strlcpy(ref->path, pb, QP_PATH_MAX);
    qp_table_invalidate(ref->id);

    /* 「添加一项」默认取第 2 列，所以列名必须是第二张表的「二」 */
    printf("  借用测试：缓存换过之后点「添加一项」…\n");
    lookup_out_add(&c);
    o = lookup_out_at(r.lk_outs, 0);
    require(o != NULL, "添加一项要真的加一项");
    if (o)
        require(wcscmp(o->col, L"二") == 0,
                "缓存被换过之后，添加一项必须重新取表，不能抱着借来的旧指针");

    lookup_out_free(r.lk_outs);
    qp_table_invalidate(ref->id);
    qp_table_set_refs(NULL);
    dtref_list_free(ref);
}

int main(void) {
    /* 出内存错误时进程会直接死掉，缓冲没刷就什么都看不到——所以不缓冲 */
    setvbuf(stdout, NULL, _IONBF, 0);
    test_move();
    test_load_out();
    test_sync_enable();
    test_fill_var_combo();
    test_select_row_only_reads();
    test_rebuild_holds_loading();
    test_borrow_table();
    if (!failures)
        printf("lookup dialog interaction checks passed\n");
    return failures ? 1 : 0;
}
'''

code = (preamble + consts + lookup_ctx + "\n" + preamble_ui + "\n" +
        helpers + tested + driver)
with tempfile.TemporaryDirectory(prefix="qp-lookup-interaction-") as tmp:
    exe = pathlib.Path(tmp) / "interaction"
    result = subprocess.run(
        ["cc", "-std=gnu11", "-O0", "-Wall", "-Wextra", *INCLUDE_FLAGS,
         "-x", "c", "-",
         str(repo / "src/core/table.c"), *CORE_SOURCES,
         str(repo / "src/core/storage.c"), str(repo / "third_party/cjson/cJSON.c"),
         str(repo / "src/core/regex.c"), "-o", str(exe)],
        input=code, text=True, capture_output=True)
    if result.stderr.strip():
        sys.stderr.write(result.stderr)
    if result.returncode:
        sys.exit(result.returncode)
    sys.exit(subprocess.run([str(exe)]).returncode)
