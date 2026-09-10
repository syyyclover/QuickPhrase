/*
 * ui_dialogs.c —— 模板编辑器、解析/条件规则编辑器、设置、输入框、关于
 *
 * 这些窗口不使用 .rc 里的对话框模板，而是用普通窗口 + 手工布局，
 * 这样可以按 DPI 缩放、按片段类型动态显隐属性行，并统一套用深浅主题。
 */
#include "ui.h"
#include "resource.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <shellapi.h>
#include <shlobj.h>

#ifndef EM_SETCUEBANNER
#  define EM_SETCUEBANNER 0x1501
#endif
#ifndef LVS_EX_DOUBLEBUFFER
#  define LVS_EX_DOUBLEBUFFER 0x00010000
#endif

/* ================================================================== */
/* 通用模态窗口骨架                                                    */
/* ================================================================== */

static void ensure_class(const wchar_t *cls, WNDPROC proc)
{
    WNDCLASSEXW wc;
    if (GetClassInfoExW(g_hInst, cls, &wc)) return;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;         /* 双击分隔条恢复默认高度（模板编辑器） */
    wc.lpfnWndProc = proc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = NULL;
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);
}

/* 创建一个以 owner 为父的模态窗口，客户区大小为 cw × ch（逻辑像素）。
   extra 里可以再加 WS_THICKFRAME 之类（模板编辑器要能拉大）。 */
static HWND make_dlg_ex(const wchar_t *cls, WNDPROC proc, const wchar_t *title,
                        int cw, int ch, HWND owner, void *param, DWORD extra)
{
    RECT rc;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN | extra;
    DWORD ex = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
    HWND hwnd;

    /* 可缩放的窗口用普通边框：WS_EX_DLGMODALFRAME 的双线边框与 WS_THICKFRAME
       叠在一起时，拉伸边其实是那圈粗框，视觉上却像细框，很难瞄准 */
    if (extra & WS_THICKFRAME) ex &= ~(DWORD)WS_EX_DLGMODALFRAME;
    ensure_class(cls, proc);
    rc.left = 0; rc.top = 0; rc.right = DPIX(cw); rc.bottom = DPIX(ch);
    AdjustWindowRectEx(&rc, style, FALSE, ex);
    hwnd = CreateWindowExW(ex, cls, title, style, 0, 0,
                           rc.right - rc.left, rc.bottom - rc.top,
                           owner, NULL, g_hInst, param);
    if (!hwnd) return NULL;
    qp_center_window(hwnd, owner);
    return hwnd;
}

static HWND make_dlg(const wchar_t *cls, WNDPROC proc, const wchar_t *title,
                     int cw, int ch, HWND owner, void *param)
{
    return make_dlg_ex(cls, proc, title, cw, ch, owner, param, 0);
}

/*
 * 模态窗口的关闭顺序很讲究：必须先把 owner 恢复可用并接管激活，再销毁自己。
 * 否则销毁的那一刻整条 owner 链还都是 disabled 状态，Windows 只能把激活权
 * 临时丢给 Z 序里的下一个窗口（嵌套对话框时就是主界面），随后又被抢回来，
 * 视觉上就是「主界面闪一下」。
 *
 * 所以所有退出路径（确定 / 取消 / 标题栏关闭 / Esc / Alt+F4）都走 dlg_close，
 * 并在 dlg_common 的 WM_DESTROY 里再兜一次底（例如 owner 被销毁时连带销毁）。
 */
static void dlg_release_owner(HWND hwnd)
{
    HWND owner = GetWindow(hwnd, GW_OWNER);
    if (!owner || !IsWindow(owner)) return;
    EnableWindow(owner, TRUE);
    if (GetActiveWindow() == hwnd) SetActiveWindow(owner);
}

/* 关闭模态窗口：先「不改变激活地」隐藏自己，再把激活权交还 owner，最后销毁 */
static void dlg_close(HWND hwnd)
{
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                 SWP_HIDEWINDOW | SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    dlg_release_owner(hwnd);
    DestroyWindow(hwnd);
}

static int run_modal(HWND owner, HWND dlg, const int *result)
{
    int r;
    if (!dlg) return 0;
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);
    r = qp_modal_loop(owner, dlg, result);
    /* 正常路径下 WM_DESTROY 已经把 owner 恢复并激活，这里只是兜底 */
    if (owner && IsWindow(owner)) {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }
    return r;
}

static LRESULT dlg_common(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, int *handled)
{
    *handled = 0;
    switch (msg) {
    case WM_DESTROY:
        dlg_release_owner(hwnd);
        break;                       /* 不吞消息，继续交给 DefWindowProc */
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, g_brushBk);
        *handled = 1;
        return 1;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
        return qp_handle_ctlcolor(msg, wp, lp, handled);
    default:
        break;
    }
    return 0;
}

/* 常用小控件 */
static HWND mk_label(HWND p, const wchar_t *text, int id)
{
    return qp_create(L"Static", text, SS_LEFT | SS_ENDELLIPSIS, 0, 0, 0, 10, 10, p, id);
}
static HWND mk_label_r(HWND p, const wchar_t *text, int id)
{
    return qp_create(L"Static", text, SS_RIGHT | SS_ENDELLIPSIS, 0, 0, 0, 10, 10, p, id);
}
static HWND mk_edit(HWND p, int id, DWORD extra)
{
    return qp_create(L"Edit", L"", ES_AUTOHSCROLL | WS_TABSTOP | extra,
                     WS_EX_CLIENTEDGE, 0, 0, 10, 10, p, id);
}
static HWND mk_btn(HWND p, const wchar_t *text, int id)
{
    return qp_create(L"Button", text, BS_PUSHBUTTON | WS_TABSTOP, 0,
                     0, 0, 10, 10, p, id);
}
static HWND mk_check(HWND p, const wchar_t *text, int id)
{
    return qp_create(L"Button", text, BS_AUTOCHECKBOX | WS_TABSTOP, 0,
                     0, 0, 10, 10, p, id);
}
static HWND mk_combo(HWND p, int id)
{
    return qp_create(L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0,
                     0, 0, 10, DPIX(260), p, id);
}
/* 可下拉也可自己敲的组合框（时间格式：预置几个够用的，剩下的让人自己写） */
static HWND mk_combo_edit(HWND p, int id)
{
    return qp_create(L"ComboBox", L"",
                     CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP, 0,
                     0, 0, 10, DPIX(220), p, id);
}
static HWND mk_list(HWND p, int id, DWORD extra)
{
    HWND h = qp_create(WC_LISTVIEWW, L"",
                       LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP | extra,
                       WS_EX_CLIENTEDGE, 0, 0, 10, 10, p, id);
    if (h) {
        ListView_SetExtendedListViewStyle(h, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        qp_theme_list(h);
        if (g_dark) {
            ListView_SetBkColor(h, theme_ctl_bk());
            ListView_SetTextBkColor(h, theme_ctl_bk());
            ListView_SetTextColor(h, theme_text());
        }
    }
    return h;
}
static void lv_add_col(HWND h, int idx, const wchar_t *title, int width)
{
    LVCOLUMNW c;
    memset(&c, 0, sizeof(c));
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    c.iSubItem = idx;
    c.pszText = (LPWSTR)title;
    c.cx = DPIX(width);
    ListView_InsertColumn(h, idx, &c);
}
static void lv_set_row(HWND h, int row, int col, const wchar_t *text)
{
    LVITEMW it;
    memset(&it, 0, sizeof(it));
    it.mask = LVIF_TEXT;
    it.iItem = row;
    it.iSubItem = col;
    it.pszText = (LPWSTR)text;
    if (col == 0) {
        if (row >= ListView_GetItemCount(h)) ListView_InsertItem(h, &it);
        else ListView_SetItem(h, &it);
    } else {
        ListView_SetItem(h, &it);
    }
}
static int lv_sel(HWND h) { return ListView_GetNextItem(h, -1, LVNI_SELECTED); }
static void lv_select(HWND h, int i)
{
    int n = ListView_GetItemCount(h);
    if (n == 0) return;
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    ListView_SetItemState(h, i, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(h, i, FALSE);
}

/* ================================================================== */
/* 简单输入对话框                                                      */
/* ================================================================== */

typedef struct {
    HWND hwnd, hEdit;
    const wchar_t *prompt;
    wchar_t *buf;
    size_t cap;
    int result;
    int multiline;
} InputCtx;

static LRESULT CALLBACK input_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    InputCtx *c = (InputCtx *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    int handled = 0;
    LRESULT r;

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        RECT rc;
        int m = DPIX(14), w;
        HWND lbl;
        c = (InputCtx *)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)c);
        c->hwnd = hwnd;
        GetClientRect(hwnd, &rc);
        w = rc.right - m * 2;

        lbl = mk_label(hwnd, c->prompt, 0);
        c->hEdit = mk_edit(hwnd, IDC_INPUT_EDIT,
                           c->multiline ? (ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL) : 0);
        mk_btn(hwnd, L"确定", IDOK);
        mk_btn(hwnd, L"取消", IDCANCEL);

        MoveWindow(lbl, m, m, w, DPIX(18), TRUE);
        MoveWindow(c->hEdit, m, m + DPIX(24), w,
                   c->multiline ? DPIX(72) : DPIX(24), TRUE);
        MoveWindow(GetDlgItem(hwnd, IDOK), rc.right - m - DPIX(180), rc.bottom - m - DPIX(26),
                   DPIX(86), DPIX(26), TRUE);
        MoveWindow(GetDlgItem(hwnd, IDCANCEL), rc.right - m - DPIX(88), rc.bottom - m - DPIX(26),
                   DPIX(86), DPIX(26), TRUE);
        SetWindowTextW(c->hEdit, c->buf);
        SendMessageW(GetDlgItem(hwnd, IDOK), BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);
        qp_theme_children(hwnd);
        SetFocus(c->hEdit);
        SendMessageW(c->hEdit, EM_SETSEL, 0, -1);
        return 0;
    }
    case WM_COMMAND:
        if (!c) break;
        if (LOWORD(wp) == IDOK) {
            qp_get_text(c->hEdit, c->buf, c->cap);
            c->result = 1;
            dlg_close(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) {
            c->result = 0;
            dlg_close(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (c) c->result = 0;
        dlg_close(hwnd);
        return 0;
    default:
        break;
    }
    r = dlg_common(hwnd, msg, wp, lp, &handled);
    if (handled) return r;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int dlg_input_ex(HWND owner, const wchar_t *title, const wchar_t *prompt,
                        wchar_t *buf, size_t cap, int multiline)
{
    InputCtx c;
    HWND dlg;
    memset(&c, 0, sizeof(c));
    c.prompt = prompt;
    c.buf = buf;
    c.cap = cap;
    c.multiline = multiline;
    dlg = make_dlg(L"QuickPhraseInput", input_proc, title,
                   360, multiline ? 170 : 116, owner, &c);
    return run_modal(owner, dlg, &c.result);
}

int dlg_input(HWND owner, const wchar_t *title, const wchar_t *prompt,
              wchar_t *buf, size_t cap)
{
    return dlg_input_ex(owner, title, prompt, buf, cap, 0);
}

/* ================================================================== */
/* 关于                                                                */
/* ================================================================== */

void dlg_about(HWND owner)
{
    wchar_t dir[600] = L"";
    storage_get_data_dir(dir, 600);
    qp_msgbox(owner, MB_OK | MB_ICONINFORMATION, L"关于 口径生成器",
              L"%ls  QuickPhrase  v%ls\n\n"
              L"面向重复性文字工作的口径生成工具。\n"
              L"C11 + Win32 API 实现，单文件绿色程序，无运行时依赖。\n\n"
              L"全局热键：唤出主界面（可在「设置」中修改）\n"
              L"Ctrl+Enter：一键复制    Ctrl+F：搜索模板\n"
              L"Ctrl+N：新建模板        Ctrl+E：编辑模板\n\n"
              L"数据目录：\n%ls",
              QP_APP_TITLE, QP_APP_VERSION, dir);
}

/* ================================================================== */
/* 解析规则编辑器                                                      */
/* ================================================================== */

typedef struct {
    HWND hwnd;
    ParseRule *rule;             /* 直接编辑的对象（调用方负责副本语义） */
    const Template *tmpl;
    const Segment *owner_seg;
    int result;
    HWND hType, hDelim, hIndex, hKeyword, hMarkerAfter, hTarget, hFill, hPara, hTip;
    HWND hDelimLbl, hIndexLbl, hKeywordLbl, hMarkerAfterLbl;
} ParseDlgCtx;

/* 把模板里可作为目标的变量填进下拉框，选中 sel_id */
static void fill_var_combo(HWND combo, const Template *t, const wchar_t *sel_id,
                           const Segment *exclude, int include_static)
{
    const Segment *s;
    int i = 0, sel = -1;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (s = t->segments; s; s = s->next) {
        wchar_t item[QP_LABEL_MAX + 32];
        if (s == exclude) continue;
        if (s->type == SEG_STATIC && !include_static) continue;
        _snwprintf(item, QP_LABEL_MAX + 32, L"%ls", seg_display_name(s));
        item[QP_LABEL_MAX + 31] = L'\0';
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)item);
        SendMessageW(combo, CB_SETITEMDATA, (WPARAM)i, (LPARAM)s);
        if (sel_id && wcscmp(s->id, sel_id) == 0) sel = i;
        ++i;
    }
    if (sel < 0 && i > 0) sel = 0;
    if (sel >= 0) SendMessageW(combo, CB_SETCURSEL, (WPARAM)sel, 0);
}

static const Segment *combo_selected_seg(HWND combo)
{
    int idx = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (idx == CB_ERR) return NULL;
    return (const Segment *)SendMessageW(combo, CB_GETITEMDATA, (WPARAM)idx, 0);
}

static void parse_dlg_sync_enable(ParseDlgCtx *c)
{
    int sel = (int)SendMessageW(c->hType, CB_GETCURSEL, 0, 0);
    int is_split = (sel == 0), is_between = (sel == 2);
    /* 分隔符 / 第几段：只有「按分隔符切割」用得上 */
    EnableWindow(c->hDelim, is_split);
    EnableWindow(c->hIndex, is_split);
    EnableWindow(c->hDelimLbl, is_split);
    EnableWindow(c->hIndexLbl, is_split);
    /* 后标记：只有「取两标记之间」用得上 */
    EnableWindow(c->hMarkerAfter, is_between);
    EnableWindow(c->hMarkerAfterLbl, is_between);
    /* 关键词一栏三种规则都有意义：分隔=可选命中词、包含=必填词、取两标记之间=前标记。
       不禁用，只按类型改标签与提示语。 */
    SetWindowTextW(c->hKeywordLbl, is_between ? L"前标记：" : L"命中关键词：");
    SendMessageW(c->hKeyword, EM_SETCUEBANNER, TRUE,
                 (LPARAM)(is_between ? L"留空表示从头开始取"
                                     : L"留空表示只要取到内容就算命中"));
    SetWindowTextW(c->hTip,
        is_between
          ? L"例：源里「网络设备」之后、「经核查」之前 → 取出设备名填到目标变量。"
            L"勾「逐段」可对多段话逐段各取一次、结果分行。"
        : is_split
          ? L"例：设备编号按「-」切割取第 8 段，命中「DBS」→ 设备类型 填「存储服务器」。"
          : L"例：内容里包含「硬件」→ 把目标变量填成指定值。");
}

