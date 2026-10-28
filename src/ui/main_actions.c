#include "ui_internal.h"
/* ================================================================== */
/* 复制 / 重置                                                         */
/* ================================================================== */

void ui_copy_current(void)
{
    wchar_t out[QP_OUTPUT_MAX];
    int marks[256];
    if (!g_ctx) return;
    ui_sync_preview_mode_controls();
    QpRenderResult result=ui_render_current_checked(out,QP_OUTPUT_MAX,marks,256);
    if(result.status!=QP_RENDER_OK) {
        EnableWindow(g_hBtnCopy,FALSE);
        qp_set_text_if_diff(g_hHint,qp_render_status_hint(result.status));
        return;
    }
    if(!ui_preview_show_result(out,marks,result.mark_count)) {
        EnableWindow(g_hBtnCopy,FALSE);
        qp_set_text_if_diff(g_hHint,qp_render_status_hint(QP_RENDER_NOMEM));
        return;
    }
    EnableWindow(g_hBtnCopy,TRUE);
    if (!out[0]) {
        SetWindowTextW(g_hHint, g_previewMode == OUTPUT_TABLE
                       ? L"当前表格为空，没有可复制的内容"
                       : L"当前文本为空，没有可复制的内容");
        return;
    }
    if (qp_copy_to_clipboard(g_hwndMain, out)) {
        SetWindowTextW(g_hBtnCopy, g_previewMode == OUTPUT_TABLE
                       ? L"✓ 已复制表格" : L"✓ 已复制文本");
        SetWindowTextW(g_hHint, g_previewMode == OUTPUT_TABLE
                       ? L"表格已复制到剪贴板，可直接粘贴到 Excel"
                       : L"文本已复制到剪贴板");
        SetTimer(g_hwndMain, TIMER_COPY_FEEDBACK, 1200, NULL);
    } else {
        SetWindowTextW(g_hHint, L"复制失败。请稍后重试，并检查剪贴板是否可用。");
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
    if (g_hTabs) InvalidateRect(g_hTabs, NULL, TRUE);   /* 标签条也是自绘的 */
    if (g_hPreview) {                                   /* 预览自绘，配色重取 */
        dark_apply_window(g_hPreview);
        qp_preview_theme_changed(g_hPreview);
    }
    if (s_flushMenuThemes) s_flushMenuThemes();
}

/* ================================================================== */
/* 菜单                                                                */
/* ================================================================== */

void ui_build_menu(HWND hwnd)
{
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    HMENU tpl = CreatePopupMenu();
    HMENU data = CreatePopupMenu();
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
    AppendMenuW(tpl, MF_SEPARATOR, 0, NULL);
    AppendMenuW(tpl, MF_STRING, IDM_SESS_NEW, L"新开一份填写(&P)\tCtrl+T");
    AppendMenuW(tpl, MF_STRING, IDM_SESS_NEXT, L"切到下一份(&K)\tCtrl+Tab");
    AppendMenuW(tpl, MF_STRING, IDM_SESS_CLOSE, L"关闭当前这一份(&W)\tCtrl+W");
    AppendMenuW(tpl, MF_SEPARATOR, 0, NULL);
    AppendMenuW(tpl, MF_STRING, IDM_TPL_DEMO, L"插入示例模板(&X)…");

    AppendMenuW(data, MF_STRING, IDM_TABLES_MANAGE, L"管理数据表（名单 / 联系表）(&M)…");

    AppendMenuW(set, MF_STRING, IDM_SETTINGS, L"选项(&O)…");

    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"关于(&A)");

    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"文件(&F)");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)tpl, L"模板(&T)");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)data, L"数据表(&D)");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)set, L"设置(&S)");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"帮助(&H)");
    SetMenu(hwnd, bar);
}

void ui_tree_context_menu(int x, int y)
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
