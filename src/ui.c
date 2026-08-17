/*
 * ui.c —— 主窗口、模板树、动态生成区、实时预览、复制、主题
 */
#include "ui.h"
#include "resource.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commdlg.h>
#include <uxtheme.h>

#ifndef TVS_EX_DOUBLEBUFFER
#  define TVS_EX_DOUBLEBUFFER 0x0004
#endif
#ifndef EM_SETCUEBANNER
#  define EM_SETCUEBANNER 0x1501
#endif
#ifndef LVS_EX_DOUBLEBUFFER
#  define LVS_EX_DOUBLEBUFFER 0x00010000
#endif

/* ================================================================== */
/* 模块内全局                                                          */
/* ================================================================== */

static HWND g_hTree, g_hSearch, g_hTitle, g_hHost, g_hPreview;
static HWND g_hBtnCopy, g_hBtnReset, g_hBtnEdit, g_hHint, g_hPreviewLabel;

static Template   *g_curTmpl;      /* 当前选中的模板（指向 g_app 中的对象） */
static EvalContext *g_ctx;         /* 当前模板的求值上下文                  */

/* 动态字段控件 */
typedef struct {
    int   field_index;             /* 在 g_ctx->fields 中的下标 */
    HWND  hLabel;
    HWND  hCtl;                    /* Edit / ComboBox / 只读 Edit */
    HWND  hBtn;                    /* multi-select 的下拉按钮     */
    int   y;                       /* 逻辑 y（未减去滚动量）      */
    int   extraIndex;              /* select：为规则值临时追加的项，-1 = 无 */
} FieldCtl;

static FieldCtl *g_fields;
static int       g_fieldCount;
static int       g_hostScroll;     /* 字段区滚动偏移 */
static int       g_hostContentH;

/* 多选下拉弹层（自绘 + 鼠标捕获，不改变焦点与激活，详见下方章节注释） */
static HWND  g_popup;
static int   g_popupField = -1;
static HWND  g_popupOwnerCtl;          /* 锚点控件（只读 Edit）        */
static const Segment *g_popupSeg;
static unsigned char *g_popupChecked;  /* g_popupCount 字节，1 = 已勾选 */
static int   g_popupCount;
static int   g_popupHot = -1;          /* 鼠标/键盘所在行，-1 = 无      */
static int   g_popupTop;               /* 首个可见行                    */
static int   g_popupRows;              /* 可见行数                      */
static int   g_popupItemH;             /* 行高                          */
static int   g_popupSbDrag;            /* 正在拖动自绘滚动条            */
static int   g_popupSbGrabDy;          /* 滑块内抓取偏移                */

/* 拖拽（模板树 / 列表） */
static HTREEITEM g_dragItem;
static int        g_dragging;

/* 分隔条拖动 */
static int g_splitDrag;
static int g_splitGrabDx;          /* 按下点相对分隔条左沿的偏移 */
static int g_splitLastX = -1;      /* 上次绘制的分隔条位置       */

typedef struct { int kind; wchar_t id[QP_ID_MAX]; } NodeRef;  /* kind: 0=分组 1=模板 */

static const wchar_t *CLS_MAIN  = L"QuickPhraseMain";
static const wchar_t *CLS_HOST  = L"QuickPhraseFieldHost";
static const wchar_t *CLS_POPUP = L"QuickPhraseMultiPopup";

static void ui_layout(HWND hwnd);
static void ui_build_fields(void);
static void ui_select_template(Template *t);
static void popup_close(void);

/* ================================================================== */
/* 主题                                                                */
/* ================================================================== */

COLORREF theme_bk(void)      { return g_dark ? CLR_DARK_BK : CLR_LIGHT_BK; }
COLORREF theme_ctl_bk(void)  { return g_dark ? CLR_DARK_CTL : CLR_LIGHT_CTL; }
COLORREF theme_text(void)    { return g_dark ? CLR_DARK_TEXT : CLR_LIGHT_TEXT; }
COLORREF theme_dim(void)     { return g_dark ? CLR_DARK_DIM : CLR_LIGHT_DIM; }
COLORREF theme_accent(void)  { return g_dark ? CLR_DARK_ACCENT : CLR_LIGHT_ACCENT; }

/* Win10+ 深色控件：uxtheme 的未文档化导出，按序号加载，不可用时静默降级 */
typedef enum { APPMODE_DEFAULT = 0, APPMODE_ALLOWDARK = 1, APPMODE_FORCEDARK = 2,
               APPMODE_FORCELIGHT = 3 } QpAppMode;
