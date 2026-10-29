#include "dialogs_internal.h"
/* ================================================================== */
/* 查表匹配（vlookup 口径）                                            */
/*                                                                    */
/* 一句话：拿某个值当键，去「数据表」的某一列里找那一行，把那一行的若干 */
/* 列填进模板变量。所有「查不到怎么办」的分支都写在同一处，见 engine.c   */
/* 的 apply_lookup_rule。                                              */
/* ================================================================== */

static const int kLookupKeyModes[] = { LOOKUP_KEY_HOST, LOOKUP_KEY_VAR, LOOKUP_KEY_FIX };
#define QP_LK_KEYMODE_COUNT ((int)(sizeof(kLookupKeyModes) / sizeof(kLookupKeyModes[0])))
static const int kLookupHits[] = { LOOKUP_FIRST, LOOKUP_LAST, LOOKUP_ALL };
#define QP_LK_HIT_COUNT ((int)(sizeof(kLookupHits) / sizeof(kLookupHits[0])))

typedef struct {
    HWND hwnd;
    ParseRule *rule;
    const Template *tmpl;
    int result;              /* 1 = 用户确认；dirty 不代表确认 */
    int dirty;
    int loading;
    HWND hTable, hTableInfo, hKeyMode, hKeyVar, hKeyFix, hKeyCol;
    HWND hHit, hSep, hUnique;
    HWND hOutList, hOutCol, hOutTarget, hTip;
    HWND hKeyVarLbl, hKeyFixLbl, hSepLbl;
    HWND hLblTable, hLblKeyMode, hLblKeyCol, hLblHit, hLblOut, hLblOutCol, hLblOutTarget;
    HWND hBtnManage, hOutAdd, hOutDel, hOutUp, hOutDown;
    QpTable *tbl;            /* 上次取到的表（借用缓存，不释放；用前重新取） */
} LookupDlgCtx;

/* 表句柄是「借用」的：缓存一共就 4 个槽，别处一取别的表就可能把这一张换掉
   （数据表窗口列出清单时会挨个 probe）。所以每次真要读表之前重新取一次，
   拿到的一定是当下有效的那张——不然点击时用的就是个已释放的指针。 */
static QpTable *lookup_table(LookupDlgCtx *c)
{
    if (!c->rule->lk_table_id[0]) { c->tbl = NULL; return NULL; }
    c->tbl = qp_table_get(c->rule->lk_table_id, NULL, 0);
    return c->tbl;
}

static int lookup_keymode_index(int m)
{
    int i;
    for (i = 0; i < QP_LK_KEYMODE_COUNT; ++i)
        if (kLookupKeyModes[i] == m) return i;
    return 0;
}

static int lookup_hit_index(int h)
{
    int i;
    for (i = 0; i < QP_LK_HIT_COUNT; ++i)
        if (kLookupHits[i] == h) return i;
    return 0;
}

static const wchar_t *lookup_keymode_hint(int m)
{
    switch (m) {
    case LOOKUP_KEY_VAR: return L"其他变量的值";
    case LOOKUP_KEY_FIX: return L"固定值";
    default:             return L"当前变量的值";
    }
}

/* 表里的列名（没有表头时给「第 N 列」占位）。combo 的下标 == 列下标。 */
static void lookup_fill_col_combo(HWND combo, const QpTable *t, const wchar_t *want_name,
                                 int want_index)
{
    int c, cols = t ? qp_table_cols(t) : 0;
    int sel = qp_table_col_index(t,want_name,want_index);
    if ((!want_name || !want_name[0]) && want_index<0 && cols) sel=0;

    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    if (!cols) return;
    for (c = 0; c < cols; ++c) {
        const wchar_t *nm = t ? qp_table_col_name(t, c) : NULL;
        wchar_t item[QP_LABEL_MAX + 32];
        if (nm && nm[0]) {
            SendMessageW(combo,CB_ADDSTRING,0,(LPARAM)nm);
            continue;
        } else {
            _snwprintf(item, QP_LABEL_MAX + 32, L"第 %d 列", c + 1);
        }
        item[QP_LABEL_MAX + 31] = L'\0';
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)item);
    }
    SendMessageW(combo, CB_SETCURSEL, (WPARAM)sel, 0);
}

