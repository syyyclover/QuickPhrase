#include "ui_internal.h"
/* ================================================================== */
/* 主题                                                                */
/* ================================================================== */

COLORREF theme_bk(void)      { return g_dark ? CLR_DARK_BK : CLR_LIGHT_BK; }
COLORREF theme_ctl_bk(void)  { return g_dark ? CLR_DARK_CTL : CLR_LIGHT_CTL; }
COLORREF theme_text(void)    { return g_dark ? CLR_DARK_TEXT : CLR_LIGHT_TEXT; }
COLORREF theme_dim(void)     { return g_dark ? CLR_DARK_DIM : CLR_LIGHT_DIM; }
COLORREF theme_accent(void)  { return g_dark ? CLR_DARK_ACCENT : CLR_LIGHT_ACCENT; }
COLORREF theme_error(void)   { return g_dark ? CLR_DARK_ERROR : CLR_LIGHT_ERROR; }

/* Win10+ 深色控件：uxtheme 的未文档化导出，按序号加载，不可用时静默降级 */








static FnAllowDarkModeForWindow s_allowDarkWindow;
FnFlushMenuThemes        s_flushMenuThemes;
static FnDwmSetWindowAttribute  s_dwmSetAttr;
static int s_darkApiReady;
static DWORD s_winBuild;

static void dark_api_init(void)
{
    HMODULE hUx, hDwm, hNt;
    if (s_darkApiReady) return;
    s_darkApiReady = 1;

    hNt = GetModuleHandleW(L"ntdll.dll");
    if (hNt) {
        FnRtlGetNtVersionNumbers fn =
            (FnRtlGetNtVersionNumbers)(void *)GetProcAddress(hNt, "RtlGetNtVersionNumbers");
        if (fn) {
            DWORD maj = 0, min = 0, bld = 0;
            fn(&maj, &min, &bld);
            s_winBuild = bld & 0x0FFFFFFF;
        }
    }
    hDwm = LoadLibraryW(L"dwmapi.dll");
    if (hDwm)
        s_dwmSetAttr = (FnDwmSetWindowAttribute)(void *)GetProcAddress(hDwm, "DwmSetWindowAttribute");

    if (s_winBuild < 17763) return;   /* 早于 Win10 1809 无深色控件支持 */
    hUx = LoadLibraryW(L"uxtheme.dll");
    if (!hUx) return;
    s_allowDarkWindow =
        (FnAllowDarkModeForWindow)(void *)GetProcAddress(hUx, MAKEINTRESOURCEA(133));
    s_flushMenuThemes =
        (FnFlushMenuThemes)(void *)GetProcAddress(hUx, MAKEINTRESOURCEA(136));
    if (s_winBuild >= 18362) {
        FnSetPreferredAppMode set =
            (FnSetPreferredAppMode)(void *)GetProcAddress(hUx, MAKEINTRESOURCEA(135));
        if (set) set(APPMODE_ALLOWDARK);
    } else {
        FnAllowDarkModeForApp allow =
            (FnAllowDarkModeForApp)(void *)GetProcAddress(hUx, MAKEINTRESOURCEA(135));
        if (allow) allow(TRUE);
    }
}

void dark_apply_window(HWND hwnd)
{
    dark_api_init();
    if (s_allowDarkWindow) s_allowDarkWindow(hwnd, g_dark ? TRUE : FALSE);
    if (s_dwmSetAttr) {
        BOOL v = g_dark ? TRUE : FALSE;
        /* 20 = DWMWA_USE_IMMERSIVE_DARK_MODE（1903 之前为 19） */
        if (FAILED(s_dwmSetAttr(hwnd, 20, &v, sizeof(v))))
            s_dwmSetAttr(hwnd, 19, &v, sizeof(v));
    }
}

