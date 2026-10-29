#include "ui_internal.h"
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

NodeRef *tree_ref(HTREEITEM item)
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
    sess_prune();          /* 模板被删/被整体换掉后，扔掉无主的那几份填写 */
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
Template *tree_selected_template(void)
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

void ui_new_template(void)
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
        ui_select_template(NULL);       /* 断开旧选择，让树重建后重新选中新模板 */
        ui_rebuild_tree(t->id);
    } else {
        tmpl_free(t);
    }
}

/* 插入一份复杂示例模板。示例数据只在首次运行时生成，老用户升级上来看不到，
   所以从「模板」菜单里也给一个入口。 */
void ui_insert_demo_template(void)
{
    Template *t;
    Group *g = tree_selected_group();
    if (!g_app) return;
    if (qp_msgbox(g_hwndMain, MB_YESNO | MB_ICONINFORMATION, L"插入示例模板",
                  L"将新建一份「复杂示例 · 违规处置通报」。\n\n"
                  L"该模板覆盖隐藏变量、多行文本、按分隔符取段、按关键词匹配与"
                  L"多条件项规则，可参照它体验各项功能。\n\n是否现在插入？")
            != IDYES)
        return;
    t = tmpl_create_demo();
    if (!t) return;
    if (g) qp_strlcpy(t->group_id, g->id, QP_ID_MAX);
    tmpl_list_append(&g_app->templates, t);
    app_save_data();
    ui_select_template(NULL);
    ui_rebuild_tree(t->id);
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
    /* The modal editor disables its owner: current input cannot change while
       editing. Save it before accepting a candidate, so deactivation is a
       no-allocation equality path even under later memory pressure. */
    if (!sess_capture_current()) {
        qp_msgbox(g_hwndMain,MB_OK|MB_ICONERROR,QP_APP_TITLE,
                  L"无法保存当前填写状态，编辑器未打开。请关闭不需要的窗口后重试。");
        return;
    }
    work = tmpl_clone(cur);
    if (!work) return;
    if (dlg_edit_template(g_hwndMain, work, 0)) {
        wchar_t id[QP_ID_MAX];
        qp_now_iso(work->updated_at, QP_TIME_MAX);
        qp_strlcpy(id, work->id, QP_ID_MAX);
        /* 替换会释放旧对象，先切断引用 */
        if (!ui_select_template(NULL)) { tmpl_free(work); return; }
        tmpl_list_replace(&g_app->templates, work);
        app_save_data();
        ui_rebuild_tree(id);
    } else {
        tmpl_free(work);
    }
}

void ui_duplicate_template(void)
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

void ui_delete_template(void)
{
    Template *cur = tree_selected_template();
    if (!cur) return;
    if (qp_msgbox(g_hwndMain, MB_YESNO | MB_ICONWARNING, L"删除模板",
                  L"确定要删除模板「%ls」吗？\n\n此操作不可撤销。", cur->name) != IDYES)
        return;
    if (g_curTmpl == cur && !ui_select_template(NULL)) return;
    tmpl_list_remove(&g_app->templates, cur->id);
    app_save_data();
    ui_rebuild_tree(NULL);
}

void ui_new_group(void)
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

void ui_rename_group(void)
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

void ui_delete_group(void)
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
void ui_move_template_to_group(const wchar_t *tmpl_id, const wchar_t *group_id)
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

void ui_export(void)
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
                  L"已导出 %d 个模板、%d 个分组及 %d 张数据表快照到：\n%ls",
                  tmpl_list_count(g_app->templates), group_list_count(g_app->groups),
                  dtref_list_count(g_app->tables), path);
    else
        qp_msgbox(g_hwndMain, MB_OK | MB_ICONERROR, L"导出失败",
                  L"无法导出。请检查数据表文件是否可读取，以及导出位置是否可写入：\n%ls", path);
}

void ui_import(void)
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
                  L"「是」：合并模板与分组；相同 ID 的记录使用文件内容替换\n"
                  L"「否」：替换全部模板与分组；数据表引用仍按 ID 合并\n"
                  L"「取消」：放弃导入");
    if (r == IDCANCEL) return;
    mode = (r == IDYES) ? QP_IMPORT_MERGE : QP_IMPORT_REPLACE;

    if (mode == QP_IMPORT_REPLACE &&
        qp_msgbox(g_hwndMain, MB_YESNO | MB_ICONWARNING, L"确认覆盖",
                  L"覆盖导入会删除当前全部 %d 个模板，确定继续？",
                  tmpl_list_count(g_app->templates)) != IDYES)
        return;

    if (!ui_select_template(NULL)) return;
    if (storage_import(g_app, path, mode, &added, &updated)) {
        qp_table_set_refs(g_app->tables);
        qp_table_invalidate_all();
        int saved = app_save_data();
        ui_rebuild_tree(NULL);
        if (saved) qp_msgbox(g_hwndMain, MB_OK | MB_ICONINFORMATION, L"导入完成",
                  L"新增 %d 个模板，更新 %d 个模板。", added, updated);
    } else {
        ui_rebuild_tree(NULL);
        qp_msgbox(g_hwndMain, MB_OK | MB_ICONERROR, L"导入失败",
                  L"文件无法读取或不是有效的 QuickPhrase JSON：\n%ls", path);
    }
}
