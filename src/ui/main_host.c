#include "ui_internal.h"
/* ================================================================== */
/* 字段区（可滚动容器）                                                */
/* ================================================================== */


/* 内容变短 / 可视区变高时把滚动位置收回来，否则底部会露出空白 */
void host_clamp_scroll(void)
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

void host_update_scroll(void)
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

void host_reposition(void)
{
    RECT rc;
    HDWP dwp;
    int i, labelW = DPIX(96), pad = DPIX(12);
    if (!g_hHost || g_fieldCount <= 0 || !g_fields) return;
    GetClientRect(g_hHost, &rc);
    dwp = qp_layout_begin(g_fieldCount * 3);
    for (i = 0; i < g_fieldCount; ++i) {
        FieldCtl *f = &g_fields[i];
        int y = f->y - g_hostScroll;
        int ctlH = f->h > 0 ? f->h : QP_ROW_CTL_H;
        int cx = pad + labelW + DPIX(8);
        int cw = (rc.right - rc.left) - cx - pad;
        int btnW = f->btnW > 0 ? f->btnW : DPIX(26);
        if (cw < DPIX(80)) cw = DPIX(80);
        /* 多行框：标签跟第一行文字对齐，不跟着框子居中 */
        dwp = qp_defer(dwp, f->hLabel, pad, y + DPIX(4), labelW, DPIX(18));
        if (f->hBtn) {
            dwp = qp_defer(dwp, f->hCtl, cx, y, cw - btnW - DPIX(2), ctlH);
            dwp = qp_defer(dwp, f->hBtn, cx + cw - btnW, y, btnW, ctlH);
        } else {
            dwp = qp_defer(dwp, f->hCtl, cx, y, cw, ctlH);
        }
    }
    qp_layout_end(dwp);
}

LRESULT CALLBACK host_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND: qp_draw_background(hwnd,(HDC)wp); return 1;
    case WM_PAINT: qp_paint_background(hwnd); return 0;
    case WM_PRINTCLIENT: qp_draw_background(hwnd,(HDC)wp); return 0;
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
LRESULT CALLBACK multi_edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
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