typedef int  (WINAPI *FnSetPreferredAppMode)(int);
typedef BOOL (WINAPI *FnAllowDarkModeForApp)(BOOL);
typedef BOOL (WINAPI *FnAllowDarkModeForWindow)(HWND, BOOL);
typedef void (WINAPI *FnFlushMenuThemes)(void);
typedef HRESULT (WINAPI *FnDwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
typedef void (WINAPI *FnRtlGetNtVersionNumbers)(DWORD *, DWORD *, DWORD *);

static FnAllowDarkModeForWindow s_allowDarkWindow;
static FnFlushMenuThemes        s_flushMenuThemes;
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

static void dark_apply_window(HWND hwnd)
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

HWND qp_create(const wchar_t *cls, const wchar_t *text, DWORD style, DWORD ex,
               int x, int y, int w, int h, HWND parent, int id)
{
    HWND hwnd = CreateWindowExW(ex, cls, text, style | WS_CHILD | WS_VISIBLE,
                                x, y, w, h, parent, (HMENU)(INT_PTR)id, g_hInst, NULL);
    if (hwnd) {
        SendMessageW(hwnd, WM_SETFONT, (WPARAM)g_fontUI, TRUE);
        theme_control(hwnd);
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
    wchar_t cur[QP_VALUE_MAX];
    if (!h) return;
    if (!text) text = L"";
    GetWindowTextW(h, cur, QP_VALUE_MAX);
    if (wcscmp(cur, text) != 0) SetWindowTextW(h, text);
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

void qp_center_window(HWND hwnd, HWND owner)
{
    RECT rc, ro;
    int x, y, w, h;
    if (!hwnd) return;
    GetWindowRect(hwnd, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    if (owner && IsWindowVisible(owner)) {
        GetWindowRect(owner, &ro);
    } else {
        ro.left = 0; ro.top = 0;
        ro.right = GetSystemMetrics(SM_CXSCREEN);
        ro.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    x = ro.left + ((ro.right - ro.left) - w) / 2;
    y = ro.top + ((ro.bottom - ro.top) - h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
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

    if (!combo || (code != CBN_DROPDOWN && code != CBN_CLOSEUP)) return 0;
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

/* ================================================================== */
/* 多选下拉弹层                                                        */
/*                                                                     */
/* 这里同时是「连续点击多选控件后主窗口被压到最底层」和「重开后只剩第一 */
/* 项被勾选」两个问题的根因所在，改动思路记录如下。                    */
/*                                                                     */
/* 一、Z 序问题。旧实现是「WS_EX_NOACTIVATE 弹层 + 内嵌 ListView +     */
/*    SetFocus(list)」：SetFocus 会把弹层提升为激活窗口，与             */
/*    WS_EX_NOACTIVATE「不要激活我」的语义直接矛盾；紧接着又在弹层的    */
/*    WM_ACTIVATE(WA_INACTIVE) 和 ListView 的 WM_KILLFOCUS 里          */
/*    DestroyWindow —— 等于在系统的激活切换进行到一半时把切换目标销毁， */
/*    再加上切换途中插一脚 SetWindowPos(主窗口, HWND_TOP)。连续点击会不 */
/*    断重入这个过程，系统对 owner/owned 窗口的 Z 序记账被打乱，主窗口  */
/*    最终被排到所有窗口之后。单选走系统 ComboBox 下拉，全程只用鼠标捕  */
/*    获、从不改变焦点与激活，所以「影响范围」这类控件不会复现。        */
/*    新实现与系统下拉保持一致：SetCapture 接管全部鼠标输入，焦点与激活 */
/*    始终留在主窗口，全程零激活切换，竞态从根上消失。                 */
/*                                                                     */
/* 二、勾选丢失。旧实现把勾选状态存在 ListView 里，而                  */
/*    LVS_EX_FULLROWSELECT 下点击行文字只会「高亮」不会「勾选」（复选框 */
/*    只在最左侧十几像素内响应），LVN_ITEMCHANGED 报的是 LVIS_SELECTED  */
/*    变化、0x2000 位前后都是 0，于是 popup_apply 根本没被调用；再叠加  */
/*    LVS_SINGLESEL 让高亮跟着走，看起来就是「只有一个被选中」。       */
/*    新实现用 g_popupChecked 数组作为唯一事实来源，整行任意位置点击都  */
/*    切换勾选，且不再依赖「拼字符串 → 存引擎 → 重开时反解析」的往返。 */
/* ================================================================== */

/* 多选拼接分隔符：片段自带 > 全局设置 > 顿号 */
static const wchar_t *multi_sep(const Segment *seg)
{
    if (seg && seg->separator[0]) return seg->separator;
    if (g_settings.default_separator[0]) return g_settings.default_separator;
    return L"、";
}

/* 把多选值拆开，判断某个选项是否被选中 */
static int multi_has(const wchar_t *value, const wchar_t *sep, const wchar_t *opt)
{
    wchar_t part[QP_VALUE_MAX];
    int idx = 1;
    if (!value || !*value || !opt) return 0;
    while (qp_split_take(value, (sep && *sep) ? sep : L"、", idx, part, QP_VALUE_MAX)) {
        qp_trim(part);
        if (qp_icmp(part, opt) == 0) return 1;
        ++idx;
        if (idx > 128) break;
    }
    return 0;
}

/* --------------------------- 几何 --------------------------------- */

static int popup_pad(void)  { return DPIX(3); }
/* 需要滚动时才占位；系统滚动条属非客户区，捕获期间收不到消息，只能自绘 */
static int popup_sb_w(void) { return (g_popupCount > g_popupRows) ? DPIX(9) : 0; }

static void popup_clamp_scroll(void)
{
    int maxTop = g_popupCount - g_popupRows;
    if (maxTop < 0) maxTop = 0;
    if (g_popupTop > maxTop) g_popupTop = maxTop;
    if (g_popupTop < 0) g_popupTop = 0;
}

/* 客户坐标 → 行号；落在空白/滚动条/边界外返回 -1 */
static int popup_row_at_pt(int x, int y)
{
    RECT rc;
    int row;
    if (!g_popup || g_popupItemH <= 0) return -1;
    GetClientRect(g_popup, &rc);
    if (x < 0 || x >= rc.right - popup_sb_w()) return -1;
    if (y < popup_pad()) return -1;
    row = g_popupTop + (y - popup_pad()) / g_popupItemH;
    if (row < g_popupTop || row >= g_popupTop + g_popupRows) return -1;
    if (row < 0 || row >= g_popupCount) return -1;
    return row;
}

static void popup_invalidate_row(int row)
{
    RECT rc, r;
    if (!g_popup || row < g_popupTop || row >= g_popupTop + g_popupRows) return;
    if (row < 0 || row >= g_popupCount) return;
    GetClientRect(g_popup, &rc);
    r.left = 0;
    r.right = rc.right - popup_sb_w();
    r.top = popup_pad() + (row - g_popupTop) * g_popupItemH;
    r.bottom = r.top + g_popupItemH;
    InvalidateRect(g_popup, &r, FALSE);
}

/* 自绘滚动条：轨道 + 滑块位置 */
static void popup_sb_geom(int *trackTop, int *trackH, int *thumbTop, int *thumbH)
{
    RECT rc;
    int maxTop = g_popupCount - g_popupRows;
    int tt, th, ht;
    GetClientRect(g_popup, &rc);
    tt = popup_pad();
    th = (rc.bottom - rc.top) - popup_pad() * 2;
    if (th < 1) th = 1;
    if (maxTop < 1) maxTop = 1;
    ht = MulDiv(th, g_popupRows, g_popupCount > 0 ? g_popupCount : 1);
    if (ht < DPIX(18)) ht = DPIX(18);
    if (ht > th) ht = th;
    *trackTop = tt;
    *trackH = th;
    *thumbH = ht;
    *thumbTop = tt + MulDiv(th - ht, g_popupTop, maxTop);
}

static void popup_sb_track(int y)
{
    int tt, th, ht, hy, maxTop, newTop;
    popup_sb_geom(&tt, &th, &hy, &ht);
    maxTop = g_popupCount - g_popupRows;
    if (maxTop < 1 || th - ht <= 0) return;
    newTop = MulDiv(y - g_popupSbGrabDy - tt, maxTop, th - ht);
    if (newTop < 0) newTop = 0;
    if (newTop > maxTop) newTop = maxTop;
    if (newTop != g_popupTop) {
        g_popupTop = newTop;
        InvalidateRect(g_popup, NULL, FALSE);
    }
}

static void popup_sb_begin(int y)
{
    int tt, th, ht, hy;
    popup_sb_geom(&tt, &th, &hy, &ht);
    g_popupSbDrag = 1;
    if (y < hy || y >= hy + ht) {
        g_popupSbGrabDy = ht / 2;      /* 点在滑块外：滑块中心跳到光标处 */
        popup_sb_track(y);
    } else {
        g_popupSbGrabDy = y - hy;      /* 记录抓取偏移，避免滑块跳位 */
    }
}

/* --------------------------- 状态 --------------------------------- */

/* 由 g_popupChecked 现场拼出字段值 —— 不再从控件里读回 */
static void popup_build_value(wchar_t *out, size_t cap)
{
    const wchar_t *sep = multi_sep(g_popupSeg);
    int i;
    out[0] = L'\0';
    if (!g_popupSeg) return;
    for (i = 0; i < g_popupCount; ++i) {
        size_t used;
        if (!g_popupChecked[i]) continue;
        used = wcslen(out);
        if (used + 1 >= cap) break;
        if (out[0]) {
            wcsncat(out, sep, cap - used - 1);
            used = wcslen(out);
            if (used + 1 >= cap) break;
        }
        wcsncat(out, g_popupSeg->options[i], cap - used - 1);
    }
}

static void popup_apply(void)
{
    wchar_t value[QP_VALUE_MAX];
    if (!g_popup || !g_ctx || !g_popupSeg || !g_popupChecked) return;
    if (g_popupField < 0 || g_popupField >= g_ctx->count) return;
    popup_build_value(value, QP_VALUE_MAX);
    eval_set_user_value(g_ctx, g_popupField, value);
    ui_refresh_current();
}

static void popup_close(void)
{
    HWND p = g_popup;
    if (!p) return;
    /* 先清空全局，再释放捕获/销毁：ReleaseCapture 会同步回送
       WM_CAPTURECHANGED、DestroyWindow 会同步回送 WM_DESTROY，
       两者都会再调到这里，靠 g_popup == NULL 挡住重入。 */
    g_popup = NULL;
    g_popupField = -1;
    g_popupSeg = NULL;
    g_popupOwnerCtl = NULL;
    g_popupHot = -1;
    g_popupTop = 0;
    g_popupSbDrag = 0;
    if (GetCapture() == p) ReleaseCapture();
    DestroyWindow(p);
    free(g_popupChecked);
    g_popupChecked = NULL;
    g_popupCount = 0;
}

static void popup_ensure_visible(int row)
{
    int old = g_popupTop;
    if (!g_popup || row < 0) return;
    if (row < g_popupTop) g_popupTop = row;
    else if (row >= g_popupTop + g_popupRows) g_popupTop = row - g_popupRows + 1;
    popup_clamp_scroll();
    if (g_popupTop != old) InvalidateRect(g_popup, NULL, FALSE);
}

static void popup_set_hot(int row)
{
    int old = g_popupHot;
    if (row == old) return;
    g_popupHot = row;
    popup_invalidate_row(old);
    popup_invalidate_row(row);
}

static void popup_move_hot(int delta)
{
    int old = g_popupHot, row = g_popupHot;
    if (!g_popup || g_popupCount <= 0) return;
    if (row < 0) row = (delta > 0) ? g_popupTop - 1 : g_popupTop + g_popupRows;
    row += delta;
    if (row < 0) row = 0;
    if (row >= g_popupCount) row = g_popupCount - 1;
    if (row == old) return;
    g_popupHot = row;
    popup_ensure_visible(row);
    popup_invalidate_row(old);
    popup_invalidate_row(row);
}

/* 切换一行并立即写回引擎：每次点击都落库，关闭时不需要再兜底收集 */
static void popup_toggle(int row)
{
    if (!g_popup || !g_popupChecked || row < 0 || row >= g_popupCount) return;
    g_popupChecked[row] = (unsigned char)(g_popupChecked[row] ? 0 : 1);
    popup_invalidate_row(row);
    popup_apply();
}

/* 细分隔线颜色（分隔条、弹层滚动条） */
static COLORREF theme_line(void)
{
    return g_dark ? RGB(62, 66, 74) : RGB(205, 210, 218);
}

/* --------------------------- 绘制 --------------------------------- */

static void popup_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc, mem;
    HBITMAP bmp, oldBmp;
    HFONT oldFont;
    HPEN boxPen, checkPen, oldPen;
    HBRUSH bkBrush, hotBrush, accBrush;
    RECT rc;
    int i, cw, ch, bs, boxX, textX, sbw;

    hdc = BeginPaint(hwnd, &ps);
    if (!hdc) return;
    GetClientRect(hwnd, &rc);
    if (g_popup != hwnd || !g_popupSeg || !g_popupChecked) {
        /* 理论上不会走到（弹层存在期间这些状态必然有效），兜底避免空指针 */
        FillRect(hdc, &rc, g_brushCtl ? g_brushCtl : g_brushBk);
        EndPaint(hwnd, &ps);
        return;
    }
    cw = rc.right - rc.left;
    ch = rc.bottom - rc.top;
    sbw = popup_sb_w();

    /* 整幅双缓冲：勾选/悬停切换时不闪 */
    mem = CreateCompatibleDC(hdc);
    bmp = mem ? CreateCompatibleBitmap(hdc, cw, ch) : NULL;
    if (!mem || !bmp) {
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return;
    }
    oldBmp = (HBITMAP)SelectObject(mem, bmp);
    oldFont = (HFONT)SelectObject(mem, g_fontUI);
    SetBkMode(mem, TRANSPARENT);

    bkBrush  = CreateSolidBrush(theme_ctl_bk());
    hotBrush = CreateSolidBrush(g_dark ? RGB(58, 62, 70) : RGB(226, 236, 248));
    accBrush = CreateSolidBrush(theme_accent());
    boxPen   = CreatePen(PS_SOLID, 1, theme_dim());
    checkPen = CreatePen(PS_SOLID, DPIX(2) > 1 ? DPIX(2) : 1, RGB(255, 255, 255));

    if (bkBrush) FillRect(mem, &rc, bkBrush);

    /* 自绘复选框：DrawFrameControl 在深色主题下会画出系统浅色外观 */
    bs    = DPIX(14);
    boxX  = DPIX(7);
    textX = boxX + bs + DPIX(8);

    for (i = g_popupTop; i < g_popupTop + g_popupRows && i < g_popupCount; ++i) {
        RECT row, box, txt;
        row.left   = 0;
        row.right  = cw - sbw;
        row.top    = popup_pad() + (i - g_popupTop) * g_popupItemH;
        row.bottom = row.top + g_popupItemH;
        if (i == g_popupHot && hotBrush) FillRect(mem, &row, hotBrush);

        box.left   = boxX;
        box.top    = row.top + (g_popupItemH - bs) / 2;
        box.right  = box.left + bs;
        box.bottom = box.top + bs;
        if (g_popupChecked[i]) {
            if (accBrush) FillRect(mem, &box, accBrush);
        } else if (g_brushCtl) {
            FillRect(mem, &box, g_brushCtl);
        }
        oldPen = (HPEN)SelectObject(mem, boxPen);
        SelectObject(mem, GetStockObject(NULL_BRUSH));
        Rectangle(mem, box.left, box.top, box.right, box.bottom);
        if (g_popupChecked[i]) {
            POINT pts[3];
            pts[0].x = box.left + MulDiv(bs, 3, 14);
            pts[0].y = box.top  + MulDiv(bs, 7, 14);
            pts[1].x = box.left + MulDiv(bs, 6, 14);
            pts[1].y = box.top  + MulDiv(bs, 10, 14);
            pts[2].x = box.left + MulDiv(bs, 11, 14);
            pts[2].y = box.top  + MulDiv(bs, 4, 14);
            SelectObject(mem, checkPen);
            Polyline(mem, pts, 3);
        }
        SelectObject(mem, oldPen);

        txt = row;
        txt.left   = textX;
        txt.right -= DPIX(4);
        SetTextColor(mem, theme_text());
        DrawTextW(mem, g_popupSeg->options[i], -1, &txt,
                  DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    if (sbw) {
        RECT thumb;
        int tt, th, hy, ht;
        HBRUSH tb = CreateSolidBrush(theme_line());
        popup_sb_geom(&tt, &th, &hy, &ht);
        thumb.left   = cw - sbw + DPIX(2);
        thumb.right  = cw - DPIX(2);
        if (thumb.right <= thumb.left) thumb.right = thumb.left + 1;
        thumb.top    = hy;
        thumb.bottom = hy + ht;
        if (tb) { FillRect(mem, &thumb, tb); DeleteObject(tb); }
    }

    BitBlt(hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldFont);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    if (bkBrush)  DeleteObject(bkBrush);
    if (hotBrush) DeleteObject(hotBrush);
    if (accBrush) DeleteObject(accBrush);
    if (boxPen)   DeleteObject(boxPen);
    if (checkPen) DeleteObject(checkPen);
    EndPaint(hwnd, &ps);
}

/* --------------------------- 窗口过程 ------------------------------ */
/* 全程持有鼠标捕获，因此所有鼠标消息都会送到这里，且 lParam 已换算成本
   窗口客户坐标；落在客户区外就是「点了别处」，与系统下拉行为一致。 */
static LRESULT CALLBACK popup_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        popup_paint(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;                       /* 背景在 WM_PAINT 里一次画完 */

    case WM_MOUSEMOVE: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        if (g_popupSbDrag) { popup_sb_track(y); return 0; }
        popup_set_hot(popup_row_at_pt(x, y));
        return 0;
    }

    case WM_LBUTTONDOWN: {
        RECT rc;
        POINT pt;
        int sbw;
        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        GetClientRect(hwnd, &rc);
        if (!PtInRect(&rc, pt)) {
            /* 点到弹层外：收起并「吞掉」这次点击，避免顺手点到别的控件 */
            popup_close();
            return 0;
        }
        sbw = popup_sb_w();
        if (sbw && pt.x >= rc.right - sbw) { popup_sb_begin(pt.y); return 0; }
        popup_toggle(popup_row_at_pt(pt.x, pt.y));   /* 整行任意位置均可勾选 */
        return 0;
    }

    case WM_LBUTTONUP:
        /* 打开弹层的那一次按下，其抬起会落到这里；一律忽略，保持弹层敞开
           以便连续勾选多项。 */
        g_popupSbDrag = 0;
        return 0;

    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        popup_close();
        return 0;

    case WM_MOUSEWHEEL: {
        int old = g_popupTop;
        g_popupTop -= (GET_WHEEL_DELTA_WPARAM(wp) > 0) ? 3 : -3;
        popup_clamp_scroll();
        if (g_popupTop != old) InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_CAPTURECHANGED:
        /* 捕获被别人拿走（Alt+Tab、系统菜单、其他窗口）→ 收起 */
        if ((HWND)lp != hwnd) popup_close();
        return 0;

    case WM_CANCELMODE:
        popup_close();
        return 0;

    case WM_DESTROY:
        if (g_popup == hwnd) { g_popup = NULL; g_popupField = -1; g_popupSeg = NULL; }
        break;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* --------------------------- 打开 --------------------------------- */

static void popup_open(int field_index, HWND anchor)
{
    const Segment *seg;
    const wchar_t *sep;
    RECT rc, wa;
    MONITORINFO mi;
    HMONITOR mon;
    int i, w, h, x, y, textH, wantRows, capRows, below, above, flip = 0;

    if (!g_ctx || !anchor || field_index < 0 || field_index >= g_ctx->count) return;
    /* 已打开 → 再次点击即收起（开关手感）。旧实现靠 250ms 时间窗防抖，
       在捕获模型下不再需要：锚点上的这次点击本就被弹层吞掉了。 */
    if (g_popup) { popup_close(); return; }

    seg = g_ctx->fields[field_index].seg;
    if (!seg || seg->option_count <= 0) {
        qp_msgbox(g_hwndMain, MB_OK | MB_ICONINFORMATION, QP_APP_TITLE,
                  L"变量「%ls」还没有配置选项。\n\n可在「编辑模板」中为它添加选项。",
                  seg_display_name(seg));
        return;
    }

    /* 行高按实际字体算，避免高 DPI 下文字被裁 */
    textH = DPIX(15);
    {
        HDC hdc = GetDC(anchor);
        if (hdc) {
            HFONT old = (HFONT)SelectObject(hdc, g_fontUI);
            TEXTMETRICW tm;
            if (GetTextMetricsW(hdc, &tm)) textH = tm.tmHeight;
            SelectObject(hdc, old);
            ReleaseDC(anchor, hdc);
        }
    }
    g_popupItemH = textH + DPIX(10);
    if (g_popupItemH < DPIX(22)) g_popupItemH = DPIX(22);

    g_popupCount = seg->option_count;
    g_popupChecked = (unsigned char *)calloc((size_t)g_popupCount, 1);
    if (!g_popupChecked) { g_popupCount = 0; return; }
    sep = multi_sep(seg);
    for (i = 0; i < g_popupCount; ++i)
        g_popupChecked[i] = (unsigned char)
            (multi_has(g_ctx->fields[field_index].value, sep, seg->options[i]) ? 1 : 0);

    GetWindowRect(anchor, &rc);
    w = rc.right - rc.left;
    if (w < DPIX(180)) w = DPIX(180);

    mon = MonitorFromWindow(anchor, MONITOR_DEFAULTTONEAREST);
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) {
        wa = mi.rcWork;
    } else {
        wa.left = 0; wa.top = 0;
        wa.right = GetSystemMetrics(SM_CXSCREEN);
        wa.bottom = GetSystemMetrics(SM_CYSCREEN);
    }

    /* 优先展开在下方，下方不够且上方更宽裕则翻到上方（同系统下拉） */
    capRows = (DPIX(320) - popup_pad() * 2) / g_popupItemH;
    below   = (wa.bottom - rc.bottom - DPIX(6) - popup_pad() * 2) / g_popupItemH;
    above   = (rc.top - wa.top - DPIX(6) - popup_pad() * 2) / g_popupItemH;
    if (capRows < 1) capRows = 1;
    if (below < 1) below = 1;
    if (above < 1) above = 1;
    wantRows = g_popupCount;
    if (wantRows > capRows) wantRows = capRows;
    if (wantRows <= below) {
        g_popupRows = wantRows;
    } else if (above > below) {
        g_popupRows = (wantRows <= above) ? wantRows : above;
        flip = 1;
    } else {
        g_popupRows = below;
    }
    if (g_popupRows < 1) g_popupRows = 1;
    h = g_popupRows * g_popupItemH + popup_pad() * 2;

    x = rc.left;
    if (x + w > wa.right) x = wa.right - w;
    if (x < wa.left) x = wa.left;
    y = flip ? (rc.top - h) : rc.bottom;

    g_popup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, CLS_POPUP, L"",
                              WS_POPUP | WS_BORDER, x, y, w, h,
                              g_hwndMain, NULL, g_hInst, NULL);
    if (!g_popup) {
        free(g_popupChecked);
        g_popupChecked = NULL;
        g_popupCount = 0;
        return;
    }
    g_popupField = field_index;
    g_popupSeg = seg;
    g_popupOwnerCtl = anchor;
    g_popupHot = -1;
    g_popupTop = 0;
    for (i = 0; i < g_popupCount; ++i)      /* 首个已勾选项滚进可见区 */
        if (g_popupChecked[i]) { g_popupTop = i; break; }
    popup_clamp_scroll();

    dark_apply_window(g_popup);
    /* 焦点放回锚点 Edit：从 ▼ 按钮打开时焦点在按钮上，键盘操作就转发不进来。
       这是同一顶层窗口内子控件之间的焦点转移，不涉及窗口激活，安全。 */
    if (GetFocus() != anchor) SetFocus(anchor);
    ShowWindow(g_popup, SW_SHOWNA);
    /* 关键：只接管鼠标，绝不调用 SetFocus / SetActiveWindow /
       SetForegroundWindow（对弹层本身）。焦点留在 anchor 上（键盘由
       multi_edit_proc 转发进来），主窗口始终是激活窗口，因此不存在任何激活
       切换，也就不会再出现「销毁正在参与激活切换的窗口」导致的 Z 序错乱。 */
    SetCapture(g_popup);
}

/* ================================================================== */
/* 字段区（可滚动容器）                                                */
/* ================================================================== */

/* 批量移动控件：同一批位置变化合并成一次 EndDeferWindowPos 提交，避免逐个
   MoveWindow 造成的「一格一格挪」和闪烁。位置没变的控件直接跳过。 */
static HDWP qp_defer(HDWP dwp, HWND h, int x, int y, int w, int ht)
{
    RECT rc;
    HWND parent;
    if (!dwp || !h) return dwp;
    if (w < 0) w = 0;
    if (ht < 0) ht = 0;
    parent = GetParent(h);
    GetWindowRect(h, &rc);
    if (parent) MapWindowPoints(NULL, parent, (POINT *)&rc, 2);
    if (rc.left == x && rc.top == y &&
        rc.right - rc.left == w && rc.bottom - rc.top == ht)
        return dwp;
    return DeferWindowPos(dwp, h, NULL, x, y, w, ht,
                          SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
}

/* 内容变短 / 可视区变高时把滚动位置收回来，否则底部会露出空白 */
static void host_clamp_scroll(void)
{
    RECT rc;
    int maxPos;
    if (!g_hHost) return;
    GetClientRect(g_hHost, &rc);
    maxPos = g_hostContentH - (rc.bottom - rc.top);
    if (maxPos < 0) maxPos = 0;
    if (g_hostScroll > maxPos) g_hostScroll = maxPos;
    if (g_hostScroll < 0) g_hostScroll = 0;
}

static void host_update_scroll(void)
{
    SCROLLINFO si, cur;
    RECT rc;
    if (!g_hHost) return;
    GetClientRect(g_hHost, &rc);
    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = g_hostContentH > 0 ? g_hostContentH - 1 : 0;
    si.nPage = (UINT)((rc.bottom - rc.top) > 0 ? (rc.bottom - rc.top) : 0);
    si.nPos = g_hostScroll;
    /* 数值没变就别调 SetScrollInfo：它可能改变客户区宽度（滚动条显隐），
       在 WM_SIZE 期间会引起「布局 → 滚动条显隐 → 再布局」的抖动。 */
    memset(&cur, 0, sizeof(cur));
    cur.cbSize = sizeof(cur);
    cur.fMask = SIF_ALL;
    if (GetScrollInfo(g_hHost, SB_VERT, &cur) &&
        cur.nMin == si.nMin && cur.nMax == si.nMax &&
        cur.nPage == si.nPage && cur.nPos == si.nPos)
        return;
    SetScrollInfo(g_hHost, SB_VERT, &si, TRUE);
}

static void host_reposition(void)
{
    RECT rc;
    HDWP dwp;
    int i, labelW = DPIX(96), pad = DPIX(12), ctlH = DPIX(24);
    int btnW = DPIX(26);
    if (!g_hHost || g_fieldCount <= 0 || !g_fields) return;
    GetClientRect(g_hHost, &rc);
    dwp = BeginDeferWindowPos(g_fieldCount * 3);
    for (i = 0; i < g_fieldCount; ++i) {
        FieldCtl *f = &g_fields[i];
        int y = f->y - g_hostScroll;
        int cx = pad + labelW + DPIX(8);
        int cw = (rc.right - rc.left) - cx - pad;
        if (cw < DPIX(80)) cw = DPIX(80);
        dwp = qp_defer(dwp, f->hLabel, pad, y + DPIX(4), labelW, DPIX(18));
        if (f->hBtn) {
            dwp = qp_defer(dwp, f->hCtl, cx, y, cw - btnW - DPIX(2), ctlH);
            dwp = qp_defer(dwp, f->hBtn, cx + cw - btnW, y, btnW, ctlH);
        } else {
            dwp = qp_defer(dwp, f->hCtl, cx, y, cw, ctlH);
        }
    }
    if (dwp) EndDeferWindowPos(dwp);
}

static LRESULT CALLBACK host_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND: {
        RECT rc;
        HDC hdc = (HDC)wp;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, g_brushBk);
        return 1;
    }
    case WM_VSCROLL: {
        SCROLLINFO si;
        int old = g_hostScroll, pos;
        popup_close();          /* 内容要滚动了，弹层锚点会失位 */
        memset(&si, 0, sizeof(si));
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(hwnd, SB_VERT, &si);
        pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_TOP:           pos = si.nMin; break;
        case SB_BOTTOM:        pos = si.nMax; break;
        case SB_LINEUP:        pos -= DPIX(20); break;
        case SB_LINEDOWN:      pos += DPIX(20); break;
        case SB_PAGEUP:        pos -= (int)si.nPage; break;
        case SB_PAGEDOWN:      pos += (int)si.nPage; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        default: break;
        }
        if (pos > si.nMax - (int)si.nPage + 1) pos = si.nMax - (int)si.nPage + 1;
        if (pos < 0) pos = 0;
        if (pos != old) {
            g_hostScroll = pos;
            si.fMask = SIF_POS;
            si.nPos = pos;
            SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
            /* 只做一次批量移动即可：字段区带 WS_CLIPCHILDREN，控件挪走后
               露出的背景由 SetWindowPos 自动失效，不需要整片重刷（整片重刷
               在滚动时会明显闪一下）。 */
            host_reposition();
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int lines = delta > 0 ? -3 : 3;
        int i;
        for (i = 0; i < (lines < 0 ? -lines : lines); ++i)
            SendMessageW(hwnd, WM_VSCROLL,
                         MAKEWPARAM(lines < 0 ? SB_LINEUP : SB_LINEDOWN, 0), 0);
        return 0;
    }
    case WM_COMMAND:
    case WM_NOTIFY:
        return SendMessageW(GetParent(hwnd), msg, wp, lp);
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN: {
        int handled = 0;
        LRESULT r = qp_handle_ctlcolor(msg, wp, lp, &handled);
        if (handled) return r;
        break;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* 只读 Edit（多选显示框）子类：点击即弹出选项。
   弹层不取焦点，焦点始终留在这个 Edit 上，因此弹层打开期间的键盘操作也在
   这里处理并转交给弹层。 */
static LRESULT CALLBACK multi_edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                        UINT_PTR id, DWORD_PTR ref)
{
    int open = (g_popup != NULL && g_popupOwnerCtl == hwnd);
    (void)ref;

    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
        popup_open((int)id, hwnd);
        return 0;
    }
    if (msg == WM_GETDLGCODE) {
        /* 弹层打开时把方向键/空格/回车/Esc 全要过来，否则 main.c 的
           IsDialogMessageW 会先把它们当成对话框导航键消费掉。 */
        if (open) return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS;
        return DLGC_WANTARROWS | DefSubclassProc(hwnd, msg, wp, lp);
    }
    if (msg == WM_KEYDOWN) {
        if (open) {
            switch (wp) {
            case VK_ESCAPE:
            case VK_RETURN:
            case VK_TAB:
            case VK_F4:    popup_close();          return 0;
            case VK_UP:    popup_move_hot(-1);     return 0;
            case VK_DOWN:  popup_move_hot(+1);     return 0;
            case VK_PRIOR: popup_move_hot(-g_popupRows); return 0;
            case VK_NEXT:  popup_move_hot(+g_popupRows); return 0;
            case VK_HOME:  popup_move_hot(-g_popupCount); return 0;
            case VK_END:   popup_move_hot(+g_popupCount); return 0;
            case VK_SPACE:
                popup_toggle(g_popupHot >= 0 ? g_popupHot : g_popupTop);
                return 0;
            default: break;
            }
        } else if (wp == VK_DOWN || wp == VK_F4 || wp == VK_SPACE) {
            popup_open((int)id, hwnd);
            return 0;
        }
    }
    if (msg == WM_CHAR && open) return 0;   /* 空格已被吃掉，别再发出提示音 */
    if (msg == WM_SETCURSOR) {
        SetCursor(LoadCursorW(NULL, IDC_ARROW));
        return 1;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

/* ================================================================== */
/* 字段构建与刷新                                                      */
/* ================================================================== */

static void ui_clear_fields(void)
{
    int i;
    popup_close();
    for (i = 0; i < g_fieldCount; ++i) {
        if (g_fields[i].hLabel) DestroyWindow(g_fields[i].hLabel);
        if (g_fields[i].hCtl)   DestroyWindow(g_fields[i].hCtl);
        if (g_fields[i].hBtn)   DestroyWindow(g_fields[i].hBtn);
    }
    free(g_fields);
    g_fields = NULL;
    g_fieldCount = 0;
    g_hostScroll = 0;
    g_hostContentH = 0;
    if (g_hHost) {
        host_update_scroll();
        InvalidateRect(g_hHost, NULL, TRUE);
    }
}

/* 填写界面是否给该片段建控件：固定句不需要；勾了「在填写界面隐藏」的
   变量也不需要（只靠变量匹配/默认值填充，但照常参与输出）。 */
static int seg_needs_input(const Segment *s)
{
    return s && s->type != SEG_STATIC && !s->hidden;
}

static void ui_build_fields(void)
{
    int i, count = 0, y = DPIX(10), rowH = DPIX(30);
    wchar_t label[QP_LABEL_MAX + 8];

    ui_clear_fields();
    if (!g_ctx || !g_hHost) return;

    for (i = 0; i < g_ctx->count; ++i)
        if (seg_needs_input(g_ctx->fields[i].seg)) ++count;
    if (count == 0) {
        g_hostContentH = 0;
        host_update_scroll();
        InvalidateRect(g_hHost, NULL, TRUE);
        return;
    }

    g_fields = (FieldCtl *)calloc((size_t)count, sizeof(FieldCtl));
    if (!g_fields) return;

    /* 控件先在 (0,0) 建出来再统一摆位，期间关掉重绘，避免左上角闪一片控件 */
    SendMessageW(g_hHost, WM_SETREDRAW, FALSE, 0);

    for (i = 0; i < g_ctx->count; ++i) {
        const Segment *seg = g_ctx->fields[i].seg;
        FieldCtl *f;
        int ctlId;
        if (!seg_needs_input(seg)) continue;

        f = &g_fields[g_fieldCount];
        f->field_index = i;
        f->y = y;
        f->extraIndex = -1;
        ctlId = IDC_FIELD_BASE + g_fieldCount * IDC_FIELD_STRIDE;

        _snwprintf(label, QP_LABEL_MAX + 8, L"%ls：", seg_display_name(seg));
        label[QP_LABEL_MAX + 7] = L'\0';
        f->hLabel = qp_create(L"Static", label, SS_RIGHT | SS_ENDELLIPSIS, 0,
                              0, 0, 10, 10, g_hHost, ctlId + 2);

        if (seg->type == SEG_TEXT) {
            f->hCtl = qp_create(L"Edit", g_ctx->fields[i].value,
                                ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE,
                                0, 0, 10, 10, g_hHost, ctlId);
            if (seg->placeholder[0])
                SendMessageW(f->hCtl, EM_SETCUEBANNER, TRUE, (LPARAM)seg->placeholder);
        } else if (seg->type == SEG_SELECT) {
            int k;
            f->hCtl = qp_create(L"ComboBox", L"",
                                CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0,
                                0, 0, 10, DPIX(300), g_hHost, ctlId);
            for (k = 0; k < seg->option_count; ++k)
                SendMessageW(f->hCtl, CB_ADDSTRING, 0, (LPARAM)seg->options[k]);
        } else { /* SEG_MULTI_SELECT */
            f->hCtl = qp_create(L"Edit", g_ctx->fields[i].value,
                                ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP,
                                WS_EX_CLIENTEDGE, 0, 0, 10, 10, g_hHost, ctlId);
            SetWindowSubclass(f->hCtl, multi_edit_proc, (UINT_PTR)i, 0);
            if (seg->placeholder[0])
                SendMessageW(f->hCtl, EM_SETCUEBANNER, TRUE, (LPARAM)seg->placeholder);
            f->hBtn = qp_create(L"Button", L"▼", BS_PUSHBUTTON | WS_TABSTOP, 0,
                                0, 0, 10, 10, g_hHost, ctlId + 1);
        }
        ++g_fieldCount;
        y += rowH;
    }

    g_hostContentH = y + DPIX(4);
    host_clamp_scroll();
    host_reposition();
    host_update_scroll();
    SendMessageW(g_hHost, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(g_hHost, NULL, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

/* 把求值结果同步到控件（跳过正在输入的控件，避免光标跳动） */
static void ui_sync_controls(void)
{
    HWND focus = GetFocus();
    int i;
    if (!g_ctx) return;
    for (i = 0; i < g_fieldCount; ++i) {
        FieldCtl *f = &g_fields[i];
        const EvalField *ef = &g_ctx->fields[f->field_index];
        const Segment *seg = ef->seg;
        if (!seg) continue;
        if (seg->type == SEG_SELECT) {
            /* 控件必须严格跟随引擎里的值：值为空时也要清掉选中态，
               否则界面上还留着上一次匹配到的文字，看着「已填」，
               实际预览里那一段是空的（点一下才恢复正常）。 */
            int idx = ef->value[0]
                        ? (int)SendMessageW(f->hCtl, CB_FINDSTRINGEXACT, (WPARAM)-1,
                                            (LPARAM)ef->value)
                        : CB_ERR;
            if (f->extraIndex >= 0 && idx != f->extraIndex) {
                /* 上次为显示而临时加入的项已经过期 → 撤掉。
                   临时项总是加在末尾，删除它不会影响正式选项的索引。 */
                SendMessageW(f->hCtl, CB_DELETESTRING, (WPARAM)f->extraIndex, 0);
                f->extraIndex = -1;
                if (ef->value[0])
                    idx = (int)SendMessageW(f->hCtl, CB_FINDSTRINGEXACT, (WPARAM)-1,
                                            (LPARAM)ef->value);
            }
            if (idx == CB_ERR && ef->value[0]) {
                /* 规则推导出的值不在选项列表中 → 临时加入以便显示。
                   只保留「一个」临时项：解析规则的 fill_value 为空时推导值会
                   跟着输入逐字变化，旧实现每次刷新都 CB_ADDSTRING，下拉列表
                   会随敲键无限膨胀。 */
                idx = (int)SendMessageW(f->hCtl, CB_ADDSTRING, 0, (LPARAM)ef->value);
                if (idx >= 0) f->extraIndex = idx;
            }
            if (idx < 0) idx = -1;                    /* CB_ERR / 空值 → 不选中 */
            if ((int)SendMessageW(f->hCtl, CB_GETCURSEL, 0, 0) != idx)
                SendMessageW(f->hCtl, CB_SETCURSEL, (WPARAM)idx, 0);
        } else {
            /* 只有正在输入的文本框才跳过刷新（避免光标跳动）。多选的只读框
               现在全程持有焦点（弹层不抢焦点），必须照常刷新，否则勾选了也
               看不到文字变化。 */
            if (!(seg->type == SEG_TEXT && f->hCtl == focus))
                qp_set_text_if_diff(f->hCtl, ef->value);
        }
    }
}

void ui_refresh_current(void)
{
    wchar_t out[QP_OUTPUT_MAX];
    if (!g_ctx) {
        if (g_hPreview) SetWindowTextW(g_hPreview, L"");
        return;
    }
    eval_run(g_ctx);
    ui_sync_controls();
    eval_render(g_ctx, out, QP_OUTPUT_MAX);
    if (g_hPreview) qp_set_text_if_diff(g_hPreview, out);
}

static void ui_select_template(Template *t)
{
    wchar_t title[QP_NAME_MAX + 32];

    ui_clear_fields();
    if (g_ctx) { eval_destroy(g_ctx); g_ctx = NULL; }
    g_curTmpl = t;

    if (!t) {
        SetWindowTextW(g_hTitle, L"未选择模板");
        SetWindowTextW(g_hPreview, L"");
        SetWindowTextW(g_hHint, L"在左侧选择一个模板开始生成口径");
        EnableWindow(g_hBtnCopy, FALSE);
        EnableWindow(g_hBtnReset, FALSE);
        EnableWindow(g_hBtnEdit, FALSE);
        return;
    }

    _snwprintf(title, QP_NAME_MAX + 32, L"%ls", t->name);
    title[QP_NAME_MAX + 31] = L'\0';
    SetWindowTextW(g_hTitle, title);
    EnableWindow(g_hBtnCopy, TRUE);
    EnableWindow(g_hBtnReset, TRUE);
    EnableWindow(g_hBtnEdit, TRUE);

    g_ctx = eval_create(t);
    ui_build_fields();
    if (g_fieldCount == 0)
        SetWindowTextW(g_hHint, L"该模板没有需要填写的变量，可直接复制");
    else
        SetWindowTextW(g_hHint, L"填写后自动套用规则，Ctrl+Enter 可直接复制");
    ui_refresh_current();
}

Template *ui_current_template(void) { return g_curTmpl; }

/* 清空当前模板选择（数据目录切换 / 数据整体替换后调用，避免悬空指针） */
void ui_reset_selection(void)
{
    ui_select_template(NULL);
}

/* ================================================================== */
/* 模板树                                                              */
/* ================================================================== */

static HTREEITEM tree_insert(HTREEITEM parent, const wchar_t *text, int kind,
                             const wchar_t *id, int bold)
{
    TVINSERTSTRUCTW is;
    NodeRef *ref = (NodeRef *)calloc(1, sizeof(NodeRef));
    if (!ref) return NULL;
    ref->kind = kind;
    qp_strlcpy(ref->id, id ? id : L"", QP_ID_MAX);

    memset(&is, 0, sizeof(is));
    is.hParent = parent ? parent : TVI_ROOT;
    is.hInsertAfter = TVI_LAST;
    is.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_STATE;
    is.item.pszText = (LPWSTR)text;
    is.item.lParam = (LPARAM)ref;
    is.item.stateMask = TVIS_BOLD;
    is.item.state = bold ? TVIS_BOLD : 0;
    return (HTREEITEM)SendMessageW(g_hTree, TVM_INSERTITEMW, 0, (LPARAM)&is);
}

static NodeRef *tree_ref(HTREEITEM item)
{
    TVITEMW it;
    if (!item) return NULL;
    memset(&it, 0, sizeof(it));
    it.mask = TVIF_PARAM;
    it.hItem = item;
    if (!SendMessageW(g_hTree, TVM_GETITEMW, 0, (LPARAM)&it)) return NULL;
    return (NodeRef *)it.lParam;
}

static int tmpl_matches(const Template *t, const wchar_t *filter)
{
    const Segment *s;
    if (!filter || !*filter) return 1;
    if (qp_istr(t->name, filter)) return 1;
    /* 也匹配片段内容，便于按口径正文搜索 */
    for (s = t->segments; s; s = s->next) {
        if (s->type == SEG_STATIC && qp_istr(s->content, filter)) return 1;
        if (s->label[0] && qp_istr(s->label, filter)) return 1;
    }
    return 0;
}

void ui_rebuild_tree(const wchar_t *select_template_id)
{
    wchar_t filter[128];
    Group *g;
    Template *t;
    HTREEITEM selItem = NULL;
    int filtering;

    if (!g_hTree) return;
    qp_get_text(g_hSearch, filter, 128);
    qp_trim(filter);
    filtering = filter[0] != L'\0';

    SendMessageW(g_hTree, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(g_hTree);

    for (g = g_app ? g_app->groups : NULL; g; g = g->next) {
        HTREEITEM hg = NULL;
        int any = 0;
        for (t = g_app->templates; t; t = t->next) {
            if (wcscmp(t->group_id, g->id) != 0) continue;
            if (!tmpl_matches(t, filter)) continue;
            if (!hg) hg = tree_insert(NULL, g->name, 0, g->id, 1);
            if (hg) {
                HTREEITEM it = tree_insert(hg, t->name, 1, t->id, 0);
                if (select_template_id && wcscmp(t->id, select_template_id) == 0)
                    selItem = it;
                any = 1;
            }
        }
        /* 空分组在未搜索时也要显示，方便拖入模板 */
        if (!any && !filtering) tree_insert(NULL, g->name, 0, g->id, 1);
    }

    for (t = g_app ? g_app->templates : NULL; t; t = t->next) {
        HTREEITEM it;
        if (t->group_id[0] && group_list_find(g_app->groups, t->group_id)) continue;
        if (!tmpl_matches(t, filter)) continue;
        it = tree_insert(NULL, t->name, 1, t->id, 0);
        if (select_template_id && wcscmp(t->id, select_template_id) == 0) selItem = it;
    }

    SendMessageW(g_hTree, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_hTree, NULL, TRUE);

    {   /* 分组默认全部展开：模板数量不多，展开更省一次点击 */
        HTREEITEM it = TreeView_GetRoot(g_hTree);
        while (it) {
            TreeView_Expand(g_hTree, it, TVE_EXPAND);
            it = TreeView_GetNextSibling(g_hTree, it);
        }
    }

    if (selItem) {
        TreeView_SelectItem(g_hTree, selItem);
        TreeView_EnsureVisible(g_hTree, selItem);
    } else if (!g_curTmpl) {
        /* 自动选中第一个模板 */
        HTREEITEM it = TreeView_GetRoot(g_hTree);
        while (it) {
            NodeRef *r = tree_ref(it);
            if (r && r->kind == 1) { TreeView_SelectItem(g_hTree, it); break; }
            {
                HTREEITEM child = TreeView_GetChild(g_hTree, it);
                if (child) { TreeView_SelectItem(g_hTree, child); break; }
            }
            it = TreeView_GetNextSibling(g_hTree, it);
        }
    }
}

/* 当前树选中的模板 / 分组 */
static Template *tree_selected_template(void)
{
    NodeRef *r = tree_ref(TreeView_GetSelection(g_hTree));
    if (!r || r->kind != 1 || !g_app) return NULL;
    return tmpl_list_find(g_app->templates, r->id);
}

static Group *tree_selected_group(void)
{
    HTREEITEM sel = TreeView_GetSelection(g_hTree);
    NodeRef *r = tree_ref(sel);
    if (!r || !g_app) return NULL;
    if (r->kind == 0) return group_list_find(g_app->groups, r->id);
    {
        Template *t = tmpl_list_find(g_app->templates, r->id);
        if (t && t->group_id[0]) return group_list_find(g_app->groups, t->group_id);
    }
    return NULL;
}

/* ================================================================== */
/* 模板增删改                                                          */
/* ================================================================== */

static void ui_new_template(void)
{
    Template *t = tmpl_create(L"新模板");
    Group *g = tree_selected_group();
    Segment *s;
    if (!t) return;
    if (g) qp_strlcpy(t->group_id, g->id, QP_ID_MAX);
    /* 预置一个固定片段，避免打开编辑器时空空如也 */
    s = seg_create(SEG_STATIC);
    if (s) {
        qp_strlcpy(s->content, L"", QP_CONTENT_MAX);
        seg_list_append(&t->segments, s);
    }
    if (dlg_edit_template(g_hwndMain, t, 1)) {
        tmpl_list_append(&g_app->templates, t);
        app_save_data();
        g_curTmpl = NULL;
        ui_rebuild_tree(t->id);
    } else {
        tmpl_free(t);
    }
}

void ui_edit_current(void)
{
    Template *cur = tree_selected_template();
    Template *work;
    if (!cur) cur = g_curTmpl;
    if (!cur) {
        qp_msgbox(g_hwndMain, MB_OK | MB_ICONINFORMATION, QP_APP_TITLE,
                  L"请先在左侧选择一个模板。");
        return;
    }
    work = tmpl_clone(cur);
    if (!work) return;
    if (dlg_edit_template(g_hwndMain, work, 0)) {
        wchar_t id[QP_ID_MAX];
        qp_now_iso(work->updated_at, QP_TIME_MAX);
        qp_strlcpy(id, work->id, QP_ID_MAX);
        /* 替换会释放旧对象，先切断引用 */
        ui_select_template(NULL);
        tmpl_list_replace(&g_app->templates, work);
        app_save_data();
        ui_rebuild_tree(id);
    } else {
        tmpl_free(work);
    }
}

static void ui_duplicate_template(void)
{
    Template *cur = tree_selected_template();
    Template *copy;
    if (!cur) return;
    copy = tmpl_clone(cur);
    if (!copy) return;
    qp_new_id(copy->id, QP_ID_MAX, L"tpl");
    {
        wchar_t name[QP_NAME_MAX];
        _snwprintf(name, QP_NAME_MAX, L"%ls - 副本", cur->name);
        name[QP_NAME_MAX - 1] = L'\0';
        qp_strlcpy(copy->name, name, QP_NAME_MAX);
    }
    qp_now_iso(copy->created_at, QP_TIME_MAX);
    qp_strlcpy(copy->updated_at, copy->created_at, QP_TIME_MAX);
    tmpl_list_append(&g_app->templates, copy);
    app_save_data();
    ui_rebuild_tree(copy->id);
}

static void ui_delete_template(void)
{
    Template *cur = tree_selected_template();
    if (!cur) return;
    if (qp_msgbox(g_hwndMain, MB_YESNO | MB_ICONWARNING, L"删除模板",
                  L"确定要删除模板「%ls」吗？\n\n此操作不可撤销。", cur->name) != IDYES)
        return;
    if (g_curTmpl == cur) ui_select_template(NULL);
    tmpl_list_remove(&g_app->templates, cur->id);
    app_save_data();
    ui_rebuild_tree(NULL);
}

static void ui_new_group(void)
{
    wchar_t name[QP_NAME_MAX] = L"";
    Group *g;
    if (!dlg_input(g_hwndMain, L"新建分组", L"分组名称：", name, QP_NAME_MAX)) return;
    qp_trim(name);
    if (!name[0]) return;
    g = group_create(name);
    if (!g) return;
    g->order = group_list_count(g_app->groups);
    group_list_append(&g_app->groups, g);
    app_save_data();
    ui_rebuild_tree(g_curTmpl ? g_curTmpl->id : NULL);
}

static void ui_rename_group(void)
{
    Group *g = tree_selected_group();
    wchar_t name[QP_NAME_MAX];
    if (!g) return;
    qp_strlcpy(name, g->name, QP_NAME_MAX);
    if (!dlg_input(g_hwndMain, L"重命名分组", L"分组名称：", name, QP_NAME_MAX)) return;
    qp_trim(name);
    if (!name[0]) return;
    qp_strlcpy(g->name, name, QP_NAME_MAX);
    app_save_data();
    ui_rebuild_tree(g_curTmpl ? g_curTmpl->id : NULL);
}

static void ui_delete_group(void)
{
    Group *g = tree_selected_group();
    Template *t;
    int used = 0;
    if (!g) return;
    for (t = g_app->templates; t; t = t->next)
        if (wcscmp(t->group_id, g->id) == 0) ++used;
    if (qp_msgbox(g_hwndMain, MB_YESNO | MB_ICONWARNING, L"删除分组",
                  used ? L"确定删除分组「%ls」吗？\n\n其中的 %d 个模板会移到「未分组」，不会被删除。"
                       : L"确定删除分组「%ls」吗？",
                  g->name, used) != IDYES)
        return;
    for (t = g_app->templates; t; t = t->next)
        if (wcscmp(t->group_id, g->id) == 0) t->group_id[0] = L'\0';
    group_list_remove(&g_app->groups, g->id);
    app_save_data();
    ui_rebuild_tree(g_curTmpl ? g_curTmpl->id : NULL);
}

/* 拖拽：把模板移动到分组（target 为 NULL 表示移出分组） */
static void ui_move_template_to_group(const wchar_t *tmpl_id, const wchar_t *group_id)
{
    Template *t = tmpl_list_find(g_app->templates, tmpl_id);
    if (!t) return;
    qp_strlcpy(t->group_id, group_id ? group_id : L"", QP_ID_MAX);
    qp_now_iso(t->updated_at, QP_TIME_MAX);
    app_save_data();
    ui_rebuild_tree(t->id);
}

/* ================================================================== */
/* 导入 / 导出                                                         */
/* ================================================================== */

static void ui_export(void)
{
    OPENFILENAMEW ofn;
    wchar_t path[MAX_PATH] = L"QuickPhrase-模板.json";
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwndMain;
    ofn.lpstrFilter = L"JSON 文件 (*.json)\0*.json\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"导出全部模板";
    ofn.lpstrDefExt = L"json";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) return;
    if (storage_export(g_app, path))
        qp_msgbox(g_hwndMain, MB_OK | MB_ICONINFORMATION, L"导出成功",
                  L"已导出 %d 个模板、%d 个分组到：\n%ls",
                  tmpl_list_count(g_app->templates), group_list_count(g_app->groups), path);
    else
        qp_msgbox(g_hwndMain, MB_OK | MB_ICONERROR, L"导出失败",
                  L"无法写入文件：\n%ls", path);
}

static void ui_import(void)
{
    OPENFILENAMEW ofn;
    wchar_t path[MAX_PATH] = L"";
    int mode, added = 0, updated = 0, r;

    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwndMain;
    ofn.lpstrFilter = L"JSON 文件 (*.json)\0*.json\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"导入模板";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;

    r = qp_msgbox(g_hwndMain, MB_YESNOCANCEL | MB_ICONQUESTION, L"导入方式",
                  L"选择导入方式：\n\n"
                  L"【是】合并 —— 保留现有模板，同 ID 的用文件内容覆盖\n"
                  L"【否】覆盖 —— 清空现有模板，只保留文件中的内容\n"
                  L"【取消】放弃导入");
    if (r == IDCANCEL) return;
    mode = (r == IDYES) ? QP_IMPORT_MERGE : QP_IMPORT_REPLACE;

    if (mode == QP_IMPORT_REPLACE &&
        qp_msgbox(g_hwndMain, MB_YESNO | MB_ICONWARNING, L"确认覆盖",
                  L"覆盖导入会删除当前全部 %d 个模板，确定继续？",
                  tmpl_list_count(g_app->templates)) != IDYES)
        return;

    ui_select_template(NULL);
    if (storage_import(g_app, path, mode, &added, &updated)) {
        app_save_data();
        ui_rebuild_tree(NULL);
        qp_msgbox(g_hwndMain, MB_OK | MB_ICONINFORMATION, L"导入完成",
                  L"新增 %d 个模板，更新 %d 个模板。", added, updated);
    } else {
        ui_rebuild_tree(NULL);
        qp_msgbox(g_hwndMain, MB_OK | MB_ICONERROR, L"导入失败",
                  L"文件无法读取或不是有效的 QuickPhrase JSON：\n%ls", path);
    }
}

/* ================================================================== */
/* 复制 / 重置                                                         */
/* ================================================================== */

void ui_copy_current(void)
{
    wchar_t out[QP_OUTPUT_MAX];
    if (!g_ctx) return;
    eval_render(g_ctx, out, QP_OUTPUT_MAX);
    if (!out[0]) {
        SetWindowTextW(g_hHint, L"当前口径为空，没有可复制的内容");
        return;
    }
    if (qp_copy_to_clipboard(g_hwndMain, out)) {
        SetWindowTextW(g_hBtnCopy, L"✓ 已复制");
        SetWindowTextW(g_hHint, L"口径已复制到剪贴板");
        SetTimer(g_hwndMain, TIMER_COPY_FEEDBACK, 1200, NULL);
    } else {
        SetWindowTextW(g_hHint, L"复制失败：剪贴板被其他程序占用");
    }
}

void ui_reset_current(void)
{
    if (!g_ctx) return;
    eval_reset(g_ctx);
    ui_sync_controls();
    ui_refresh_current();
    SetWindowTextW(g_hHint, L"已恢复为默认值");
}

void ui_focus_search(void)
{
    if (g_hSearch) {
        SetFocus(g_hSearch);
        SendMessageW(g_hSearch, EM_SETSEL, 0, -1);
    }
}

void ui_theme_changed(void)
{
    popup_close();          /* 弹层自绘配色是打开时取的，换主题直接收起重开 */
    qp_theme_children(g_hwndMain);
    if (g_hHost) qp_theme_children(g_hHost);
    if (s_flushMenuThemes) s_flushMenuThemes();
}

/* ================================================================== */
/* 菜单                                                                */
/* ================================================================== */

static void ui_build_menu(HWND hwnd)
{
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    HMENU tpl = CreatePopupMenu();
    HMENU set = CreatePopupMenu();
    HMENU help = CreatePopupMenu();

    AppendMenuW(file, MF_STRING, IDM_FILE_IMPORT, L"导入模板(&I)…");
    AppendMenuW(file, MF_STRING, IDM_FILE_EXPORT, L"导出模板(&E)…");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, IDM_OPEN_DATA_DIR, L"打开数据目录(&D)");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, IDM_FILE_HIDE, L"最小化到托盘(&M)\tEsc");
    AppendMenuW(file, MF_STRING, IDM_FILE_EXIT, L"退出(&X)");

    AppendMenuW(tpl, MF_STRING, IDM_TPL_NEW, L"新建模板(&N)\tCtrl+N");
    AppendMenuW(tpl, MF_STRING, IDM_GROUP_NEW, L"新建分组(&G)");
    AppendMenuW(tpl, MF_SEPARATOR, 0, NULL);
    AppendMenuW(tpl, MF_STRING, IDM_TPL_EDIT, L"编辑模板(&E)\tCtrl+E");
    AppendMenuW(tpl, MF_STRING, IDM_TPL_DUP, L"复制模板(&D)");
    AppendMenuW(tpl, MF_STRING, IDM_TPL_DELETE, L"删除模板(&R)");

    AppendMenuW(set, MF_STRING, IDM_SETTINGS, L"选项(&O)…");

    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"关于(&A)");

    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"文件(&F)");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)tpl, L"模板(&T)");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)set, L"设置(&S)");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"帮助(&H)");
    SetMenu(hwnd, bar);
}

static void ui_tree_context_menu(int x, int y)
{
    HMENU m = CreatePopupMenu();
    NodeRef *r;
    HTREEITEM hit;
    TVHITTESTINFO ht;
    POINT pt = { x, y };

    ScreenToClient(g_hTree, &pt);
    ht.pt = pt;
    hit = TreeView_HitTest(g_hTree, &ht);
    if (hit) TreeView_SelectItem(g_hTree, hit);
    r = tree_ref(hit);

    AppendMenuW(m, MF_STRING, IDM_TPL_NEW, L"新建模板");
    AppendMenuW(m, MF_STRING, IDM_GROUP_NEW, L"新建分组");
    if (r && r->kind == 1) {
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, IDM_TPL_EDIT, L"编辑模板");
        AppendMenuW(m, MF_STRING, IDM_TPL_DUP, L"复制模板");
        AppendMenuW(m, MF_STRING, IDM_TPL_MOVE_ROOT, L"移出分组");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, IDM_TPL_DELETE, L"删除模板");
    } else if (r && r->kind == 0) {
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, IDM_GROUP_RENAME, L"重命名分组");
        AppendMenuW(m, MF_STRING, IDM_GROUP_DELETE, L"删除分组");
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_TREE_EXPAND, L"展开全部");
    AppendMenuW(m, MF_STRING, IDM_TREE_COLLAPSE, L"折叠全部");

    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_LEFTALIGN, x, y, 0, g_hwndMain, NULL);
    DestroyMenu(m);
}

