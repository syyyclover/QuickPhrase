#include "ui_internal.h"
/* ================================================================== */
/* 布局                                                                */
/* ================================================================== */

/* 左侧树宽度：按当前客户区宽度夹取到合法范围。
   注意「只读夹取、不回写 g_settings.tree_width」—— 旧实现在布局里把夹取后
   的值写回设置，于是拖过边界再往回拖时分隔条会先粘在边界上、光标和分隔条
   错开一大段才重新跟上，这就是「拖边框手感不自然」的一半原因。 */
int ui_tree_width_for(int clientW)
{
    int minW = DPIX(150), maxW = clientW - DPIX(320);
    int w = g_settings.tree_width;
    if (maxW < minW) maxW = minW;
    if (w < minW) w = minW;
    if (w > maxW) w = maxW;
    return w;
}





int splitter_x(void)
{
    RECT rc;
    if (!g_hwndMain) return DPIX(8) + g_settings.tree_width;
    GetClientRect(g_hwndMain, &rc);
    return DPIX(8) + ui_tree_width_for(rc.right - rc.left);
}

/* 字段区顶端：与 ui_layout 同源，横分隔条的可拖范围要靠它反推 */
static int ui_host_top(void)
{
    int tabsH = (g_curTmpl && g_hTabs) ? QP_TABS_H : 0;
    return DPIX(8) + DPIX(22) + DPIX(6) + (tabsH ? tabsH + DPIX(4) : 0);
}

/* 口径预览区的合法高度区间：上限按「字段区至少留 DPIX(40)」反推 */
void ui_preview_bounds(int H, int *lo, int *hi)
{
    int minH = DPIX(64);
    int maxH = (H - DPIX(8) - DPIX(28)) - DPIX(6) - DPIX(18) - QP_HSPLIT_HIT
               - ui_host_top() - DPIX(40);
    if (maxH < minH) maxH = minH;
    *lo = minH;
    *hi = maxH;
}

/* 预览区高度：0 = 自动（沿用 H/4 的老口径），否则用记住的像素值。
   与 ui_tree_width_for 同一纪律 —— 只读夹取，绝不在布局里回写设置。 */
static int ui_preview_height_for(int H)
{
    int lo, hi, h = g_settings.preview_height;
    ui_preview_bounds(H, &lo, &hi);
    if (h <= 0) {
        h = H / 4;
        if (h < DPIX(72)) h = DPIX(72);
        if (h > DPIX(200)) h = DPIX(200);
    }
    if (h < lo) h = lo;
    if (h > hi) h = hi;
    return h;
}

/* 横分隔条上沿（右栏「字段区 ↔ 口径预览」之间那一条） */
int hsplitter_y(int H)
{
    return H - DPIX(8) - DPIX(28) - DPIX(6) - ui_preview_height_for(H)
             - DPIX(18) - QP_HSPLIT_HIT;
}

/* 命中判定：只在右栏生效，与竖条的判定区不重叠 */
int hsplit_hit(int x, int y)
{
    RECT rc;
    int top;
    if (!g_hwndMain) return 0;
    GetClientRect(g_hwndMain, &rc);
    top = hsplitter_y(rc.bottom - rc.top);
    return x > splitter_x() + QP_SPLIT_HIT && x < rc.right - DPIX(2) &&
           y >= top && y < top + QP_HSPLIT_HIT;
}

