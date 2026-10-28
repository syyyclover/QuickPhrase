#include "dialogs_internal.h"
/* ================================================================== */
/* 通用模态窗口骨架                                                    */
/* ================================================================== */

void ensure_class(const wchar_t *cls, WNDPROC proc)
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
HWND make_dlg_ex(const wchar_t *cls, WNDPROC proc, const wchar_t *title,
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
    int fitted=1;
    if(!(extra&WS_THICKFRAME))fitted=qp_dialog_fit_fixed(hwnd,owner);
    else if(!wcscmp(cls,L"QuickPhraseTables"))fitted=qp_dialog_fit_resizable(hwnd,owner,DPIX(580),DPIX(440));
    else if(!wcscmp(cls,L"QuickPhraseParseRule"))fitted=qp_dialog_fit_resizable(hwnd,owner,DPIX(440),DPIX(240));
    if (!fitted) {
        DestroyWindow(hwnd);
        qp_msgbox(owner,MB_OK|MB_ICONERROR,L"无法打开窗口",
                  L"无法准备适合当前工作区的窗口。请关闭不需要的窗口后重试。原有编辑未提交。");
        return NULL;
    }
    qp_center_window(hwnd, owner);
    return hwnd;
}

HWND make_dlg(const wchar_t *cls, WNDPROC proc, const wchar_t *title,
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
void dlg_close(HWND hwnd)
{
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                 SWP_HIDEWINDOW | SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    dlg_release_owner(hwnd);
    DestroyWindow(hwnd);
}

int run_modal(HWND owner, HWND dlg, const int *result)
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

LRESULT dlg_common(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, int *handled)
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
HWND mk_label(HWND p, const wchar_t *text, int id)
{
    return qp_create(L"Static", text, SS_LEFT | SS_ENDELLIPSIS, 0, 0, 0, 10, 10, p, id);
}
HWND mk_label_r(HWND p, const wchar_t *text, int id)
{
    return qp_create(L"Static", text, SS_RIGHT | SS_ENDELLIPSIS, 0, 0, 0, 10, 10, p, id);
}
HWND mk_wrapped_label(HWND p,const wchar_t *text,int id)
{
    return qp_create(L"Static",text,SS_LEFT|SS_NOPREFIX,0,0,0,10,10,p,id);
}
HWND mk_edit(HWND p, int id, DWORD extra)
{
    return qp_create(L"Edit", L"", ES_AUTOHSCROLL | WS_TABSTOP | extra,
                     WS_EX_CLIENTEDGE, 0, 0, 10, 10, p, id);
}
HWND mk_btn(HWND p, const wchar_t *text, int id)
{
    return qp_create(L"Button", text, BS_PUSHBUTTON | WS_TABSTOP, 0,
                     0, 0, 10, 10, p, id);
}
HWND mk_check(HWND p, const wchar_t *text, int id)
{
    return qp_create(L"Button", text, BS_AUTOCHECKBOX | WS_TABSTOP, 0,
                     0, 0, 10, 10, p, id);
}
HWND mk_combo(HWND p, int id)
{
    return qp_create(L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0,
                     0, 0, 10, DPIX(260), p, id);
}
/* 对话框里的单选组由下一个 WS_GROUP 终止；属性区的第一个普通控件另起一组。 */
HWND mk_combo_group(HWND p, int id)
{
    return qp_create(L"ComboBox", L"",
                     CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_GROUP, 0,
                     0, 0, 10, DPIX(260), p, id);
}
HWND mk_radio(HWND p, const wchar_t *text, int id, int first_in_group)
{
    return qp_create(L"Button", text,
                     BS_AUTORADIOBUTTON | WS_TABSTOP |
                     (first_in_group ? WS_GROUP : 0), 0,
                     0, 0, 10, 10, p, id);
}
/* 可下拉也可自己敲的组合框（时间格式：预置几个够用的，剩下的让人自己写） */
HWND mk_combo_edit(HWND p, int id)
{
    return qp_create(L"ComboBox", L"",
                     CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP, 0,
                     0, 0, 10, DPIX(220), p, id);
}
HWND mk_list(HWND p, int id, DWORD extra)
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
void lv_add_col(HWND h, int idx, const wchar_t *title, int width)
{
    LVCOLUMNW c;
    memset(&c, 0, sizeof(c));
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    c.iSubItem = idx;
    c.pszText = (LPWSTR)title;
    c.cx = DPIX(width);
    ListView_InsertColumn(h, idx, &c);
}
void lv_set_row(HWND h, int row, int col, const wchar_t *text)
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
int lv_sel(HWND h) { return ListView_GetNextItem(h, -1, LVNI_SELECTED); }
void lv_select(HWND h, int i)
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
    int *custom;
    int custom_available;
    HWND hCustom;
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
        SendMessageW(c->hEdit, EM_SETLIMITTEXT, c->cap > 0 ? c->cap - 1 : 0, 0);
        mk_btn(hwnd, L"确定", IDOK);
        mk_btn(hwnd, L"取消", IDCANCEL);

        MoveWindow(lbl, m, m, w, DPIX(18), TRUE);
        MoveWindow(c->hEdit, m, m + DPIX(24), w,
                   c->multiline ? DPIX(72) : DPIX(24), TRUE);
        if(c->custom) {
            c->hCustom=mk_check(hwnd,L"允许填写者自主补充",IDC_INPUT_CUSTOM);
            SendMessageW(c->hCustom,BM_SETCHECK,*c->custom?BST_CHECKED:BST_UNCHECKED,0);
            EnableWindow(c->hCustom,c->custom_available);
            MoveWindow(c->hCustom,m,m+DPIX(56),w,DPIX(24),TRUE);
            HWND help=mk_wrapped_label(hwnd,c->custom_available
                ? L"选择此项后填写自己的内容，不改变模板选项。"
                : L"该变量已有自主补充项，请先修改或删除原项。",0);
            int height=qp_wrapped_text_height(help,w);
            MoveWindow(help,m,m+DPIX(86),w,height,TRUE);
        }
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
        qp_combo_dropdown_fix(wp, lp);
        if (!c) break;
        if (LOWORD(wp) == IDOK) {
            qp_get_text(c->hEdit, c->buf, c->cap);
            if(c->custom)*c->custom=SendMessageW(c->hCustom,BM_GETCHECK,0,0)==BST_CHECKED;
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

int dlg_option(HWND owner,const wchar_t *title,wchar_t *buf,size_t cap,
               int *custom,int custom_available)
{
    InputCtx c={0};
    c.prompt=L"选项名称：";
    c.buf=buf;c.cap=cap;c.custom=custom;c.custom_available=custom_available;
    HWND dlg=make_dlg(L"QuickPhraseInput",input_proc,title,420,200,owner,&c);
    return run_modal(owner,dlg,&c.result);
}

/* ================================================================== */
/* 关于                                                                */
/* ================================================================== */

void dlg_about(HWND owner)
{
    wchar_t dir[600] = L"";
    storage_get_data_dir(dir, 600);
    qp_msgbox(owner, MB_OK | MB_ICONINFORMATION, L"关于口径生成器",
              L"%ls  QuickPhrase  v%ls\n\n"
              L"使用模板与规则生成重复性工作中的输出文本。\n"
              L"C11 + Win32 API 实现，单文件绿色程序，无运行时依赖。\n\n"
              L"全局热键：打开主界面（可在「选项」中修改）\n"
              L"Ctrl+Enter：一键复制    Ctrl+F：搜索模板\n"
              L"Ctrl+N：新建模板        Ctrl+E：编辑模板\n\n"
              L"填写会话仅在本次运行中保留；模板保存在数据目录：\n%ls",
              QP_APP_TITLE, QP_APP_VERSION, dir);
}