static LRESULT CALLBACK parse_dlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ParseDlgCtx *c = (ParseDlgCtx *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    int handled = 0;
    LRESULT r;

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        int m = DPIX(14), lw = DPIX(84), y = DPIX(14), rowh = DPIX(28);
        int cw, cx;
        RECT rc;
        HWND lbl[7];
        wchar_t buf[32];

        c = (ParseDlgCtx *)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)c);
        c->hwnd = hwnd;
        GetClientRect(hwnd, &rc);
        cx = m + lw + DPIX(8);
        cw = rc.right - m - cx;

        lbl[0] = mk_label_r(hwnd, L"规则类型：", 0);
        c->hType = mk_combo(hwnd, IDC_RL_TYPE);
        SendMessageW(c->hType, CB_ADDSTRING, 0, (LPARAM)L"按分隔符切割取第 N 段");
        SendMessageW(c->hType, CB_ADDSTRING, 0, (LPARAM)L"包含关键词");
        SendMessageW(c->hType, CB_ADDSTRING, 0, (LPARAM)L"取两标记之间");
        SendMessageW(c->hType, CB_SETCURSEL,
                     c->rule->type == PARSE_CONTAINS ? 1 :
                     c->rule->type == PARSE_BETWEEN ? 2 : 0, 0);

        lbl[1] = c->hDelimLbl = mk_label_r(hwnd, L"分隔符：", 0);
        c->hDelim = mk_edit(hwnd, IDC_RL_DELIM, 0);
        SetWindowTextW(c->hDelim, c->rule->delimiter);

        lbl[2] = c->hIndexLbl = mk_label_r(hwnd, L"取第几段：", 0);
        c->hIndex = mk_edit(hwnd, IDC_RL_INDEX, ES_NUMBER);
        _snwprintf(buf, 32, L"%d", c->rule->segment_index > 0 ? c->rule->segment_index : 1);
        SetWindowTextW(c->hIndex, buf);

        /* 命中关键词 / 前标记（同一控件，按类型改标签） */
        lbl[3] = c->hKeywordLbl = mk_label_r(hwnd, L"命中关键词：", 0);
        c->hKeyword = mk_edit(hwnd, IDC_RL_KEYWORD, 0);
        SetWindowTextW(c->hKeyword, c->rule->keyword);

        /* 后标记：只在「取两标记之间」时可用 */
        lbl[4] = c->hMarkerAfterLbl = mk_label_r(hwnd, L"后标记：", 0);
        c->hMarkerAfter = mk_edit(hwnd, IDC_RL_MARKER_AFTER, 0);
        SetWindowTextW(c->hMarkerAfter, c->rule->marker_after);
        SendMessageW(c->hMarkerAfter, EM_SETCUEBANNER, TRUE, (LPARAM)L"留空表示取到末尾");

        lbl[5] = mk_label_r(hwnd, L"填充到变量：", 0);
        c->hTarget = mk_combo(hwnd, IDC_RL_TARGET);
        fill_var_combo(c->hTarget, c->tmpl, c->rule->target_segment_id, c->owner_seg, 0);

        lbl[6] = mk_label_r(hwnd, L"填充值：", 0);
        c->hFill = mk_edit(hwnd, IDC_RL_FILL, 0);
        SetWindowTextW(c->hFill, c->rule->fill_value);
        SendMessageW(c->hFill, EM_SETCUEBANNER, TRUE, (LPARAM)L"留空表示填入取到的原文");

        /* 逐段处理：自带文字，左侧与输入框对齐，对三种规则都可用 */
        c->hPara = mk_check(hwnd, L"逐段处理：源含多段（换行分隔）时，每段各抽取一次、结果分行",
                            IDC_RL_PARA);
        SendMessageW(c->hPara, BM_SETCHECK,
                     c->rule->per_paragraph ? BST_CHECKED : BST_UNCHECKED, 0);

        c->hTip = mk_label(hwnd, L"", IDC_RL_TIP);
        mk_btn(hwnd, L"确定", IDOK);
        mk_btn(hwnd, L"取消", IDCANCEL);
        SendMessageW(GetDlgItem(hwnd, IDOK), BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);

        {
            HWND ctl[7];
            int k;
            ctl[0] = c->hType;    ctl[1] = c->hDelim;       ctl[2] = c->hIndex;
            ctl[3] = c->hKeyword; ctl[4] = c->hMarkerAfter; ctl[5] = c->hTarget;
            ctl[6] = c->hFill;
            for (k = 0; k < 7; ++k) {
                int iw = (k == 1 || k == 2) ? DPIX(90) : cw;
                MoveWindow(lbl[k], m, y + DPIX(4), lw, DPIX(18), TRUE);
                MoveWindow(ctl[k], cx, y, iw, DPIX(24), TRUE);
                y += rowh;
            }
            MoveWindow(c->hPara, cx, y, cw, DPIX(22), TRUE);
            y += rowh;
        }
        MoveWindow(c->hTip, m, y + DPIX(4), rc.right - m * 2, DPIX(34), TRUE);
        MoveWindow(GetDlgItem(hwnd, IDOK), rc.right - m - DPIX(180),
                   rc.bottom - m - DPIX(26), DPIX(86), DPIX(26), TRUE);
        MoveWindow(GetDlgItem(hwnd, IDCANCEL), rc.right - m - DPIX(88),
                   rc.bottom - m - DPIX(26), DPIX(86), DPIX(26), TRUE);

        parse_dlg_sync_enable(c);
        qp_theme_children(hwnd);
        SetFocus(c->hType);
        return 0;
    }
    case WM_COMMAND:
        qp_combo_dropdown_fix(wp, lp);
        if (!c) break;
        if (LOWORD(wp) == IDC_RL_TYPE && HIWORD(wp) == CBN_SELCHANGE) {
            parse_dlg_sync_enable(c);
            return 0;
        }
        if (LOWORD(wp) == IDOK) {
            wchar_t buf[QP_VALUE_MAX];
            const Segment *target = combo_selected_seg(c->hTarget);
            int sel = (int)SendMessageW(c->hType, CB_GETCURSEL, 0, 0);
            int is_split = (sel == 0), is_contains = (sel == 1), is_between = (sel == 2);

            if (!target) {
                qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"提示",
                          L"请先在模板中添加一个变量作为填充目标。");
                return 0;
            }
            qp_get_text(c->hDelim, buf, QP_VALUE_MAX);
            if (is_split && !buf[0]) {
                qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"提示", L"请填写分隔符。");
                SetFocus(c->hDelim);
                return 0;
            }
            c->rule->type = is_split ? PARSE_SPLIT :
                            is_between ? PARSE_BETWEEN : PARSE_CONTAINS;
            qp_strlcpy(c->rule->delimiter, buf, QP_SEP_MAX);
            qp_get_text(c->hIndex, buf, QP_VALUE_MAX);
            c->rule->segment_index = _wtoi(buf);
            if (c->rule->segment_index <= 0) c->rule->segment_index = 1;
            /* 关键词 = 命中词（分隔/包含）或前标记（取两标记之间） */
            qp_get_text(c->hKeyword, buf, QP_VALUE_MAX);
            qp_trim(buf);
            qp_strlcpy(c->rule->keyword, buf, QP_KEYWORD_MAX);
            if (is_contains && !buf[0]) {
                qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"提示",
                          L"「包含关键词」规则必须填写关键词。");
                SetFocus(c->hKeyword);
                return 0;
            }
            /* 后标记（仅取两标记之间用；其余类型留空不影响） */
            qp_get_text(c->hMarkerAfter, buf, QP_VALUE_MAX);
            qp_trim(buf);
            qp_strlcpy(c->rule->marker_after, buf, QP_KEYWORD_MAX);
            c->rule->per_paragraph =
                (SendMessageW(c->hPara, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
            qp_strlcpy(c->rule->target_segment_id, target->id, QP_ID_MAX);
            qp_get_text(c->hFill, buf, QP_VALUE_MAX);
            qp_strlcpy(c->rule->fill_value, buf, QP_VALUE_MAX);
            c->result = 1;
            dlg_close(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) { c->result = 0; dlg_close(hwnd); return 0; }
        break;
    case WM_CLOSE:
        if (c) c->result = 0;
        dlg_close(hwnd);
        return 0;
    default:
        break;
    }
    r = dlg_common(hwnd, msg, wp, lp, &handled);
    if (handled) return r;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int dlg_parse_rule(HWND owner, ParseRule *rule, const Template *t,
                          const Segment *owner_seg)
{
    ParseDlgCtx c;
    HWND dlg;
    memset(&c, 0, sizeof(c));
    c.rule = rule;
    c.tmpl = t;
    c.owner_seg = owner_seg;
    dlg = make_dlg(L"QuickPhraseParseRule", parse_dlg_proc, L"解析规则",
                   440, 330, owner, &c);
    return run_modal(owner, dlg, &c.result);
}

/* ================================================================== */
/* 条件规则编辑器                                                      */
/*                                                                    */
/* 一条规则 = 若干「条件项」 + 组合方式（全部 / 任意） + 输出内容。      */
/* 列表选中一项，下面的编辑区即时改写它——不再另开一层弹窗。            */
/* ================================================================== */

/* 关系下拉的显示顺序（与枚举值解耦，以后插入新关系不影响存档） */
static const CondOperator kCondOps[] = {
    OP_CONTAINS, OP_EQUALS, OP_STARTS_WITH, OP_ENDS_WITH, OP_IN, OP_EMPTY
};
#define QP_COND_OP_COUNT ((int)(sizeof(kCondOps) / sizeof(kCondOps[0])))

typedef struct {
    HWND hwnd;
    ConditionRule *rule;
    const Template *tmpl;
    const Segment *owner_seg;
    int result;
    int loading;                 /* 1 = 正在往控件里灌值，忽略变更通知 */
    HWND hJoin, hList, hResult;
    HWND hTrigger, hOp, hNegate, hMatch;
    HWND hTrigLbl, hOpLbl, hMatchLbl;
} CondDlgCtx;

static int cond_op_to_index(CondOperator op)
{
    int i;
    for (i = 0; i < QP_COND_OP_COUNT; ++i)
        if (kCondOps[i] == op) return i;
    return 0;
}

static CondOperator cond_op_from_combo(HWND combo)
{
    int i = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (i < 0 || i >= QP_COND_OP_COUNT) return OP_CONTAINS;
    return kCondOps[i];
}

/* 刷新列表里某一行的摘要文字 */
static void cond_row_text(CondDlgCtx *c, int row)
{
    wchar_t txt[QP_MATCH_MAX + QP_LABEL_MAX + 32];
    wchar_t line[QP_MATCH_MAX + QP_LABEL_MAX + 48];
    CondTerm *t = cond_term_at(c->rule->terms, row);
    if (!t) return;
    cond_term_summary(t, c->tmpl, txt, sizeof(txt) / sizeof(txt[0]));
    _snwprintf(line, sizeof(line) / sizeof(line[0]), L"%d.  %ls", row + 1, txt);
    line[sizeof(line) / sizeof(line[0]) - 1] = L'\0';
    lv_set_row(c->hList, row, 0, line);
}

/* 匹配值框的占位提示：跟着「关系」走，一次只说一件事 */
static const wchar_t *cond_cue_for(CondOperator op)
{
    switch (op) {
    case OP_EQUALS:      return L"填与变量完全一样的值，例如：数据库服务器";
    case OP_STARTS_WITH: return L"填开头的文字，例如：数据库";
    case OP_ENDS_WITH:   return L"填结尾的文字，例如：服务器";
    case OP_IN:          return L"填一个值即可，多个值请用「添加条件」逐条添加";
    case OP_EMPTY:       return L"「为空」不需要匹配值";
    default:             return L"填变量里出现的文字，例如：数据库";
    }
}

static void cond_sync_enable(CondDlgCtx *c)
{
    int on = cond_term_at(c->rule->terms, lv_sel(c->hList)) != NULL;
    int needs = on && cond_op_needs_value(cond_op_from_combo(c->hOp));
    SendMessageW(c->hMatch, EM_SETCUEBANNER, TRUE,
                 (LPARAM)cond_cue_for(cond_op_from_combo(c->hOp)));
    EnableWindow(c->hTrigLbl, on);
    EnableWindow(c->hTrigger, on);
    EnableWindow(c->hOpLbl, on);
    EnableWindow(c->hOp, on);
    EnableWindow(c->hNegate, on);
    EnableWindow(c->hMatchLbl, needs);
    EnableWindow(c->hMatch, needs);
    /* 至少留一个条件项，避免出现永远不会命中的空规则 */
    EnableWindow(GetDlgItem(c->hwnd, IDC_RL_TERM_DEL),
                 on && cond_term_count(c->rule->terms) > 1);
}

/* 把选中的条件项灌进编辑区 */
static void cond_load_term(CondDlgCtx *c)
{
    int row = lv_sel(c->hList);
    CondTerm *t = cond_term_at(c->rule->terms, row);

    c->loading = 1;
    if (t) {
        fill_var_combo(c->hTrigger, c->tmpl, t->trigger_segment_id, c->owner_seg, 0);
        SendMessageW(c->hOp, CB_SETCURSEL, (WPARAM)cond_op_to_index(t->op), 0);
        CheckDlgButton(c->hwnd, IDC_RL_NEGATE, t->negate ? BST_CHECKED : BST_UNCHECKED);
        SetWindowTextW(c->hMatch, t->match_value);
    } else {
        SendMessageW(c->hTrigger, CB_RESETCONTENT, 0, 0);
        SendMessageW(c->hOp, CB_SETCURSEL, 0, 0);
        CheckDlgButton(c->hwnd, IDC_RL_NEGATE, BST_UNCHECKED);
        SetWindowTextW(c->hMatch, L"");
    }
    c->loading = 0;

    /* 新条件项还没选变量：直接采纳下拉框里显示的那个，做到「看到的就是存下的」 */
    if (t && !t->trigger_segment_id[0]) {
        const Segment *s = combo_selected_seg(c->hTrigger);
        if (s) {
            qp_strlcpy(t->trigger_segment_id, s->id, QP_ID_MAX);
            cond_row_text(c, row);
        }
    }
    cond_sync_enable(c);
}

/* 把编辑区的内容写回选中的条件项 */
static void cond_commit_term(CondDlgCtx *c)
{
    int row = lv_sel(c->hList);
    CondTerm *t = cond_term_at(c->rule->terms, row);
    const Segment *trig;
    wchar_t buf[QP_MATCH_MAX];

    if (c->loading || !t) return;
    trig = combo_selected_seg(c->hTrigger);
    qp_strlcpy(t->trigger_segment_id, trig ? trig->id : L"", QP_ID_MAX);
    t->op = cond_op_from_combo(c->hOp);
    t->negate = (IsDlgButtonChecked(c->hwnd, IDC_RL_NEGATE) == BST_CHECKED);
    qp_get_text(c->hMatch, buf, QP_MATCH_MAX);
    qp_trim(buf);
    cond_term_set_match(t, buf);
    cond_row_text(c, row);
}

static void cond_fill_terms(CondDlgCtx *c, int sel)
{
    int n = cond_term_count(c->rule->terms), i;
    ListView_DeleteAllItems(c->hList);
    for (i = 0; i < n; ++i) cond_row_text(c, i);
    if (n > 0) lv_select(c->hList, sel);
    cond_load_term(c);
}

static LRESULT CALLBACK cond_dlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    CondDlgCtx *c = (CondDlgCtx *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    int handled = 0;
    LRESULT r;

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        int m = DPIX(14), y = DPIX(14), rowh = DPIX(28);
        int lw = DPIX(64), cx, cw, full, bw = DPIX(86);
        RECT rc;
        HWND lJoin, lWhen, lSect, lOut, tip, add, del;
        int i;

        c = (CondDlgCtx *)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)c);
        c->hwnd = hwnd;
        GetClientRect(hwnd, &rc);
        full = rc.right - m * 2;
        cx = m + lw + DPIX(8);
        cw = rc.right - m - cx;

        /* ---- 第一行：组合方式 + 增删按钮 ---- */
        lJoin = mk_label(hwnd, L"当以下条件", 0);
        c->hJoin = mk_combo(hwnd, IDC_RL_JOIN);
        SendMessageW(c->hJoin, CB_ADDSTRING, 0, (LPARAM)L"全部满足（且）");
        SendMessageW(c->hJoin, CB_ADDSTRING, 0, (LPARAM)L"任意满足（或）");
        SendMessageW(c->hJoin, CB_SETCURSEL,
                     c->rule->join == COND_JOIN_ANY ? 1 : 0, 0);
        lWhen = mk_label(hwnd, L"时：", 0);
        add = mk_btn(hwnd, L"添加条件", IDC_RL_TERM_ADD);
        del = mk_btn(hwnd, L"删除条件", IDC_RL_TERM_DEL);

        MoveWindow(lJoin, m, y + DPIX(5), DPIX(70), DPIX(18), TRUE);
        MoveWindow(c->hJoin, m + DPIX(74), y, DPIX(126), DPIX(24), TRUE);
        MoveWindow(lWhen, m + DPIX(204), y + DPIX(5), DPIX(28), DPIX(18), TRUE);
        MoveWindow(add, rc.right - m - bw * 2 - DPIX(8), y, bw, DPIX(24), TRUE);
        MoveWindow(del, rc.right - m - bw, y, bw, DPIX(24), TRUE);
        y += DPIX(32);

        /* ---- 条件项列表 ---- */
        c->hList = mk_list(hwnd, IDC_RL_TERMLIST, LVS_NOCOLUMNHEADER);
        lv_add_col(c->hList, 0, L"条件", 300);
        MoveWindow(c->hList, m, y, full, DPIX(116), TRUE);
        ListView_SetColumnWidth(c->hList, 0, full - DPIX(26));
        y += DPIX(126);

        /* ---- 选中条件项的编辑区 ---- */
        lSect = mk_label(hwnd, L"编辑选中的条件：", 0);
        MoveWindow(lSect, m, y, full, DPIX(18), TRUE);
        y += DPIX(24);

        c->hTrigLbl = mk_label_r(hwnd, L"变量：", 0);
        c->hTrigger = mk_combo(hwnd, IDC_RL_TRIGGER);
        MoveWindow(c->hTrigLbl, m, y + DPIX(4), lw, DPIX(18), TRUE);
        MoveWindow(c->hTrigger, cx, y, cw, DPIX(24), TRUE);
        y += rowh;

        c->hOpLbl = mk_label_r(hwnd, L"关系：", 0);
        c->hOp = mk_combo(hwnd, IDC_RL_OP);
        for (i = 0; i < QP_COND_OP_COUNT; ++i)
            SendMessageW(c->hOp, CB_ADDSTRING, 0, (LPARAM)cond_op_name(kCondOps[i]));
        c->hNegate = mk_check(hwnd, L"不满足时才成立（取反）", IDC_RL_NEGATE);
        MoveWindow(c->hOpLbl, m, y + DPIX(4), lw, DPIX(18), TRUE);
        MoveWindow(c->hOp, cx, y, DPIX(120), DPIX(24), TRUE);
        MoveWindow(c->hNegate, cx + DPIX(132), y + DPIX(2), cw - DPIX(132), DPIX(20), TRUE);
        y += rowh;

        c->hMatchLbl = mk_label_r(hwnd, L"匹配值：", 0);
        c->hMatch = mk_edit(hwnd, IDC_RL_MATCH, 0);
        SendMessageW(c->hMatch, EM_SETCUEBANNER, TRUE,
                     (LPARAM)cond_cue_for(OP_CONTAINS));
        MoveWindow(c->hMatchLbl, m, y + DPIX(4), lw, DPIX(18), TRUE);
        MoveWindow(c->hMatch, cx, y, cw, DPIX(24), TRUE);
        y += DPIX(30);

        tip = mk_label(hwnd,
                       L"要多个值就点「添加条件」；勾选「取反」可表达「不包含 / 不等于」。",
                       IDC_RL_TIP);
        MoveWindow(tip, m, y, full, DPIX(18), TRUE);
        y += DPIX(26);

        /* ---- 输出内容 ---- */
        lOut = mk_label(hwnd, L"命中时，本片段的内容替换为：", 0);
        MoveWindow(lOut, m, y, full, DPIX(18), TRUE);
        y += DPIX(22);
        c->hResult = mk_edit(hwnd, IDC_RL_RESULT,
                             ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | WS_VSCROLL);
        SetWindowTextW(c->hResult, c->rule->result_value);
        MoveWindow(c->hResult, m, y, full, DPIX(64), TRUE);

        mk_btn(hwnd, L"确定", IDOK);
        mk_btn(hwnd, L"取消", IDCANCEL);
        SendMessageW(GetDlgItem(hwnd, IDOK), BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);
        MoveWindow(GetDlgItem(hwnd, IDOK), rc.right - m - DPIX(180),
                   rc.bottom - m - DPIX(26), bw, DPIX(26), TRUE);
        MoveWindow(GetDlgItem(hwnd, IDCANCEL), rc.right - m - DPIX(88),
                   rc.bottom - m - DPIX(26), bw, DPIX(26), TRUE);

        qp_theme_children(hwnd);
        cond_fill_terms(c, 0);
        SetFocus(c->hList);
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (c && nh && nh->idFrom == IDC_RL_TERMLIST && nh->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *nv = (NMLISTVIEW *)lp;
            if ((nv->uNewState & LVIS_SELECTED) && !(nv->uOldState & LVIS_SELECTED))
                cond_load_term(c);
        }
        break;
    }
    case WM_COMMAND:
        qp_combo_dropdown_fix(wp, lp);
        if (!c) break;
        switch (LOWORD(wp)) {
        case IDC_RL_JOIN:
            if (HIWORD(wp) == CBN_SELCHANGE)
                c->rule->join = (SendMessageW(c->hJoin, CB_GETCURSEL, 0, 0) == 1)
                                ? COND_JOIN_ANY : COND_JOIN_ALL;
            return 0;
        case IDC_RL_TRIGGER:
            if (HIWORD(wp) == CBN_SELCHANGE) cond_commit_term(c);
            return 0;
        case IDC_RL_OP:
            if (HIWORD(wp) == CBN_SELCHANGE) { cond_commit_term(c); cond_sync_enable(c); }
            return 0;
        case IDC_RL_NEGATE:
            if (HIWORD(wp) == BN_CLICKED) cond_commit_term(c);
            return 0;
        case IDC_RL_MATCH:
            if (HIWORD(wp) == EN_CHANGE) cond_commit_term(c);
            return 0;
        case IDC_RL_TERM_ADD:
            cond_term_append(&c->rule->terms, cond_term_create());
            cond_fill_terms(c, cond_term_count(c->rule->terms) - 1);
            SetFocus(c->hTrigger);
            return 0;
        case IDC_RL_TERM_DEL: {
            int row = lv_sel(c->hList);
            if (row >= 0 && cond_term_count(c->rule->terms) > 1) {
                cond_term_remove_at(&c->rule->terms, row);
                cond_fill_terms(c, row);
            }
            return 0;
        }
        case IDOK: {
            wchar_t buf[QP_RESULT_MAX];
            const CondTerm *t;
            const Segment *sg;
            int i = 0, nvar = 0;

            /* 模板里一个变量都没有，条件规则无从下手（口径要先有变量） */
            for (sg = c->tmpl->segments; sg; sg = sg->next)
                if (sg != c->owner_seg && sg->type != SEG_STATIC) ++nvar;
            if (nvar == 0) {
                qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"提示",
                          L"请先在模板中添加一个变量作为触发条件。");
                return 0;
            }
            if (!c->rule->terms) {
                qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"提示",
                          L"请至少添加一个条件。");
                return 0;
            }
            for (t = c->rule->terms; t; t = t->next, ++i) {
                if (!tmpl_find_segment(c->tmpl, t->trigger_segment_id)) {
                    qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"提示",
                              L"第 %d 个条件还没有选择变量。", i + 1);
                    lv_select(c->hList, i);
                    SetFocus(c->hTrigger);
                    return 0;
                }
                if (cond_op_needs_value(t->op) && !t->match_value[0]) {
                    qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"提示",
                              L"第 %d 个条件（%ls）需要填写匹配值。\n"
                              L"若想表达「这个变量是空的」，请把关系改成「为空」。",
                              i + 1, cond_op_name(t->op));
                    lv_select(c->hList, i);
                    SetFocus(c->hMatch);
                    return 0;
                }
            }
            qp_get_text(c->hResult, buf, QP_RESULT_MAX);
            qp_strlcpy(c->rule->result_value, buf, QP_RESULT_MAX);
            c->result = 1;
            dlg_close(hwnd);
            return 0;
        }
        case IDCANCEL:
            c->result = 0;
            dlg_close(hwnd);
            return 0;
        default:
            break;
        }
        break;
    case WM_CLOSE:
        if (c) c->result = 0;
        dlg_close(hwnd);
        return 0;
    default:
        break;
    }
    r = dlg_common(hwnd, msg, wp, lp, &handled);
    if (handled) return r;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int dlg_cond_rule(HWND owner, ConditionRule *rule, const Template *t,
                         const Segment *owner_seg)
{
    CondDlgCtx c;
    HWND dlg;
    memset(&c, 0, sizeof(c));
    c.rule = rule;
    c.tmpl = t;
    c.owner_seg = owner_seg;
    dlg = make_dlg(L"QuickPhraseCondRule", cond_dlg_proc, L"条件规则",
                   520, 486, owner, &c);
    return run_modal(owner, dlg, &c.result);
}