void qp_theme_list(HWND h)
{
    if (!h) return;
    dark_api_init();
    if (s_allowDarkWindow) s_allowDarkWindow(h, g_dark ? TRUE : FALSE);
    SetWindowTheme(h, g_dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
}

static void theme_control(HWND h)
{
    wchar_t cls[64];
    if (!h) return;
    GetClassNameW(h, cls, 64);
    dark_api_init();
    if (s_allowDarkWindow) s_allowDarkWindow(h, g_dark ? TRUE : FALSE);

    if (lstrcmpiW(cls, L"SysTreeView32") == 0) {
        SetWindowTheme(h, g_dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
        SendMessageW(h, TVM_SETBKCOLOR, 0, (LPARAM)theme_ctl_bk());
        SendMessageW(h, TVM_SETTEXTCOLOR, 0, (LPARAM)theme_text());
    } else if (lstrcmpiW(cls, L"SysListView32") == 0) {
        SetWindowTheme(h, g_dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
        SendMessageW(h, LVM_SETBKCOLOR, 0, (LPARAM)theme_ctl_bk());
        SendMessageW(h, LVM_SETTEXTBKCOLOR, 0, (LPARAM)theme_ctl_bk());
        SendMessageW(h, LVM_SETTEXTCOLOR, 0, (LPARAM)theme_text());
    } else if (lstrcmpiW(cls, L"Button") == 0 || lstrcmpiW(cls, L"ComboBox") == 0 ||
               lstrcmpiW(cls, L"Edit") == 0 || lstrcmpiW(cls, L"ListBox") == 0) {
        SetWindowTheme(h, g_dark ? L"DarkMode_Explorer" : NULL,
                       g_dark ? NULL : NULL);
        if (lstrcmpiW(cls, L"ComboBox") == 0)
            SetWindowTheme(h, g_dark ? L"DarkMode_CFD" : NULL, NULL);
    }
    InvalidateRect(h, NULL, TRUE);
}

static BOOL CALLBACK theme_child_cb(HWND child, LPARAM lp)
{
    (void)lp;
    theme_control(child);
    return TRUE;
}

void qp_theme_children(HWND parent)
{
    if (!parent) return;
    dark_apply_window(parent);
    EnumChildWindows(parent, theme_child_cb, 0);
    /* 主窗口/字段区带 WS_CLIPCHILDREN，父窗口的 InvalidateRect 不会下达到
       子控件，必须显式 RDW_ALLCHILDREN，否则标签列会停在旧配色/空白状态 */
    RedrawWindow(parent, NULL, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

static BOOL CALLBACK font_child_cb(HWND child, LPARAM lp)
{
    SendMessageW(child, WM_SETFONT, (WPARAM)lp, TRUE);
    return TRUE;
}

void qp_apply_font(HWND parent)
{
    if (!parent || !g_fontUI) return;
    EnumChildWindows(parent, font_child_cb, (LPARAM)g_fontUI);
}

/* 统一处理各种 WM_CTLCOLOR*，供所有窗口过程复用 */
LRESULT qp_handle_ctlcolor(UINT msg, WPARAM wp, LPARAM lp, int *handled)
{
    HDC hdc = (HDC)wp;
    HWND ctl = (HWND)lp;
    wchar_t cls[64] = L"";

    *handled = 0;
    if (!g_dark) {
        /* 浅色主题下仅统一静态控件背景，其余交给系统 */
        if (msg == WM_CTLCOLORSTATIC) {
            SetBkColor(hdc, theme_bk());
            SetTextColor(hdc, theme_text());
            *handled = 1;
            return (LRESULT)g_brushBk;
        }
        return 0;
    }

    if (ctl) GetClassNameW(ctl, cls, 64);
    switch (msg) {
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
        SetBkColor(hdc, theme_bk());
        SetTextColor(hdc, theme_text());
        SetBkMode(hdc, OPAQUE);
        *handled = 1;
        return (LRESULT)g_brushBk;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetBkColor(hdc, theme_ctl_bk());
        SetTextColor(hdc, theme_text());
        *handled = 1;
        return (LRESULT)g_brushCtl;
    case WM_CTLCOLORBTN:
        SetBkColor(hdc, theme_bk());
        SetTextColor(hdc, theme_text());
        *handled = 1;
        return (LRESULT)g_brushBk;
    default:
        break;
    }
    return 0;
}

/* ================================================================== */
/* 通用小工具                                                          */
/* ================================================================== */

/* Input limits */
static size_t edit_capacity(int id)
{
    switch (id) {
    case IDC_ED_NAME: return QP_NAME_MAX;
    case IDC_ED_SEG_LABEL: return QP_LABEL_MAX;
    case IDC_ED_SEG_CONTENT: return QP_CONTENT_MAX;
    case IDC_ED_SEG_PLACE: return QP_PLACEHOLD_MAX;
    case IDC_ED_SEG_SEP: case IDC_SET_SEP: case IDC_LK_SEP: return QP_SEP_MAX;
    case IDC_RL_DELIM: return QP_SEP_MAX;
    case IDC_RL_MARKER_AFTER: return QP_KEYWORD_MAX;
    case IDC_RL_MATCH: return QP_MATCH_MAX;
    case IDC_ED_TABLE_FORMAT: return QP_OUTPUT_MAX;
    case IDC_ED_SEG_FORMAT: return QP_FORMAT_MAX;
    case IDC_SET_DATADIR: return QP_PATH_MAX;
    default: return QP_VALUE_MAX;
    }
}

HWND qp_create(const wchar_t *cls, const wchar_t *text, DWORD style, DWORD ex,
               int x, int y, int w, int h, HWND parent, int id)
{
    HWND hwnd = CreateWindowExW(ex, cls, text, style | WS_CHILD | WS_VISIBLE,
                                x, y, w, h, parent, (HMENU)(INT_PTR)id, g_hInst, NULL);
    if (hwnd) {
        if (lstrcmpiW(cls, L"Edit") == 0)
            SendMessageW(hwnd, EM_SETLIMITTEXT, (WPARAM)(edit_capacity(id) - 1), 0);
        SendMessageW(hwnd, WM_SETFONT, (WPARAM)g_fontUI, TRUE);
        theme_control(hwnd);
        if(lstrcmpiW(cls,L"Static")==0) qp_buffer_static(hwnd);
    }
    return hwnd;
}

void qp_get_text(HWND h, wchar_t *buf, size_t cap)
{
    if (!buf || cap == 0) return;
    buf[0] = L'\0';
    if (h) GetWindowTextW(h, buf, (int)cap);
}

void qp_set_text_if_diff(HWND h, const wchar_t *text)
{
    wchar_t stackbuf[QP_VALUE_MAX];
    wchar_t *cur = stackbuf;
    int len, cap = QP_VALUE_MAX;
    if (!h) return;
    if (!text) text = L"";
    /* 预览框 / 多行输入可能远超 QP_VALUE_MAX：截断着比较会「永远不相等」，
       每次刷新都重设文本，光标与滚动位置就会跟着跳。 */
    len = GetWindowTextLengthW(h);
    if (len >= cap) {
        cur = (wchar_t *)malloc((size_t)(len + 1) * sizeof(wchar_t));
        if (!cur) { SetWindowTextW(h, text); return; }
        cap = len + 1;
    }
    GetWindowTextW(h, cur, cap);
    if (wcscmp(cur, text) != 0) SetWindowTextW(h, text);
    if (cur != stackbuf) free(cur);
}

int qp_msgbox(HWND owner, UINT type, const wchar_t *title, const wchar_t *fmt, ...)
{
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 1024, fmt, ap);
    buf[1023] = L'\0';
    va_end(ap);
    return MessageBoxW(owner, buf, title ? title : QP_APP_TITLE, type);
}

int qp_copy_to_clipboard(HWND owner, const wchar_t *text)
{
    size_t bytes;
    HGLOBAL hmem;
    void *dst;

    if (!text) return 0;
    bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    if (!OpenClipboard(owner)) return 0;
    EmptyClipboard();
    hmem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hmem) { CloseClipboard(); return 0; }
    dst = GlobalLock(hmem);
    if (!dst) { GlobalFree(hmem); CloseClipboard(); return 0; }
    memcpy(dst, text, bytes);
    GlobalUnlock(hmem);
    if (!SetClipboardData(CF_UNICODETEXT, hmem)) {
        GlobalFree(hmem);
        CloseClipboard();
        return 0;
    }
    CloseClipboard();
    return 1;
}

int qp_window_work_area(HWND owner, RECT *work)
{
    MONITORINFO monitor = {0};
    monitor.cbSize = sizeof(monitor);
    HMONITOR screen = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    if (screen && GetMonitorInfoW(screen, &monitor)) {
        *work = monitor.rcWork;
        return 1;
    }
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, work, 0)) return 1;
    *work = (RECT){0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN)};
    return work->right > 0 && work->bottom > 0;
}