/* ================================================================== */
/* 布局                                                                */
/* ================================================================== */

/* 左侧树宽度：按当前客户区宽度夹取到合法范围。
   注意「只读夹取、不回写 g_settings.tree_width」—— 旧实现在布局里把夹取后
   的值写回设置，于是拖过边界再往回拖时分隔条会先粘在边界上、光标和分隔条
   错开一大段才重新跟上，这就是「拖边框手感不自然」的一半原因。 */
static int ui_tree_width_for(int clientW)
{
    int minW = DPIX(150), maxW = clientW - DPIX(320);
    int w = g_settings.tree_width;
    if (maxW < minW) maxW = minW;
    if (w < minW) w = minW;
    if (w > maxW) w = maxW;
    return w;
}

#define QP_SPLIT_HIT DPIX(10)      /* 分隔条命中宽度 */

static int splitter_x(void)
{
    RECT rc;
    if (!g_hwndMain) return DPIX(8) + g_settings.tree_width;
    GetClientRect(g_hwndMain, &rc);
    return DPIX(8) + ui_tree_width_for(rc.right - rc.left);
}

static void ui_layout(HWND hwnd)
{
    RECT rc;
    HDWP dwp;
    int m = DPIX(8), searchH = DPIX(24), gap = DPIX(6);
    int treeW, rx, rw, y, previewH, btnH = DPIX(28), btnW = DPIX(104);
    int W, H, hostTop, hostH, sx;

    if (!g_hTree) return;
    GetClientRect(hwnd, &rc);
    W = rc.right - rc.left;
    H = rc.bottom - rc.top;
    if (W <= 0 || H <= 0) return;      /* 最小化时不做无意义的布局 */

    treeW = ui_tree_width_for(W);

    rx = m + treeW + QP_SPLIT_HIT;
    rw = W - rx - m;
    if (rw < DPIX(200)) rw = DPIX(200);

    previewH = H / 4;
    if (previewH < DPIX(72)) previewH = DPIX(72);
    if (previewH > DPIX(200)) previewH = DPIX(200);

    y = H - m - btnH;
    hostTop = m + DPIX(22) + gap;
    hostH = (y - gap - previewH) - DPIX(18) - gap - hostTop;
    if (hostH < DPIX(40)) hostH = DPIX(40);

    /* 一次性提交所有位置变化：逐个 MoveWindow 会让每个控件各自重绘一次，
       拖动边框时看起来就是控件一个个错位跟上，很不流畅。 */
    dwp = BeginDeferWindowPos(12);
    dwp = qp_defer(dwp, g_hSearch, m, m, treeW, searchH);
    dwp = qp_defer(dwp, g_hTree, m, m + searchH + gap, treeW,
                   H - m * 2 - searchH - gap);
    dwp = qp_defer(dwp, g_hTitle, rx, m, rw, DPIX(22));
    dwp = qp_defer(dwp, g_hBtnCopy, rx + rw - btnW, y, btnW, btnH);
    dwp = qp_defer(dwp, g_hBtnReset, rx + rw - btnW * 2 - DPIX(6), y, btnW, btnH);
    dwp = qp_defer(dwp, g_hBtnEdit, rx + rw - btnW * 3 - DPIX(12), y, btnW, btnH);
    dwp = qp_defer(dwp, g_hHint, rx, y + DPIX(6),
                   rw - btnW * 3 - DPIX(20), DPIX(18));
    y -= gap + previewH;
    dwp = qp_defer(dwp, g_hPreview, rx, y, rw, previewH);
    dwp = qp_defer(dwp, g_hPreviewLabel, rx, y - DPIX(18), rw, DPIX(16));
    dwp = qp_defer(dwp, g_hHost, rx, hostTop, rw, hostH);
    if (dwp) EndDeferWindowPos(dwp);

    host_clamp_scroll();
    host_reposition();
    host_update_scroll();

    /* 只重刷分隔条挪动过的那一竖条。旧实现在这里 InvalidateRect(hwnd, NULL,
       TRUE)：主窗口当时还没有 WS_CLIPCHILDREN，整片擦背景会把字段区和它的
       标签一起涂掉，而标签的位置没变、系统不会主动重绘它，于是「产品名称：
       版本号：…」那一列就整列消失了。 */
    sx = m + treeW;
    if (g_splitLastX != sx) {
        RECT strip;
        strip.left   = (g_splitLastX < 0 ? sx : (g_splitLastX < sx ? g_splitLastX : sx)) - DPIX(2);
        strip.right  = (g_splitLastX > sx ? g_splitLastX : sx) + QP_SPLIT_HIT + DPIX(2);
        strip.top    = 0;
        strip.bottom = H;
        InvalidateRect(hwnd, &strip, TRUE);
        g_splitLastX = sx;
    }
}