/* 把选中列写回规则：有表头存列名（表列顺序变了也不怕），没表头存列号 */
static int lookup_store_col(const QpTable *t, int idx, wchar_t *name, size_t namecap,
                             int *index)
{
    const wchar_t *nm = (t && idx >= 0) ? qp_table_col_name(t, idx) : NULL;
    if (idx<0 || (t && idx>=qp_table_cols(t)) || !name || !namecap || !index) return 0;
    if (nm && nm[0]) {
        if (wcslen(nm)>=namecap) return 0;
        qp_strlcpy(name, nm, namecap);
        *index = -1;
    } else {
        name[0] = L'\0';
        *index = idx;
    }
    return 1;
}

static void lookup_reject_column(LookupDlgCtx *c)
{
    qp_set_text_if_diff(c->hTip,L"无法保存所选列。请确认数据表可读；列名过长时请先缩短表头，再重新选择。现有映射未改变。");
}

static void lookup_row_text(LookupDlgCtx *c, int row)
{
    const LookupOut *o = lookup_out_at(c->rule->lk_outs, row);
    wchar_t col[QP_LABEL_MAX + 24], tgt[QP_LABEL_MAX + 24], line[2 * QP_LABEL_MAX + 64];
    const Segment *s;

    if (!o) return;
    if (o->col[0]) qp_strlcpy(col, o->col, QP_LABEL_MAX + 24);
    else if (o->col_index >= 0) swprintf(col, QP_LABEL_MAX + 24, L"第 %d 列", o->col_index + 1);
    else qp_strlcpy(col, L"（未选择列）", QP_LABEL_MAX + 24);
    s = tmpl_find_segment(c->tmpl, o->target_segment_id);
    swprintf(tgt, QP_LABEL_MAX + 24, L"%ls", s ? seg_display_name(s) : L"(变量已删除)");
    swprintf(line, 2 * QP_LABEL_MAX + 64, L"数据列「%ls」", col);
    lv_set_row(c->hOutList, row, 0, line);
    lv_set_row(c->hOutList, row, 1, tgt);
}

static void lookup_load_out(LookupDlgCtx *c)
{
    int row = lv_sel(c->hOutList);
    const LookupOut *o = lookup_out_at(c->rule->lk_outs, row);
    int was = c->loading;

    /* 灌下拉框会反过来发 CBN_SELCHANGE / LVN_ITEMCHANGED 通知；把 loading
       抬起来挡住，走完再恢复成调用方原来的值——直接写 0 会把外层的
       「正在重建」抹掉，让重建过程中的通知被当成用户操作。 */
    c->loading = 1;
    if (o) {
        lookup_fill_col_combo(c->hOutCol, lookup_table(c), o->col, o->col_index);
        fill_var_combo(c->hOutTarget, c->tmpl, o->target_segment_id, NULL, 0);
    } else {
        SendMessageW(c->hOutCol, CB_RESETCONTENT, 0, 0);
        SendMessageW(c->hOutTarget, CB_RESETCONTENT, 0, 0);
    }
    c->loading = was;
    {
        int on = (o != NULL);
        EnableWindow(c->hOutCol, on);
        EnableWindow(c->hOutTarget, on);
        EnableWindow(GetDlgItem(c->hwnd, IDC_LK_OUT_DEL), on);
        EnableWindow(GetDlgItem(c->hwnd, IDC_LK_OUT_UP), on && row > 0);
        EnableWindow(GetDlgItem(c->hwnd, IDC_LK_OUT_DOWN),
                     on && row + 1 < lookup_out_count(c->rule->lk_outs));
    }
}

static void lookup_commit_out(LookupDlgCtx *c)
{
    LookupOut *o;
    const Segment *tgt;
    int idx;

    if (c->loading) return;
    o = lookup_out_at(c->rule->lk_outs, lv_sel(c->hOutList));
    if (!o) return;
    idx = (int)SendMessageW(c->hOutCol, CB_GETCURSEL, 0, 0);
    if (idx != CB_ERR && !lookup_store_col(lookup_table(c),idx,o->col,QP_LABEL_MAX,&o->col_index)) {
        lookup_reject_column(c);
        lookup_load_out(c);
        return;
    }
    tgt = combo_selected_seg(c->hOutTarget);
    qp_strlcpy(o->target_segment_id, tgt ? tgt->id : L"", QP_ID_MAX);
    lookup_row_text(c, lv_sel(c->hOutList));
}