/* ================================================================== */
/* 模板编辑器                                                          */
/* ================================================================== */

/* 设计尺寸 = 能缩到的最小尺寸；窗口可以往大拉，多出来的空间给列表和预览。
   长模板（上百个片段）在这里编辑时，能拉大 + 预览能拖高才看得过来。 */
#define ED_MIN_W       880
#define ED_MIN_H       690         /* 头部多了「批量」一行（+28），随之抬高下限 */
#define ED_PREVIEW_DEF 46          /* 默认值预览的设计高度 */
#define ED_SPLIT_HIT   DPIX(8)     /* 预览分隔条的命中高度 */

typedef struct {
    HWND hwnd;
    Template *t;
    int is_new;
    int result;
    int loading;                 /* 正在填充控件，忽略 EN_CHANGE */
    int cur;                     /* 当前片段索引 */

    HWND hName, hGroup, hSegLbl, hSegList;
    HWND hBatch, hBatchSrc, hBatchSrcLbl;     /* 批量：源变量按行重复整模板输出 */
    HWND hAddStatic, hAddText, hAddSelect, hAddMulti, hAddTime;
    HWND hSegDel, hSegUp, hSegDown;

    HWND hPropsHost, hPropTitle;
    int propsScroll;             /* 属性区的垂直滚动位置 */
    int propsWheelDelta;
    HWND hTypeLbl, hType;
    HWND hLabelLbl, hLabel;
    HWND hContentLbl, hContent;
    HWND hHint;                       /* 固定内容下方的占位符提示（灰字一行） */
    HWND hPlaceLbl, hPlace;
    HWND hDefLbl, hDef;
    HWND hSepLbl, hSep;
    HWND hFmtLbl, hFmt, hFmtSample;   /* 时间格式 + 实时效果 */
    HWND hHidden;
    HWND hNoOutput;                   /* 参与匹配/引用/占位符，但不进入最终输出 */
    HWND hMultiline;
    HWND hRefLbl, hRef;                       /* 引用哪个变量 */
    HWND hRefModeLbl, hRefMode, hRefA, hRefB; /* 取整体 / 第 N 段 / 第 N 个字起 */

    HWND hOptLbl, hOptList, hOptAdd, hOptEdit, hOptDel, hOptUp, hOptDown;
    HWND hParseLbl, hParseList, hParseAdd, hParseEdit, hParseDel, hParseUp, hParseDown;
    HWND hCondLbl, hCondList, hCondAdd, hCondEdit, hCondDel, hCondUp, hCondDown;

    HWND hPreviewLbl, hPreview;
    int  splitDrag;              /* 正在拖预览分隔条 */
    int  splitGrabDy;            /* 按下点相对分隔条上沿的偏移 */
    int  splitLastY;             /* 上次画出的分隔条位置，-1 = 还没画过 */
} EditorCtx;

static void ed_layout(EditorCtx *c);

/* ------------------- 编辑器的可变几何（与主窗口同一套做法） -------- */

/* 同一条底边供布局、高度夹取和拖动反算使用。 */
static int ed_preview_bottom(int H)
{
    return H - DPIX(12) - DPIX(26) - DPIX(8);
}

static int ed_splitter_base(int H)
{
    return ed_preview_bottom(H) - DPIX(18) - ED_SPLIT_HIT;
}

/* 默认值预览的高度区间：上限按「上半部分至少留 DPIX(180)」反推 */
static void ed_preview_bounds(int H, int *lo, int *hi)
{
    int minH = DPIX(32);
    int maxH = ed_splitter_base(H) - DPIX(130) - DPIX(180);
    if (maxH < minH) maxH = minH;
    *lo = minH;
    *hi = maxH;
}

/* 0 = 用设计高度。与 ui_preview_height_for 同一纪律：只读夹取，不回写设置 */
static int ed_preview_height_for(int H)
{
    int lo, hi, h = g_settings.editor_preview_height;
    ed_preview_bounds(H, &lo, &hi);
    if (h <= 0) h = DPIX(ED_PREVIEW_DEF);
    if (h < lo) h = lo;
    if (h > hi) h = hi;
    return h;
}

/* 预览分隔条的上沿；也就是上半部分（片段列表 / 属性区）的底线 */
static int ed_splitter_y(int H)
{
    return ed_splitter_base(H) - ed_preview_height_for(H);
}

/* 片段列表宽度 / 属性区左沿；纵向底线由 ed_layout 统一计算。 */
static void ed_metrics(HWND hwnd, int *listw, int *propsx)
{
    RECT rc;
    int extra, lw;
    GetClientRect(hwnd, &rc);
    /* 窗口拉宽时把多出来的宽度按 2:3 分给片段列表和属性区：片段列表要看得见
       摘要，属性区要放得下规则列表，只喂一边都不合适 */
    extra = (rc.right - rc.left) - DPIX(ED_MIN_W);
    if (extra < 0) extra = 0;
    lw = DPIX(332) + extra * 2 / 5;
    if (listw)   *listw = lw;
    if (propsx)  *propsx = DPIX(12) + lw + DPIX(12);
}

/* 命中判定：预览分隔条横贯整个宽度 */
static int ed_split_hit(HWND hwnd, int x, int y)
{
    RECT rc;
    int top;
    GetClientRect(hwnd, &rc);
    top = ed_splitter_y(rc.bottom - rc.top);
    return x >= DPIX(12) && x < rc.right - DPIX(12) &&
           y >= top && y < top + ED_SPLIT_HIT;
}

static Segment *ed_cur_seg(EditorCtx *c)
{
    return seg_list_at(c->t->segments, c->cur);
}

static void ed_update_preview(EditorCtx *c)
{
    wchar_t out[QP_OUTPUT_MAX];
    tmpl_render_defaults(c->t, out, QP_OUTPUT_MAX);
    qp_set_text_if_diff(c->hPreview, out);
}

/* 片段列表里显示的摘要；隐藏 / 多行 / 时间格式 / 引用都带个标记，
   一眼能看出这个变量的形态，不用逐个点开属性区确认 */
static void ed_seg_summary(const Segment *s, const Template *t, wchar_t *out, size_t cap)
{
    wchar_t *p;
    size_t n;
    if (!out || cap == 0) return;
    out[0] = L'\0';
    if (!s) return;
    if (s->type == SEG_STATIC) {
        qp_strlcpy(out, s->content, cap);
    } else {
        _snwprintf(out, cap, L"【%ls】%ls%ls%ls", seg_display_name(s),
                   s->hidden ? L"（隐藏）" : L"",
                   s->no_output ? L"（不计入）" : L"",
                   (s->type == SEG_TEXT && s->multiline) ? L"（多行）" : L"");
        out[cap - 1] = L'\0';
        n = wcslen(out);
        if (s->type == SEG_DATETIME && n + 1 < cap) {
            _snwprintf(out + n, cap - n, L"（%ls）",
                       s->time_format[0] ? s->time_format : qp_time_format_at(0));
            out[cap - 1] = L'\0';
            n = wcslen(out);
        }
        if (s->ref_segment_id[0] && n + 1 < cap) {
            wchar_t sum[256];
            seg_ref_summary(s, t, sum, 256);
            _snwprintf(out + n, cap - n, L"（%ls）", sum);
            out[cap - 1] = L'\0';
        }
    }
    /* 列表里不显示换行 */
    for (p = out; *p; ++p) if (*p == L'\r' || *p == L'\n') *p = L' ';
}