void qp_center_window(HWND hwnd, HWND owner)
{
    RECT rc, ro, work;
    int x, y, w, h;
    if (!hwnd) return;
    GetWindowRect(hwnd, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    qp_window_work_area(owner ? owner : hwnd, &work);
    if (owner && IsWindowVisible(owner)) {
        GetWindowRect(owner, &ro);
    } else {
        ro = work;
    }
    x = ro.left + ((ro.right - ro.left) - w) / 2;
    y = ro.top + ((ro.bottom - ro.top) - h) / 2;
    if (x + w > work.right) x = work.right - w;
    if (y + h > work.bottom) y = work.bottom - h;
    if (x < work.left) x = work.left;
    if (y < work.top) y = work.top;
    SetWindowPos(hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

/* 模态窗口消息循环：owner 在调用前后由调用方禁用/恢复 */
int qp_modal_loop(HWND owner, HWND modal, const int *result_flag)
{
    MSG msg;
    (void)owner;
    while (IsWindow(modal)) {
        if (GetMessageW(&msg, NULL, 0, 0) <= 0) {
            PostQuitMessage(0);      /* 把 WM_QUIT 交还给主循环 */
            break;
        }
        if (!IsWindow(modal)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            break;
        }
        if (IsDialogMessageW(modal, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return result_flag ? *result_flag : 0;
}

/*
 * ComboBox 的窗口高度同时决定下拉列表高度：布局时只给 24px 会让下拉框
 * 只剩一行。这里在即将下拉时临时拉高、收起后还原，避免额外占用点击区域。
 */
int qp_combo_dropdown_fix(WPARAM wp, LPARAM lp)
{
    HWND combo = (HWND)lp;
    WORD code = HIWORD(wp);
    RECT rc;
    int w, h;

    if (!combo) return 0;
    if (code == EN_MAXTEXT) {
        wchar_t cls[32];
        GetClassNameW(combo, cls, 32);
        if (lstrcmpiW(cls, L"Edit") == 0) {
            qp_msgbox(GetAncestor(combo, GA_ROOT), MB_OK | MB_ICONWARNING, L"输入长度已达上限",
                      L"当前输入框最多支持 %ld 个字符。请减少内容后重试。",
                      (long)SendMessageW(combo, EM_GETLIMITTEXT, 0, 0));
            return 1;
        }
    }
    if (code != CBN_DROPDOWN && code != CBN_CLOSEUP) return 0;
    {
        wchar_t cls[32];
        GetClassNameW(combo, cls, 32);
        if (lstrcmpiW(cls, L"ComboBox") != 0) return 0;
    }
    GetWindowRect(combo, &rc);
    w = rc.right - rc.left;

    if (code == CBN_DROPDOWN) {
        int items = (int)SendMessageW(combo, CB_GETCOUNT, 0, 0);
        int ih = (int)SendMessageW(combo, CB_GETITEMHEIGHT, 0, 0);
        h = rc.bottom - rc.top;
        SetPropW(combo, L"qpClosedH", (HANDLE)(INT_PTR)h);
        if (items < 1) items = 1;
        if (items > 14) items = 14;
        if (ih <= 0) ih = DPIX(18);
        SetWindowPos(combo, NULL, 0, 0, w, h + items * ih + DPIX(4),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        h = (int)(INT_PTR)GetPropW(combo, L"qpClosedH");
        if (h > 0) {
            SetWindowPos(combo, NULL, 0, 0, w, h,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            RemovePropW(combo, L"qpClosedH");
        }
    }
    return 0;   /* 不吞掉通知，调用方仍可处理 CBN_SELCHANGE 等 */
}

/* ListView 简易拖拽排序：按住左键移动到目标行后释放 */
int qp_lv_drag_reorder(HWND hlv, int from_index)
{
    MSG msg;
    int target = -1;
    RECT rc;

    if (!hlv || from_index < 0) return -1;
    SetCapture(hlv);
    SetCursor(LoadCursorW(NULL, IDC_SIZENS));

    while (GetCapture() == hlv) {
        if (GetMessageW(&msg, NULL, 0, 0) <= 0) {
            PostQuitMessage(0);          /* WM_QUIT 交还主循环 */
            target = -1;
            break;
        }
        if (msg.message == WM_MOUSEMOVE) {
            LVHITTESTINFO ht;
            ht.pt.x = (short)LOWORD(msg.lParam);
            ht.pt.y = (short)HIWORD(msg.lParam);
            SetCursor(LoadCursorW(NULL, IDC_SIZENS));
            ListView_HitTest(hlv, &ht);
            if (ht.iItem >= 0) {
                ListView_SetItemState(hlv, ht.iItem, LVIS_DROPHILITED, LVIS_DROPHILITED);
                target = ht.iItem;
            } else if (GetClientRect(hlv, &rc), ht.pt.y > rc.bottom - 4) {
                target = ListView_GetItemCount(hlv) - 1;
            }
        } else if (msg.message == WM_LBUTTONUP) {
            break;
        } else if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            target = -1;
            break;
        } else {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    ReleaseCapture();
    {
        int i, n = ListView_GetItemCount(hlv);
        for (i = 0; i < n; ++i) ListView_SetItemState(hlv, i, 0, LVIS_DROPHILITED);
    }
    if (target == from_index) return -1;
    return target;
}