/* 列表选中行变了：只把那一行读进下面的编辑区，绝不往回写。
   ListView_DeleteAllItems / InsertItem / SetItemState 都会同步发 LVN_ITEMCHANGED，
   所以整段重建都压在 c->loading 里，通知进来直接跳过：既不能在列表自己的通知
   里反过来再动列表（会踩到控件内部状态），也不能提交——那一刻下拉框里还是
   上一行的值，会把刚选中的那一行覆盖掉。 */
static void lookup_list_changed(LookupDlgCtx *c, const NMLISTVIEW *nv)
{
    if (!c || c->loading) return;
    if (!(nv->uNewState & LVIS_SELECTED)) return;
    if (nv->uOldState & LVIS_SELECTED) return;      /* 本来就选着：不是换行 */
    lookup_load_out(c);
}

static void lookup_fill_outs(LookupDlgCtx *c, int sel)
{
    int n = lookup_out_count(c->rule->lk_outs), i;
    int was = c->loading;

    c->loading = 1;
    ListView_DeleteAllItems(c->hOutList);
    for (i = 0; i < n; ++i) lookup_row_text(c, i);
    if (n > 0) lv_select(c->hOutList, sel);
    c->loading = was;
    lookup_load_out(c);
}

/* 交换两个输出对的位置。调用方只传相邻下标（row, row-1 / row, row+1），
   所以这就是「上移/下移一格」；顺序决定「多条规则抢同一个目标变量」时谁生效。 */
static int lookup_out_move(LookupOut **head, int from, int to)
{
    int n = lookup_out_count(*head), i;
    LookupOut **arr;
    if (!head || n < 2 || from < 0 || to < 0 || from >= n || to >= n) return 0;
    if (from == to) return 0;
    arr = (LookupOut **)malloc((size_t)n * sizeof(LookupOut *));
    if (!arr) return 0;
    {
        LookupOut *p = *head;
        for (i = 0; i < n; ++i) { arr[i] = p; p = p ? p->next : NULL; }
    }
    {
        LookupOut *tmp = arr[from];
        arr[from] = arr[to];
        arr[to] = tmp;
    }
    for (i = 0; i < n; ++i) arr[i]->next = (i + 1 < n) ? arr[i + 1] : NULL;
    *head = arr[0];
    free(arr);
    return 1;
}

/* 表的当前情况：几行几列、有哪些列。选表之后立刻刷新，选列的下拉也据此重建。 */
static void lookup_sync_table(LookupDlgCtx *c, int refill_cols)
{
    DataTableRef *ref = NULL;
    wchar_t err[256], info[1024];
    int idx = (int)SendMessageW(c->hTable, CB_GETCURSEL, 0, 0);
    int rows = 0, cols = 0;
    int was = c->loading;

    c->loading = 1;
    if (idx != CB_ERR)
        ref = (DataTableRef *)SendMessageW(c->hTable, CB_GETITEMDATA, (WPARAM)idx, 0);
    err[0] = L'\0';
    c->tbl = ref ? qp_table_get(ref->id, err, 256) : NULL;
    if (!ref) {
        qp_strlcpy(info, L"尚未选择数据表。请点击「管理数据表…」导入 CSV 或 TSV 文件。",
                   1024);
    } else if (!c->tbl) {
        swprintf(info, 1024, L"「%ls」无法读取：%ls", ref->name,
                 err[0] ? err : L"原因未知");
    } else {
        int k, used = 0;
        rows = qp_table_rows(c->tbl);
        cols = qp_table_cols(c->tbl);
        used += swprintf(info, 1024, L"「%ls」：%d 行 × %d 列　列名：", ref->name,
                         rows, cols);
        for (k = 0; k < cols && k < 16; ++k) {
            int w = swprintf(info + used, 1024 - (size_t)used, L"%ls%ls",
                             k ? L"、" : L"",
                             qp_table_col_name(c->tbl, k) ? qp_table_col_name(c->tbl, k)
                                                          : L"");
            if (w < 0) break;
            used += w;
        }
        if (cols > 16) qp_strlcpy(info + used, L"…", 1024 - (size_t)used);
    }
    info[1023] = L'\0';
    qp_set_text_if_diff(c->hTableInfo, info);

    if (refill_cols) {
        lookup_fill_col_combo(c->hKeyCol, c->tbl, c->rule->lk_key_col,
                              c->rule->lk_key_col_index);
        lookup_load_out(c);
    }
    /* 键列/取值列的下拉都要能选：表读不出来时先禁用，免得选了个空 */
    EnableWindow(c->hKeyCol, c->tbl != NULL);
    c->loading = was;
}

