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
    wc.lpfnWndProc = proc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = NULL;
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);
}

/* 创建一个以 owner 为父的模态窗口，客户区大小为 cw × ch（逻辑像素） */
static HWND make_dlg(const wchar_t *cls, WNDPROC proc, const wchar_t *title,
                     int cw, int ch, HWND owner, void *param)
{
    RECT rc;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    DWORD ex = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
    HWND hwnd;

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

static int run_modal(HWND owner, HWND dlg, const int *result)
{
    int r;
    if (!dlg) return 0;
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);
    r = qp_modal_loop(owner, dlg, result);
    if (owner) {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
        SetForegroundWindow(owner);
    }
    return r;
}

static LRESULT dlg_common(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, int *handled)
{
    *handled = 0;
    switch (msg) {
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
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) {
            c->result = 0;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (c) c->result = 0;
        DestroyWindow(hwnd);
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
    HWND hType, hDelim, hIndex, hKeyword, hTarget, hFill;
    HWND hDelimLbl, hIndexLbl;
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
    int is_split = (int)SendMessageW(c->hType, CB_GETCURSEL, 0, 0) == 0;
    EnableWindow(c->hDelim, is_split);
    EnableWindow(c->hIndex, is_split);
    EnableWindow(c->hDelimLbl, is_split);
    EnableWindow(c->hIndexLbl, is_split);
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
        HWND tip;
        HWND lbl[6];
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
        SendMessageW(c->hType, CB_SETCURSEL, c->rule->type == PARSE_CONTAINS ? 1 : 0, 0);

        lbl[1] = c->hDelimLbl = mk_label_r(hwnd, L"分隔符：", 0);
        c->hDelim = mk_edit(hwnd, IDC_RL_DELIM, 0);
        SetWindowTextW(c->hDelim, c->rule->delimiter);

        lbl[2] = c->hIndexLbl = mk_label_r(hwnd, L"取第几段：", 0);
        c->hIndex = mk_edit(hwnd, IDC_RL_INDEX, ES_NUMBER);
        _snwprintf(buf, 32, L"%d", c->rule->segment_index > 0 ? c->rule->segment_index : 1);
        SetWindowTextW(c->hIndex, buf);

        lbl[3] = mk_label_r(hwnd, L"命中关键词：", 0);
        c->hKeyword = mk_edit(hwnd, IDC_RL_KEYWORD, 0);
        SetWindowTextW(c->hKeyword, c->rule->keyword);
        SendMessageW(c->hKeyword, EM_SETCUEBANNER, TRUE, (LPARAM)L"留空表示只要取到内容就算命中");

        lbl[4] = mk_label_r(hwnd, L"填充到变量：", 0);
        c->hTarget = mk_combo(hwnd, IDC_RL_TARGET);
        fill_var_combo(c->hTarget, c->tmpl, c->rule->target_segment_id, c->owner_seg, 0);

        lbl[5] = mk_label_r(hwnd, L"填充值：", 0);
        c->hFill = mk_edit(hwnd, IDC_RL_FILL, 0);
        SetWindowTextW(c->hFill, c->rule->fill_value);
        SendMessageW(c->hFill, EM_SETCUEBANNER, TRUE, (LPARAM)L"留空表示填入取到的原文");

        tip = mk_label(hwnd, L"例：设备编号按「-」切割取第 8 段，命中「DBS」→ 设备类型 填「存储服务器」。",
                       IDC_RL_TIP);
        mk_btn(hwnd, L"确定", IDOK);
        mk_btn(hwnd, L"取消", IDCANCEL);
        SendMessageW(GetDlgItem(hwnd, IDOK), BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);

        {
            HWND ctl[6];
            int k;
            ctl[0] = c->hType;   ctl[1] = c->hDelim;  ctl[2] = c->hIndex;
            ctl[3] = c->hKeyword; ctl[4] = c->hTarget; ctl[5] = c->hFill;
            for (k = 0; k < 6; ++k) {
                int iw = (k == 1 || k == 2) ? DPIX(90) : cw;
                MoveWindow(lbl[k], m, y + DPIX(4), lw, DPIX(18), TRUE);
                MoveWindow(ctl[k], cx, y, iw, DPIX(24), TRUE);
                y += rowh;
            }
        }
        MoveWindow(tip, m, y + DPIX(4), rc.right - m * 2, DPIX(34), TRUE);
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
            int is_split = (int)SendMessageW(c->hType, CB_GETCURSEL, 0, 0) == 0;

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
            c->rule->type = is_split ? PARSE_SPLIT : PARSE_CONTAINS;
            qp_strlcpy(c->rule->delimiter, buf, QP_SEP_MAX);
            qp_get_text(c->hIndex, buf, QP_VALUE_MAX);
            c->rule->segment_index = _wtoi(buf);
            if (c->rule->segment_index <= 0) c->rule->segment_index = 1;
            qp_get_text(c->hKeyword, buf, QP_VALUE_MAX);
            qp_trim(buf);
            qp_strlcpy(c->rule->keyword, buf, QP_KEYWORD_MAX);
            if (!is_split && !buf[0]) {
                qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"提示",
                          L"「包含关键词」规则必须填写关键词。");
                SetFocus(c->hKeyword);
                return 0;
            }
            qp_strlcpy(c->rule->target_segment_id, target->id, QP_ID_MAX);
            qp_get_text(c->hFill, buf, QP_VALUE_MAX);
            qp_strlcpy(c->rule->fill_value, buf, QP_VALUE_MAX);
            c->result = 1;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) { c->result = 0; DestroyWindow(hwnd); return 0; }
        break;
    case WM_CLOSE:
        if (c) c->result = 0;
        DestroyWindow(hwnd);
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
                   440, 268, owner, &c);
    return run_modal(owner, dlg, &c.result);
}

