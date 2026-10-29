#include "ui_internal.h"
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
const wchar_t *multi_sep(const Segment *seg)
{
    if (seg && seg->separator[0]) return seg->separator;
    if (g_settings.default_separator[0]) return g_settings.default_separator;
    return L"、";
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
static int popup_build_value(wchar_t *out, size_t cap)
{
    if (!out || !cap) return 0;
    out[0] = L'\0';
    if (!g_popupSeg || !g_popupChecked) return 0;
    const wchar_t *custom=g_ctx && g_popupField>=0 && g_popupField<g_ctx->count
        ? g_ctx->fields[g_popupField].custom_value:L"";
    return qp_choice_join(g_popupSeg,g_popupChecked,g_popupCount,custom,multi_sep(g_popupSeg),out,cap);
}

static int popup_apply(void)
{
    wchar_t value[QP_VALUE_MAX];
    if (!g_popup || !g_ctx || !g_popupSeg || !g_popupChecked) return 0;
    if (g_popupField < 0 || g_popupField >= g_ctx->count) return 0;
    if (!popup_build_value(value, QP_VALUE_MAX)) {
        qp_set_text_if_diff(g_hHint,L"所选内容超过变量长度上限，本次选择未生效。请减少选项或缩短选项内容后重试。");
        return 0;
    }
    if(!qp_choice_set(g_ctx,g_popupField,g_popupChecked,g_popupCount,
                      g_ctx->fields[g_popupField].custom_value,multi_sep(g_popupSeg)))return 0;
    ui_refresh_current();
    return 1;
}

void popup_close(void)
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

void popup_move_hot(int delta)
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
void popup_toggle(int row)
{
    if (!g_popup || !g_popupChecked || row < 0 || row >= g_popupCount) return;
    if(row==qp_choice_custom_index(g_popupSeg) && !g_popupChecked[row]) {
        ui_choice_multi(g_popupField,g_popupChecked,g_popupCount);
        return;
    }
    g_popupChecked[row] = (unsigned char)(g_popupChecked[row] ? 0 : 1);
    if (!popup_apply()) g_popupChecked[row] ^= 1;
    popup_invalidate_row(row);
}

/* 细分隔线颜色（分隔条、弹层滚动条）；ui_dialogs.c 的编辑器分隔条也用它 */
COLORREF theme_line(void)
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
LRESULT CALLBACK popup_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        popup_paint(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;                       /* 背景在 WM_PAINT 里一次画完 */

    case WM_SETCURSOR:
        /* 捕获期间系统不再把 WM_SETCURSOR 发给任何窗口，光标的形状只能由捕获
           方自己摆；而 DefWindowProc 会把这条消息转给父窗口（主窗口），主窗口
           按「分隔条命中」回一个左右/上下箭头 —— 弹层压在分隔条那一列上时，
           光标就变成缩放箭头。所以这里显式摆回箭头并自己吃掉这条消息。
           与 multi_edit_proc 对锚点 Edit 的处理同一口径。 */
        SetCursor(LoadCursorW(NULL, IDC_ARROW));
        return TRUE;

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

void popup_open(int field_index, HWND anchor)
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
                  L"变量「%ls」尚未配置选项。\n\n可在「编辑模板」中为其添加选项。",
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
    const EvalField *field=&g_ctx->fields[field_index];
    for (i = 0; i < g_popupCount; ++i) {
        g_popupChecked[i]=(unsigned char)qp_choice_is_checked(field,i,sep);
    }

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
    /* 捕获期间系统不会再发 WM_SETCURSOR（见 popup_proc 里的说明），所以摆光标
       这件事只能在这里补一次。「自主补充」的路径是 popup_close → 模态输入框 →
       popup_open：输入框销毁后光标由谁接手没人管，漏了这句指针会直接消失。 */
    SetCursor(LoadCursorW(NULL, IDC_ARROW));
}