static void lookup_sync_enable(LookupDlgCtx *c)
{
    int mode = lookup_keymode_index(c->rule->lk_key_mode);
    int hit = (int)SendMessageW(c->hHit, CB_GETCURSEL, 0, 0);
    int is_all = (hit == lookup_hit_index(LOOKUP_ALL));

    EnableWindow(c->hKeyVarLbl, mode == lookup_keymode_index(LOOKUP_KEY_VAR));
    EnableWindow(c->hKeyVar, mode == lookup_keymode_index(LOOKUP_KEY_VAR));
    EnableWindow(c->hKeyFixLbl, mode == lookup_keymode_index(LOOKUP_KEY_FIX));
    EnableWindow(c->hKeyFix, mode == lookup_keymode_index(LOOKUP_KEY_FIX));
    EnableWindow(c->hSepLbl, is_all);
    EnableWindow(c->hSep, is_all);
    EnableWindow(c->hUnique, is_all);
}

/* 输出对列表要能增删改：列表里选中一行，下面用两个下拉即时改写它 */
static void lookup_out_add(LookupDlgCtx *c)
{
    LookupOut *o = lookup_out_create();
    QpTable *tbl = lookup_table(c);      /* 用前重新取：缓存可能已经换了 */
    int cols = tbl ? qp_table_cols(tbl) : 0;

    if (!o) return;
    o->col_index = -1;
    if (cols > 0) {
        /* 默认取「第 2 列」：第 1 列通常是键列，取它多半是笔误 */
        int pick = cols > 1 ? 1 : 0;
        if(!lookup_store_col(tbl,pick,o->col,QP_LABEL_MAX,&o->col_index)) {
            lookup_out_free(o);
            lookup_reject_column(c);
            return;
        }
    }
    lookup_out_append(&c->rule->lk_outs, o);
    lookup_fill_outs(c, lookup_out_count(c->rule->lk_outs) - 1);
    c->dirty = 1;
}

static LRESULT CALLBACK lookup_dlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