/* ================================================================== */
/* 条件规则编辑器                                                      */
/* ================================================================== */

typedef struct {
    HWND hwnd;
    ConditionRule *rule;
    const Template *tmpl;
    const Segment *owner_seg;
    int result;
    HWND hTrigger, hOp, hMatch, hResult;
} CondDlgCtx;

static LRESULT CALLBACK cond_dlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    CondDlgCtx *c = (CondDlgCtx *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    int handled = 0;
    LRESULT r;

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        int m = DPIX(14), lw = DPIX(84), y = DPIX(14), rowh = DPIX(28);
        int cx, cw;
        RECT rc;
        HWND l1, l2, l3, l4, tip;

        c = (CondDlgCtx *)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)c);
        c->hwnd = hwnd;
        GetClientRect(hwnd, &rc);
        cx = m + lw + DPIX(8);
        cw = rc.right - m - cx;

        l1 = mk_label_r(hwnd, L"当变量：", 0);
        c->hTrigger = mk_combo(hwnd, IDC_RL_TRIGGER);
        fill_var_combo(c->hTrigger, c->tmpl, c->rule->trigger_segment_id, c->owner_seg, 0);

        l2 = mk_label_r(hwnd, L"满足：", 0);
        c->hOp = mk_combo(hwnd, IDC_RL_OP);
        SendMessageW(c->hOp, CB_ADDSTRING, 0, (LPARAM)L"包含");
        SendMessageW(c->hOp, CB_ADDSTRING, 0, (LPARAM)L"等于");
        SendMessageW(c->hOp, CB_ADDSTRING, 0, (LPARAM)L"开头是");
        SendMessageW(c->hOp, CB_ADDSTRING, 0, (LPARAM)L"属于其中之一");
        SendMessageW(c->hOp, CB_SETCURSEL, (WPARAM)c->rule->op, 0);

        l3 = mk_label_r(hwnd, L"匹配值：", 0);
        c->hMatch = mk_edit(hwnd, IDC_RL_MATCH, 0);
        SetWindowTextW(c->hMatch, c->rule->match_value);
        SendMessageW(c->hMatch, EM_SETCUEBANNER, TRUE,
                     (LPARAM)L"「属于其中之一」可用逗号 / 、 分隔多个值");

        l4 = mk_label_r(hwnd, L"则本片段：", 0);
        c->hResult = mk_edit(hwnd, IDC_RL_RESULT,
                             ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | WS_VSCROLL);
        SetWindowTextW(c->hResult, c->rule->result_value);

        tip = mk_label(hwnd, L"多条规则按列表顺序判断，命中第一条即停止；都不命中时使用默认值。",
                       IDC_RL_TIP);
        mk_btn(hwnd, L"确定", IDOK);
        mk_btn(hwnd, L"取消", IDCANCEL);
        SendMessageW(GetDlgItem(hwnd, IDOK), BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);

        MoveWindow(l1, m, y + DPIX(4), lw, DPIX(18), TRUE);
        MoveWindow(c->hTrigger, cx, y, cw, DPIX(24), TRUE); y += rowh;
        MoveWindow(l2, m, y + DPIX(4), lw, DPIX(18), TRUE);
        MoveWindow(c->hOp, cx, y, DPIX(150), DPIX(24), TRUE); y += rowh;
        MoveWindow(l3, m, y + DPIX(4), lw, DPIX(18), TRUE);
        MoveWindow(c->hMatch, cx, y, cw, DPIX(24), TRUE); y += rowh;
        MoveWindow(l4, m, y + DPIX(4), lw, DPIX(18), TRUE);
        MoveWindow(c->hResult, cx, y, cw, DPIX(76), TRUE); y += DPIX(82);
        MoveWindow(tip, m, y, rc.right - m * 2, DPIX(34), TRUE);
        MoveWindow(GetDlgItem(hwnd, IDOK), rc.right - m - DPIX(180),
                   rc.bottom - m - DPIX(26), DPIX(86), DPIX(26), TRUE);
        MoveWindow(GetDlgItem(hwnd, IDCANCEL), rc.right - m - DPIX(88),
                   rc.bottom - m - DPIX(26), DPIX(86), DPIX(26), TRUE);

        qp_theme_children(hwnd);
        SetFocus(c->hTrigger);
        return 0;
    }
    case WM_COMMAND:
        qp_combo_dropdown_fix(wp, lp);
        if (!c) break;
        if (LOWORD(wp) == IDOK) {
            wchar_t buf[QP_RESULT_MAX];
            const Segment *trig = combo_selected_seg(c->hTrigger);
            int op = (int)SendMessageW(c->hOp, CB_GETCURSEL, 0, 0);
            if (!trig) {
                qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"提示",
                          L"请先在模板中添加一个变量作为触发条件。");
                return 0;
            }
            if (op == CB_ERR) op = OP_CONTAINS;
            qp_get_text(c->hMatch, buf, QP_RESULT_MAX);
            qp_trim(buf);
            if (!buf[0] && op != OP_EQUALS) {
                qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"提示", L"请填写匹配值。");
                SetFocus(c->hMatch);
                return 0;
            }
            qp_strlcpy(c->rule->trigger_segment_id, trig->id, QP_ID_MAX);
            c->rule->op = (CondOperator)op;
            cond_rule_set_match(c->rule, buf);
            qp_get_text(c->hResult, buf, QP_RESULT_MAX);
            qp_strlcpy(c->rule->result_value, buf, QP_RESULT_MAX);
            c->result = 1;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) { c->result = 0; DestroyWindow(hwnd); return 0; }
        break;
    case WM_CLOSE:
        if (c) c->result = 0;
        DestroyWindow(hwnd);
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
                   460, 300, owner, &c);
    return run_modal(owner, dlg, &c.result);
}