void ui_layout(HWND hwnd)
{
    RECT rc;
    HDWP dwp;
    int m = DPIX(8), searchH = DPIX(24), gap = DPIX(6);
    int treeW, rx, rw, y, previewH, btnH = DPIX(28), btnW = DPIX(104);
    int W, H, hostTop, hostH, sx, sy, tabsH, tabsTop;
    int previewLabelW = DPIX(96), modeW = DPIX(62);

    if (!hwnd || !g_hTree) return;
    GetClientRect(hwnd, &rc);
    W = rc.right - rc.left;
    H = rc.bottom - rc.top;
    if (W <= 0 || H <= 0) return;      /* 最小化时不做无意义的布局 */

    treeW = ui_tree_width_for(W);

    rx = m + treeW + QP_SPLIT_HIT;
    rw = W - rx - m;
    if (rw < DPIX(200)) rw = DPIX(200);

    previewH = ui_preview_height_for(H);

    y = H - m - btnH;
    /* 没选模板时标签条整条收掉，界面和以前一模一样 */
    tabsH = (g_curTmpl && g_hTabs) ? QP_TABS_H : 0;
    tabsTop = m + DPIX(22) + gap;
    hostTop = ui_host_top();
    /* 字段区底下留出 QP_HSPLIT_HIT 而不是 gap：那一条正好当横分隔条的抓取带，
       视觉高度只差 2px，却不用把命中区叠到字段区的子窗口上 */
    hostH = (y - gap - previewH) - DPIX(18) - QP_HSPLIT_HIT - hostTop;
    if (hostH < DPIX(40)) hostH = DPIX(40);

    /* 一次性提交所有位置变化：逐个 MoveWindow 会让每个控件各自重绘一次，
       拖动边框时看起来就是控件一个个错位跟上，很不流畅。 */
    dwp = qp_layout_begin(15);
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
    dwp = qp_defer(dwp, g_hPreviewLabel, rx, y - DPIX(18), previewLabelW, DPIX(16));
    dwp = qp_defer(dwp, g_hPreviewTextMode, rx + previewLabelW, y - DPIX(20),
                   modeW, DPIX(18));
    dwp = qp_defer(dwp, g_hPreviewTableMode, rx + previewLabelW + modeW, y - DPIX(20),
                   modeW, DPIX(18));
    dwp = qp_defer(dwp, g_hHost, rx, hostTop, rw, hostH);
    if (tabsH) dwp = qp_defer(dwp, g_hTabs, rx, tabsTop, rw, tabsH);
    qp_layout_end(dwp);

    if (tabsH) tabs_measure();         /* 宽度变了，标签得重新排一遍 */
    host_clamp_scroll();
    host_reposition();
    host_update_scroll();

    /* 只重刷分隔条挪动过的那一竖条。旧实现在这里 InvalidateRect(hwnd, NULL,
       TRUE)：主窗口当时还没有 WS_CLIPCHILDREN，整片擦背景会把字段区和它的
       标签一起涂掉，而标签的位置没变、系统不会主动重绘它，于是「产品名称：
       版本号：…」那一列就整列消失了。
       现在一律用 RDW_NOERASE：背景由 WM_PAINT 连同分隔条一趟画完（见那里的
       注释），再让系统单独擦一遍就等于把条子涂掉又画回来——拖动时看到的
       「边框抽搐」就是这一擦一画。 */
    sx = m + treeW;
    if (g_splitLastX != sx) {
        RECT strip;
        strip.left   = (g_splitLastX < 0 ? sx : (g_splitLastX < sx ? g_splitLastX : sx)) - DPIX(2);
        strip.right  = (g_splitLastX > sx ? g_splitLastX : sx) + QP_SPLIT_HIT + DPIX(2);
        strip.top    = 0;
        strip.bottom = H;
        RedrawWindow(hwnd, &strip, NULL, RDW_INVALIDATE | RDW_NOERASE);
        g_splitLastX = sx;
    }
    /* 横分隔条同理：只重刷它挪过的那一横条 */
    sy = hsplitter_y(H);
    if (g_hsplitLastY != sy) {
        RECT strip;
        strip.left   = rx;
        strip.right  = rx + rw;
        strip.top    = (g_hsplitLastY < 0 ? sy : (g_hsplitLastY < sy ? g_hsplitLastY : sy)) - DPIX(2);
        strip.bottom = (g_hsplitLastY > sy ? g_hsplitLastY : sy) + QP_HSPLIT_HIT + DPIX(2);
        RedrawWindow(hwnd, &strip, NULL, RDW_INVALIDATE | RDW_NOERASE);
        g_hsplitLastY = sy;
    }
    /* 连同上面 EndDeferWindowPos 让出来的空档，攒成一次 WM_PAINT 立刻画完。
       不立刻画的话，下一个 WM_MOUSEMOVE 会插在中间，一次拖动就分几趟落地。 */
    /* 完成所有嵌套布局后才绘制；保留原生控件需要的擦除标志。
       WS_CLIPCHILDREN 下必须显式包含子控件，任务栏还原也走这一条路径。 */
    RedrawWindow(hwnd, NULL, NULL, RDW_UPDATENOW | RDW_ALLCHILDREN);
}