static void ed_fill_seglist(EditorCtx *c, int select_index)
{
    Segment *s;
    int i = 0;
    SendMessageW(c->hSegList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(c->hSegList);
    for (s = c->t->segments; s; s = s->next, ++i) {
        wchar_t num[16], summary[QP_CONTENT_MAX];
        _snwprintf(num, 16, L"%d", i + 1);
        ed_seg_summary(s, c->t, summary, QP_CONTENT_MAX);
        lv_set_row(c->hSegList, i, 0, num);
        lv_set_row(c->hSegList, i, 1, seg_type_name(s->type));
        lv_set_row(c->hSegList, i, 2, summary);
    }
    SendMessageW(c->hSegList, WM_SETREDRAW, TRUE, 0);
    if (i > 0) lv_select(c->hSegList, select_index);
}

static void ed_fill_optlist(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    int i;
    ListView_DeleteAllItems(c->hOptList);
    if (!s) return;
    for (i = 0; i < s->option_count; ++i)
        lv_set_row(c->hOptList, i, 0, s->options[i]);
}

static void ed_fill_rulelists(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    ParseRule *pr;
    ConditionRule *cr;
    int i = 0;

    ListView_DeleteAllItems(c->hParseList);
    ListView_DeleteAllItems(c->hCondList);
    if (!s) return;
    for (pr = s->parse_rules; pr; pr = pr->next, ++i) {
        wchar_t txt[512];
        parse_rule_summary(pr, c->t, txt, 512);
        lv_set_row(c->hParseList, i, 0, txt);
    }
    i = 0;
    for (cr = s->condition_rules; cr; cr = cr->next, ++i) {
        wchar_t txt[512];
        cond_rule_summary(cr, c->t, txt, 512);
        lv_set_row(c->hCondList, i, 0, txt);
    }
}

static void ed_props_scroll_to(EditorCtx *c, int pos)
{
    SCROLLINFO si = {0};
    int maxpos;
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;
    GetScrollInfo(c->hPropsHost, SB_VERT, &si);
    maxpos = si.nMax - (int)si.nPage + 1;
    if (maxpos < 0) maxpos = 0;
    if (pos > maxpos) pos = maxpos;
    if (pos < 0) pos = 0;
    if (pos != c->propsScroll) {
        c->propsScroll = pos;
        ed_layout(c);
    }
}

/* Tab / 程序 SetFocus 到视口外的属性时，把控件滚进视口。 */
static LRESULT CALLBACK ed_prop_focus_proc(HWND hwnd, UINT msg, WPARAM wp,
                                          LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    EditorCtx *c = (EditorCtx *)ref;
    LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
    if (msg == WM_SETFOCUS) {
        RECT view, ctl;
        int dy = 0;
        GetClientRect(c->hPropsHost, &view);
        GetWindowRect(hwnd, &ctl);
        MapWindowPoints(NULL, c->hPropsHost, (POINT *)&ctl, 2);
        if (ctl.bottom > view.bottom) dy = ctl.bottom - view.bottom;
        if (ctl.top - dy < 0) dy = ctl.top;
        if (dy) ed_props_scroll_to(c, c->propsScroll + dy);
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, ed_prop_focus_proc, id);
    return r;
}

static BOOL CALLBACK ed_prop_hook_focus(HWND hwnd, LPARAM lp)
{
    SetWindowSubclass(hwnd, ed_prop_focus_proc, 0, (DWORD_PTR)lp);
    return TRUE;
}

/* 与主窗口字段 host 一样：裁剪子控件、处理滚动，通知和主题交回编辑器。 */
static LRESULT CALLBACK ed_props_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    EditorCtx *c = (EditorCtx *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                         (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
        return 0;
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, g_brushBk);
        return 1;
    }
    case WM_VSCROLL:
        if (c) {
            SCROLLINFO si = {0};
            int pos = c->propsScroll;
            si.cbSize = sizeof(si);
            si.fMask = SIF_ALL;
            GetScrollInfo(hwnd, SB_VERT, &si);
            switch (LOWORD(wp)) {
            case SB_TOP:           pos = 0; break;
            case SB_BOTTOM:        pos = si.nMax; break;
            case SB_LINEUP:        pos -= DPIX(20); break;
            case SB_LINEDOWN:      pos += DPIX(20); break;
            case SB_PAGEUP:        pos -= (int)si.nPage; break;
            case SB_PAGEDOWN:      pos += (int)si.nPage; break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: pos = si.nTrackPos; break;
            default: break;
            }
            ed_props_scroll_to(c, pos);
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (c) {
            int steps;
            UINT lines = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            c->propsWheelDelta += (short)HIWORD(wp);
            steps = c->propsWheelDelta / WHEEL_DELTA;
            c->propsWheelDelta %= WHEEL_DELTA;
            if (lines == WHEEL_PAGESCROLL) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                ed_props_scroll_to(c, c->propsScroll - steps * rc.bottom);
            } else {
                ed_props_scroll_to(c, c->propsScroll - steps * (int)lines * DPIX(20));
            }
        }
        return 0;
    case WM_COMMAND:
    case WM_NOTIFY:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
        return SendMessageW(GetParent(hwnd), msg, wp, lp);
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* host 已被 ed_layout 限定到 [72, bodybot)，这里使用 host 的局部坐标。 */
static void ed_layout_props(EditorCtx *c, int bodybot)
{
    Segment *s = ed_cur_seg(c);
    RECT rc;
    HDWP dwp;
    int x = 0, lw = DPIX(80), viewh = bodybot - DPIX(72);
    int cx, cw, y = DPIX(24) - c->propsScroll;
    int has = s != NULL;
    int t = has ? (int)s->type : -1;
    int show_label = has && t != SEG_STATIC;
    int show_content = has && t == SEG_STATIC;
    int show_place = has && (t == SEG_TEXT || t == SEG_MULTI_SELECT);
    /* 时间变量不给默认值：格式就是它的全部配置，再来个默认值就是第二个真相 */
    int show_def = has && t != SEG_STATIC && t != SEG_DATETIME;
    int show_sep = has && t == SEG_MULTI_SELECT;
    int show_fmt = has && t == SEG_DATETIME;
    int show_hidden = has && t != SEG_STATIC;
    int show_multi = has && t == SEG_TEXT;
    int show_ref = has && t != SEG_STATIC;
    int show_refmode = show_ref && s->ref_segment_id[0] != L'\0';
    int show_refparam = show_refmode && s->ref_mode != REF_WHOLE;
    int show_opt = has && (t == SEG_SELECT || t == SEG_MULTI_SELECT);
    int show_parse = has && t == SEG_TEXT;
    int show_cond = has;
    int right, nblock, blockh, contenth, oldScroll = c->propsScroll;
    SCROLLINFO si = {0};

    if (viewh < 0) viewh = 0;
    GetClientRect(c->hPropsHost, &rc);
    right = rc.right;
    cx = x + lw + DPIX(8);
    cw = right - cx;
    if (cw < DPIX(120)) cw = DPIX(120);
    dwp = BeginDeferWindowPos(48);

#define ED_ROW(lbl, ctl, show, hgt)                                            \
    do {                                                                       \
        ShowWindow((lbl), (show) ? SW_SHOW : SW_HIDE);                          \
        ShowWindow((ctl), (show) ? SW_SHOW : SW_HIDE);                          \
        if (show) {                                                            \
            dwp = qp_defer(dwp, (lbl), x, y + DPIX(4), lw, DPIX(18));            \
            dwp = qp_defer(dwp, (ctl), cx, y, cw, DPIX(hgt));                    \
            y += DPIX(hgt) + DPIX(4);                                          \
        }                                                                      \
    } while (0)

    ShowWindow(c->hPropTitle, has ? SW_SHOW : SW_HIDE);
    dwp = qp_defer(dwp, c->hPropTitle, x, -c->propsScroll, right, DPIX(18));

    ED_ROW(c->hTypeLbl, c->hType, has, 24);
    ED_ROW(c->hLabelLbl, c->hLabel, show_label, 24);
    ED_ROW(c->hContentLbl, c->hContent, show_content, 80);
    /* 占位符用法就一行提示，跟在固定内容下方：固定片段的属性本来就少，这一行
       不挤占别人的位置，也免得用户去文档里找「怎么在一句话里插变量」 */
    ShowWindow(c->hHint, show_content ? SW_SHOW : SW_HIDE);
    if (show_content) {
        dwp = qp_defer(dwp, c->hHint, cx, y, cw, DPIX(18));
        y += DPIX(18) + DPIX(4);
    }
    ED_ROW(c->hPlaceLbl, c->hPlace, show_place, 24);
    ED_ROW(c->hDefLbl, c->hDef, show_def, 24);
    ED_ROW(c->hSepLbl, c->hSep, show_sep, 24);
    ED_ROW(c->hFmtLbl, c->hFmt, show_fmt, 24);
    /* 格式的实时效果：跟在格式框下方，与它左边对齐，改一个字就能看到结果 */
    ShowWindow(c->hFmtSample, show_fmt ? SW_SHOW : SW_HIDE);
    if (show_fmt) {
        dwp = qp_defer(dwp, c->hFmtSample, cx, y, cw, DPIX(18));
        y += DPIX(18) + DPIX(4);
    }

    /* 「隐藏」开关自带文字，不需要左侧标签，与其他控件左对齐即可 */
    ShowWindow(c->hHidden, show_hidden ? SW_SHOW : SW_HIDE);
    if (show_hidden) {
        dwp = qp_defer(dwp, c->hHidden, cx, y, cw, DPIX(22));
        y += DPIX(22) + DPIX(6);
    }
    /* 「不计入输出」与「隐藏」正交：隐藏只是填写界面不显示、照常拼进输出；
       不计入输出则照常参与匹配 / 引用 / 占位符，但不出现在最终口径里。
       对所有非固定变量都可用（拼表格行时源句子与列变量都用它） */
    ShowWindow(c->hNoOutput, show_hidden ? SW_SHOW : SW_HIDE);
    if (show_hidden) {
        dwp = qp_defer(dwp, c->hNoOutput, cx, y, cw, DPIX(22));
        y += DPIX(22) + DPIX(6);
    }
    ShowWindow(c->hMultiline, show_multi ? SW_SHOW : SW_HIDE);
    if (show_multi) {
        dwp = qp_defer(dwp, c->hMultiline, cx, y, cw, DPIX(22));
        y += DPIX(22) + DPIX(6);
    }

    ED_ROW(c->hRefLbl, c->hRef, show_ref, 24);
    /* 「取法」一行：下拉占一半，右边两个小框用占位文字当标签（分隔符 / 第几段，
       或 从第几个字 / 取几个），按取法显隐 —— 不引用时整行都不出现 */
    ShowWindow(c->hRefModeLbl, show_refmode ? SW_SHOW : SW_HIDE);
    ShowWindow(c->hRefMode, show_refmode ? SW_SHOW : SW_HIDE);
    ShowWindow(c->hRefA, show_refparam ? SW_SHOW : SW_HIDE);
    ShowWindow(c->hRefB, show_refparam ? SW_SHOW : SW_HIDE);
    if (show_refmode) {
        int mw = show_refparam ? (cw - DPIX(8)) / 2 : cw;
        dwp = qp_defer(dwp, c->hRefModeLbl, x, y + DPIX(4), lw, DPIX(18));
        dwp = qp_defer(dwp, c->hRefMode, cx, y, mw, DPIX(24));
        if (show_refparam) {
            int px = cx + mw + DPIX(8);
            int pw = (right - px - DPIX(6)) / 2;
            dwp = qp_defer(dwp, c->hRefA, px, y, pw, DPIX(24));
            dwp = qp_defer(dwp, c->hRefB, px + pw + DPIX(6), y,
                           right - px - pw - DPIX(6), DPIX(24));
        }
        y += DPIX(24) + DPIX(4);
    }

    /* 有余量时等分给列表；不足时保持可操作的最小高度，由 host 裁剪并滚动。 */
    nblock = (show_opt ? 1 : 0) + (show_parse ? 1 : 0) + (show_cond ? 1 : 0);
    blockh = DPIX(92);
    if (nblock > 0) {
        blockh = (viewh - (y + c->propsScroll)) / nblock
                   - (DPIX(22) + DPIX(4) + DPIX(8));
        if (blockh < DPIX(56)) blockh = DPIX(56);
    }

    /* 选项 / 规则区：标题行（含右侧按钮）+ 列表 */
#define ED_BLOCK(lbl, list, b1, b2, b3, b4, b5, show, listh)                   \
    do {                                                                       \
        int bw = DPIX(46), sw = DPIX(28), bh = DPIX(22), bx;                    \
        int vis = (show) ? SW_SHOW : SW_HIDE;                                  \
        ShowWindow((lbl), vis); ShowWindow((list), vis);                        \
        ShowWindow((b1), vis); ShowWindow((b2), vis); ShowWindow((b3), vis);    \
        ShowWindow((b4), vis); ShowWindow((b5), vis);                           \
        if (show) {                                                            \
            bx = right - (bw * 3 + sw * 2 + DPIX(8));                          \
            dwp = qp_defer(dwp, (lbl), x, y + DPIX(3), bx - x - DPIX(4), DPIX(18)); \
            dwp = qp_defer(dwp, (b1), bx, y, bw, bh); bx += bw + DPIX(2);        \
            dwp = qp_defer(dwp, (b2), bx, y, bw, bh); bx += bw + DPIX(2);        \
            dwp = qp_defer(dwp, (b3), bx, y, bw, bh); bx += bw + DPIX(2);        \
            dwp = qp_defer(dwp, (b4), bx, y, sw, bh); bx += sw + DPIX(2);        \
            dwp = qp_defer(dwp, (b5), bx, y, sw, bh);                           \
            y += bh + DPIX(4);                                                 \
            dwp = qp_defer(dwp, (list), x, y, right - x, (listh));              \
            y += (listh) + DPIX(8);                                            \
        }                                                                      \
    } while (0)

    ED_BLOCK(c->hOptLbl, c->hOptList, c->hOptAdd, c->hOptEdit, c->hOptDel,
             c->hOptUp, c->hOptDown, show_opt, blockh);
    ED_BLOCK(c->hParseLbl, c->hParseList, c->hParseAdd, c->hParseEdit, c->hParseDel,
             c->hParseUp, c->hParseDown, show_parse, blockh);
    ED_BLOCK(c->hCondLbl, c->hCondList, c->hCondAdd, c->hCondEdit, c->hCondDel,
             c->hCondUp, c->hCondDown, show_cond, blockh);

#undef ED_ROW
#undef ED_BLOCK

    if (dwp) EndDeferWindowPos(dwp);

    contenth = has ? y + c->propsScroll : 0;
    if (c->propsScroll > contenth - viewh) c->propsScroll = contenth - viewh;
    if (c->propsScroll < 0) c->propsScroll = 0;
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMax = contenth > 0 ? contenth - 1 : 0;
    si.nPage = (UINT)viewh;
    si.nPos = c->propsScroll;
    SetScrollInfo(c->hPropsHost, SB_VERT, &si, TRUE);
    GetClientRect(c->hPropsHost, &rc);
    /* 滚动条显隐改变宽度、或内容变短夹回滚动位置时，以最终视口再排一次。
       内容高度不依赖宽度，因此这里最多补排一次。 */
    if (right != rc.right || oldScroll != c->propsScroll) {
        ed_layout_props(c, bodybot);
        return;
    }

    /* 列表列宽随窗口 */
    ListView_SetColumnWidth(c->hOptList, 0, right - x - DPIX(26));
    ListView_SetColumnWidth(c->hParseList, 0, right - x - DPIX(26));
    ListView_SetColumnWidth(c->hCondList, 0, right - x - DPIX(26));
}

/* 时间格式的实时效果：拿此刻的时间套一遍格式，改一个字就能看到结果 */
static void ed_update_fmt_sample(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    wchar_t sample[160], line[192];
    if (!s || s->type != SEG_DATETIME) {
        qp_set_text_if_diff(c->hFmtSample, L"");
        return;
    }
    qp_format_time_now(s->time_format, sample, 160);
    _snwprintf(line, 192, L"效果：%ls", sample);
    line[191] = L'\0';
    qp_set_text_if_diff(c->hFmtSample, line);
}

/* 时间格式：从下拉里选一个，或者自己敲，两条路都落到这里 */
static void ed_commit_format(EditorCtx *c, int from_list)
{
    Segment *s = ed_cur_seg(c);
    wchar_t buf[QP_FORMAT_MAX];
    if (!s || c->loading || s->type != SEG_DATETIME) return;
    buf[0] = L'\0';
    if (from_list) {
        /* CBN_SELCHANGE 时编辑框里还是旧文字，得从列表项取 */
        int sel = (int)SendMessageW(c->hFmt, CB_GETCURSEL, 0, 0);
        if (sel != CB_ERR &&
            SendMessageW(c->hFmt, CB_GETLBTEXTLEN, (WPARAM)sel, 0) < QP_FORMAT_MAX)
            SendMessageW(c->hFmt, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)buf);
    } else {
        qp_get_text(c->hFmt, buf, QP_FORMAT_MAX);
    }
    qp_strlcpy(s->time_format, buf, QP_FORMAT_MAX);
    ed_update_fmt_sample(c);
    ed_update_preview(c);
}

/* 引用来源下拉：第 0 项固定「不引用」，其余是本模板里除自己以外的变量。
   会形成环的来源照样列出来，选中时再拦——列表里凭空少几项，人只会以为是 bug。
   顺手做一件事：来源已经被删掉（悬空）时，把引用清掉，界面与数据保持一个说法。 */
static void ed_fill_ref_combo(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    const Segment *p;
    int i = 1, sel = 0;
    SendMessageW(c->hRef, CB_RESETCONTENT, 0, 0);
    SendMessageW(c->hRef, CB_ADDSTRING, 0, (LPARAM)L"（不引用）");
    SendMessageW(c->hRef, CB_SETITEMDATA, 0, (LPARAM)NULL);
    if (s) {
        for (p = c->t->segments; p; p = p->next) {
            if (p == s || p->type == SEG_STATIC) continue;
            SendMessageW(c->hRef, CB_ADDSTRING, 0, (LPARAM)seg_display_name(p));
            SendMessageW(c->hRef, CB_SETITEMDATA, (WPARAM)i, (LPARAM)p);
            if (wcscmp(s->ref_segment_id, p->id) == 0) sel = i;
            ++i;
        }
        if (s->ref_segment_id[0] && sel == 0) s->ref_segment_id[0] = L'\0';
    }
    SendMessageW(c->hRef, CB_SETCURSEL, (WPARAM)sel, 0);
}

/* 取法下拉 + 两个参数框：按当前取法填内容与占位提示 */
static void ed_load_ref_params(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    int save = c->loading;
    wchar_t buf[32];
    c->loading = 1;
    if (s && s->ref_segment_id[0]) {
        SendMessageW(c->hRefMode, CB_SETCURSEL, (WPARAM)s->ref_mode, 0);
        if (s->ref_mode == REF_SPLIT) {
            SendMessageW(c->hRefA, EM_SETCUEBANNER, 0, (LPARAM)L"分隔符");
            SendMessageW(c->hRefB, EM_SETCUEBANNER, 0, (LPARAM)L"第几段");
            SetWindowTextW(c->hRefA, s->ref_delimiter);
            _snwprintf(buf, 32, L"%d", s->ref_index > 0 ? s->ref_index : 1);
            SetWindowTextW(c->hRefB, buf);
        } else if (s->ref_mode == REF_SUBSTR) {
            SendMessageW(c->hRefA, EM_SETCUEBANNER, 0, (LPARAM)L"从第几个字");
            SendMessageW(c->hRefB, EM_SETCUEBANNER, 0, (LPARAM)L"取几个（空=到末尾）");
            _snwprintf(buf, 32, L"%d", s->ref_start > 0 ? s->ref_start : 1);
            SetWindowTextW(c->hRefA, buf);
            if (s->ref_len > 0) {
                _snwprintf(buf, 32, L"%d", s->ref_len);
                SetWindowTextW(c->hRefB, buf);
            } else {
                SetWindowTextW(c->hRefB, L"");
            }
        } else {
            SetWindowTextW(c->hRefA, L"");
            SetWindowTextW(c->hRefB, L"");
        }
    } else {
        SendMessageW(c->hRefMode, CB_SETCURSEL, 0, 0);
        SetWindowTextW(c->hRefA, L"");
        SetWindowTextW(c->hRefB, L"");
    }
    c->loading = save;
}