/* ================================================================== */
/* 模板编辑器                                                          */
/* ================================================================== */

typedef struct {
    HWND hwnd;
    Template *t;
    int is_new;
    int result;
    int loading;                 /* 正在填充控件，忽略 EN_CHANGE */
    int cur;                     /* 当前片段索引 */

    HWND hName, hGroup, hSegLbl, hSegList;
    HWND hAddStatic, hAddText, hAddSelect, hAddMulti, hSegDel, hSegUp, hSegDown;

    HWND hPropTitle;
    HWND hTypeLbl, hType;
    HWND hLabelLbl, hLabel;
    HWND hContentLbl, hContent;
    HWND hPlaceLbl, hPlace;
    HWND hDefLbl, hDef;
    HWND hSepLbl, hSep;
    HWND hHidden;

    HWND hOptLbl, hOptList, hOptAdd, hOptEdit, hOptDel, hOptUp, hOptDown;
    HWND hParseLbl, hParseList, hParseAdd, hParseEdit, hParseDel, hParseUp, hParseDown;
    HWND hCondLbl, hCondList, hCondAdd, hCondEdit, hCondDel, hCondUp, hCondDown;

    HWND hPreviewLbl, hPreview;
} EditorCtx;

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

/* 片段列表里显示的摘要；隐藏的变量加个标记，一眼能看出它不出现在填写界面 */
static void ed_seg_summary(const Segment *s, wchar_t *out, size_t cap)
{
    wchar_t *p;
    if (!out || cap == 0) return;
    out[0] = L'\0';
    if (!s) return;
    if (s->type == SEG_STATIC) {
        qp_strlcpy(out, s->content, cap);
    } else {
        _snwprintf(out, cap, L"【%ls】%ls", seg_display_name(s),
                   s->hidden ? L"（隐藏）" : L"");
        out[cap - 1] = L'\0';
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
        ed_seg_summary(s, summary, QP_CONTENT_MAX);
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

/* 按当前片段类型显示 / 隐藏并重新排布右侧属性区 */
static void ed_layout_props(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    RECT rc;
    int m = DPIX(12), x = DPIX(356), lw = DPIX(80);
    int cx, cw, y = DPIX(68);
    int has = s != NULL;
    int t = has ? (int)s->type : -1;
    int show_label = has && t != SEG_STATIC;
    int show_content = has && t == SEG_STATIC;
    int show_place = has && (t == SEG_TEXT || t == SEG_MULTI_SELECT);
    int show_def = has && t != SEG_STATIC;
    int show_sep = has && t == SEG_MULTI_SELECT;
    int show_hidden = has && t != SEG_STATIC;
    int show_opt = has && (t == SEG_SELECT || t == SEG_MULTI_SELECT);
    int show_parse = has && t == SEG_TEXT;
    int show_cond = has;
    int right;

    GetClientRect(c->hwnd, &rc);
    right = rc.right - m;
    cx = x + lw + DPIX(8);
    cw = right - cx;

#define ED_ROW(lbl, ctl, show, hgt)                                            \
    do {                                                                       \
        ShowWindow((lbl), (show) ? SW_SHOW : SW_HIDE);                          \
        ShowWindow((ctl), (show) ? SW_SHOW : SW_HIDE);                          \
        if (show) {                                                            \
            MoveWindow((lbl), x, y + DPIX(4), lw, DPIX(18), TRUE);              \
            MoveWindow((ctl), cx, y, cw, DPIX(hgt), TRUE);                      \
            y += DPIX(hgt) + DPIX(4);                                          \
        }                                                                      \
    } while (0)

    ShowWindow(c->hPropTitle, has ? SW_SHOW : SW_HIDE);
    MoveWindow(c->hPropTitle, x, DPIX(44), cw + lw, DPIX(18), TRUE);

    ED_ROW(c->hTypeLbl, c->hType, has, 24);
    ED_ROW(c->hLabelLbl, c->hLabel, show_label, 24);
    ED_ROW(c->hContentLbl, c->hContent, show_content, 80);
    ED_ROW(c->hPlaceLbl, c->hPlace, show_place, 24);
    ED_ROW(c->hDefLbl, c->hDef, show_def, 24);
    ED_ROW(c->hSepLbl, c->hSep, show_sep, 24);

    /* 「隐藏」开关自带文字，不需要左侧标签，与其他控件左对齐即可 */
    ShowWindow(c->hHidden, show_hidden ? SW_SHOW : SW_HIDE);
    if (show_hidden) {
        MoveWindow(c->hHidden, cx, y, cw, DPIX(22), TRUE);
        y += DPIX(22) + DPIX(6);
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
            MoveWindow((lbl), x, y + DPIX(3), bx - x - DPIX(4), DPIX(18), TRUE);\
            MoveWindow((b1), bx, y, bw, bh, TRUE); bx += bw + DPIX(2);          \
            MoveWindow((b2), bx, y, bw, bh, TRUE); bx += bw + DPIX(2);          \
            MoveWindow((b3), bx, y, bw, bh, TRUE); bx += bw + DPIX(2);          \
            MoveWindow((b4), bx, y, sw, bh, TRUE); bx += sw + DPIX(2);          \
            MoveWindow((b5), bx, y, sw, bh, TRUE);                              \
            y += bh + DPIX(4);                                                 \
            MoveWindow((list), x, y, right - x, DPIX(listh), TRUE);             \
            y += DPIX(listh) + DPIX(8);                                        \
        }                                                                      \
    } while (0)

    ED_BLOCK(c->hOptLbl, c->hOptList, c->hOptAdd, c->hOptEdit, c->hOptDel,
             c->hOptUp, c->hOptDown, show_opt, 92);
    ED_BLOCK(c->hParseLbl, c->hParseList, c->hParseAdd, c->hParseEdit, c->hParseDel,
             c->hParseUp, c->hParseDown, show_parse, 92);
    ED_BLOCK(c->hCondLbl, c->hCondList, c->hCondAdd, c->hCondEdit, c->hCondDel,
             c->hCondUp, c->hCondDown, show_cond, 92);

#undef ED_ROW
#undef ED_BLOCK

    /* 列表列宽随窗口 */
    ListView_SetColumnWidth(c->hOptList, 0, right - x - DPIX(26));
    ListView_SetColumnWidth(c->hParseList, 0, right - x - DPIX(26));
    ListView_SetColumnWidth(c->hCondList, 0, right - x - DPIX(26));
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
    } else {
        SendMessageW(c->hType, CB_SETCURSEL, (WPARAM)-1, 0);
        SetWindowTextW(c->hLabel, L"");
        SetWindowTextW(c->hContent, L"");
        SetWindowTextW(c->hPlace, L"");
        SetWindowTextW(c->hDef, L"");
        SetWindowTextW(c->hSep, L"");
        SendMessageW(c->hHidden, BM_SETCHECK, BST_UNCHECKED, 0);
    }
    ed_fill_optlist(c);
    ed_fill_rulelists(c);
    ed_layout_props(c);
    c->loading = 0;
}