/* 一次性把控件建好摆好（下拉里的内容是后面 sync 阶段灌的） */
static void lookup_dlg_layout(LookupDlgCtx *c)
{
    HWND hwnd = c->hwnd;
    int m = DPIX(14), y = DPIX(14), full, lw = DPIX(96), cx, cw, i;
    int rowh = DPIX(28), bw = DPIX(86);
    RECT rc;

    GetClientRect(hwnd, &rc);
    full = rc.right - m * 2;
    cx = m + lw + DPIX(8);
    cw = rc.right - m - cx;

    /* ---- 选表 ---- */
    c->hLblTable = mk_label_r(hwnd, L"数据表：", 0);
    c->hTable = mk_combo(hwnd, IDC_LK_TABLE);
    c->hBtnManage = mk_btn(hwnd, L"管理数据表…", IDC_LK_TABLE_MANAGE);
    MoveWindow(c->hLblTable, m, y + DPIX(4), lw, DPIX(18), TRUE);
    MoveWindow(c->hTable, cx, y, cw - DPIX(178), DPIX(24), TRUE);
    MoveWindow(c->hBtnManage, rc.right - m - DPIX(170), y, DPIX(170), DPIX(24), TRUE);
    y += DPIX(26);

    c->hTableInfo = qp_create(L"Edit", L"", ES_MULTILINE | ES_READONLY |
                             ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP,
                             0, 0, 0, 10, 10, hwnd, IDC_LK_TABLE_INFO);
    MoveWindow(c->hTableInfo, cx, y, cw, DPIX(42), TRUE);
    y += DPIX(50);

    /* ---- 键 ---- */
    c->hLblKeyMode = mk_label_r(hwnd, L"键的来源：", 0);
    c->hKeyMode = mk_combo(hwnd, IDC_LK_KEYMODE);
    for (i = 0; i < QP_LK_KEYMODE_COUNT; ++i)
        SendMessageW(c->hKeyMode, CB_ADDSTRING, 0,
                     (LPARAM)lookup_keymode_hint(kLookupKeyModes[i]));
    SendMessageW(c->hKeyMode, CB_SETCURSEL,
                 (WPARAM)lookup_keymode_index(c->rule->lk_key_mode), 0);
    MoveWindow(c->hLblKeyMode, m, y + DPIX(4), lw, DPIX(18), TRUE);
    MoveWindow(c->hKeyMode, cx, y, cw, DPIX(24), TRUE);
    y += rowh;

    c->hKeyVarLbl = mk_label_r(hwnd, L"键的变量：", 0);
    c->hKeyVar = mk_combo(hwnd, IDC_LK_KEYVAR);
    fill_var_combo(c->hKeyVar, c->tmpl, c->rule->lk_key_var_id, NULL, 0);
    MoveWindow(c->hKeyVarLbl, m, y + DPIX(4), lw, DPIX(18), TRUE);
    MoveWindow(c->hKeyVar, cx, y, cw, DPIX(24), TRUE);
    y += rowh;

    c->hKeyFixLbl = mk_label_r(hwnd, L"固定值：", 0);
    c->hKeyFix = mk_edit(hwnd, IDC_LK_KEYFIX, 0);
    SendMessageW(c->hKeyFix, EM_SETCUEBANNER, TRUE,
                 (LPARAM)L"每次都用该固定值查询，例如某个固定编号");
    SetWindowTextW(c->hKeyFix, c->rule->lk_key_value);
    MoveWindow(c->hKeyFixLbl, m, y + DPIX(4), lw, DPIX(18), TRUE);
    MoveWindow(c->hKeyFix, cx, y, cw, DPIX(24), TRUE);
    y += rowh;

    c->hLblKeyCol = mk_label_r(hwnd, L"键列：", 0);
    c->hKeyCol = mk_combo(hwnd, IDC_LK_KEYCOL);
    MoveWindow(c->hLblKeyCol, m, y + DPIX(4), lw, DPIX(18), TRUE);
    MoveWindow(c->hKeyCol, cx, y, cw, DPIX(24), TRUE);
    y += rowh;

    c->hLblHit = mk_label_r(hwnd, L"匹配多行时：", 0);
    c->hHit = mk_combo(hwnd, IDC_LK_HIT);
    for (i = 0; i < QP_LK_HIT_COUNT; ++i)
        SendMessageW(c->hHit, CB_ADDSTRING, 0, (LPARAM)qp_table_hit_name(kLookupHits[i]));
    SendMessageW(c->hHit, CB_SETCURSEL,
                 (WPARAM)lookup_hit_index(c->rule->lk_hit), 0);
    MoveWindow(c->hLblHit, m, y + DPIX(4), lw, DPIX(18), TRUE);
    MoveWindow(c->hHit, cx, y, DPIX(160), DPIX(24), TRUE);

    c->hSepLbl = mk_label_r(hwnd, L"连接符：", 0);
    c->hSep = mk_edit(hwnd, IDC_LK_SEP, 0);
    SendMessageW(c->hSep, EM_SETCUEBANNER, TRUE, (LPARAM)L"默认「、」");
    SetWindowTextW(c->hSep, c->rule->lk_sep);
    MoveWindow(c->hSepLbl, cx + DPIX(168), y + DPIX(4), DPIX(70), DPIX(18), TRUE);
    MoveWindow(c->hSep, cx + DPIX(242), y, DPIX(56), DPIX(24), TRUE);

    c->hUnique = mk_check(hwnd, L"重复值只留一个", IDC_LK_UNIQUE);
    SendMessageW(c->hUnique, BM_SETCHECK,
                 c->rule->lk_unique ? BST_CHECKED : BST_UNCHECKED, 0);
    MoveWindow(c->hUnique, cx + DPIX(310), y + DPIX(2),
               cw - DPIX(310), DPIX(20), TRUE);
    y += rowh + DPIX(6);

    /* ---- 输出对 ---- */
    c->hLblOut = mk_label(hwnd, L"输出映射（数据列 → 目标变量）：", 0);
    c->hOutAdd = mk_btn(hwnd, L"添加映射", IDC_LK_OUT_ADD);
    c->hOutDel = mk_btn(hwnd, L"删除", IDC_LK_OUT_DEL);
    MoveWindow(c->hLblOut, m, y, full - DPIX(184), DPIX(18), TRUE);
    MoveWindow(c->hOutAdd, rc.right - m - bw, y - DPIX(3), bw, DPIX(24), TRUE);
    MoveWindow(c->hOutDel, rc.right - m - bw * 2 - DPIX(8), y - DPIX(3),
               bw, DPIX(24), TRUE);
    y += DPIX(26);

    c->hOutList = mk_list(hwnd, IDC_LK_OUTLIST, 0);
    lv_add_col(c->hOutList, 0, L"数据列", 260);
    lv_add_col(c->hOutList, 1, L"目标变量", 200);
    MoveWindow(c->hOutList, m, y, full, DPIX(96), TRUE);
    ListView_SetColumnWidth(c->hOutList, 0, (full - DPIX(26)) * 3 / 5);
    ListView_SetColumnWidth(c->hOutList, 1, (full - DPIX(26)) * 2 / 5);
    y += DPIX(104);

    c->hLblOutCol = mk_label_r(hwnd, L"数据列：", 0);
    c->hOutCol = mk_combo(hwnd, IDC_LK_OUT_COL);
    MoveWindow(c->hLblOutCol, m, y + DPIX(4), lw, DPIX(18), TRUE);
    MoveWindow(c->hOutCol, cx, y, cw - DPIX(180), DPIX(24), TRUE);

    c->hLblOutTarget = mk_label_r(hwnd, L"目标变量：", 0);
    c->hOutTarget = mk_combo(hwnd, IDC_LK_OUT_TARGET);
    MoveWindow(c->hLblOutTarget, cx + cw - DPIX(172), y + DPIX(4), DPIX(70), DPIX(18), TRUE);
    MoveWindow(c->hOutTarget, cx + cw - DPIX(98), y, DPIX(98), DPIX(24), TRUE);
    y += rowh + DPIX(2);

    c->hOutUp = mk_btn(hwnd, L"上移", IDC_LK_OUT_UP);
    c->hOutDown = mk_btn(hwnd, L"下移", IDC_LK_OUT_DOWN);
    MoveWindow(c->hOutUp, rc.right - m - bw * 2 - DPIX(8), y, bw, DPIX(24), TRUE);
    MoveWindow(c->hOutDown, rc.right - m - bw, y, bw, DPIX(24), TRUE);
    y += DPIX(30);

    c->hTip = mk_wrapped_label(hwnd, L"", IDC_LK_TIP);
    SetWindowTextW(c->hTip,
        L"先去除键的首尾空白，按不区分大小写的完整文本匹配；无文本匹配时，十进制键再按精确数值匹配。"
        L"例如 1.3800138E+10 与 13800138000 等值；表格软件已舍弃的数字无法恢复，编号建议存为文本。"
        L"未匹配时保留手填值或默认值；匹配到空单元格时目标可为空。取消本窗口不撤销数据表管理中的修改。");
    { int height = qp_wrapped_text_height(c->hTip, full);
      MoveWindow(c->hTip, m, y, full, height, TRUE); }

    mk_btn(hwnd, L"确定", IDOK);
    mk_btn(hwnd, L"取消", IDCANCEL);
    MoveWindow(GetDlgItem(hwnd, IDOK), rc.right - m - DPIX(176),
               rc.bottom - m - DPIX(26), DPIX(86), DPIX(26), TRUE);
    MoveWindow(GetDlgItem(hwnd, IDCANCEL), rc.right - m - DPIX(84),
               rc.bottom - m - DPIX(26), DPIX(86), DPIX(26), TRUE);
    SendMessageW(GetDlgItem(hwnd, IDOK), BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);
}