/* 批量源变量下拉：第一项固定「（未选）」，其后列出本模板的「文本输入」变量
   —— 多行粘贴只有文本变量装得下。选中当前源；源已删除 / 改成别的类型时回落到
   「（未选）」（保存时再把悬空 id 一并清掉，与引用悬空同一口径）。 */
static void ed_fill_batch_combo(EditorCtx *c)
{
    Segment *s;
    int i = 1, sel = 0;
    SendMessageW(c->hBatchSrc, CB_RESETCONTENT, 0, 0);
    SendMessageW(c->hBatchSrc, CB_ADDSTRING, 0, (LPARAM)L"（未选）");
    SendMessageW(c->hBatchSrc, CB_SETITEMDATA, 0, (LPARAM)NULL);
    for (s = c->t->segments; s; s = s->next) {
        wchar_t item[QP_LABEL_MAX + 32];
        if (s->type != SEG_TEXT) continue;
        _snwprintf(item, QP_LABEL_MAX + 32, L"%ls", seg_display_name(s));
        item[QP_LABEL_MAX + 31] = L'\0';
        SendMessageW(c->hBatchSrc, CB_ADDSTRING, 0, (LPARAM)item);
        SendMessageW(c->hBatchSrc, CB_SETITEMDATA, (WPARAM)i, (LPARAM)s);
        if (c->t->batch_source_id[0] && wcscmp(s->id, c->t->batch_source_id) == 0)
            sel = i;
        ++i;
    }
    SendMessageW(c->hBatchSrc, CB_SETCURSEL, (WPARAM)sel, 0);
}

