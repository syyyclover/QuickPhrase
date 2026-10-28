#include "ui_internal.h"
/* ================================================================== */
/* 主窗口过程                                                          */
/* ================================================================== */

/* Keep a restored surface out of the desktop until every child has painted.
   Windows erases the newly exposed surface before WM_SIZE/ShowWindow return;
   repainting at those later points alone can expose black child rectangles.
   Cloaking keeps composition active, unlike hiding or disabling redraw. */
static int restore_cloaked;
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

    if (!g_ctx || g_syncFields || !g_fields || slot < 0 || slot >= g_fieldCount) return;
    f = &g_fields[slot];
    if (f->field_index < 0 || f->field_index >= g_ctx->count) return;

    if (which == 1) {                       /* ▼ 按钮 / 时间的「现在」 */
        const Segment *seg = g_ctx->fields[f->field_index].seg;
        if (code != BN_CLICKED) return;
        if (seg && seg->type == SEG_DATETIME) {
            /* 重新拍一张此刻的快照。仍走「手动填写」那一层，因此后面不会被
               默认值/上次匹配值顶掉，也不会自己悄悄跳动。 */
            wchar_t now[QP_VALUE_MAX];
            qp_format_time_now(seg->time_format, now, QP_VALUE_MAX);
            eval_set_user_value(g_ctx, f->field_index, now);
            ui_refresh_current();
            return;
        }
        popup_open(f->field_index, f->hCtl);
        return;
    }
    if (which != 0) return;

    if (code == EN_CHANGE) {
        wchar_t buf[QP_VALUE_MAX];
        const Segment *seg = g_ctx->fields[f->field_index].seg;
        if (g_syncFields) return;           /* 这一下是同步写进去的，不是人敲的 */
        /* 多选的只读框由弹层驱动，不在这里回读 */
        if (!seg || (seg->type != SEG_TEXT && seg->type != SEG_DATETIME)) return;
        qp_get_text(f->hCtl, buf, QP_VALUE_MAX);
        eval_set_user_value(g_ctx, f->field_index, buf);
        ui_refresh_current();
    } else if (code == CBN_SELCHANGE || code == CBN_SELENDOK) {
        wchar_t buf[QP_VALUE_MAX];
        int idx = (int)SendMessageW(f->hCtl, CB_GETCURSEL, 0, 0);
        if (idx == CB_ERR) return;
        if(idx==qp_choice_custom_index(f->field_index>=0?g_ctx->fields[f->field_index].seg:NULL) ||
           (g_ctx->fields[f->field_index].custom_selected && idx==f->extraIndex)) {
            if(code==CBN_SELENDOK)ui_choice_single_queue(f->field_index,f->hCtl);
            return;
        }
        if(code==CBN_SELENDOK)return;
        buf[0] = L'\0';
        SendMessageW(f->hCtl, CB_GETLBTEXT, (WPARAM)idx, (LPARAM)buf);
        eval_set_user_value(g_ctx, f->field_index, buf);
        g_ctx->fields[f->field_index].custom_selected=0;
        if(g_ctx->fields[f->field_index].choice_tracked)
            qp_strlcpy(g_ctx->fields[f->field_index].choice_base,buf,QP_VALUE_MAX);
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

/* Ordinary paint and system print requests share the complete client renderer.
   Printing uses the supplied DC and never validates the pending paint region. */
void qp_draw_window_frame(HWND hwnd,HDC hdc,const RECT *area,int editorY)
{
    if(!hdc)return;
    RECT rc, bar, clip = *area;
    HDC dc = hdc, mem = NULL;
    HBITMAP bmp = NULL, oldBmp = NULL;
    HBRUSH b;
    int cw = clip.right - clip.left, ch = clip.bottom - clip.top;
    int one = DPIX(1) > 1 ? DPIX(1) : 1;
    GetClientRect(hwnd, &rc);
    /* 背景与分隔条在离屏位图上一趟画完再整块贴上。
       以前是 WM_ERASEBKGND 先把背景涂一遍、WM_PAINT 再把线画上去，屏幕上
       就是「条子被涂掉 → 条子又出现」两步；拖动时每格都来一次，看起来
       就是边框在抽搐。所以擦除趟和绘制趟现在都调这一个函数（见 WM_ERASEBKGND
       里的说明）：两趟像素完全相同，中间态不会露到屏幕上，也不会先黑一帧。 */
    if (cw > 0 && ch > 0 && (mem = CreateCompatibleDC(hdc)) != NULL) {
        bmp = CreateCompatibleBitmap(hdc, cw, ch);
        if (bmp) {
            oldBmp = (HBITMAP)SelectObject(mem, bmp);
            /* 把离屏位图的坐标原点对到客户区坐标，下面的计算不用改 */
            SetViewportOrgEx(mem, -clip.left, -clip.top, NULL);
            dc = mem;
        } else {
            DeleteDC(mem);
            mem = NULL;
        }
    }
    FillRect(dc, &clip, g_brushBk);
    b = CreateSolidBrush(theme_line());
    /* 画出分隔条本体：以前这里什么都不画，用户只能靠光标形状猜哪里
       可以拖，拖起来自然没有「抓住了」的感觉。 */
    if (b) {
        if (editorY >= 0) {
            bar.left = DPIX(12); bar.right = rc.right - DPIX(12);
            bar.top = editorY; bar.bottom = editorY + one;
            if (bar.right > bar.left) FillRect(dc, &bar, b);
        } else {
        bar.left   = splitter_x() + QP_SPLIT_HIT / 2;
        bar.right  = bar.left + one;
        bar.top    = DPIX(36);
        bar.bottom = rc.bottom - DPIX(10);
        if (bar.bottom > bar.top) FillRect(dc, &bar, b);
        /* 右栏的横分隔条（字段区 ↔ 输出预览） */
        bar.left   = splitter_x() + QP_SPLIT_HIT + DPIX(1);
        bar.right  = rc.right - DPIX(8);
        bar.top    = hsplitter_y(rc.bottom - rc.top) + QP_HSPLIT_HIT / 2;
        bar.bottom = bar.top + one;
        if (bar.right > bar.left) FillRect(dc, &bar, b);
        }
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

static void main_draw_client(HWND hwnd,HDC hdc,const RECT *area)
{
    qp_draw_window_frame(hwnd,hdc,area,-1);
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
    case WM_QUERYOPEN:
        /* Sent before the iconic flag is cleared, including shell/taskbar and
           direct ShowWindow restoration. DefWindowProc normally returns TRUE. */
        qp_prepare_present(hwnd, &restore_cloaked);
        return TRUE;

    case WM_SHOWWINDOW:
        if (wp && !IsWindowVisible(hwnd)) qp_prepare_present(hwnd, &restore_cloaked);
        break;

    case WM_WINDOWPOSCHANGED: {
        WINDOWPOS *pos = (WINDOWPOS *)lp;
        /* Let the default procedure deliver WM_SIZE/WM_MOVE and nested layout
           first. Unrelated z-order changes must not publish an unfinished frame. */
        LRESULT result = DefWindowProcW(hwnd, msg, wp, lp);
        if (restore_cloaked && !IsIconic(hwnd) && IsWindowVisible(hwnd) &&
            ((pos->flags & SWP_SHOWWINDOW) || !(pos->flags & SWP_NOSIZE)))
            qp_finish_present(hwnd, &restore_cloaked);
        return result;
    }

    case WM_QP_CHOICE_INPUT:
        ui_choice_dispatch();
        return 0;
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

        /* 并行填写的标签条：没选模板时隐藏，选中后由 ui_layout 让出位置 */
        g_hTabs = qp_create(CLS_TABS, L"", 0, 0, 0, 0, 10, 10, hwnd, IDC_TABS);
        ShowWindow(g_hTabs, SW_HIDE);
        g_tabsShown = 0;

        g_hPreviewLabel = qp_create(L"Static", L"输出预览", SS_LEFT, 0,
                                    0, 0, 10, 10, hwnd, IDC_PREVIEW_LABEL);
        g_hPreviewTextMode = qp_create(L"Button", L"文本", BS_AUTORADIOBUTTON |
                                       WS_GROUP | WS_TABSTOP, 0,
                                       0, 0, 10, 10, hwnd, IDC_PREVIEW_MODE_TEXT);
        g_hPreviewTableMode = qp_create(L"Button", L"表格", BS_AUTORADIOBUTTON |
                                        WS_TABSTOP, 0,
                                        0, 0, 10, 10, hwnd, IDC_PREVIEW_MODE_TABLE);
        /* 输出预览：文本 / 表格共用一个自绘控件（见 preview.c），
           失败处红字标出、表格按列对齐 */
        g_hPreview = qp_preview_create(hwnd, IDC_PREVIEW, WS_EX_CLIENTEDGE);
        dark_apply_window(g_hPreview);

        g_hBtnCopy = qp_create(L"Button", L"一键复制文本", BS_DEFPUSHBUTTON | WS_TABSTOP, 0,
                               0, 0, 10, 10, hwnd, IDC_BTN_COPY);
        g_hBtnReset = qp_create(L"Button", L"重置", BS_PUSHBUTTON | WS_TABSTOP, 0,
                                0, 0, 10, 10, hwnd, IDC_BTN_RESET);
        g_hBtnEdit = qp_create(L"Button", L"编辑模板", BS_PUSHBUTTON | WS_TABSTOP, 0,
                               0, 0, 10, 10, hwnd, IDC_BTN_EDIT);
        g_hHint = qp_create(L"Static", L"", SS_LEFT | SS_ENDELLIPSIS, 0,
                            0, 0, 10, 10, hwnd, IDC_HINT);
        return 0;
    }

    case WM_SIZE: {
        static WPARAM previous = SIZE_RESTORED;
        int transition = previous != wp;
        previous = wp;
        popup_close();          /* 尺寸一变，弹层锚点就失位了 */
        if (wp == SIZE_MINIMIZED) return 0; /* 保留子控件的有效尺寸和绘制内容 */
        ui_layout(hwnd);
        /* Restoring can discard the backing surface without moving any child.
           Layout's damage-only flush cannot repaint an otherwise valid child. */
        if (transition)
            RedrawWindow(hwnd, NULL, NULL,
                         RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        if (wp != SIZE_MINIMIZED) {
            SetTimer(hwnd, TIMER_SAVE_PLACEMENT, 500, NULL);
        }
        return 0;
    }

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
        if (wp == QP_TIMER_PRESENT) {
            qp_finish_present(hwnd, &restore_cloaked);
        } else if (wp == TIMER_COPY_FEEDBACK) {
            KillTimer(hwnd, TIMER_COPY_FEEDBACK);
            ui_update_preview_mode_labels();
        } else if (wp == TIMER_SAVE_PLACEMENT) {
            KillTimer(hwnd, TIMER_SAVE_PLACEMENT);
            save_placement(hwnd);
            app_save_settings();
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc=BeginPaint(hwnd,&ps);
        main_draw_client(hwnd,dc,&ps.rcPaint);
        EndPaint(hwnd,&ps);
        return 0;
    }

    case WM_PRINTCLIENT: {
        RECT rc;
        GetClientRect(hwnd,&rc);
        main_draw_client(hwnd,(HDC)wp,&rc);
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
        if (hsplit_hit(pt.x, pt.y)) {
            SetCursor(LoadCursorW(NULL, IDC_SIZENS));
            return TRUE;
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        int sx = splitter_x();
        RECT rc;
        GetClientRect(hwnd, &rc);
        popup_close();
        if (x >= sx && x <= sx + QP_SPLIT_HIT && y > DPIX(30)) {
            g_splitDrag = 1;
            g_splitGrabDx = x - sx;   /* 记住抓取偏移，分隔条不再跳到光标下 */
            SetCapture(hwnd);
        } else if (hsplit_hit(x, y)) {
            g_hsplitDrag = 1;
            g_hsplitGrabDy = y - hsplitter_y(rc.bottom - rc.top);
            SetCapture(hwnd);
        }
        return 0;
    }

    /* 双击任一条分隔条 → 恢复默认（竖条回默认宽度，横条回自动高度）。
       两条分隔条的交互保持一致，不搞「一条能双击一条不能」。 */
    case WM_LBUTTONDBLCLK: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        int sx = splitter_x();
        if (x >= sx && x <= sx + QP_SPLIT_HIT && y > DPIX(30)) {
            g_settings.tree_width = QP_TREE_W_DEF;
        } else if (hsplit_hit(x, y)) {
            g_settings.preview_height = 0;
        } else {
            break;
        }
        ui_layout(hwnd);
        app_save_settings();
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
        } else if (g_hsplitDrag) {
            RECT rc;
            int y = (short)HIWORD(lp);
            int H, lo, hi, h;
            GetClientRect(hwnd, &rc);
            H = rc.bottom - rc.top;
            ui_preview_bounds(H, &lo, &hi);
            /* 往上拖 = 预览变高；公式与 hsplitter_y 互为反函数 */
            h = H - DPIX(8) - DPIX(28) - DPIX(6) - DPIX(18) - QP_HSPLIT_HIT
                  - (y - g_hsplitGrabDy);
            if (h < lo) h = lo;
            if (h > hi) h = hi;
            if (h != g_settings.preview_height) {
                g_settings.preview_height = h;
                ui_layout(hwnd);
            }
        }
        return 0;

    case WM_LBUTTONUP:
        if (g_dragging) { drag_drop(); return 0; }
        if (g_splitDrag || g_hsplitDrag) {
            g_splitDrag = 0;
            g_hsplitDrag = 0;
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
        g_hsplitDrag = 0;
        break;

    case WM_COMMAND: {
        WORD id = LOWORD(wp), code = HIWORD(wp);

        qp_combo_dropdown_fix(wp, lp);

        if (id >= IDC_FIELD_BASE && id < IDC_FIELD_BASE + 3000) {
            handle_field_command(id, code);
            return 0;
        }
        /* 标签条右键菜单的颜色项、以及 Ctrl+1~9 直接跳到第几份 */
        if (id >= IDM_SESS_COLOR_BASE && id < IDM_SESS_COLOR_BASE + QP_SESS_MAX) {
            sess_set_color(sess_target_vis(), id - IDM_SESS_COLOR_BASE);
            return 0;
        }
        if (id >= IDM_SESS_SLOT_BASE && id < IDM_SESS_SLOT_BASE + QP_SESS_MAX) {
            sess_switch_to(id - IDM_SESS_SLOT_BASE);
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
        case IDC_PREVIEW_MODE_TEXT:
            if (code == BN_CLICKED) {
                g_previewMode = OUTPUT_TEXT;
                ui_sync_preview_mode_controls();
                ui_refresh_current();
            }
            return 0;
        case IDC_PREVIEW_MODE_TABLE:
            if (code == BN_CLICKED) {
                g_previewMode = OUTPUT_TABLE;
                ui_sync_preview_mode_controls();
                ui_refresh_current();
            }
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
        case IDM_TPL_DEMO:     ui_insert_demo_template(); return 0;
        /* 并行填写：多份互不干扰的填写，标签条在填写区上方 */
        case IDM_SESS_NEW:     sess_new(); return 0;
        case IDM_SESS_CLOSE:   sess_close(sess_target_vis()); return 0;
        case IDM_SESS_RENAME:  sess_rename(sess_target_vis()); return 0;
        case IDM_SESS_DUP:     sess_duplicate(sess_target_vis()); return 0;
        case IDM_SESS_NEXT:    sess_cycle(1); return 0;
        case IDM_SESS_PREV:    sess_cycle(-1); return 0;
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
        case IDM_TABLES_MANAGE:
            /* 表清单变了就立刻存盘并让缓存作废：下次填写用新名单 */
            if (dlg_data_tables(hwnd, g_app)) {
                qp_table_set_refs(g_app->tables);
                qp_table_invalidate_all();
                app_save_data();
                ui_refresh_current();
            }
            return 0;
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
                          L"无法更改开机启动项。请检查注册表访问权限后重试。");
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
                Template *picked = r && r->kind == 1 ? tmpl_list_find(g_app->templates, r->id) : NULL;
                if (!ui_select_template(picked))
                    ui_rebuild_tree(g_curTmpl ? g_curTmpl->id : NULL);
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
        /* 擦除趟必须画出与 WM_PAINT 完全相同的一份像素：这里调的就是 WM_PAINT
           用的同一个合成函数（背景 + 分隔条一趟画完再整块贴上）。以前只回一句
           「已经擦过了」而不画，从任务栏还原／放大缩小时新露出来的那圈底会先黑
           一帧（等 WM_PAINT 到了才补上）；而若这里只涂背景、不画分隔条，分隔条
           又会先消失一帧，拖动时表现为边框抽搐。两头走同一份合成，两个毛病一起
           没有。子控件自带 WS_CLIPCHILDREN，不受影响。 */
        RECT rc;
        int kind = GetClipBox((HDC)wp, &rc);
        if (kind == NULLREGION) return 1;          /* 没有要擦的地方 */
        if (kind == ERROR) GetClientRect(hwnd, &rc);
        main_draw_client(hwnd, (HDC)wp, &rc);
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
        restore_cloaked = 0;
        ui_clear_fields();
        if (g_ctx) { eval_destroy(g_ctx); g_ctx = NULL; }
        g_curSess = NULL;
        sess_free_all();
        g_hTabs = NULL;
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
    static const struct { WNDPROC proc; const wchar_t *name; UINT style; } classes[] = {
        {main_proc, CLS_MAIN, CS_DBLCLKS},
        {host_proc, CLS_HOST, 0},
        {popup_proc, CLS_POPUP, 0},
        {tabs_proc, CLS_TABS, CS_DBLCLKS}
    };
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm = wc.hIcon;
    for (unsigned i = 0; i < sizeof(classes) / sizeof(classes[0]); ++i) {
        wc.lpfnWndProc = classes[i].proc;
        wc.lpszClassName = classes[i].name;
        wc.style = classes[i].style;
        if (!RegisterClassExW(&wc)) return 0;
        wc.hIcon = wc.hIconSm = NULL; /* Only the main window owns an icon. */
    }
    return qp_preview_register_class(g_hInst);
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
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
        if (!mon) {
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
        /* 并行填写：新开 / 关闭 / 前后切换 / 直接跳到第 N 份 */
        { FVIRTKEY | FCONTROL, 'T',       IDM_SESS_NEW },
        { FVIRTKEY | FCONTROL, 'W',       IDM_SESS_CLOSE },
        { FVIRTKEY | FCONTROL,           VK_TAB, IDM_SESS_NEXT },
        { FVIRTKEY | FCONTROL | FSHIFT,  VK_TAB, IDM_SESS_PREV },
        { FVIRTKEY | FCONTROL, '1',       IDM_SESS_SLOT_BASE + 0 },
        { FVIRTKEY | FCONTROL, '2',       IDM_SESS_SLOT_BASE + 1 },
        { FVIRTKEY | FCONTROL, '3',       IDM_SESS_SLOT_BASE + 2 },
        { FVIRTKEY | FCONTROL, '4',       IDM_SESS_SLOT_BASE + 3 },
        { FVIRTKEY | FCONTROL, '5',       IDM_SESS_SLOT_BASE + 4 },
        { FVIRTKEY | FCONTROL, '6',       IDM_SESS_SLOT_BASE + 5 },
        { FVIRTKEY | FCONTROL, '7',       IDM_SESS_SLOT_BASE + 6 },
        { FVIRTKEY | FCONTROL, '8',       IDM_SESS_SLOT_BASE + 7 },
        { FVIRTKEY | FCONTROL, '9',       IDM_SESS_SLOT_BASE + 8 },
        { FVIRTKEY,            VK_ESCAPE, IDM_HIDE_WINDOW },
    };
    return CreateAcceleratorTableW(acc, (int)(sizeof(acc) / sizeof(acc[0])));
}
