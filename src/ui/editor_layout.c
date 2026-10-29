#include "editor_internal.h"

/* 同一条底边供布局、高度夹取和拖动反算使用。 */
static int ed_preview_bottom(int H)
{
    return H - DPIX(12) - DPIX(26) - DPIX(8);
}

int ed_splitter_base(int H)
{
    return ed_preview_bottom(H) - DPIX(18) - ED_SPLIT_HIT;
}

/* 默认值预览的高度区间：上限按「上半部分至少留 DPIX(180)」反推 */
void ed_preview_bounds(int H,int *lo,int *hi)
{
    qp_editor_preview_bounds(g_dpi,H,lo,hi);
}

/* 0 = 用设计高度。与 ui_preview_height_for 同一纪律：只读夹取，不回写设置 */
static int ed_preview_height_for(int H)
{
    return qp_editor_preview_height(g_dpi,H,g_settings.editor_preview_height);
}

/* 预览分隔条的上沿；也就是上半部分（片段列表 / 属性区）的底线 */
int ed_splitter_y(int H)
{
    return ed_splitter_base(H) - ed_preview_height_for(H);
}

/* 片段列表宽度 / 属性区左沿；纵向底线由 ed_layout 统一计算。 */
/* 命中判定：预览分隔条横贯整个宽度 */
int ed_split_hit(HWND hwnd, int x, int y)
{
    RECT rc;
    int top;
    GetClientRect(hwnd, &rc);
    top = ed_splitter_y(rc.bottom - rc.top);
    return x >= DPIX(12) && x < rc.right - DPIX(12) &&
           y >= top && y < top + ED_SPLIT_HIT;
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
    if (msg == WM_MOUSEWHEEL) {
        wchar_t cls[32];
        if (GetClassNameW(hwnd, cls, 32) && lstrcmpiW(cls, L"ComboBox") == 0)
            return SendMessageW(c->hPropsHost, msg, wp, lp);
    }
    if(msg==WM_SETFOCUS && IsChild(c->hPropsHost, hwnd) && c->narrow && c->activePage!=1) {
        c->activePage=1;
        ed_layout(c);
    }
    LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
    if (msg == WM_SETFOCUS && IsChild(c->hPropsHost, hwnd)) {
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

BOOL CALLBACK ed_prop_hook_focus(HWND hwnd, LPARAM lp)
{
    SetWindowSubclass(hwnd, ed_prop_focus_proc, 0, (DWORD_PTR)lp);
    return TRUE;
}

/* 与主窗口字段 host 一样：裁剪子控件、处理滚动，通知和主题交回编辑器。 */
LRESULT CALLBACK ed_props_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    EditorCtx *c = (EditorCtx *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                         (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
        return 0;
    case WM_ERASEBKGND: qp_draw_background(hwnd, (HDC)wp); return 1;
    case WM_PRINTCLIENT: qp_draw_background(hwnd, (HDC)wp); return 0;
    case WM_PAINT: qp_paint_background(hwnd); return 0;
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

/* State changes control visibility; resizing only changes geometry. */
static void ed_sync_props_visibility(EditorCtx *c)
{
    Segment *s=ed_cur_seg(c);
    int has=s!=NULL, t=has?(int)s->type:-1;
    int variable=has && t!=SEG_STATIC;
    int refmode=variable && s->ref_segment_id[0];
    int refparam=refmode && s->ref_mode!=REF_WHOLE;
    int key=1+(has?2:0)+(t+1)*8+refmode*64+refparam*128;
    if(c->propsVisibilityKey==key) return;
    c->propsVisibilityKey=key;
    struct { HWND h; int visible; } controls[]={
        {c->hPropTitle,has},{c->hTableOutput,1},{c->hTableFmtLbl,1},{c->hTableFmt,1},
        {c->hTableInsert,1},{c->hTableInsertBtn,1},{c->hTableCellBtn,1},{c->hPreviewText,1},{c->hPreviewTable,1},
        {c->hTypeLbl,has},{c->hType,has},{c->hLabelLbl,variable},{c->hLabel,variable},
        {c->hContentLbl,has && t==SEG_STATIC},{c->hContent,has && t==SEG_STATIC},
        {c->hHint,has && t==SEG_STATIC},
        {c->hPlaceLbl,has && (t==SEG_TEXT || t==SEG_MULTI_SELECT)},
        {c->hPlace,has && (t==SEG_TEXT || t==SEG_MULTI_SELECT)},
        {c->hDefLbl,variable && t!=SEG_DATETIME},{c->hDef,variable && t!=SEG_DATETIME},
        {c->hSepLbl,has && t==SEG_MULTI_SELECT},{c->hSep,has && t==SEG_MULTI_SELECT},
        {c->hFmtLbl,has && t==SEG_DATETIME},{c->hFmt,has && t==SEG_DATETIME},
        {c->hFmtSample,has && t==SEG_DATETIME},
        {c->hHidden,variable},{c->hNoOutput,variable},{c->hMultiline,has && t==SEG_TEXT},
        {c->hRefLbl,variable},{c->hRef,variable},{c->hRefModeLbl,refmode},{c->hRefMode,refmode},
        {c->hRefA,refparam},{c->hRefB,refparam}
    };
    for(size_t i=0;i<sizeof(controls)/sizeof(controls[0]);++i)
        qp_show_if_needed(controls[i].h,controls[i].visible);
    HWND lists[3][7]={
        {c->hOptLbl,c->hOptList,c->hOptAdd,c->hOptEdit,c->hOptDel,c->hOptUp,c->hOptDown},
        {c->hParseLbl,c->hParseList,c->hParseAdd,c->hParseEdit,c->hParseDel,c->hParseUp,c->hParseDown},
        {c->hCondLbl,c->hCondList,c->hCondAdd,c->hCondEdit,c->hCondDel,c->hCondUp,c->hCondDown}
    };
    int visible[3]={has && (t==SEG_SELECT || t==SEG_MULTI_SELECT),has && t==SEG_TEXT,has};
    for(int i=0;i<3;++i) for(int k=0;k<7;++k) qp_show_if_needed(lists[i][k],visible[i]);
}

/* host 已被 ed_layout 限定到 [72, bodybot)，这里使用 host 的局部坐标。 */
static int ed_layout_props_pass(EditorCtx *c, int bodybot)
{
    Segment *s = ed_cur_seg(c);
    RECT rc;
    HDWP dwp;
    int x = 0, lw = DPIX(80), viewh = 0;
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
    viewh = rc.bottom;
    cx = x + lw + DPIX(8);
    cw = right - cx;
    if (cw < DPIX(120)) cw = DPIX(120);
    dwp = qp_layout_begin(48);

#define ED_ROW(lbl, ctl, show, hgt)                                            \
    do {                                                                       \
        if (show) {                                                            \
            dwp = qp_defer(dwp, (lbl), x, y + DPIX(4), lw, DPIX(18));            \
            dwp = qp_defer(dwp, (ctl), cx, y, cw, DPIX(hgt));                    \
            y += DPIX(hgt) + DPIX(4);                                          \
        }                                                                      \
    } while (0)

    dwp = qp_defer(dwp, c->hPropTitle, x, -c->propsScroll, right, DPIX(18));

    /* 模板级表格格式也放进这个有裁剪/滚动边界的 host：预览拖高时不会越过分隔条。 */
    dwp = qp_defer(dwp, c->hTableOutput, x, y, right - x, DPIX(22));
    y += DPIX(22) + DPIX(4);
    ED_ROW(c->hTableFmtLbl, c->hTableFmt, 1, 72);
    dwp = qp_defer(dwp, c->hTableInsert, cx, y, cw - DPIX(62), DPIX(24));
    dwp = qp_defer(dwp, c->hTableInsertBtn, right - DPIX(56), y, DPIX(56), DPIX(24));
    y += DPIX(24) + DPIX(4);
    dwp = qp_defer(dwp, c->hTableCellBtn, cx, y, cw, DPIX(24));
    y += DPIX(24) + DPIX(4);
    dwp = qp_defer(dwp, c->hPreviewText, cx, y, DPIX(92), DPIX(22));
    dwp = qp_defer(dwp, c->hPreviewTable, cx + DPIX(98), y, DPIX(92), DPIX(22));
    y += DPIX(22) + DPIX(8);

    ED_ROW(c->hTypeLbl, c->hType, has, 24);
    ED_ROW(c->hLabelLbl, c->hLabel, show_label, 24);
    ED_ROW(c->hContentLbl, c->hContent, show_content, 80);
    /* 占位符用法就一行提示，跟在固定内容下方：固定片段的属性本来就少，这一行
       不挤占别人的位置，也免得用户去文档里找「怎么在一句话里插变量」 */
    if (show_content) {
        int h=qp_wrapped_text_height(c->hHint,cw);
        dwp = qp_defer(dwp, c->hHint, cx, y, cw, h);
        y += h + DPIX(4);
    }
    ED_ROW(c->hPlaceLbl, c->hPlace, show_place, 24);
    ED_ROW(c->hDefLbl, c->hDef, show_def, 24);
    ED_ROW(c->hSepLbl, c->hSep, show_sep, 24);
    ED_ROW(c->hFmtLbl, c->hFmt, show_fmt, 24);
    /* 格式的实时效果：跟在格式框下方，与它左边对齐，改一个字就能看到结果 */
    if (show_fmt) {
        int h=qp_wrapped_text_height(c->hFmtSample,cw);
        dwp = qp_defer(dwp, c->hFmtSample, cx, y, cw, h);
        y += h + DPIX(4);
    }

    /* 「隐藏」开关自带文字，不需要左侧标签，与其他控件左对齐即可 */
    if (show_hidden) {
        int h=qp_wrapped_text_height(c->hHidden,cw-DPIX(22))+DPIX(4);
        if(h<DPIX(22)) h=DPIX(22);
        dwp = qp_defer(dwp, c->hHidden, cx, y, cw, h);
        y += h + DPIX(6);
    }
    /* 「不计入输出」与「隐藏」正交：隐藏只是填写界面不显示、照常拼进输出；
       不计入输出则照常参与匹配 / 引用 / 占位符，但不出现在最终口径里。
       对所有非固定变量都可用（拼表格行时源句子与列变量都用它） */
    if (show_hidden) {
        int h=qp_wrapped_text_height(c->hNoOutput,cw-DPIX(22))+DPIX(4);
        if(h<DPIX(22)) h=DPIX(22);
        dwp = qp_defer(dwp, c->hNoOutput, cx, y, cw, h);
        y += h + DPIX(6);
    }
    if (show_multi) {
        int h=qp_wrapped_text_height(c->hMultiline,cw-DPIX(22))+DPIX(4);
        if(h<DPIX(22)) h=DPIX(22);
        dwp = qp_defer(dwp, c->hMultiline, cx, y, cw, h);
        y += h + DPIX(6);
    }

    ED_ROW(c->hRefLbl, c->hRef, show_ref, 24);
    /* 「取法」一行：下拉占一半，右边两个小框用占位文字当标签（分隔符 / 第几段，
       或 从第几个字 / 取几个），按取法显隐 —— 不引用时整行都不出现 */
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
        int bw = DPIX(46), sw = DPIX(28), bh = DPIX(22), bx, lh;                \
        if (show) {                                                            \
            bx = right - (bw * 3 + sw * 2 + DPIX(8));                          \
            lh = qp_wrapped_text_height((lbl), right - x);                     \
            dwp = qp_defer(dwp, (lbl), x, y, right - x, lh);                     \
            y += lh + DPIX(4);                                                 \
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

    qp_layout_end(dwp);

    contenth = y + c->propsScroll;
    if (c->propsScroll > contenth - viewh) c->propsScroll = contenth - viewh;
    if (c->propsScroll < 0) c->propsScroll = 0;
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    if(c->propsForceScroll) si.fMask |= SIF_DISABLENOSCROLL;
    si.nMax = contenth > 0 ? contenth - 1 : 0;
    si.nPage = (UINT)viewh;
    si.nPos = c->propsScroll;
    qp_set_scroll_info_if_needed(c->hPropsHost, SB_VERT, &si);
    GetClientRect(c->hPropsHost, &rc);
    /* 文字高度按实际视口计算；滚动条显隐改变宽度时补排到稳定视口。 */
    if (right != rc.right || oldScroll != c->propsScroll) {
        return 1;
    }

    /* 列表列宽随窗口 */
    qp_set_column_width_if_needed(c->hOptList, 0, right - x - DPIX(26));
    qp_set_column_width_if_needed(c->hParseList, 0, right - x - DPIX(26));
    qp_set_column_width_if_needed(c->hCondList, 0, right - x - DPIX(26));
    return 0;
}


static void ed_layout_props(EditorCtx *c,int bottom)
{
    c->propsForceScroll=0;
    for(int pass=0;pass<3;++pass) if(!ed_layout_props_pass(c,bottom)) return;
    c->propsForceScroll=1;
    ShowScrollBar(c->hPropsHost,SB_VERT,TRUE);
    (void)ed_layout_props_pass(c,bottom);
}


static HDWP ed_place(HDWP batch,HWND h,QpRect r)
{
    return qp_defer(batch,h,r.x,r.y,r.w,r.h);
}

void ed_layout(EditorCtx *c)
{
    RECT client;
    GetClientRect(c->hwnd,&client);
    if(client.right<=0 || client.bottom<=0) return;
    QpEditorLayoutInput in={g_dpi,client.right,client.bottom,g_settings.editor_preview_height,c->activePage};
    QpEditorLayout out;
    qp_editor_layout(&in,&out);
    c->narrow=out.narrow;
    ed_sync_props_visibility(c);
    int left=!out.narrow || c->activePage==0, right=!out.narrow || c->activePage==1;
    HWND left_controls[]={c->hSegLbl,c->hAddStatic,c->hAddText,c->hAddSelect,c->hAddMulti,
        c->hAddTime,c->hSegDel,c->hSegUp,c->hSegDown,c->hSegList};
    for(size_t i=0;i<sizeof(left_controls)/sizeof(*left_controls);++i)qp_show_if_needed(left_controls[i],left);
    qp_show_if_needed(c->hPropsHost,right);
    qp_show_if_needed(c->hPageSegments,out.narrow);qp_show_if_needed(c->hPageProps,out.narrow);
    SendMessageW(c->hPageSegments,BM_SETCHECK,c->activePage==0?BST_CHECKED:BST_UNCHECKED,0);
    SendMessageW(c->hPageProps,BM_SETCHECK,c->activePage==1?BST_CHECKED:BST_UNCHECKED,0);
    HDWP batch=qp_layout_begin(24);
    batch=ed_place(batch,GetDlgItem(c->hwnd,IDC_ED_NAME+1000),out.name_label);
    batch=ed_place(batch,c->hName,out.name);
    batch=ed_place(batch,GetDlgItem(c->hwnd,IDC_ED_GROUP+1000),out.group_label);
    batch=ed_place(batch,c->hGroup,out.group);
    batch=ed_place(batch,c->hBatch,out.batch);batch=ed_place(batch,c->hBatchSrcLbl,out.batch_label);
    batch=ed_place(batch,c->hBatchSrc,out.batch_source);
    batch=ed_place(batch,c->hPageSegments,out.page_tabs[0]);batch=ed_place(batch,c->hPageProps,out.page_tabs[1]);
    batch=ed_place(batch,c->hSegLbl,out.segment_label);
    HWND tools[]={c->hAddStatic,c->hAddText,c->hAddSelect,c->hAddMulti,c->hAddTime,c->hSegDel,c->hSegUp,c->hSegDown};
    for(int i=0;i<8;++i)batch=ed_place(batch,tools[i],out.toolbar[i]);
    batch=ed_place(batch,c->hSegList,out.segment);batch=ed_place(batch,c->hPropsHost,out.properties);
    batch=ed_place(batch,c->hPreviewLbl,out.preview_label);batch=ed_place(batch,c->hPreview,out.preview);
    batch=ed_place(batch,GetDlgItem(c->hwnd,IDOK),out.ok);batch=ed_place(batch,GetDlgItem(c->hwnd,IDCANCEL),out.cancel);
    qp_layout_end(batch);
    qp_set_column_width_if_needed(c->hSegList,2,out.segment.w-DPIX(116));
    ed_layout_props(c,out.body_bottom);
    if(c->splitLastY!=out.body_bottom){
        RECT band={DPIX(12),out.body_bottom,client.right-DPIX(12),out.body_bottom+ED_SPLIT_HIT};
        if(c->splitLastY>=0){
            if(c->splitLastY<band.top)band.top=c->splitLastY;
            if(c->splitLastY+ED_SPLIT_HIT>band.bottom)band.bottom=c->splitLastY+ED_SPLIT_HIT;
        }
        c->splitLastY=out.body_bottom;
        RedrawWindow(c->hwnd,&band,NULL,RDW_INVALIDATE|RDW_NOERASE);
    }
    /* 属性区与预览均已摆位，一次完成待处理绘制，避免分隔线先于内容显示。 */
    RedrawWindow(c->hwnd,NULL,NULL,RDW_UPDATENOW|RDW_ALLCHILDREN);
}

void ed_restore_size(HWND hwnd,HWND owner)
{
    if(!hwnd)return;
    RECT frame={0,0,g_settings.editor_w>0?g_settings.editor_w:DPIX(ED_MIN_W),
                    g_settings.editor_h>0?g_settings.editor_h:DPIX(ED_MIN_H)},work;
    DWORD style=(DWORD)GetWindowLongPtrW(hwnd,GWL_STYLE),ex=(DWORD)GetWindowLongPtrW(hwnd,GWL_EXSTYLE);
    AdjustWindowRectEx(&frame,style,FALSE,ex);
    qp_window_work_area(owner?owner:hwnd,&work);
    int width=frame.right-frame.left,height=frame.bottom-frame.top;
    if(width>work.right-work.left)width=work.right-work.left;
    if(height>work.bottom-work.top)height=work.bottom-work.top;
    SetWindowPos(hwnd,NULL,0,0,width,height,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    qp_center_window(hwnd,owner);
}