static LRESULT CALLBACK lookup_dlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LookupDlgCtx *c = (LookupDlgCtx *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    int handled = 0;
    LRESULT r;

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        const DataTableRef *tr;
        c = (LookupDlgCtx *)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)c);
        c->hwnd = hwnd;
        lookup_dlg_layout(c);
        for (tr = g_app ? g_app->tables : NULL; tr; tr = tr->next) {
            int idx = (int)SendMessageW(c->hTable, CB_ADDSTRING, 0, (LPARAM)tr->name);
            SendMessageW(c->hTable, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)tr);
            if (wcscmp(tr->id, c->rule->lk_table_id) == 0)
                SendMessageW(c->hTable, CB_SETCURSEL, (WPARAM)idx, 0);
        }
        qp_theme_children(hwnd);
        lookup_sync_table(c, 1);
        lookup_fill_outs(c, 0);
        lookup_sync_enable(c);
        return 0;
    }

    case WM_COMMAND:
        qp_combo_dropdown_fix(wp, lp);
        if (!c) break;
        switch (LOWORD(wp)) {
        case IDC_LK_TABLE:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int idx = (int)SendMessageW(c->hTable, CB_GETCURSEL, 0, 0);
                DataTableRef *ref = (idx == CB_ERR) ? NULL
                    : (DataTableRef *)SendMessageW(c->hTable, CB_GETITEMDATA,
                                                   (WPARAM)idx, 0);
                qp_strlcpy(c->rule->lk_table_id, ref ? ref->id : L"", QP_ID_MAX);
                c->dirty = 1;
                lookup_sync_table(c, 1);
                lookup_sync_enable(c);
            }
            return 0;
        case IDC_LK_TABLE_MANAGE:
            if (dlg_data_tables(hwnd, g_app)) {
                DataTableRef *tr;
                int was = c->loading;
                /* 表清单变了：刷新下拉，尽量保持原来选中的那张 */
                c->loading = 1;
                SendMessageW(c->hTable, CB_RESETCONTENT, 0, 0);
                for (tr = g_app ? g_app->tables : NULL; tr; tr = tr->next) {
                    int idx = (int)SendMessageW(c->hTable, CB_ADDSTRING, 0,
                                                (LPARAM)tr->name);
                    SendMessageW(c->hTable, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)tr);
                    if (wcscmp(tr->id, c->rule->lk_table_id) == 0)
                        SendMessageW(c->hTable, CB_SETCURSEL, (WPARAM)idx, 0);
                }
                c->loading = was;
                app_save_data();
                lookup_sync_table(c, 1);
                lookup_sync_enable(c);
                c->dirty = 1;
            }
            return 0;
        case IDC_LK_KEYMODE: {
            int idx = (int)SendMessageW(c->hKeyMode, CB_GETCURSEL, 0, 0);
            if (idx >= 0 && idx < QP_LK_KEYMODE_COUNT)
                c->rule->lk_key_mode = kLookupKeyModes[idx];
            lookup_sync_enable(c);
            c->dirty = 1;
            return 0;
        }
        case IDC_LK_KEYCOL:
            if (HIWORD(wp) == CBN_SELCHANGE &&
                !lookup_store_col(lookup_table(c),
                                 (int)SendMessageW(c->hKeyCol, CB_GETCURSEL, 0, 0),
                                 c->rule->lk_key_col, QP_LABEL_MAX,
                                 &c->rule->lk_key_col_index)) {
                int was=c->loading;c->loading=1;
                lookup_fill_col_combo(c->hKeyCol,lookup_table(c),c->rule->lk_key_col,c->rule->lk_key_col_index);
                c->loading=was;
                lookup_reject_column(c);
                return 0;
            }
            c->dirty = 1;
            return 0;
        case IDC_LK_HIT: {
            int idx = (int)SendMessageW(c->hHit, CB_GETCURSEL, 0, 0);
            if (idx >= 0 && idx < QP_LK_HIT_COUNT) c->rule->lk_hit = kLookupHits[idx];
            lookup_sync_enable(c);
            c->dirty = 1;
            return 0;
        }
        case IDC_LK_UNIQUE:
            c->rule->lk_unique =
                (SendMessageW(c->hUnique, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
            c->dirty = 1;
            return 0;
        case IDC_LK_OUT_ADD:
            lookup_out_add(c);
            return 0;
        case IDC_LK_OUT_DEL: {
            int row = lv_sel(c->hOutList);
            if (row < 0) return 0;
            lookup_out_remove_at(&c->rule->lk_outs, row);
            lookup_fill_outs(c, row);
            c->dirty = 1;
            return 0;
        }
        case IDC_LK_OUT_UP: {
            int row = lv_sel(c->hOutList);
            if (lookup_out_move(&c->rule->lk_outs, row, row - 1)) {
                lookup_fill_outs(c, row - 1);
                c->dirty = 1;
            }
            return 0;
        }
        case IDC_LK_OUT_DOWN: {
            int row = lv_sel(c->hOutList);
            if (lookup_out_move(&c->rule->lk_outs, row, row + 1)) {
                lookup_fill_outs(c, row + 1);
                c->dirty = 1;
            }
            return 0;
        }
        case IDC_LK_OUT_COL:
            if (HIWORD(wp) == CBN_SELCHANGE) { lookup_commit_out(c); c->dirty = 1; }
            return 0;
        case IDC_LK_OUT_TARGET:
            if (HIWORD(wp) == CBN_SELCHANGE) { lookup_commit_out(c); c->dirty = 1; }
            return 0;
        case IDOK: {
            wchar_t buf[QP_VALUE_MAX];
            int kc = (int)SendMessageW(c->hKeyCol, CB_GETCURSEL, 0, 0);
            if (!c->rule->lk_table_id[0]) {
                qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"提示",
                          L"请先选择数据表（尚未导入时请点击「管理数据表…」）。");
                SetFocus(c->hTable);
                return 0;
            }
            if (!lookup_table(c)) {
                qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"提示",
                          L"所选数据表无法读取，请点击「管理数据表…」检查"
                          L"文件路径与编码。");
                return 0;
            }
            if (kc == CB_ERR) {
                qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"提示",
                          L"请选择键列。");
                SetFocus(c->hKeyCol);
                return 0;
            }
            if (!c->rule->lk_outs) {
                if (qp_msgbox(hwnd, MB_YESNO | MB_ICONQUESTION, L"提示",
                              L"尚未添加输出映射，该规则即使命中"
                              L"也无处填写。\n\n是否现在添加映射？") == IDYES) {
                    lookup_out_add(c);
                    return 0;
                }
            }
            if (lookup_keymode_index(c->rule->lk_key_mode) ==
                lookup_keymode_index(LOOKUP_KEY_FIX)) {
                qp_get_text(c->hKeyFix, buf, QP_VALUE_MAX);
                qp_trim(buf);
                qp_strlcpy(c->rule->lk_key_value, buf, QP_MATCH_MAX);
                if (!buf[0]) {
                    qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"提示",
                              L"键来源为「固定值」时必须填写固定值。");
                    SetFocus(c->hKeyFix);
                    return 0;
                }
            }
            qp_get_text(c->hSep, buf, QP_VALUE_MAX);
            qp_trim(buf);
            if (!buf[0]) qp_strlcpy(buf, L"、", QP_VALUE_MAX);
            qp_strlcpy(c->rule->lk_sep, buf, QP_SEP_MAX);
            lookup_commit_out(c);
            c->result = 1;
            dlg_close(hwnd);
            return 0;
        }
        case IDCANCEL:
            c->result = 0;
            dlg_close(hwnd);
            return 0;
        default:
            break;
        }
        break;

    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (c && nh && nh->idFrom == IDC_LK_OUTLIST && nh->code == LVN_ITEMCHANGED)
            lookup_list_changed(c, (const NMLISTVIEW *)lp);
        break;
    }

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

int dlg_lookup_rule(HWND owner, ParseRule *rule, const Template *t)
{
    LookupDlgCtx c;
    HWND dlg;
    memset(&c, 0, sizeof(c));
    if (!rule) return 0;
    c.rule = parse_rule_clone(rule);
    if (!c.rule) {
        qp_msgbox(owner, MB_OK | MB_ICONERROR, L"无法编辑查表规则",
                  L"内存不足，无法创建规则副本。现有规则未改变。请关闭不需要的窗口后重试。");
        return 0;
    }
    c.tmpl = t;
    dlg = make_dlg(L"QuickPhraseLookup", lookup_dlg_proc, L"查表匹配", 700, 620,
                   owner, &c);
    int accepted = run_modal(owner, dlg, &c.result);
    if (accepted) qp_parse_rule_take(rule, c.rule);
    else parse_rule_free(c.rule);
    return accepted;
}