static void ed_commit_props(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    wchar_t buf[QP_CONTENT_MAX];
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
    s->hidden = (s->type != SEG_STATIC &&
                 SendMessageW(c->hHidden, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    ed_update_preview(c);
}

/* 片段列表某行的文字随属性变化刷新 */
static void ed_refresh_cur_row(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    wchar_t summary[QP_CONTENT_MAX];
    if (!s) return;
    ed_seg_summary(s, summary, QP_CONTENT_MAX);
    lv_set_row(c->hSegList, c->cur, 1, seg_type_name(s->type));
    lv_set_row(c->hSegList, c->cur, 2, summary);
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
    if (!s) return;
    if (qp_msgbox(c->hwnd, MB_YESNO | MB_ICONQUESTION, L"删除片段",
                  L"确定删除第 %d 个片段吗？", c->cur + 1) != IDYES)
        return;
    s = seg_list_remove_at(&c->t->segments, c->cur);
    seg_free(s);
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
    if (s->type == SEG_STATIC) s->hidden = 0;   /* 固定句本来就不出现在填写界面 */
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
}

static void ed_parse_move(EditorCtx *c, int delta)
{
    Segment *s = ed_cur_seg(c);
    int i = lv_sel(c->hParseList);
    if (!s || i < 0) return;
    if (!parse_rule_move(&s->parse_rules, i, i + delta)) return;
    ed_fill_rulelists(c);
    lv_select(c->hParseList, i + delta);
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
        ConditionRule *keep = orig->next;
        wchar_t id[QP_ID_MAX];
        qp_strlcpy(id, orig->id, QP_ID_MAX);
        /* 释放原对象自己的 match_values，再整体接管副本内容 */
        {
            int k;
            for (k = 0; k < orig->match_value_count; ++k) free(orig->match_values[k]);
            free(orig->match_values);
        }
        *orig = *copy;
        qp_strlcpy(orig->id, id, QP_ID_MAX);
        orig->next = keep;
        free(copy);                 /* 内部指针已转移给 orig */
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
    int m = DPIX(12), y, lw = DPIX(66);
    int listx = DPIX(12), listw = DPIX(332);
    int bw, bh = DPIX(24);

    GetClientRect(c->hwnd, &rc);

    /* 第一行：名称 + 分组 */
    y = m;
    MoveWindow(GetDlgItem(c->hwnd, IDC_ED_NAME + 1000), m, y + DPIX(4), lw, DPIX(18), TRUE);
    MoveWindow(c->hName, m + lw + DPIX(6), y, DPIX(230), DPIX(24), TRUE);
    MoveWindow(GetDlgItem(c->hwnd, IDC_ED_GROUP + 1000),
               m + lw + DPIX(244), y + DPIX(4), DPIX(44), DPIX(18), TRUE);
    MoveWindow(c->hGroup, m + lw + DPIX(292), y, DPIX(180), DPIX(24), TRUE);

    /* 左侧：片段列表 */
    y = DPIX(44);
    MoveWindow(c->hSegLbl, listx, y, listw, DPIX(18), TRUE);
    y = DPIX(68);
    bw = (listw - DPIX(9)) / 4;
    MoveWindow(c->hAddStatic, listx, y, bw, bh, TRUE);
    MoveWindow(c->hAddText, listx + bw + DPIX(3), y, bw, bh, TRUE);
    MoveWindow(c->hAddSelect, listx + (bw + DPIX(3)) * 2, y, bw, bh, TRUE);
    MoveWindow(c->hAddMulti, listx + (bw + DPIX(3)) * 3, y, bw, bh, TRUE);
    y += bh + DPIX(4);
    MoveWindow(c->hSegDel, listx, y, bw, bh, TRUE);
    MoveWindow(c->hSegUp, listx + bw + DPIX(3), y, DPIX(40), bh, TRUE);
    MoveWindow(c->hSegDown, listx + bw + DPIX(46), y, DPIX(40), bh, TRUE);
    y += bh + DPIX(6);
    MoveWindow(c->hSegList, listx, y, listw, DPIX(546) - y, TRUE);
    ListView_SetColumnWidth(c->hSegList, 2, listw - DPIX(96) - DPIX(20));

    /* 底部：预览 + 按钮 */
    MoveWindow(c->hPreviewLbl, m, DPIX(552), rc.right - m * 2, DPIX(16), TRUE);
    MoveWindow(c->hPreview, m, DPIX(570), rc.right - m * 2, DPIX(46), TRUE);
    MoveWindow(GetDlgItem(c->hwnd, IDOK), rc.right - m - DPIX(196),
               DPIX(624), DPIX(92), DPIX(26), TRUE);
    MoveWindow(GetDlgItem(c->hwnd, IDCANCEL), rc.right - m - DPIX(98),
               DPIX(624), DPIX(92), DPIX(26), TRUE);

    ed_layout_props(c);
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

static LRESULT CALLBACK editor_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    EditorCtx *c = (EditorCtx *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    int handled = 0;
    LRESULT r;

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
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

        /* 左侧 */
        c->hSegLbl = mk_label(hwnd, L"片段（自上而下拼接，可拖动排序）", 0);
        c->hAddStatic = mk_btn(hwnd, L"+固定", IDC_ED_SEG_ADD_STATIC);
        c->hAddText = mk_btn(hwnd, L"+输入", IDC_ED_SEG_ADD_TEXT);
        c->hAddSelect = mk_btn(hwnd, L"+单选", IDC_ED_SEG_ADD_SELECT);
        c->hAddMulti = mk_btn(hwnd, L"+多选", IDC_ED_SEG_ADD_MULTI);
        c->hSegDel = mk_btn(hwnd, L"删除", IDC_ED_SEG_DEL);
        c->hSegUp = mk_btn(hwnd, L"↑", IDC_ED_SEG_UP);
        c->hSegDown = mk_btn(hwnd, L"↓", IDC_ED_SEG_DOWN);
        c->hSegList = mk_list(hwnd, IDC_ED_SEGLIST, 0);
        lv_add_col(c->hSegList, 0, L"#", 32);
        lv_add_col(c->hSegList, 1, L"类型", 64);
        lv_add_col(c->hSegList, 2, L"内容 / 变量", 200);

        /* 右侧属性 */
        c->hPropTitle = mk_label(hwnd, L"片段属性", 0);
        SendMessageW(c->hPropTitle, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
        c->hTypeLbl = mk_label_r(hwnd, L"类型：", 0);
        c->hType = mk_combo(hwnd, IDC_ED_SEG_TYPE);
        SendMessageW(c->hType, CB_ADDSTRING, 0, (LPARAM)L"固定文本");
        SendMessageW(c->hType, CB_ADDSTRING, 0, (LPARAM)L"文本输入");
        SendMessageW(c->hType, CB_ADDSTRING, 0, (LPARAM)L"下拉单选");
        SendMessageW(c->hType, CB_ADDSTRING, 0, (LPARAM)L"下拉多选");
        c->hLabelLbl = mk_label_r(hwnd, L"变量名：", 0);
        c->hLabel = mk_edit(hwnd, IDC_ED_SEG_LABEL, 0);
        c->hContentLbl = mk_label_r(hwnd, L"固定内容：", 0);
        c->hContent = mk_edit(hwnd, IDC_ED_SEG_CONTENT,
                              ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | WS_VSCROLL);
        c->hPlaceLbl = mk_label_r(hwnd, L"占位提示：", 0);
        c->hPlace = mk_edit(hwnd, IDC_ED_SEG_PLACE, 0);
        c->hDefLbl = mk_label_r(hwnd, L"默认值：", 0);
        c->hDef = mk_edit(hwnd, IDC_ED_SEG_DEFAULT, 0);
        c->hSepLbl = mk_label_r(hwnd, L"分隔符：", 0);
        c->hSep = mk_edit(hwnd, IDC_ED_SEG_SEP, 0);
        c->hHidden = mk_check(hwnd, L"在填写界面隐藏（只由变量匹配 / 默认值填充）",
                              IDC_ED_SEG_HIDDEN);

        c->hOptLbl = mk_label(hwnd, L"选项", 0);
        c->hOptAdd = mk_btn(hwnd, L"添加", IDC_ED_OPT_ADD);
        c->hOptEdit = mk_btn(hwnd, L"修改", IDC_ED_OPT_EDIT);
        c->hOptDel = mk_btn(hwnd, L"删除", IDC_ED_OPT_DEL);
        c->hOptUp = mk_btn(hwnd, L"↑", IDC_ED_OPT_UP);
        c->hOptDown = mk_btn(hwnd, L"↓", IDC_ED_OPT_DOWN);
        c->hOptList = mk_list(hwnd, IDC_ED_OPTLIST, LVS_NOCOLUMNHEADER);
        lv_add_col(c->hOptList, 0, L"选项", 300);

        c->hParseLbl = mk_label(hwnd, L"解析规则（从本输入中提取信息，自动填充其他变量）", 0);
        c->hParseAdd = mk_btn(hwnd, L"添加", IDC_ED_PARSE_ADD);
        c->hParseEdit = mk_btn(hwnd, L"修改", IDC_ED_PARSE_EDIT);
        c->hParseDel = mk_btn(hwnd, L"删除", IDC_ED_PARSE_DEL);
        c->hParseUp = mk_btn(hwnd, L"↑", IDC_ED_PARSE_UP);
        c->hParseDown = mk_btn(hwnd, L"↓", IDC_ED_PARSE_DOWN);
        c->hParseList = mk_list(hwnd, IDC_ED_PARSELIST, LVS_NOCOLUMNHEADER);
        lv_add_col(c->hParseList, 0, L"规则", 300);

        c->hCondLbl = mk_label(hwnd, L"条件规则（按其他变量的值决定本片段输出）", 0);
        c->hCondAdd = mk_btn(hwnd, L"添加", IDC_ED_COND_ADD);
        c->hCondEdit = mk_btn(hwnd, L"修改", IDC_ED_COND_EDIT);
        c->hCondDel = mk_btn(hwnd, L"删除", IDC_ED_COND_DEL);
        c->hCondUp = mk_btn(hwnd, L"↑", IDC_ED_COND_UP);
        c->hCondDown = mk_btn(hwnd, L"↓", IDC_ED_COND_DOWN);
        c->hCondList = mk_list(hwnd, IDC_ED_CONDLIST, LVS_NOCOLUMNHEADER);
        lv_add_col(c->hCondList, 0, L"规则", 300);

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
        c->cur = seg_list_count(c->t->segments) > 0 ? 0 : -1;
        ed_layout(c);
        ed_fill_seglist(c, c->cur < 0 ? 0 : c->cur);
        ed_load_props(c);
        ed_update_preview(c);
        qp_theme_children(hwnd);
        c->loading = 0;
        SetFocus(c->hName);
        SendMessageW(c->hName, EM_SETSEL, 0, -1);
        return 0;
    }

    case WM_SIZE:
        if (c) ed_layout(c);
        return 0;

    case WM_COMMAND: {
        WORD id = LOWORD(wp), code = HIWORD(wp);
        qp_combo_dropdown_fix(wp, lp);
        if (!c) break;
        switch (id) {
        case IDC_ED_SEG_ADD_STATIC: ed_add_segment(c, SEG_STATIC); return 0;
        case IDC_ED_SEG_ADD_TEXT:   ed_add_segment(c, SEG_TEXT); return 0;
        case IDC_ED_SEG_ADD_SELECT: ed_add_segment(c, SEG_SELECT); return 0;
        case IDC_ED_SEG_ADD_MULTI:  ed_add_segment(c, SEG_MULTI_SELECT); return 0;
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
            if (code == BN_CLICKED && !c->loading) {
                ed_commit_props(c);
                ed_refresh_cur_row(c);
            }
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
            int gi;
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
            qp_strlcpy(c->t->name, name, QP_NAME_MAX);
            gi = (int)SendMessageW(c->hGroup, CB_GETCURSEL, 0, 0);
            g = (gi > 0) ? (Group *)SendMessageW(c->hGroup, CB_GETITEMDATA, (WPARAM)gi, 0)
                         : NULL;
            qp_strlcpy(c->t->group_id, g ? g->id : L"", QP_ID_MAX);
            c->result = 1;
            DestroyWindow(hwnd);
            return 0;
        }
        case IDCANCEL:
            c->result = 0;
            DestroyWindow(hwnd);
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
        DestroyWindow(hwnd);
        return 0;

    default:
        break;
    }

    r = dlg_common(hwnd, msg, wp, lp, &handled);
    if (handled) return r;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int dlg_edit_template(HWND owner, Template *working, int is_new)
{
    EditorCtx c;
    HWND dlg;
    memset(&c, 0, sizeof(c));
    c.t = working;
    c.is_new = is_new;
    c.cur = -1;
    dlg = make_dlg(L"QuickPhraseEditor", editor_proc,
                   is_new ? L"新建模板" : L"编辑模板", 880, 662, owner, &c);
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
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) { c->result = 0; DestroyWindow(hwnd); return 0; }
        break;
    case WM_CLOSE:
        if (c) c->result = 0;
        DestroyWindow(hwnd);
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