/* ================================================================== */
/* 主窗口过程                                                          */
/* ================================================================== */

static void save_placement(HWND hwnd)
{
    WINDOWPLACEMENT wp;
    memset(&wp, 0, sizeof(wp));
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(hwnd, &wp)) return;
    g_settings.win_maximized = (wp.showCmd == SW_SHOWMAXIMIZED) ? 1 : 0;
    if (wp.rcNormalPosition.right > wp.rcNormalPosition.left) {
        g_settings.win_x = wp.rcNormalPosition.left;
        g_settings.win_y = wp.rcNormalPosition.top;
        g_settings.win_w = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
        g_settings.win_h = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
    }
}

static void handle_field_command(WORD id, WORD code)
{
    int slot = (id - IDC_FIELD_BASE) / IDC_FIELD_STRIDE;
    int which = (id - IDC_FIELD_BASE) % IDC_FIELD_STRIDE;
    FieldCtl *f;

    if (!g_ctx || slot < 0 || slot >= g_fieldCount) return;
    f = &g_fields[slot];

    if (which == 1) {                       /* ▼ 按钮 */
        if (code == BN_CLICKED) popup_open(f->field_index, f->hCtl);
        return;
    }
    if (which != 0) return;

    if (code == EN_CHANGE) {
        wchar_t buf[QP_VALUE_MAX];
        const Segment *seg = g_ctx->fields[f->field_index].seg;
        if (!seg || seg->type != SEG_TEXT) return;   /* 只读框由弹窗驱动 */
        qp_get_text(f->hCtl, buf, QP_VALUE_MAX);
        eval_set_user_value(g_ctx, f->field_index, buf);
        ui_refresh_current();
    } else if (code == CBN_SELCHANGE) {
        wchar_t buf[QP_VALUE_MAX];
        int idx = (int)SendMessageW(f->hCtl, CB_GETCURSEL, 0, 0);
        if (idx == CB_ERR) return;
        buf[0] = L'\0';
        SendMessageW(f->hCtl, CB_GETLBTEXT, (WPARAM)idx, (LPARAM)buf);
        eval_set_user_value(g_ctx, f->field_index, buf);
        ui_refresh_current();
    }
}