static void ed_load_props(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    c->loading = 1;
    if (s) {
        SendMessageW(c->hType, CB_SETCURSEL, (WPARAM)s->type, 0);
        SetWindowTextW(c->hLabel, s->label);
        SetWindowTextW(c->hContent, s->content);
        SetWindowTextW(c->hPlace, s->placeholder);
        SetWindowTextW(c->hDef, s->default_value);
        SetWindowTextW(c->hSep, s->separator);
        SendMessageW(c->hHidden, BM_SETCHECK,
                     s->hidden ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(c->hNoOutput, BM_SETCHECK,
                     s->no_output ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(c->hMultiline, BM_SETCHECK,
                     s->multiline ? BST_CHECKED : BST_UNCHECKED, 0);
        SetWindowTextW(c->hFmt, s->time_format);
    } else {
        SendMessageW(c->hType, CB_SETCURSEL, (WPARAM)-1, 0);
        SetWindowTextW(c->hLabel, L"");
        SetWindowTextW(c->hContent, L"");
        SetWindowTextW(c->hPlace, L"");
        SetWindowTextW(c->hDef, L"");
        SetWindowTextW(c->hSep, L"");
        SendMessageW(c->hHidden, BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessageW(c->hNoOutput, BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessageW(c->hMultiline, BM_SETCHECK, BST_UNCHECKED, 0);
        SetWindowTextW(c->hFmt, L"");
    }
    ed_update_fmt_sample(c);
    ed_fill_ref_combo(c);
    ed_fill_batch_combo(c);          /* 变量增删 / 改类型后，源下拉随之刷新 */
    ed_load_ref_params(c);
    ed_fill_optlist(c);
    ed_fill_rulelists(c);
    ed_layout(c);
    c->loading = 0;
}

static void ed_commit_props(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    /* 一个缓冲喂所有属性框：容量取最大的那个（默认值可以是多行大段文字） */
    wchar_t buf[QP_VALUE_MAX > QP_CONTENT_MAX ? QP_VALUE_MAX : QP_CONTENT_MAX];
    if (!s || c->loading) return;
    qp_get_text(c->hLabel, buf, QP_LABEL_MAX);
    qp_strlcpy(s->label, buf, QP_LABEL_MAX);
    qp_get_text(c->hContent, buf, QP_CONTENT_MAX);
    qp_strlcpy(s->content, buf, QP_CONTENT_MAX);
    qp_get_text(c->hPlace, buf, QP_PLACEHOLD_MAX);
    qp_strlcpy(s->placeholder, buf, QP_PLACEHOLD_MAX);
    qp_get_text(c->hDef, buf, QP_VALUE_MAX);
    qp_strlcpy(s->default_value, buf, QP_VALUE_MAX);
    qp_get_text(c->hSep, buf, QP_SEP_MAX);
    qp_strlcpy(s->separator, buf, QP_SEP_MAX);
    if (s->type == SEG_DATETIME) {
        qp_get_text(c->hFmt, buf, QP_FORMAT_MAX);
        qp_strlcpy(s->time_format, buf, QP_FORMAT_MAX);
    }
    s->hidden = (s->type != SEG_STATIC &&
                 SendMessageW(c->hHidden, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    s->no_output = (s->type != SEG_STATIC &&
                    SendMessageW(c->hNoOutput, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    s->multiline = (s->type == SEG_TEXT &&
                    SendMessageW(c->hMultiline, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    ed_update_preview(c);
}

/* 片段列表某行的文字随属性变化刷新 */
static void ed_refresh_cur_row(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    wchar_t summary[QP_CONTENT_MAX];
    if (!s) return;
    ed_seg_summary(s, c->t, summary, QP_CONTENT_MAX);
    lv_set_row(c->hSegList, c->cur, 1, seg_type_name(s->type));
    lv_set_row(c->hSegList, c->cur, 2, summary);
}

/* ------------------------------ 引用 ------------------------------ */

/* 换引用来源。会成环的选择直接拒绝：A 引用 B、B 又引用 A，两边都算不出值，
   与「条件项指向不存在的变量一律不成立」同一个态度——宁可不生效，不能乱生效。 */
static void ed_ref_changed(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    const Segment *pick;
    if (!s || c->loading) return;
    pick = combo_selected_seg(c->hRef);
    if (pick && tmpl_ref_would_cycle(c->t, s, pick->id)) {
        qp_msgbox(c->hwnd, MB_OK | MB_ICONWARNING, L"不能这样引用",
                  L"【%ls】已经（直接或间接）引用了【%ls】，反过来再引用会形成"
                  L"循环，两边都算不出值。", seg_display_name(pick),
                  seg_display_name(s));
        ed_fill_ref_combo(c);            /* 退回原来的选择 */
        return;
    }
    qp_strlcpy(s->ref_segment_id, pick ? pick->id : L"", QP_ID_MAX);
    if (s->ref_segment_id[0]) {
        if (!s->ref_delimiter[0]) qp_strlcpy(s->ref_delimiter, L"-", QP_SEP_MAX);
        if (s->ref_index < 1) s->ref_index = 1;
        if (s->ref_start < 1) s->ref_start = 1;
    }
    ed_load_ref_params(c);
    ed_layout(c);                        /* 「取法」那一行随之出现 / 收起 */
    ed_refresh_cur_row(c);
    ed_update_preview(c);
}

static void ed_ref_mode_changed(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    int sel = (int)SendMessageW(c->hRefMode, CB_GETCURSEL, 0, 0);
    if (!s || c->loading || sel == CB_ERR) return;
    s->ref_mode = (RefMode)sel;
    ed_load_ref_params(c);
    ed_layout(c);                        /* 两个参数框按取法显隐 */
    ed_refresh_cur_row(c);
    ed_update_preview(c);
}

static void ed_ref_param_changed(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    wchar_t buf[64];
    if (!s || c->loading || !s->ref_segment_id[0]) return;
    if (s->ref_mode == REF_SPLIT) {
        qp_get_text(c->hRefA, buf, QP_SEP_MAX);
        qp_strlcpy(s->ref_delimiter, buf, QP_SEP_MAX);
        qp_get_text(c->hRefB, buf, 64);
        s->ref_index = _wtoi(buf);
        if (s->ref_index < 1) s->ref_index = 1;
    } else if (s->ref_mode == REF_SUBSTR) {
        qp_get_text(c->hRefA, buf, 64);
        s->ref_start = _wtoi(buf);
        if (s->ref_start < 1) s->ref_start = 1;
        qp_get_text(c->hRefB, buf, 64);
        s->ref_len = _wtoi(buf);          /* 空 / 0 = 一直取到末尾 */
        if (s->ref_len < 0) s->ref_len = 0;
    }
    ed_refresh_cur_row(c);
    ed_update_preview(c);
}

static void ed_add_segment(EditorCtx *c, SegmentType type)
{
    Segment *s = seg_create(type);
    int at;
    if (!s) return;
    if (type != SEG_STATIC) {
        wchar_t name[QP_LABEL_MAX];
        int n = seg_list_count(c->t->segments) + 1;
        _snwprintf(name, QP_LABEL_MAX, L"变量%d", n);
        name[QP_LABEL_MAX - 1] = L'\0';
        qp_strlcpy(s->label, name, QP_LABEL_MAX);
    }
    if (type == SEG_MULTI_SELECT)
        qp_strlcpy(s->separator, g_settings.default_separator[0]
                                    ? g_settings.default_separator : L"、", QP_SEP_MAX);
    at = c->cur >= 0 ? c->cur + 1 : seg_list_count(c->t->segments);
    seg_list_insert_at(&c->t->segments, s, at);
    c->cur = at;
    ed_fill_seglist(c, at);
    ed_load_props(c);
    ed_update_preview(c);
    if (type == SEG_STATIC) SetFocus(c->hContent);
    else SetFocus(c->hLabel);
}

static void ed_del_segment(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    wchar_t id[QP_ID_MAX];
    if (!s) return;
    if (qp_msgbox(c->hwnd, MB_YESNO | MB_ICONQUESTION, L"删除片段",
                  L"确定删除第 %d 个片段吗？", c->cur + 1) != IDYES)
        return;
    qp_strlcpy(id, s->id, QP_ID_MAX);
    s = seg_list_remove_at(&c->t->segments, c->cur);
    seg_free(s);
    /* 别人引用了这一个的话，引用一并撤掉：悬空引用本来就不生效，
       留着只会让人以为还跟着来源在变（与保存时清悬空引用同一口径） */
    for (s = c->t->segments; s; s = s->next)
        if (wcscmp(s->ref_segment_id, id) == 0) s->ref_segment_id[0] = L'\0';
    if (c->cur >= seg_list_count(c->t->segments)) c->cur = seg_list_count(c->t->segments) - 1;
    ed_fill_seglist(c, c->cur);
    ed_load_props(c);
    ed_update_preview(c);
}

static void ed_move_segment(EditorCtx *c, int delta)
{
    int n = seg_list_count(c->t->segments);
    int to = c->cur + delta;
    if (c->cur < 0 || to < 0 || to >= n) return;
    if (!seg_list_move(&c->t->segments, c->cur, to)) return;
    c->cur = to;
    ed_fill_seglist(c, to);
    ed_load_props(c);
    ed_update_preview(c);
}

static void ed_change_type(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    int sel = (int)SendMessageW(c->hType, CB_GETCURSEL, 0, 0);
    if (!s || sel == CB_ERR || sel == (int)s->type) return;
    s->type = (SegmentType)sel;
    if (s->type == SEG_STATIC) {
        s->hidden = 0;                    /* 固定句本来就不出现在填写界面 */
        s->no_output = 0;                 /* 固定句必须重新参与输出 */
        s->ref_segment_id[0] = L'\0';     /* 固定句没有「值」，引用无从谈起 */
    }
    if (s->type != SEG_TEXT) s->multiline = 0;  /* 多行只对文本输入有意义 */
    if (s->type == SEG_DATETIME) {
        /* 格式就是时间变量的全部配置；留着默认值只会变成第二个真相 */
        if (!s->time_format[0])
            qp_strlcpy(s->time_format, qp_time_format_at(0), QP_FORMAT_MAX);
        s->default_value[0] = L'\0';
    }
    if (s->type != SEG_STATIC && !s->label[0]) {
        wchar_t name[QP_LABEL_MAX];
        _snwprintf(name, QP_LABEL_MAX, L"变量%d", c->cur + 1);
        name[QP_LABEL_MAX - 1] = L'\0';
        qp_strlcpy(s->label, name, QP_LABEL_MAX);
    }
    if (s->type == SEG_MULTI_SELECT && !s->separator[0])
        qp_strlcpy(s->separator, g_settings.default_separator[0]
                                    ? g_settings.default_separator : L"、", QP_SEP_MAX);
    ed_load_props(c);
    ed_refresh_cur_row(c);
    ed_update_preview(c);
}

/* ------------------------------ 选项 ------------------------------ */

static void ed_opt_add(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    wchar_t buf[QP_VALUE_MAX] = L"";
    if (!s) return;
    if (!dlg_input(c->hwnd, L"添加选项", L"选项内容：", buf, QP_VALUE_MAX)) return;
    qp_trim(buf);
    if (!buf[0]) return;
    seg_add_option(s, buf);
    ed_fill_optlist(c);
    lv_select(c->hOptList, s->option_count - 1);
    ed_update_preview(c);
}

static void ed_opt_edit(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    int i = lv_sel(c->hOptList);
    wchar_t buf[QP_VALUE_MAX];
    if (!s || i < 0 || i >= s->option_count) return;
    qp_strlcpy(buf, s->options[i], QP_VALUE_MAX);
    if (!dlg_input(c->hwnd, L"修改选项", L"选项内容：", buf, QP_VALUE_MAX)) return;
    qp_trim(buf);
    if (!buf[0]) return;
    free(s->options[i]);
    s->options[i] = qp_wcsdup(buf);
    ed_fill_optlist(c);
    lv_select(c->hOptList, i);
    ed_update_preview(c);
}

static void ed_opt_del(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    int i = lv_sel(c->hOptList);
    if (!s || i < 0) return;
    seg_remove_option(s, i);
    ed_fill_optlist(c);
    lv_select(c->hOptList, i);
    ed_update_preview(c);
}

static void ed_opt_move(EditorCtx *c, int delta)
{
    Segment *s = ed_cur_seg(c);
    int i = lv_sel(c->hOptList);
    if (!s || i < 0) return;
    if (!seg_move_option(s, i, delta)) return;
    ed_fill_optlist(c);
    lv_select(c->hOptList, i + delta);
    ed_update_preview(c);
}

/* --------------------------- 解析规则 ----------------------------- */

static void ed_parse_add(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    ParseRule *r;
    if (!s) return;
    r = parse_rule_create();
    if (!r) return;
    qp_strlcpy(r->delimiter, L"-", QP_SEP_MAX);
    r->segment_index = 1;
    if (dlg_parse_rule(c->hwnd, r, c->t, s)) {
        parse_rule_append(&s->parse_rules, r);
        ed_fill_rulelists(c);
        lv_select(c->hParseList, parse_rule_count(s->parse_rules) - 1);
        ed_update_preview(c);
    } else {
        parse_rule_free(r);
    }
}

static void ed_parse_edit(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    int i = lv_sel(c->hParseList);
    ParseRule *orig, *copy;
    if (!s || i < 0) return;
    orig = parse_rule_at(s->parse_rules, i);
    if (!orig) return;
    copy = parse_rule_clone(orig);
    if (!copy) return;
    if (dlg_parse_rule(c->hwnd, copy, c->t, s)) {
        ParseRule *keep = orig->next;
        wchar_t id[QP_ID_MAX];
        qp_strlcpy(id, orig->id, QP_ID_MAX);
        *orig = *copy;
        qp_strlcpy(orig->id, id, QP_ID_MAX);
        orig->next = keep;
        free(copy);
        ed_fill_rulelists(c);
        lv_select(c->hParseList, i);
        ed_update_preview(c);
    } else {
        parse_rule_free(copy);
    }
}

static void ed_parse_del(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    int i = lv_sel(c->hParseList);
    if (!s || i < 0) return;
    parse_rule_remove_at(&s->parse_rules, i);
    ed_fill_rulelists(c);
    lv_select(c->hParseList, i);
    ed_update_preview(c);
}

static void ed_parse_move(EditorCtx *c, int delta)
{
    Segment *s = ed_cur_seg(c);
    int i = lv_sel(c->hParseList);
    if (!s || i < 0) return;
    if (!parse_rule_move(&s->parse_rules, i, i + delta)) return;
    ed_fill_rulelists(c);
    lv_select(c->hParseList, i + delta);
    ed_update_preview(c);
}

/* --------------------------- 条件规则 ----------------------------- */

static void ed_cond_add(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    ConditionRule *r;
    if (!s) return;
    r = cond_rule_create();
    if (!r) return;
    if (dlg_cond_rule(c->hwnd, r, c->t, s)) {
        cond_rule_append(&s->condition_rules, r);
        ed_fill_rulelists(c);
        lv_select(c->hCondList, cond_rule_count(s->condition_rules) - 1);
        ed_update_preview(c);
    } else {
        cond_rule_free(r);
    }
}

static void ed_cond_edit(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    int i = lv_sel(c->hCondList);
    ConditionRule *orig, *copy;
    if (!s || i < 0) return;
    orig = cond_rule_at(s->condition_rules, i);
    if (!orig) return;
    copy = cond_rule_clone(orig);
    if (!copy) return;
    if (dlg_cond_rule(c->hwnd, copy, c->t, s)) {
        cond_rule_assign(orig, copy);   /* 保留 orig 的 id 与链表位置 */
        cond_rule_free(copy);
        ed_fill_rulelists(c);
        lv_select(c->hCondList, i);
        ed_update_preview(c);
    } else {
        cond_rule_free(copy);
    }
}

static void ed_cond_del(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    int i = lv_sel(c->hCondList);
    if (!s || i < 0) return;
    cond_rule_remove_at(&s->condition_rules, i);
    ed_fill_rulelists(c);
    lv_select(c->hCondList, i);
    ed_update_preview(c);
}

static void ed_cond_move(EditorCtx *c, int delta)
{
    Segment *s = ed_cur_seg(c);
    int i = lv_sel(c->hCondList);
    if (!s || i < 0) return;
    if (!cond_rule_move(&s->condition_rules, i, i + delta)) return;
    ed_fill_rulelists(c);
    lv_select(c->hCondList, i + delta);
    ed_update_preview(c);
}

/* ---------------------------- 布局 -------------------------------- */

static void ed_layout(EditorCtx *c)
{
    RECT rc;
    HDWP dwp;
    int m = DPIX(12), y, lw = DPIX(66);
    int listx = DPIX(12), listw, propsx, bodybot;
    int bw, bh = DPIX(24), btnH = DPIX(26), btnY, prevH, prevY;
    int W, H;

    GetClientRect(c->hwnd, &rc);
    W = rc.right - rc.left;
    H = rc.bottom - rc.top;
    if (W <= 0 || H <= 0) return;
    ed_metrics(c->hwnd, &listw, &propsx);

    /* 纵向全部由客户区高度反推（以前是写死的 546 / 552 / 570 / 624，所以就算
       给了可缩放边框，拉大窗口也只会在下方留一片空白） */
    btnY  = H - m - btnH;
    prevH = ed_preview_height_for(H);
    prevY = ed_preview_bottom(H) - prevH;
    bodybot = prevY - DPIX(18) - ED_SPLIT_HIT;
    /* 最小跟踪尺寸以上的窗口必须满足完整几何约束；最小化的零客户区在上面跳过。 */
    if (H >= DPIX(ED_MIN_H)) {
        assert(bodybot >= DPIX(130));
        assert(prevY - DPIX(18) == bodybot + ED_SPLIT_HIT);
        assert(prevY + prevH <= btnY - DPIX(8));
    }

    dwp = BeginDeferWindowPos(20);

    /* 第一行：名称 + 分组 */
    y = m;
    dwp = qp_defer(dwp, GetDlgItem(c->hwnd, IDC_ED_NAME + 1000), m, y + DPIX(4), lw, DPIX(18));
    dwp = qp_defer(dwp, c->hName, m + lw + DPIX(6), y, DPIX(230), DPIX(24));
    dwp = qp_defer(dwp, GetDlgItem(c->hwnd, IDC_ED_GROUP + 1000),
                   m + lw + DPIX(244), y + DPIX(4), DPIX(44), DPIX(18));
    dwp = qp_defer(dwp, c->hGroup, m + lw + DPIX(292), y, DPIX(180), DPIX(24));

    /* 第二行：批量开关 + 源变量下拉（模板级，横贯头部，两列都在它下方） */
    y = DPIX(42);
    dwp = qp_defer(dwp, c->hBatch, m, y, DPIX(230), DPIX(22));
    dwp = qp_defer(dwp, c->hBatchSrcLbl, m + DPIX(236), y + DPIX(3), DPIX(52), DPIX(18));
    dwp = qp_defer(dwp, c->hBatchSrc, m + DPIX(292), y, DPIX(200), DPIX(24));

    /* 左侧：片段列表 */
    y = DPIX(72);
    dwp = qp_defer(dwp, c->hSegLbl, listx, y, listw, DPIX(18));
    y = DPIX(96);
    bw = (listw - DPIX(12)) / 5;
    dwp = qp_defer(dwp, c->hAddStatic, listx, y, bw, bh);
    dwp = qp_defer(dwp, c->hAddText, listx + bw + DPIX(3), y, bw, bh);
    dwp = qp_defer(dwp, c->hAddSelect, listx + (bw + DPIX(3)) * 2, y, bw, bh);
    dwp = qp_defer(dwp, c->hAddMulti, listx + (bw + DPIX(3)) * 3, y, bw, bh);
    dwp = qp_defer(dwp, c->hAddTime, listx + (bw + DPIX(3)) * 4, y, bw, bh);
    y += bh + DPIX(4);
    dwp = qp_defer(dwp, c->hSegDel, listx, y, bw, bh);
    dwp = qp_defer(dwp, c->hSegUp, listx + bw + DPIX(3), y, DPIX(40), bh);
    dwp = qp_defer(dwp, c->hSegDown, listx + bw + DPIX(46), y, DPIX(40), bh);
    y += bh + DPIX(6);
    dwp = qp_defer(dwp, c->hSegList, listx, y, listw, bodybot > y ? bodybot - y : 0);
    dwp = qp_defer(dwp, c->hPropsHost, propsx, DPIX(72),
                   W > propsx + m ? W - propsx - m : 0,
                   bodybot > DPIX(72) ? bodybot - DPIX(72) : 0);

    /* 底部：预览 + 按钮 */
    dwp = qp_defer(dwp, c->hPreviewLbl, m, prevY - DPIX(18), W - m * 2, DPIX(16));
    dwp = qp_defer(dwp, c->hPreview, m, prevY, W - m * 2, prevH);
    dwp = qp_defer(dwp, GetDlgItem(c->hwnd, IDOK), W - m - DPIX(196),
                   btnY, DPIX(92), btnH);
    dwp = qp_defer(dwp, GetDlgItem(c->hwnd, IDCANCEL), W - m - DPIX(98),
                   btnY, DPIX(92), btnH);
    if (dwp) EndDeferWindowPos(dwp);

    ListView_SetColumnWidth(c->hSegList, 2, listw - DPIX(96) - DPIX(20));

    ed_layout_props(c, bodybot);

    /* 旧、新分隔条及中间腾出的区域一起失效，避免拖动后的残线。 */
    if (c->splitLastY != bodybot) {
        RECT band = {m, bodybot, W - m, bodybot + ED_SPLIT_HIT};
        if (c->splitLastY >= 0) {
            if (c->splitLastY < band.top) band.top = c->splitLastY;
            if (c->splitLastY + ED_SPLIT_HIT > band.bottom)
                band.bottom = c->splitLastY + ED_SPLIT_HIT;
        }
        c->splitLastY = bodybot;
        RedrawWindow(c->hwnd, &band, NULL, RDW_INVALIDATE | RDW_NOERASE | RDW_UPDATENOW);
    }
}

static void ed_fill_groups(EditorCtx *c)
{
    Group *g;
    int i = 1, sel = 0;
    SendMessageW(c->hGroup, CB_RESETCONTENT, 0, 0);
    SendMessageW(c->hGroup, CB_ADDSTRING, 0, (LPARAM)L"（未分组）");
    SendMessageW(c->hGroup, CB_SETITEMDATA, 0, (LPARAM)NULL);
    for (g = g_app ? g_app->groups : NULL; g; g = g->next, ++i) {
        SendMessageW(c->hGroup, CB_ADDSTRING, 0, (LPARAM)g->name);
        SendMessageW(c->hGroup, CB_SETITEMDATA, (WPARAM)i, (LPARAM)g);
        if (wcscmp(c->t->group_id, g->id) == 0) sel = i;
    }
    SendMessageW(c->hGroup, CB_SETCURSEL, (WPARAM)sel, 0);
}

/* 批量开关 + 源下拉：勾选才启用源下拉；把当前选择写回模板。
   loading 时只摆状态、不回写（与其它属性同一纪律）。 */
static void ed_sync_batch(EditorCtx *c)
{
    int on = SendMessageW(c->hBatch, BM_GETCHECK, 0, 0) == BST_CHECKED;
    EnableWindow(c->hBatchSrc, on);
    EnableWindow(c->hBatchSrcLbl, on);
    if (c->loading) return;
    c->t->batch_mode = on ? 1 : 0;
    if (on) {
        const Segment *src = combo_selected_seg(c->hBatchSrc);
        qp_strlcpy(c->t->batch_source_id, src ? src->id : L"", QP_ID_MAX);
    }
    ed_update_preview(c);
}


/*
 * 保存前的体检：把「配好了但永远不会生效」的规则摆出来。
 * 只提醒不拦人 —— 判定再保守也可能有我想不到的用法，最终由你说了算。
 * 返回 1 = 继续保存，0 = 回去修改（顺手把光标停在第一条问题那一段上）。
 */
#define ED_LINT_SHOW 6              /* 一次最多列这么多条，再多也看不下去 */

static int ed_check_lint(HWND hwnd, EditorCtx *c)
{
    LintIssue *issues;
    wchar_t *msg;
    size_t cap, used;
    int n, i, shown, ret;

    issues = (LintIssue *)calloc(QP_LINT_MAX, sizeof(LintIssue));
    if (!issues) return 1;          /* 内存都没了，别为了体检挡住保存 */
    n = tmpl_lint(c->t, issues, QP_LINT_MAX);
    if (n <= 0) { free(issues); return 1; }

    cap = (size_t)(ED_LINT_SHOW + 2) * QP_RESULT_MAX;
    msg = (wchar_t *)calloc(cap, sizeof(wchar_t));
    if (!msg) { free(issues); return 1; }

    qp_strlcpy(msg, L"这几处看起来不会生效：\n\n", cap);
    used = wcslen(msg);
    shown = (n < ED_LINT_SHOW) ? n : ED_LINT_SHOW;
    for (i = 0; i < shown; ++i) {
        _snwprintf(msg + used, cap - used, L"· %ls\n", issues[i].text);
        msg[cap - 1] = L'\0';
        used += wcslen(msg + used);
    }
    if (n > shown) {
        _snwprintf(msg + used, cap - used, L"…另外还有 %d 条同类问题。\n", n - shown);
        msg[cap - 1] = L'\0';
        used += wcslen(msg + used);
    }
    qp_strlcpy(msg + used, L"\n仍然保存吗？选「否」回去修改。", cap - used);

    ret = MessageBoxW(hwnd, msg, L"模板体检",
                      MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (ret != IDYES) {
        /* 停到第一条问题所在的片段：省得自己数「第 7 段」是哪一段 */
        int at = issues[0].seg_index;
        if (at >= 0 && at < seg_list_count(c->t->segments)) {
            c->cur = at;
            ed_fill_seglist(c, at);
            ed_load_props(c);
        }
    }
    free(msg);
    free(issues);
    return ret == IDYES;
}

static LRESULT CALLBACK editor_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    EditorCtx *c = (EditorCtx *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    int handled = 0;
    LRESULT r;

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        HWND props;
        int fi;
        c = (EditorCtx *)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)c);
        c->hwnd = hwnd;
        c->loading = 1;

        /* 顶部 */
        qp_create(L"Static", L"模板名称：", SS_RIGHT, 0, 0, 0, 10, 10, hwnd,
                  IDC_ED_NAME + 1000);
        c->hName = mk_edit(hwnd, IDC_ED_NAME, 0);
        qp_create(L"Static", L"分组：", SS_RIGHT, 0, 0, 0, 10, 10, hwnd,
                  IDC_ED_GROUP + 1000);
        c->hGroup = mk_combo(hwnd, IDC_ED_GROUP);
        /* 批量：一句话→表格一行时，把源变量的多行粘贴逐行重复整模板输出 */
        c->hBatch = mk_check(hwnd, L"批量：源变量按行重复输出（一行一份）",
                             IDC_ED_BATCH);
        c->hBatchSrcLbl = mk_label_r(hwnd, L"源变量：", 0);
        c->hBatchSrc = mk_combo(hwnd, IDC_ED_BATCH_SRC);

        /* 左侧 */
        c->hSegLbl = mk_label(hwnd, L"片段（自上而下拼接，可拖动排序）", 0);
        c->hAddStatic = mk_btn(hwnd, L"+固定", IDC_ED_SEG_ADD_STATIC);
        c->hAddText = mk_btn(hwnd, L"+输入", IDC_ED_SEG_ADD_TEXT);
        c->hAddSelect = mk_btn(hwnd, L"+单选", IDC_ED_SEG_ADD_SELECT);
        c->hAddMulti = mk_btn(hwnd, L"+多选", IDC_ED_SEG_ADD_MULTI);
        c->hAddTime = mk_btn(hwnd, L"+时间", IDC_ED_SEG_ADD_TIME);
        c->hSegDel = mk_btn(hwnd, L"删除", IDC_ED_SEG_DEL);
        c->hSegUp = mk_btn(hwnd, L"↑", IDC_ED_SEG_UP);
        c->hSegDown = mk_btn(hwnd, L"↓", IDC_ED_SEG_DOWN);
        c->hSegList = mk_list(hwnd, IDC_ED_SEGLIST, 0);
        lv_add_col(c->hSegList, 0, L"#", 32);
        lv_add_col(c->hSegList, 1, L"类型", 64);
        lv_add_col(c->hSegList, 2, L"内容 / 变量", 200);

        /* 右侧属性：一个有独立裁剪边界的子窗口，无需资源 ID。 */
        ensure_class(L"QuickPhraseEditorProps", ed_props_proc);
        c->hPropsHost = CreateWindowExW(WS_EX_CONTROLPARENT,
            L"QuickPhraseEditorProps", L"片段属性",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, 0, 0, 0, hwnd, NULL, g_hInst, c);
        if (!c->hPropsHost) return -1;
        props = c->hPropsHost;
        c->hPropTitle = mk_label(props, L"片段属性", 0);
        SendMessageW(c->hPropTitle, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
        c->hTypeLbl = mk_label_r(props, L"类型：", 0);
        c->hType = mk_combo(props, IDC_ED_SEG_TYPE);
        SendMessageW(c->hType, CB_ADDSTRING, 0, (LPARAM)L"固定文本");
        SendMessageW(c->hType, CB_ADDSTRING, 0, (LPARAM)L"文本输入");
        SendMessageW(c->hType, CB_ADDSTRING, 0, (LPARAM)L"下拉单选");
        SendMessageW(c->hType, CB_ADDSTRING, 0, (LPARAM)L"下拉多选");
        SendMessageW(c->hType, CB_ADDSTRING, 0, (LPARAM)L"时间日期");
        c->hLabelLbl = mk_label_r(props, L"变量名：", 0);
        c->hLabel = mk_edit(props, IDC_ED_SEG_LABEL, 0);
        c->hContentLbl = mk_label_r(props, L"固定内容：", 0);
        c->hContent = mk_edit(props, IDC_ED_SEG_CONTENT,
                              ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | WS_VSCROLL);
        c->hHint = mk_label(props,
                            L"可以写 {变量名} 直接引用其他变量的值；{{ 输出一个 {",
                            IDC_ED_SEG_HINT);
        c->hPlaceLbl = mk_label_r(props, L"占位提示：", 0);
        c->hPlace = mk_edit(props, IDC_ED_SEG_PLACE, 0);
        c->hDefLbl = mk_label_r(props, L"默认值：", 0);
        c->hDef = mk_edit(props, IDC_ED_SEG_DEFAULT, 0);
        c->hSepLbl = mk_label_r(props, L"分隔符：", 0);
        c->hSep = mk_edit(props, IDC_ED_SEG_SEP, 0);
        c->hFmtLbl = mk_label_r(props, L"时间格式：", 0);
        c->hFmt = mk_combo_edit(props, IDC_ED_SEG_FORMAT);
        SendMessageW(c->hFmt, CB_LIMITTEXT, (WPARAM)(QP_FORMAT_MAX - 1), 0);
        for (fi = 0; fi < qp_time_format_count(); ++fi)
            SendMessageW(c->hFmt, CB_ADDSTRING, 0, (LPARAM)qp_time_format_at(fi));
        c->hFmtSample = mk_label(props, L"", IDC_ED_SEG_FMT_SAMPLE);
        c->hHidden = mk_check(props, L"在填写界面隐藏（只由变量匹配 / 默认值填充）",
                              IDC_ED_SEG_HIDDEN);
        c->hNoOutput = mk_check(props, L"不计入输出（照常参与匹配 / 引用 / 占位符，但不拼进口径）",
                                IDC_ED_SEG_NOOUTPUT);
        c->hMultiline = mk_check(props, L"多行文本框（大段文字，可换行）",
                                 IDC_ED_SEG_MULTILINE);
        c->hRefLbl = mk_label_r(props, L"引用：", 0);
        c->hRef = mk_combo(props, IDC_ED_SEG_REF);
        c->hRefModeLbl = mk_label_r(props, L"取法：", 0);
        c->hRefMode = mk_combo(props, IDC_ED_SEG_REF_MODE);
        SendMessageW(c->hRefMode, CB_ADDSTRING, 0, (LPARAM)L"整体");
        SendMessageW(c->hRefMode, CB_ADDSTRING, 0, (LPARAM)L"按分隔符取第 N 段");
        SendMessageW(c->hRefMode, CB_ADDSTRING, 0, (LPARAM)L"从第 N 个字起");
        c->hRefA = mk_edit(props, IDC_ED_SEG_REF_A, 0);
        c->hRefB = mk_edit(props, IDC_ED_SEG_REF_B, 0);

        c->hOptLbl = mk_label(props, L"选项", 0);
        c->hOptAdd = mk_btn(props, L"添加", IDC_ED_OPT_ADD);
        c->hOptEdit = mk_btn(props, L"修改", IDC_ED_OPT_EDIT);
        c->hOptDel = mk_btn(props, L"删除", IDC_ED_OPT_DEL);
        c->hOptUp = mk_btn(props, L"↑", IDC_ED_OPT_UP);
        c->hOptDown = mk_btn(props, L"↓", IDC_ED_OPT_DOWN);
        c->hOptList = mk_list(props, IDC_ED_OPTLIST, LVS_NOCOLUMNHEADER);
        lv_add_col(c->hOptList, 0, L"选项", 300);

        c->hParseLbl = mk_label(props, L"解析规则（从本输入中提取信息，自动填充其他变量）", 0);
        c->hParseAdd = mk_btn(props, L"添加", IDC_ED_PARSE_ADD);
        c->hParseEdit = mk_btn(props, L"修改", IDC_ED_PARSE_EDIT);
        c->hParseDel = mk_btn(props, L"删除", IDC_ED_PARSE_DEL);
        c->hParseUp = mk_btn(props, L"↑", IDC_ED_PARSE_UP);
        c->hParseDown = mk_btn(props, L"↓", IDC_ED_PARSE_DOWN);
        c->hParseList = mk_list(props, IDC_ED_PARSELIST, LVS_NOCOLUMNHEADER);
        lv_add_col(c->hParseList, 0, L"规则", 300);

        c->hCondLbl = mk_label(props, L"条件规则（按其他变量的值决定本片段输出）", 0);
        c->hCondAdd = mk_btn(props, L"添加", IDC_ED_COND_ADD);
        c->hCondEdit = mk_btn(props, L"修改", IDC_ED_COND_EDIT);
        c->hCondDel = mk_btn(props, L"删除", IDC_ED_COND_DEL);
        c->hCondUp = mk_btn(props, L"↑", IDC_ED_COND_UP);
        c->hCondDown = mk_btn(props, L"↓", IDC_ED_COND_DOWN);
        c->hCondList = mk_list(props, IDC_ED_CONDLIST, LVS_NOCOLUMNHEADER);
        lv_add_col(c->hCondList, 0, L"规则", 300);
        EnumChildWindows(c->hPropsHost, ed_prop_hook_focus, (LPARAM)c);

        /* 底部 */
        c->hPreviewLbl = mk_label(hwnd, L"默认值预览", 0);
        c->hPreview = qp_create(L"Edit", L"",
                                ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
                                WS_EX_CLIENTEDGE, 0, 0, 10, 10, hwnd, IDC_ED_PREVIEW);
        SendMessageW(c->hPreview, WM_SETFONT, (WPARAM)g_fontMono, TRUE);
        mk_btn(hwnd, L"保存", IDOK);
        mk_btn(hwnd, L"取消", IDCANCEL);
        SendMessageW(GetDlgItem(hwnd, IDOK), BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);

        SetWindowTextW(c->hName, c->t->name);
        ed_fill_groups(c);
        SendMessageW(c->hBatch, BM_SETCHECK,
                     c->t->batch_mode ? BST_CHECKED : BST_UNCHECKED, 0);
        c->cur = seg_list_count(c->t->segments) > 0 ? 0 : -1;
        ed_layout(c);
        ed_fill_seglist(c, c->cur < 0 ? 0 : c->cur);
        ed_load_props(c);            /* 内部 ed_fill_batch_combo 选中当前源 */
        ed_sync_batch(c);            /* 按开关状态启用 / 禁用源下拉（loading 中不回写） */
        ed_update_preview(c);
        qp_theme_children(hwnd);
        c->loading = 0;
        SetFocus(c->hName);
        SendMessageW(c->hName, EM_SETSEL, 0, -1);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        /* 最小尺寸就是原来写死的那套设计尺寸：再小属性区放不下三个规则列表 */
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        RECT rc;
        rc.left = 0; rc.top = 0;
        rc.right = DPIX(ED_MIN_W);
        rc.bottom = DPIX(ED_MIN_H);
        AdjustWindowRectEx(&rc, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE,
                           (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
        mmi->ptMinTrackSize.x = rc.right - rc.left;
        mmi->ptMinTrackSize.y = rc.bottom - rc.top;
        return 0;
    }

    case WM_SIZE:
        if (c) {
            if (wp == SIZE_RESTORED) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                /* 记住客户区尺寸，与主窗口的 win_w/win_h 同一口径（绝对像素、
                   不做 DPI 折算）。最大化时不记，否则下次还原成一扇大窗。 */
                g_settings.editor_w = rc.right - rc.left;
                g_settings.editor_h = rc.bottom - rc.top;
            }
            ed_layout(c);
        }
        return 0;

    /* 拖完边框才落盘一次：拖动途中每格都写一次文件没有意义 */
    case WM_EXITSIZEMOVE:
        if (c) app_save_settings();
        return 0;

    case WM_ERASEBKGND:
        /* 什么都不做，只声称「已经擦过了」：背景连同分隔条由 WM_PAINT 在离屏
           位图上一趟画完（与主窗口同一做法，见 ui.c）。这里再涂一遍会让分隔条
           先消失一帧，拖动时就是闪。必须挡在 dlg_common 之前。 */
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (hdc) {
            RECT rc, bar, clip = ps.rcPaint;
            HDC dc = hdc, mem = NULL;
            HBITMAP bmp = NULL, oldBmp = NULL;
            HBRUSH b;
            int cw = clip.right - clip.left, ch = clip.bottom - clip.top;
            int one = DPIX(1) > 1 ? DPIX(1) : 1;
            GetClientRect(hwnd, &rc);
            if (cw > 0 && ch > 0 && (mem = CreateCompatibleDC(hdc)) != NULL) {
                bmp = CreateCompatibleBitmap(hdc, cw, ch);
                if (bmp) {
                    oldBmp = (HBITMAP)SelectObject(mem, bmp);
                    /* 原点对到客户区坐标，下面的计算不用改 */
                    SetViewportOrgEx(mem, -clip.left, -clip.top, NULL);
                    dc = mem;
                } else {
                    DeleteDC(mem);
                    mem = NULL;
                }
            }
            FillRect(dc, &clip, g_brushBk);
            /* 画出预览分隔条本体，让人看得见哪里可以拖 */
            b = CreateSolidBrush(theme_line());
            if (b) {
                bar.left   = DPIX(12);
                bar.right  = rc.right - DPIX(12);
                bar.top    = ed_splitter_y(rc.bottom - rc.top) + ED_SPLIT_HIT / 2;
                bar.bottom = bar.top + one;
                if (bar.right > bar.left) FillRect(dc, &bar, b);
                DeleteObject(b);
            }
            if (mem) {
                SetViewportOrgEx(mem, 0, 0, NULL);
                BitBlt(hdc, clip.left, clip.top, cw, ch, mem, 0, 0, SRCCOPY);
                SelectObject(mem, oldBmp);
                DeleteObject(bmp);
                DeleteDC(mem);
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SETCURSOR: {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        if (ed_split_hit(hwnd, pt.x, pt.y)) {
            SetCursor(LoadCursorW(NULL, IDC_SIZENS));
            return TRUE;
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        RECT rc;
        if (!c) break;
        GetClientRect(hwnd, &rc);
        if (ed_split_hit(hwnd, x, y)) {
            c->splitDrag = 1;
            /* 记住抓取偏移，分隔条不会跳到光标下 */
            c->splitGrabDy = y - ed_splitter_y(rc.bottom - rc.top);
            SetCapture(hwnd);
        }
        return 0;
    }

    /* 双击分隔条恢复默认高度：与主窗口那两条分隔条的交互保持一致 */
    case WM_LBUTTONDBLCLK:
        if (c && ed_split_hit(hwnd, (short)LOWORD(lp), (short)HIWORD(lp))) {
            g_settings.editor_preview_height = 0;
            ed_layout(c);
            app_save_settings();
            return 0;
        }
        break;

    case WM_MOUSEMOVE:
        if (c && c->splitDrag) {
            RECT rc;
            int y = (short)HIWORD(lp);
            int H, lo, hi, h;
            GetClientRect(hwnd, &rc);
            H = rc.bottom - rc.top;
            ed_preview_bounds(H, &lo, &hi);
            /* 往上拖 = 预览变高；公式与 ed_splitter_y 互为反函数 */
            h = ed_splitter_base(H) - (y - c->splitGrabDy);
            if (h < lo) h = lo;
            if (h > hi) h = hi;
            if (h != g_settings.editor_preview_height) {
                g_settings.editor_preview_height = h;
                ed_layout(c);
            }
        }
        return 0;

    case WM_LBUTTONUP:
        if (c && c->splitDrag) {
            c->splitDrag = 0;
            ReleaseCapture();
            app_save_settings();
        }
        return 0;

    case WM_CANCELMODE:
        if (c) c->splitDrag = 0;
        break;

    case WM_COMMAND: {
        WORD id = LOWORD(wp), code = HIWORD(wp);
        qp_combo_dropdown_fix(wp, lp);
        if (!c) break;
        switch (id) {
        case IDC_ED_SEG_ADD_STATIC: ed_add_segment(c, SEG_STATIC); return 0;
        case IDC_ED_SEG_ADD_TEXT:   ed_add_segment(c, SEG_TEXT); return 0;
        case IDC_ED_SEG_ADD_SELECT: ed_add_segment(c, SEG_SELECT); return 0;
        case IDC_ED_SEG_ADD_MULTI:  ed_add_segment(c, SEG_MULTI_SELECT); return 0;
        case IDC_ED_SEG_ADD_TIME:   ed_add_segment(c, SEG_DATETIME); return 0;
        case IDC_ED_SEG_DEL:        ed_del_segment(c); return 0;
        case IDC_ED_SEG_UP:         ed_move_segment(c, -1); return 0;
        case IDC_ED_SEG_DOWN:       ed_move_segment(c, +1); return 0;
        case IDC_ED_SEG_TYPE:
            if (code == CBN_SELCHANGE) ed_change_type(c);
            return 0;
        case IDC_ED_SEG_LABEL:
        case IDC_ED_SEG_CONTENT:
        case IDC_ED_SEG_PLACE:
        case IDC_ED_SEG_DEFAULT:
        case IDC_ED_SEG_SEP:
            if (code == EN_CHANGE && !c->loading) {
                ed_commit_props(c);
                ed_refresh_cur_row(c);
            }
            return 0;
        case IDC_ED_SEG_HIDDEN:
        case IDC_ED_SEG_MULTILINE:
        case IDC_ED_SEG_NOOUTPUT:
            if (code == BN_CLICKED && !c->loading) {
                ed_commit_props(c);
                ed_refresh_cur_row(c);
            }
            return 0;
        case IDC_ED_BATCH:
            if (code == BN_CLICKED) ed_sync_batch(c);
            return 0;
        case IDC_ED_BATCH_SRC:
            if (code == CBN_SELCHANGE) ed_sync_batch(c);
            return 0;
        case IDC_ED_SEG_FORMAT:
            /* 下拉里选一个（SELCHANGE）或自己敲（EDITCHANGE），两条路都算改格式 */
            if (code == CBN_SELCHANGE || code == CBN_EDITCHANGE) {
                ed_commit_format(c, code == CBN_SELCHANGE);
                ed_refresh_cur_row(c);
            }
            return 0;
        case IDC_ED_SEG_REF:
            if (code == CBN_SELCHANGE) ed_ref_changed(c);
            return 0;
        case IDC_ED_SEG_REF_MODE:
            if (code == CBN_SELCHANGE) ed_ref_mode_changed(c);
            return 0;
        case IDC_ED_SEG_REF_A:
        case IDC_ED_SEG_REF_B:
            if (code == EN_CHANGE && !c->loading) ed_ref_param_changed(c);
            return 0;
        case IDC_ED_OPT_ADD:   ed_opt_add(c); return 0;
        case IDC_ED_OPT_EDIT:  ed_opt_edit(c); return 0;
        case IDC_ED_OPT_DEL:   ed_opt_del(c); return 0;
        case IDC_ED_OPT_UP:    ed_opt_move(c, -1); return 0;
        case IDC_ED_OPT_DOWN:  ed_opt_move(c, +1); return 0;
        case IDC_ED_PARSE_ADD:  ed_parse_add(c); return 0;
        case IDC_ED_PARSE_EDIT: ed_parse_edit(c); return 0;
        case IDC_ED_PARSE_DEL:  ed_parse_del(c); return 0;
        case IDC_ED_PARSE_UP:   ed_parse_move(c, -1); return 0;
        case IDC_ED_PARSE_DOWN: ed_parse_move(c, +1); return 0;
        case IDC_ED_COND_ADD:   ed_cond_add(c); return 0;
        case IDC_ED_COND_EDIT:  ed_cond_edit(c); return 0;
        case IDC_ED_COND_DEL:   ed_cond_del(c); return 0;
        case IDC_ED_COND_UP:    ed_cond_move(c, -1); return 0;
        case IDC_ED_COND_DOWN:  ed_cond_move(c, +1); return 0;
        case IDOK: {
            wchar_t name[QP_NAME_MAX];
            Group *g;
            Segment *a, *b;
            int gi, ia, ib;
            qp_get_text(c->hName, name, QP_NAME_MAX);
            qp_trim(name);
            if (!name[0]) {
                qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"提示", L"请填写模板名称。");
                SetFocus(c->hName);
                return 0;
            }
            if (seg_list_count(c->t->segments) == 0) {
                qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"提示",
                          L"模板至少需要一个片段。");
                return 0;
            }
            ed_commit_props(c);
            /* 变量重名拦下来：规则和引用都是靠变量名认人的，两个同名变量在下拉里
               长得一模一样，指错了也看不出来（引擎按 id 找不到时还会退回按名字找，
               只会命中第一个）。改一个名字比事后排查便宜得多。 */
            for (a = c->t->segments, ia = 0; a; a = a->next, ++ia) {
                if (a->type == SEG_STATIC || !a->label[0]) continue;
                for (b = a->next, ib = ia + 1; b; b = b->next, ++ib) {
                    if (b->type == SEG_STATIC) continue;
                    if (wcscmp(a->label, b->label) != 0) continue;
                    qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"变量名重复",
                              L"第 %d 个和第 %d 个片段都叫【%ls】。\n"
                              L"规则和引用是按变量名认人的，重名会指错，请改一个。",
                              ia + 1, ib + 1, a->label);
                    c->cur = ib;
                    ed_fill_seglist(c, ib);
                    ed_load_props(c);
                    SetFocus(c->hLabel);
                    return 0;
                }
            }
            /* 指向已删除变量的引用（导入的老数据也可能有）直接清掉：它永远不会
               生效，留在存档里只会让下次打开的人以为还跟着来源在变 */
            for (a = c->t->segments; a; a = a->next) {
                if (a->type == SEG_STATIC ||
                    (a->ref_segment_id[0] &&
                     !tmpl_find_segment(c->t, a->ref_segment_id)))
                    a->ref_segment_id[0] = L'\0';
            }
            /* 体检：能证明不会生效的规则，保存前提一句（不拦人） */
            if (!ed_check_lint(hwnd, c)) return 0;
            /* 批量源变量指向已删除 / 非文本变量时，按引用悬空同一口径把 id 清掉
               （开关留着：运行时无有效源会自动退化为单行，不会出错；体检已在上面
               提醒过了，用户执意保存就不再留个悬空指针在存档里） */
            if (c->t->batch_source_id[0]) {
                const Segment *bs = tmpl_find_segment(c->t, c->t->batch_source_id);
                if (!bs || bs->type != SEG_TEXT) c->t->batch_source_id[0] = L'\0';
            }
            qp_strlcpy(c->t->name, name, QP_NAME_MAX);
            gi = (int)SendMessageW(c->hGroup, CB_GETCURSEL, 0, 0);
            g = (gi > 0) ? (Group *)SendMessageW(c->hGroup, CB_GETITEMDATA, (WPARAM)gi, 0)
                         : NULL;
            qp_strlcpy(c->t->group_id, g ? g->id : L"", QP_ID_MAX);
            c->result = 1;
            dlg_close(hwnd);
            return 0;
        }
        case IDCANCEL:
            c->result = 0;
            dlg_close(hwnd);
            return 0;
        default:
            break;
        }
        break;
    }

    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (!c) break;
        if (nh->idFrom == IDC_ED_SEGLIST) {
            if (nh->code == LVN_ITEMCHANGED) {
                NMLISTVIEW *n = (NMLISTVIEW *)lp;
                if ((n->uChanged & LVIF_STATE) && (n->uNewState & LVIS_SELECTED) &&
                    n->iItem != c->cur) {
                    c->cur = n->iItem;
                    ed_load_props(c);
                }
            } else if (nh->code == LVN_BEGINDRAG) {
                NMLISTVIEW *n = (NMLISTVIEW *)lp;
                int to = qp_lv_drag_reorder(c->hSegList, n->iItem);
                if (to >= 0 && seg_list_move(&c->t->segments, n->iItem, to)) {
                    c->cur = to;
                    ed_fill_seglist(c, to);
                    ed_load_props(c);
                    ed_update_preview(c);
                }
                return 0;
            }
        } else if (nh->code == NM_DBLCLK) {
            if (nh->idFrom == IDC_ED_OPTLIST) { ed_opt_edit(c); return 0; }
            if (nh->idFrom == IDC_ED_PARSELIST) { ed_parse_edit(c); return 0; }
            if (nh->idFrom == IDC_ED_CONDLIST) { ed_cond_edit(c); return 0; }
        }
        break;
    }

    case WM_CLOSE:
        if (c) c->result = 0;
        dlg_close(hwnd);
        return 0;

    default:
        break;
    }

    r = dlg_common(hwnd, msg, wp, lp, &handled);
    if (handled) return r;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* 还原上次记住的编辑器尺寸：存的是客户区绝对像素，这里夹进屏幕工作区 */
static void ed_restore_size(HWND hwnd, HWND owner)
{
    RECT rc, wa;
    DWORD style, ex;
    int w = g_settings.editor_w, h = g_settings.editor_h;
    if (!hwnd || w <= 0 || h <= 0) return;
    if (w < DPIX(ED_MIN_W)) w = DPIX(ED_MIN_W);
    if (h < DPIX(ED_MIN_H)) h = DPIX(ED_MIN_H);
    style = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
    ex = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    rc.left = 0; rc.top = 0; rc.right = w; rc.bottom = h;
    AdjustWindowRectEx(&rc, style, FALSE, ex);
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
        wa.left = 0; wa.top = 0;
        wa.right = GetSystemMetrics(SM_CXSCREEN);
        wa.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    if (w > wa.right - wa.left) w = wa.right - wa.left;
    if (h > wa.bottom - wa.top) h = wa.bottom - wa.top;
    SetWindowPos(hwnd, NULL, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    qp_center_window(hwnd, owner);
}

int dlg_edit_template(HWND owner, Template *working, int is_new)
{
    EditorCtx c;
    HWND dlg;
    memset(&c, 0, sizeof(c));
    c.t = working;
    c.is_new = is_new;
    c.cur = -1;
    c.splitLastY = -1;                /* 还没画过分隔条 */
    dlg = make_dlg_ex(L"QuickPhraseEditor", editor_proc,
                      is_new ? L"新建模板" : L"编辑模板",
                      ED_MIN_W, ED_MIN_H, owner, &c,
                      WS_THICKFRAME | WS_MAXIMIZEBOX);
    ed_restore_size(dlg, owner);
    return run_modal(owner, dlg, &c.result);
}

/* ================================================================== */
/* 设置                                                                */
/* ================================================================== */

typedef struct {
    HWND hwnd;
    int result;
    unsigned mods, vk;
    HWND hHotkey, hAuto, hTheme, hSep, hDataDir;
} SetCtx;

/* 更改数据目录：选择新目录 → 迁移数据 → 保存设置 → 重载界面 */
static void change_data_dir(HWND hwnd, SetCtx *c)
{
    BROWSEINFOW bi;
    PIDLIST_ABSOLUTE pidl;
    wchar_t dir[QP_PATH_MAX] = L"", old[QP_PATH_MAX], np[QP_PATH_MAX];
    int has_data;

    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = hwnd;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpszTitle = L"选择数据目录（存放 data.json 与模板数据）";
    pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    if (!SHGetPathFromIDListW(pidl, dir) || !dir[0]) {
        CoTaskMemFree(pidl);
        return;
    }
    CoTaskMemFree(pidl);

    storage_get_data_dir(old, QP_PATH_MAX);
    if (wcscmp(old, dir) == 0) return;   /* 相同目录，无需操作 */

    /* 临时切到新目录，检查是否已有数据文件 */
    storage_set_data_dir(dir);
    storage_ensure_dir();
    has_data = storage_get_data_path(np, QP_PATH_MAX) &&
               GetFileAttributesW(np) != INVALID_FILE_ATTRIBUTES;

    if (!has_data) {
        /* 新目录没有数据：把当前数据保存过去 */
        if (!storage_save(g_app)) {
            qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, QP_APP_TITLE,
                      L"无法写入所选目录，请检查权限后重试。");
            storage_set_data_dir(old);      /* 还原 */
            return;
        }
    }

    /* 更新设置并保存（settings.json 位于默认目录） */
    qp_strlcpy(g_settings.data_dir, dir, QP_PATH_MAX);
    app_save_settings();

    /* 重载数据并刷新界面 */
    {
        AppData *nd = storage_load(NULL);
        if (nd) {
            appdata_free(g_app);
            g_app = nd;
        }
    }
    ui_reset_selection();
    ui_rebuild_tree(NULL);

    /* 更新对话框中的路径显示 */
    SetWindowTextW(c->hDataDir, dir);

    qp_msgbox(hwnd, MB_OK | MB_ICONINFORMATION, QP_APP_TITLE,
              has_data ? L"已切换到所选目录，并使用其中的数据。"
                       : L"已把当前数据保存到新目录并切换过去。");
}

static void hotkey_text(unsigned mods, unsigned vk, wchar_t *out, size_t cap)
{
    wchar_t key[64] = L"";
    out[0] = L'\0';
    if (mods & MOD_CONTROL) qp_strlcpy(out + wcslen(out), L"Ctrl + ", cap - wcslen(out));
    if (mods & MOD_ALT)     qp_strlcpy(out + wcslen(out), L"Alt + ", cap - wcslen(out));
    if (mods & MOD_SHIFT)   qp_strlcpy(out + wcslen(out), L"Shift + ", cap - wcslen(out));
    if (mods & MOD_WIN)     qp_strlcpy(out + wcslen(out), L"Win + ", cap - wcslen(out));
    if (vk) {
        UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        LONG lp = (LONG)(sc << 16);
        switch (vk) {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
        case VK_PRIOR:  case VK_NEXT:   case VK_LEFT: case VK_RIGHT:
        case VK_UP:     case VK_DOWN:
            lp |= (1L << 24);       /* 扩展键 */
            break;
        default: break;
        }
        if (GetKeyNameTextW(lp, key, 64) <= 0)
            _snwprintf(key, 64, L"VK_%02X", vk);
        qp_strlcpy(out + wcslen(out), key, cap - wcslen(out));
    } else {
        qp_strlcpy(out + wcslen(out), L"（未设置）", cap - wcslen(out));
    }
}

static LRESULT CALLBACK hotkey_edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                         UINT_PTR id, DWORD_PTR ref)
{
    SetCtx *c = (SetCtx *)ref;
    (void)id;
    switch (msg) {
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    case WM_CHAR:
    case WM_SYSCHAR:
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        unsigned vk = (unsigned)wp;
        unsigned mods = 0;
        wchar_t txt[128];
        if (vk == VK_TAB) break;                       /* 让 Tab 正常切换焦点 */
        if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
        if (GetKeyState(VK_MENU) & 0x8000)    mods |= MOD_ALT;
        if (GetKeyState(VK_SHIFT) & 0x8000)   mods |= MOD_SHIFT;
        if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000))
            mods |= MOD_WIN;
        if (vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT ||
            vk == VK_LWIN || vk == VK_RWIN) {
            hotkey_text(mods, 0, txt, 128);
            SetWindowTextW(hwnd, txt);
            return 0;
        }
        if (vk == VK_BACK || vk == VK_DELETE) {
            c->mods = 0; c->vk = 0;
            SetWindowTextW(hwnd, L"（未设置）");
            return 0;
        }
        c->mods = mods;
        c->vk = vk;
        hotkey_text(mods, vk, txt, 128);
        SetWindowTextW(hwnd, txt);
        return 0;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        wchar_t txt[128];
        hotkey_text(c->mods, c->vk, txt, 128);
        SetWindowTextW(hwnd, txt);
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        return 0;
    default:
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK settings_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    SetCtx *c = (SetCtx *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    int handled = 0;
    LRESULT r;

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        int m = DPIX(16), lw = DPIX(90), y = DPIX(16), rowh = DPIX(30);
        int cx, cw;
        RECT rc;
        HWND l1, l2, l3, l4, tip;
        wchar_t txt[600];

        c = (SetCtx *)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)c);
        c->hwnd = hwnd;
        GetClientRect(hwnd, &rc);
        cx = m + lw + DPIX(8);
        cw = rc.right - m - cx;

        l1 = mk_label_r(hwnd, L"全局热键：", 0);
        c->hHotkey = mk_edit(hwnd, IDC_SET_HOTKEY, ES_READONLY);
        SetWindowSubclass(c->hHotkey, hotkey_edit_proc, 1, (DWORD_PTR)c);
        hotkey_text(c->mods, c->vk, txt, 600);
        SetWindowTextW(c->hHotkey, txt);

        tip = mk_label(hwnd, L"点上面的框，直接按下想用的组合键（需含 Ctrl / Alt / Shift / Win）。",
                       0);

        l2 = mk_label_r(hwnd, L"主题：", 0);
        c->hTheme = mk_combo(hwnd, IDC_SET_THEME);
        SendMessageW(c->hTheme, CB_ADDSTRING, 0, (LPARAM)L"浅色");
        SendMessageW(c->hTheme, CB_ADDSTRING, 0, (LPARAM)L"深色");
        SendMessageW(c->hTheme, CB_ADDSTRING, 0, (LPARAM)L"跟随系统");
        SendMessageW(c->hTheme, CB_SETCURSEL, (WPARAM)g_settings.theme, 0);

        l3 = mk_label_r(hwnd, L"多选分隔符：", 0);
        c->hSep = mk_edit(hwnd, IDC_SET_SEP, 0);
        SetWindowTextW(c->hSep, g_settings.default_separator);

        c->hAuto = mk_check(hwnd, L"开机自动启动（登录后驻留托盘）", IDC_SET_AUTOSTART);
        SendMessageW(c->hAuto, BM_SETCHECK,
                     app_get_autostart() ? BST_CHECKED : BST_UNCHECKED, 0);

        l4 = mk_label_r(hwnd, L"数据目录：", 0);
        c->hDataDir = mk_edit(hwnd, IDC_SET_DATADIR, ES_READONLY);
        storage_get_data_dir(txt, 600);
        SetWindowTextW(c->hDataDir, txt);

        mk_btn(hwnd, L"更改…", IDC_SET_DATADIR + 1);
        mk_btn(hwnd, L"打开", IDC_SET_DATADIR + 2);
        mk_btn(hwnd, L"确定", IDOK);
        mk_btn(hwnd, L"取消", IDCANCEL);
        SendMessageW(GetDlgItem(hwnd, IDOK), BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);

        MoveWindow(l1, m, y + DPIX(4), lw, DPIX(18), TRUE);
        MoveWindow(c->hHotkey, cx, y, DPIX(200), DPIX(24), TRUE);
        y += DPIX(26);
        MoveWindow(tip, cx, y, cw, DPIX(18), TRUE);
        y += DPIX(26);
        MoveWindow(l2, m, y + DPIX(4), lw, DPIX(18), TRUE);
        MoveWindow(c->hTheme, cx, y, DPIX(140), DPIX(24), TRUE);
        y += rowh;
        MoveWindow(l3, m, y + DPIX(4), lw, DPIX(18), TRUE);
        MoveWindow(c->hSep, cx, y, DPIX(80), DPIX(24), TRUE);
        y += rowh;
        MoveWindow(c->hAuto, cx, y, cw, DPIX(24), TRUE);
        y += rowh;
        MoveWindow(l4, m, y + DPIX(4), lw, DPIX(18), TRUE);
        MoveWindow(c->hDataDir, cx, y, cw - DPIX(128), DPIX(24), TRUE);
        MoveWindow(GetDlgItem(hwnd, IDC_SET_DATADIR + 1), rc.right - m - DPIX(126), y,
                   DPIX(62), DPIX(24), TRUE);
        MoveWindow(GetDlgItem(hwnd, IDC_SET_DATADIR + 2), rc.right - m - DPIX(62), y,
                   DPIX(62), DPIX(24), TRUE);
        MoveWindow(GetDlgItem(hwnd, IDOK), rc.right - m - DPIX(188),
                   rc.bottom - m - DPIX(26), DPIX(88), DPIX(26), TRUE);
        MoveWindow(GetDlgItem(hwnd, IDCANCEL), rc.right - m - DPIX(92),
                   rc.bottom - m - DPIX(26), DPIX(88), DPIX(26), TRUE);

        qp_theme_children(hwnd);
        SetFocus(c->hHotkey);
        return 0;
    }
    case WM_COMMAND:
        qp_combo_dropdown_fix(wp, lp);
        if (!c) break;
        if (LOWORD(wp) == IDC_SET_DATADIR + 1) {   /* 更改… */
            change_data_dir(hwnd, c);
            return 0;
        }
        if (LOWORD(wp) == IDC_SET_DATADIR + 2) {   /* 打开 */
            wchar_t dir[600];
            storage_ensure_dir();
            if (storage_get_data_dir(dir, 600))
                ShellExecuteW(hwnd, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        if (LOWORD(wp) == IDOK) {
            wchar_t sep[QP_SEP_MAX];
            int theme = (int)SendMessageW(c->hTheme, CB_GETCURSEL, 0, 0);
            int autostart = SendMessageW(c->hAuto, BM_GETCHECK, 0, 0) == BST_CHECKED;
            unsigned oldm = g_settings.hotkey_mods, oldv = g_settings.hotkey_vk;

            if (c->vk && !c->mods) {
                qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"提示",
                          L"全局热键需要至少包含 Ctrl / Alt / Shift / Win 之一，"
                          L"否则会影响其他程序的正常输入。");
                SetFocus(c->hHotkey);
                return 0;
            }
            qp_get_text(c->hSep, sep, QP_SEP_MAX);
            if (!sep[0]) qp_strlcpy(sep, L"、", QP_SEP_MAX);

            app_unregister_hotkey();
            g_settings.hotkey_mods = c->mods;
            g_settings.hotkey_vk = c->vk;
            if (c->vk && !app_register_hotkey()) {
                qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"热键被占用",
                          L"该组合键已被其他程序占用，已保留原来的热键。");
                g_settings.hotkey_mods = oldm;
                g_settings.hotkey_vk = oldv;
                app_register_hotkey();
                c->mods = oldm;
                c->vk = oldv;
                {
                    wchar_t txt[128];
                    hotkey_text(oldm, oldv, txt, 128);
                    SetWindowTextW(c->hHotkey, txt);
                }
                return 0;
            }

            qp_strlcpy(g_settings.default_separator, sep, QP_SEP_MAX);
            if (theme != CB_ERR && theme != (int)g_settings.theme) {
                g_settings.theme = (ThemeMode)theme;
                app_apply_theme();
            }
            if (autostart != app_get_autostart()) {
                if (!app_set_autostart(autostart))
                    qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"提示",
                              L"无法写入开机启动项，可能被安全软件拦截。");
            }
            g_settings.autostart = app_get_autostart();
            app_save_settings();
            c->result = 1;
            dlg_close(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) { c->result = 0; dlg_close(hwnd); return 0; }
        break;
    case WM_CLOSE:
        if (c) c->result = 0;
        dlg_close(hwnd);
        return 0;
    default:
        break;
    }
    r = dlg_common(hwnd, msg, wp, lp, &handled);
    if (handled) return r;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int dlg_settings(HWND owner)
{
    SetCtx c;
    HWND dlg;
    memset(&c, 0, sizeof(c));
    c.mods = g_settings.hotkey_mods;
    c.vk = g_settings.hotkey_vk;
    dlg = make_dlg(L"QuickPhraseSettings", settings_proc, L"选项", 520, 288, owner, &c);
    return run_modal(owner, dlg, &c.result);
}