/* 拖拽模板到分组：由主窗口捕获鼠标后驱动 */
static void drag_track(void)
{
    POINT pt;
    TVHITTESTINFO ht;
    HTREEITEM target;
    GetCursorPos(&pt);
    ScreenToClient(g_hTree, &pt);
    ht.pt = pt;
    target = TreeView_HitTest(g_hTree, &ht);
    TreeView_SelectDropTarget(g_hTree, target);
    SetCursor(LoadCursorW(NULL, target ? IDC_HAND : IDC_NO));
}

static void drag_drop(void)
{
    POINT pt;
    TVHITTESTINFO ht;
    HTREEITEM target;
    NodeRef *src, *dst;

    GetCursorPos(&pt);
    ScreenToClient(g_hTree, &pt);
    ht.pt = pt;
    target = TreeView_HitTest(g_hTree, &ht);
    src = tree_ref(g_dragItem);
    dst = tree_ref(target);

    TreeView_SelectDropTarget(g_hTree, NULL);
    ReleaseCapture();
    g_dragging = 0;
    g_dragItem = NULL;

    if (!src || src->kind != 1) return;
    if (dst && dst->kind == 0) {
        ui_move_template_to_group(src->id, dst->id);
    } else if (dst && dst->kind == 1) {
        Template *dt = tmpl_list_find(g_app->templates, dst->id);
        ui_move_template_to_group(src->id, dt ? dt->group_id : NULL);
    } else if (!target) {
        ui_move_template_to_group(src->id, NULL);
    }
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    /* 第二个实例启动时会广播这条消息，用来唤出已在运行的窗口 */
    if (msg == g_msgShowWindow && g_msgShowWindow) {
        app_show_main();
        return 0;
    }
    /* 卸载程序广播这条消息，请求本进程退出以便删除文件 */
    if (msg == g_msgRequestExit && g_msgRequestExit) {
        app_request_exit();
        return 0;
    }

    switch (msg) {
    case WM_CREATE: {
        HDC hdc = GetDC(hwnd);
        g_dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSX) : 96;
        if (hdc) ReleaseDC(hwnd, hdc);
        if (g_dpi <= 0) g_dpi = 96;
        ui_build_menu(hwnd);

        g_hSearch = qp_create(L"Edit", L"", ES_AUTOHSCROLL | WS_TABSTOP,
                              WS_EX_CLIENTEDGE, 0, 0, 10, 10, hwnd, IDC_SEARCH);
        SendMessageW(g_hSearch, EM_SETCUEBANNER, TRUE, (LPARAM)L"搜索模板… (Ctrl+F)");

        g_hTree = qp_create(WC_TREEVIEWW, L"",
                            TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT |
                            TVS_SHOWSELALWAYS | TVS_EDITLABELS | WS_TABSTOP,
                            WS_EX_CLIENTEDGE, 0, 0, 10, 10, hwnd, IDC_TREE);
        SendMessageW(g_hTree, TVM_SETEXTENDEDSTYLE, TVS_EX_DOUBLEBUFFER,
                     TVS_EX_DOUBLEBUFFER);

        g_hTitle = qp_create(L"Static", L"未选择模板", SS_LEFT | SS_ENDELLIPSIS, 0,
                             0, 0, 10, 10, hwnd, IDC_TITLE);
        SendMessageW(g_hTitle, WM_SETFONT, (WPARAM)g_fontBold, TRUE);

        g_hHost = qp_create(CLS_HOST, L"", WS_VSCROLL | WS_CLIPCHILDREN, 0,
                            0, 0, 10, 10, hwnd, IDC_FIELDHOST);

        g_hPreviewLabel = qp_create(L"Static", L"口径预览", SS_LEFT, 0,
                                    0, 0, 10, 10, hwnd, IDC_PREVIEW_LABEL);
        g_hPreview = qp_create(L"Edit", L"",
                               ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
                               WS_EX_CLIENTEDGE, 0, 0, 10, 10, hwnd, IDC_PREVIEW);
        SendMessageW(g_hPreview, WM_SETFONT, (WPARAM)g_fontMono, TRUE);

        g_hBtnCopy = qp_create(L"Button", L"一键复制", BS_DEFPUSHBUTTON | WS_TABSTOP, 0,
                               0, 0, 10, 10, hwnd, IDC_BTN_COPY);
        g_hBtnReset = qp_create(L"Button", L"重置", BS_PUSHBUTTON | WS_TABSTOP, 0,
                                0, 0, 10, 10, hwnd, IDC_BTN_RESET);
        g_hBtnEdit = qp_create(L"Button", L"编辑模板", BS_PUSHBUTTON | WS_TABSTOP, 0,
                               0, 0, 10, 10, hwnd, IDC_BTN_EDIT);
        g_hHint = qp_create(L"Static", L"", SS_LEFT | SS_ENDELLIPSIS, 0,
                            0, 0, 10, 10, hwnd, IDC_HINT);
        return 0;
    }

    case WM_SIZE:
        popup_close();          /* 尺寸一变，弹层锚点就失位了 */
        ui_layout(hwnd);
        if (wp != SIZE_MINIMIZED) {
            SetTimer(hwnd, TIMER_SAVE_PLACEMENT, 500, NULL);
        }
        return 0;

    case WM_MOVE:
        popup_close();
        SetTimer(hwnd, TIMER_SAVE_PLACEMENT, 500, NULL);
        return 0;

    case WM_ACTIVATE:
        /* 主窗口失活（Alt+Tab、点到别的程序）→ 收起弹层。此时弹层既没有焦点
           也不是激活窗口，销毁它不牵动任何激活切换，安全。 */
        if (LOWORD(wp) == WA_INACTIVE) popup_close();
        break;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        mmi->ptMinTrackSize.x = MulDiv(680, g_dpi ? g_dpi : 96, 96);
        mmi->ptMinTrackSize.y = MulDiv(460, g_dpi ? g_dpi : 96, 96);
        return 0;
    }

    case WM_TIMER:
        if (wp == TIMER_COPY_FEEDBACK) {
            KillTimer(hwnd, TIMER_COPY_FEEDBACK);
            SetWindowTextW(g_hBtnCopy, L"一键复制");
        } else if (wp == TIMER_SAVE_PLACEMENT) {
            KillTimer(hwnd, TIMER_SAVE_PLACEMENT);
            save_placement(hwnd);
            app_save_settings();
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (hdc) {
            RECT rc, bar;
            HBRUSH b = CreateSolidBrush(theme_line());
            GetClientRect(hwnd, &rc);
            /* 画出分隔条本体：以前这里什么都不画，用户只能靠光标形状猜哪里
               可以拖，拖起来自然没有「抓住了」的感觉。 */
            bar.left   = splitter_x() + QP_SPLIT_HIT / 2;
            bar.right  = bar.left + (DPIX(1) > 1 ? DPIX(1) : 1);
            bar.top    = DPIX(36);
            bar.bottom = rc.bottom - DPIX(10);
            if (b) {
                if (bar.bottom > bar.top) FillRect(hdc, &bar, b);
                DeleteObject(b);
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SETCURSOR: {
        POINT pt;
        RECT rc;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        GetClientRect(hwnd, &rc);
        if (pt.x >= splitter_x() && pt.x <= splitter_x() + QP_SPLIT_HIT &&
            pt.y > DPIX(30) && pt.y < rc.bottom) {
            SetCursor(LoadCursorW(NULL, IDC_SIZEWE));
            return TRUE;
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        int sx = splitter_x();
        popup_close();
        if (x >= sx && x <= sx + QP_SPLIT_HIT && y > DPIX(30)) {
            g_splitDrag = 1;
            g_splitGrabDx = x - sx;   /* 记住抓取偏移，分隔条不再跳到光标下 */
            SetCapture(hwnd);
        }
        return 0;
    }

    case WM_MOUSEMOVE:
        if (g_dragging) { drag_track(); return 0; }
        if (g_splitDrag) {
            RECT rc;
            int x = (short)LOWORD(lp);
            int minW = DPIX(150), maxW;
            GetClientRect(hwnd, &rc);
            maxW = (rc.right - rc.left) - DPIX(320);
            if (maxW < minW) maxW = minW;
            /* 在拖动这一侧就把宽度夹好，与 ui_tree_width_for 保持一致，
               光标和分隔条才会严格同步 */
            x -= g_splitGrabDx + DPIX(8);
            if (x < minW) x = minW;
            if (x > maxW) x = maxW;
            if (x != g_settings.tree_width) {
                g_settings.tree_width = x;
                ui_layout(hwnd);
            }
        }
        return 0;

    case WM_LBUTTONUP:
        if (g_dragging) { drag_drop(); return 0; }
        if (g_splitDrag) {
            g_splitDrag = 0;
            ReleaseCapture();
            app_save_settings();
        }
        return 0;

    case WM_CANCELMODE:
        if (g_dragging) {
            TreeView_SelectDropTarget(g_hTree, NULL);
            g_dragging = 0;
            g_dragItem = NULL;
        }
        g_splitDrag = 0;
        break;

    case WM_COMMAND: {
        WORD id = LOWORD(wp), code = HIWORD(wp);

        qp_combo_dropdown_fix(wp, lp);

        if (id >= IDC_FIELD_BASE && id < IDC_FIELD_BASE + 3000) {
            handle_field_command(id, code);
            return 0;
        }

        switch (id) {
        case IDC_SEARCH:
            if (code == EN_CHANGE) ui_rebuild_tree(g_curTmpl ? g_curTmpl->id : NULL);
            return 0;
        case IDC_BTN_COPY:
        case IDM_COPY:
            ui_copy_current();
            return 0;
        case IDC_BTN_RESET:
        case IDM_RESET:
            ui_reset_current();
            return 0;
        case IDC_BTN_EDIT:
        case IDM_TPL_EDIT:
            ui_edit_current();
            return 0;
        case IDM_TPL_NEW:      ui_new_template(); return 0;
        case IDM_TPL_DUP:      ui_duplicate_template(); return 0;
        case IDM_TPL_DELETE:   ui_delete_template(); return 0;
        case IDM_GROUP_NEW:    ui_new_group(); return 0;
        case IDM_GROUP_RENAME: ui_rename_group(); return 0;
        case IDM_GROUP_DELETE: ui_delete_group(); return 0;
        case IDM_TPL_MOVE_ROOT: {
            Template *t = tree_selected_template();
            if (t) ui_move_template_to_group(t->id, NULL);
            return 0;
        }
        case IDM_TREE_EXPAND:
        case IDM_TREE_COLLAPSE: {
            HTREEITEM it = TreeView_GetRoot(g_hTree);
            while (it) {
                TreeView_Expand(g_hTree, it,
                                id == IDM_TREE_EXPAND ? TVE_EXPAND : TVE_COLLAPSE);
                it = TreeView_GetNextSibling(g_hTree, it);
            }
            return 0;
        }
        case IDM_FILE_IMPORT: ui_import(); return 0;
        case IDM_FILE_EXPORT: ui_export(); return 0;
        case IDM_OPEN_DATA_DIR: {
            wchar_t dir[600];
            storage_ensure_dir();
            if (storage_get_data_dir(dir, 600))
                ShellExecuteW(hwnd, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        case IDM_HIDE_WINDOW:
            /* Esc 的加速键。加速键在消息到达控件之前就被翻译掉，所以弹层打开
               时按 Esc 会先走到这里；此时应该只收起弹层，而不是把整个窗口藏
               到托盘。菜单项 IDM_FILE_HIDE 不受此约束。 */
            if (g_popup) { popup_close(); return 0; }
            app_hide_main();
            return 0;
        case IDM_FILE_HIDE:
            app_hide_main();
            return 0;
        case IDM_FILE_EXIT:
            app_request_exit();
            return 0;
        case IDM_SETTINGS:
            dlg_settings(hwnd);
            return 0;
        case IDM_ABOUT:
            dlg_about(hwnd);
            return 0;
        case IDM_FOCUS_SEARCH:
            ui_focus_search();
            return 0;
        case IDM_TRAY_OPEN:
            app_show_main();
            return 0;
        case IDM_TRAY_SETTINGS:
            app_show_main();
            dlg_settings(hwnd);
            return 0;
        case IDM_TRAY_AUTOSTART:
            g_settings.autostart = app_get_autostart() ? 0 : 1;
            if (!app_set_autostart(g_settings.autostart)) {
                g_settings.autostart = app_get_autostart();
                qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, QP_APP_TITLE,
                          L"写入开机启动项失败，可能被安全软件拦截。");
            }
            app_save_settings();
            return 0;
        case IDM_TRAY_EXIT:
            app_request_exit();
            return 0;
        default:
            break;
        }
        return 0;
    }

    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (nh->idFrom == IDC_TREE) {
            switch (nh->code) {
            case TVN_SELCHANGEDW: {
                NodeRef *r = tree_ref(((NMTREEVIEWW *)lp)->itemNew.hItem);
                if (r && r->kind == 1)
                    ui_select_template(tmpl_list_find(g_app->templates, r->id));
                else
                    ui_select_template(NULL);
                return 0;
            }
            case NM_DBLCLK: {
                NodeRef *r = tree_ref(TreeView_GetSelection(g_hTree));
                if (r && r->kind == 1) ui_edit_current();
                return 0;
            }
            case TVN_DELETEITEMW: {
                NMTREEVIEWW *nt = (NMTREEVIEWW *)lp;
                free((void *)nt->itemOld.lParam);
                return 0;
            }
            case TVN_BEGINDRAGW: {
                NMTREEVIEWW *nt = (NMTREEVIEWW *)lp;
                NodeRef *r = tree_ref(nt->itemNew.hItem);
                if (r && r->kind == 1) {
                    g_dragItem = nt->itemNew.hItem;
                    g_dragging = 1;
                    TreeView_SelectItem(g_hTree, g_dragItem);
                    SetCapture(hwnd);
                    SetCursor(LoadCursorW(NULL, IDC_HAND));
                }
                return 0;
            }
            case NM_RCLICK: {
                DWORD pos = GetMessagePos();
                ui_tree_context_menu((short)LOWORD(pos), (short)HIWORD(pos));
                return 1;
            }
            default:
                break;
            }
        }
        break;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG: {
        int handled = 0;
        LRESULT r = qp_handle_ctlcolor(msg, wp, lp, &handled);
        if (handled) {
            /* 预览框用强调色文字，突出结果 */
            if ((HWND)lp == g_hPreview) SetTextColor((HDC)wp, theme_text());
            if ((HWND)lp == g_hHint) SetTextColor((HDC)wp, theme_dim());
            if ((HWND)lp == g_hTitle) SetTextColor((HDC)wp, theme_accent());
            return r;
        }
        break;
    }

    case WM_ERASEBKGND: {
        RECT rc;
        /* 主窗口带 WS_CLIPCHILDREN，这次填充不会再涂到子控件上（这正是标签列
           偶发消失的根因：以前整片涂完，位置未变的 Static 不会自动重绘）。 */
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, g_brushBk);
        return 1;
    }

    case WM_SETTINGCHANGE:
        if (lp && wcscmp((const wchar_t *)lp, L"ImmersiveColorSet") == 0 &&
            g_settings.theme == THEME_SYSTEM)
            app_apply_theme();
        return 0;

    case WM_QP_REFRESH:
        ui_rebuild_tree(g_curTmpl ? g_curTmpl->id : NULL);
        return 0;

    case WM_QP_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU)
            app_tray_show_menu();
        else if (LOWORD(lp) == WM_LBUTTONDBLCLK || LOWORD(lp) == WM_LBUTTONUP)
            app_show_main();
        return 0;

    case WM_HOTKEY:
        app_show_main();
        return 0;

    case WM_CLOSE:
        /* 关闭按钮 → 隐藏到托盘，而不是退出 */
        save_placement(hwnd);
        app_save_settings();
        app_hide_main();
        return 0;

    case WM_DESTROY:
        ui_clear_fields();
        if (g_ctx) { eval_destroy(g_ctx); g_ctx = NULL; }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ================================================================== */
/* 注册与创建                                                          */
/* ================================================================== */

int ui_register_classes(void)
{
    WNDCLASSEXW wc;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = main_proc;
    wc.hInstance = g_hInst;
    wc.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = CLS_MAIN;
    if (!RegisterClassExW(&wc)) return 0;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = host_proc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = CLS_HOST;
    if (!RegisterClassExW(&wc)) return 0;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = popup_proc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = CLS_POPUP;
    if (!RegisterClassExW(&wc)) return 0;

    return 1;
}

HWND ui_create_main_window(void)
{
    HWND hwnd;
    int x = g_settings.win_x, y = g_settings.win_y;
    int w = g_settings.win_w, h = g_settings.win_h;

    if (w < 640) w = 900;
    if (h < 460) h = 620;
    if (x < 0 || y < 0) { x = CW_USEDEFAULT; y = CW_USEDEFAULT; }

    hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, CLS_MAIN,
                           QP_APP_TITLE L" — QuickPhrase",
                           WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                           x, y, w, h, NULL, NULL, g_hInst, NULL);
    if (!hwnd) return NULL;

    /* 位置越界（例如换了显示器）时回到屏幕内 */
    if (x != CW_USEDEFAULT) {
        RECT rc;
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
        if (!mon) {
            GetWindowRect(hwnd, &rc);
            qp_center_window(hwnd, NULL);
        }
    }
    return hwnd;
}

HACCEL ui_create_accelerators(void)
{
    ACCEL acc[] = {
        { FVIRTKEY | FCONTROL, VK_RETURN, IDM_COPY },
        { FVIRTKEY | FCONTROL, 'F',       IDM_FOCUS_SEARCH },
        { FVIRTKEY | FCONTROL, 'N',       IDM_TPL_NEW },
        { FVIRTKEY | FCONTROL, 'E',       IDM_TPL_EDIT },
        { FVIRTKEY | FCONTROL, 'R',       IDM_RESET },
        { FVIRTKEY,            VK_ESCAPE, IDM_HIDE_WINDOW },
    };
    return CreateAcceleratorTableW(acc, (int)(sizeof(acc) / sizeof(acc[0])));
}
