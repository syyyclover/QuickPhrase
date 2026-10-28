#include "dialogs_internal.h"
/* ================================================================== */
/* 数据表（名单 / 联系表）管理                                          */
/*                                                                    */
/* 这里只登记「引用」：表名 + 文件路径 + 怎么读（编码/分隔符/有无表头）。 */
/* 表内容永远以磁盘上那个文件为准，所以每周覆盖同一个文件即可，不需要   */
/* 重新导入。预览区让「导入对不对」当场看得见。                          */
/* ================================================================== */

/* 编码 / 分隔符下拉的顺序（与枚举解耦：以后插入新项不影响存档） */
static const int kTableEncs[] = { QP_ENC_AUTO, QP_ENC_UTF8, QP_ENC_GBK, QP_ENC_UTF16 };
#define QP_TABLE_ENC_COUNT ((int)(sizeof(kTableEncs) / sizeof(kTableEncs[0])))
static const int kTableDelims[] = { QP_DELIM_AUTO, QP_DELIM_COMMA, QP_DELIM_TAB,
                                    QP_DELIM_SEMI };
#define QP_TABLE_DELIM_COUNT ((int)(sizeof(kTableDelims) / sizeof(kTableDelims[0])))

#define QP_DT_PREVIEW_ROWS 20
#define QP_DT_PREVIEW_COLS 10      /* 预览最多铺这么多列，再多就用文字说明 */

/* 数据表窗口能拉大缩小，所以得有个下限。宽度由那一排五个按钮定住：
   14(边距) + 5×104 + 4×8 = 566，右侧还要留 14 的边距 → 580。
   高度按三段内容 + 60 的预览下限 + 底部按钮算出来 → 440。
   与模板编辑器的 ED_MIN_W/ED_MIN_H 是同一套做法。 */
#define DT_MIN_W       580
#define DT_MIN_H       440

typedef struct {
    HWND hwnd;
    AppData *app;
    int result;                    /* 1 = 有改动，调用方据此存盘 */
    int loading;                   /* 1 = 正在灌值，忽略变更通知 */
    HWND hList, hEnc, hDelim, hHeader, hInfo, hCols, hPrev;
    HWND hImport, hRename, hRemove, hReload, hOpen;
    HWND hLblList, hLblRead, hLblEnc, hLblDelim, hLblPrev;
    int  y_info, y_cols, y_prev;   /* 三段的位置（拉大时按它们重排） */
    int  prev_cols;
    /* 拖边框时 WM_SIZE 一秒能来上百条。下面这三个记着「上次做到哪儿」，
       用来把重复活和重复重画挡掉，不然拉窗口就是一片糊。 */
    int  last_w, last_h;           /* 上次 WM_SIZE 的客户区尺寸 */
    int  sizing;                   /* 1 = 正在拖边框（WM_ENTERSIZEMOVE 期间） */
    int  prev_fit_w, prev_fit_cols;/* 预览列宽是按「多宽的控件 / 多少列」铺的 */
} TableDlgCtx;

static DataTableRef *table_dlg_ref(TableDlgCtx *c)
{
    return dtref_list_at(c->app ? c->app->tables : NULL, lv_sel(c->hList));
}

/* 把「这张表能不能读、多少行多少列」写成一句话。ok 时顺带给出列名一览。 */
static void table_dlg_info_text(const DataTableRef *r, wchar_t *out, size_t cap,
                                int *rows_out, int *cols_out)
{
    wchar_t err[256];
    int rows = 0, cols = 0;

    if (rows_out) *rows_out = 0;
    if (cols_out) *cols_out = 0;
    if (!out || cap == 0) return;
    out[0] = L'\0';
    if (!r) {
        qp_strlcpy(out, L"尚未导入数据表。请点击「导入 CSV/TSV…」选择数据表文件。", cap);
        return;
    }
    err[0] = L'\0';
    if (qp_table_probe(r->id, &rows, &cols, err, 256)) {
        swprintf(out, cap, L"共 %d 行 × %d 列（不含表头）\n%ls", rows, cols, r->path);
        if (rows_out) *rows_out = rows;
        if (cols_out) *cols_out = cols;
    } else {
        swprintf(out, cap, L"无法读取：%ls\n%ls\n"
                          L"（文件可能已被移动或改名：请点击「指定文件…」选择新位置，"
                          L"或调整上方的编码 / 分隔符后重试）",
                 err[0] ? err : L"原因未知", r->path);
    }
    out[cap - 1] = L'\0';
}

/* 预览区的列宽：第 1 列固定给行号，其余等分控件宽度。
   这里看着简单，但它是「拉窗口卡顿」的大头：ListView_SetColumnWidth 每改一列
   都会把整个列表重排 + 重画一遍，十几列连着改就是十几遍。所以
   (1) 控件宽度和列数都没变就直接返回（拖高度时一列都不用改）；
   (2) 真要改时先用 WM_SETREDRAW 把中间过程捂住，最后只重画一次。 */
static void table_dlg_fit_cols(TableDlgCtx *c, int force)
{
    RECT rc;
    int k, w, full;

    if (!c || !c->hPrev || c->prev_cols <= 0) return;
    GetClientRect(c->hPrev, &rc);
    full = rc.right;
    if (full <= 0) return;
    if (!force && full == c->prev_fit_w && c->prev_cols == c->prev_fit_cols)
        return;
    w = (full - DPIX(44)) / (c->prev_cols > 1 ? c->prev_cols - 1 : 1);
    if (w < DPIX(40)) w = DPIX(40);
    SendMessageW(c->hPrev, WM_SETREDRAW, FALSE, 0);
    for (k = 0; k < c->prev_cols; ++k)
        ListView_SetColumnWidth(c->hPrev, k, k == 0 ? DPIX(40) : w);
    SendMessageW(c->hPrev, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(c->hPrev, NULL, TRUE);
    c->prev_fit_w = full;
    c->prev_fit_cols = c->prev_cols;
}

/* 预览区：第一列行号，后面每列一个字段 */
static void table_dlg_preview(TableDlgCtx *c)
{
    DataTableRef *r = table_dlg_ref(c);
    QpTablePreview *pv;
    QpTableOpts opts;
    wchar_t err[256];
    int i, j, show_cols;

    while (ListView_DeleteColumn(c->hPrev, 0)) { }
    ListView_DeleteAllItems(c->hPrev);
    c->prev_cols = 0;
    c->prev_fit_cols = 0;               /* 列是新建的：宽度要重新铺 */
    qp_set_text_if_diff(c->hCols, L"");
    if (!r) return;

    opts.encoding = r->encoding;
    opts.delimiter = r->delimiter;
    opts.has_header = r->has_header;
    err[0] = L'\0';
    pv = qp_table_preview(r->path, &opts, QP_DT_PREVIEW_ROWS, err, 256);
    if (!pv) {
        wchar_t msg[512];
        swprintf(msg, 512, L"预览失败：%ls", err[0] ? err : L"原因未知");
        qp_set_text_if_diff(c->hCols, msg);
        return;
    }

    show_cols = pv->cols > QP_DT_PREVIEW_COLS ? QP_DT_PREVIEW_COLS : pv->cols;
    lv_add_col(c->hPrev, 0, L"#", 40);
    for (j = 0; j < show_cols; ++j)
        lv_add_col(c->hPrev, j + 1, pv->col_name[j] ? pv->col_name[j] : L"", 120);
    c->prev_cols = show_cols + 1;

    for (i = 0; i < pv->rows; ++i) {
        wchar_t num[16];
        swprintf(num, 16, L"%d", i + 1);
        lv_set_row(c->hPrev, i, 0, num);
        for (j = 0; j < show_cols; ++j)
            lv_set_row(c->hPrev, i, j + 1, pv->preview[i * pv->cols + j]);
    }
    {
        wchar_t colinfo[768];
        int k, used = 0, dup = 0;
        used += swprintf(colinfo, 768, L"列名：");
        for (k = 0; k < pv->cols && k < 24; ++k) {
            int w = swprintf(colinfo + used, 768 - (size_t)used, L"%ls%ls",
                             k ? L"、" : L"",
                             pv->col_name[k] ? pv->col_name[k] : L"");
            if (w < 0) break;
            used += w;
            if (pv->col_dup[k] > 1) ++dup;
        }
        if (pv->cols > 24) qp_strlcpy(colinfo + used, L"…", 768 - (size_t)used);
        if (pv->cols > show_cols)
            swprintf(colinfo + wcslen(colinfo), 768 - wcslen(colinfo),
                     L"（预览仅显示前 %d 列）", show_cols);
        /* 表头重名会让「按列名取列」有歧义：先说一句，免得规则悄悄取错列 */
        if (dup > 0)
            swprintf(colinfo + wcslen(colinfo), 768 - wcslen(colinfo),
                     L"　⚠ 表里有重名列，按列名取列会取到靠前的那一列，"
                     L"建议把表头改成不重名");
        colinfo[767] = L'\0';
        qp_set_text_if_diff(c->hCols, colinfo);
    }
    table_dlg_fit_cols(c, 1);           /* 列刚建好，按当前宽度铺一次 */
    qp_table_preview_free(pv);
}

static void table_dlg_load(TableDlgCtx *c)
{
    DataTableRef *r = table_dlg_ref(c);
    wchar_t info[1024];
    int on = (r != NULL);

    c->loading = 1;
    table_dlg_info_text(r, info, 1024, NULL, NULL);
    qp_set_text_if_diff(c->hInfo, info);

    EnableWindow(c->hEnc, on);
    EnableWindow(c->hDelim, on);
    EnableWindow(c->hHeader, on);
    EnableWindow(c->hRename, on);
    EnableWindow(c->hRemove, on);
    EnableWindow(c->hReload, on);
    EnableWindow(c->hOpen, on);

    if (r) {
        int i, sel = 0;
        for (i = 0; i < QP_TABLE_ENC_COUNT; ++i)
            if (kTableEncs[i] == r->encoding) sel = i;
        SendMessageW(c->hEnc, CB_SETCURSEL, (WPARAM)sel, 0);
        for (i = 0; i < QP_TABLE_DELIM_COUNT; ++i)
            if (kTableDelims[i] == r->delimiter) sel = i;
        SendMessageW(c->hDelim, CB_SETCURSEL, (WPARAM)sel, 0);
        SendMessageW(c->hHeader, BM_SETCHECK,
                     r->has_header ? BST_CHECKED : BST_UNCHECKED, 0);
    } else {
        SendMessageW(c->hEnc, CB_SETCURSEL, 0, 0);
        SendMessageW(c->hDelim, CB_SETCURSEL, 0, 0);
        SendMessageW(c->hHeader, BM_SETCHECK, BST_CHECKED, 0);
    }
    c->loading = 0;
    table_dlg_preview(c);
}

static void table_dlg_fill(TableDlgCtx *c, int select)
{
    DataTableRef *r;
    int i = 0;
    int was = c->loading;

    /* 灌列表会同步发回 LVN_ITEMCHANGED：DeleteAllItems 一条、每插一行再一条，
       而 WM_NOTIFY 收到它就 table_dlg_load() 一次 —— 结果是「刷新一次列表 →
       预览列表被拆掉重建 N+1 遍」，一屏闪。整段重建压在 loading 里，通知进来
       直接跳过，最后统一 load 一次。
       loading 必须恢复成进来时的值，不能直接写 0：嵌套调用会把外层的闸抹掉。 */
    c->loading = 1;
    ListView_DeleteAllItems(c->hList);
    for (r = c->app ? c->app->tables : NULL; r; r = r->next, ++i) {
        wchar_t scale[64], path[QP_PATH_MAX + 16];
        int rows = 0, cols = 0;
        if (qp_table_probe(r->id, &rows, &cols, NULL, 0))
            swprintf(scale, 64, L"%d 行 × %d 列", rows, cols);
        else
            qp_strlcpy(scale, L"无法读取", 64);
        swprintf(path, QP_PATH_MAX + 16, L"%ls", r->path);
        path[QP_PATH_MAX + 15] = L'\0';
        lv_set_row(c->hList, i, 0, r->name);
        lv_set_row(c->hList, i, 1, scale);
        lv_set_row(c->hList, i, 2, path);
    }
    if (i > 0) lv_select(c->hList, select);
    else {
        ListView_DeleteAllItems(c->hList);
        lv_select(c->hList, -1);
    }
    c->loading = was;
    table_dlg_load(c);
}

/* 导入：选择文件 → 先试读一遍（读不通当场说明原因）→ 命名 → 登记 */
static void table_dlg_import(TableDlgCtx *c)
{
    OPENFILENAMEW ofn;
    wchar_t path[MAX_PATH] = L"";
    wchar_t name[QP_NAME_MAX];
    wchar_t err[256];
    QpTablePreview *pv;
    DataTableRef *r;
    const wchar_t *base;
    size_t n;

    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = c->hwnd;
    ofn.lpstrFilter = L"数据表 (*.csv;*.tsv;*.txt)\0*.csv;*.tsv;*.txt\0"
                      L"所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"选择数据表文件（CSV / TSV）";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;

    pv = qp_table_preview(path, NULL, 3, err, 256);
    if (!pv) {
        qp_msgbox(c->hwnd, MB_OK | MB_ICONERROR, L"无法读取该文件",
                  L"%ls\n\n"
                  L"若该文件由 Excel 另存为「CSV」，请改用「CSV UTF-8」格式重新保存；"
                  L"也可先用记事本打开，确认内容确实是逗号或制表符分隔的文本表。",
                  err[0] ? err : L"原因未知");
        return;
    }

    /* 默认表名 = 文件名（去掉扩展名） */
    base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    qp_strlcpy(name, base, QP_NAME_MAX);
    n = wcslen(name);
    if (n > 4 && qp_icmp(name + n - 4, L".csv") == 0) name[n - 4] = L'\0';
    else if (n > 4 && qp_icmp(name + n - 4, L".tsv") == 0) name[n - 4] = L'\0';
    else if (n > 4 && qp_icmp(name + n - 4, L".txt") == 0) name[n - 4] = L'\0';
    if (!dlg_input(c->hwnd, L"数据表名称",
                   L"请设置数据表的显示名称。规则使用数据表引用关联，"
                   L"而非显示名称：",
                   name, QP_NAME_MAX))
        { qp_table_preview_free(pv); return; }
    qp_trim(name);
    if (!name[0]) qp_strlcpy(name, L"数据表", QP_NAME_MAX);

    /* 同一个文件已经导入过：询问是更新还是再登记一张 */
    r = NULL;
    {
        DataTableRef *it;
        for (it = c->app->tables; it; it = it->next) {
            if (qp_icmp(it->path, path) == 0) { r = it; break; }
        }
    }
    if (r) {
        if (qp_msgbox(c->hwnd, MB_YESNO | MB_ICONQUESTION, L"该文件已导入",
                      L"「%ls」当前引用的就是该文件。\n\n"
                      L"「是」：更新当前引用的名称与读取设置\n"
                      L"「否」：创建另一引用（两个引用指向同一文件）",
                      r->name) != IDYES) {
            r = NULL;
        }
    }
    if (!r) {
        r = dtref_create(name, path);
        if (!r) { qp_table_preview_free(pv); return; }
        dtref_list_append(&c->app->tables, r);
    }
    qp_strlcpy(r->name, name, QP_NAME_MAX);
    qp_strlcpy(r->path, path, QP_PATH_MAX);
    r->encoding = pv->encoding;         /* 用识别结果：下次读同一个文件更稳 */
    r->delimiter = pv->delimiter;
    r->has_header = pv->has_header;
    qp_table_preview_free(pv);

    qp_table_set_refs(c->app->tables);
    qp_table_invalidate_all();
    c->result = 1;
    table_dlg_fill(c, dtref_list_count(c->app->tables) - 1);
}

/* 这张表被哪些查表规则用着：删表之前得说一句 */
static int table_dlg_used_by(const AppData *app, const wchar_t *table_id)
{
    const Template *t;
    int n = 0;
    for (t = app ? app->templates : NULL; t; t = t->next) {
        const Segment *s;
        for (s = t->segments; s; s = s->next) {
            const ParseRule *r;
            for (r = s->parse_rules; r; r = r->next)
                if (r->type == PARSE_LOOKUP && wcscmp(r->lk_table_id, table_id) == 0)
                    ++n;
        }
    }
    return n;
}

static void table_dlg_apply_settings(TableDlgCtx *c)
{
    DataTableRef *r = table_dlg_ref(c);
    int idx;
    if (!r || c->loading) return;
    idx = (int)SendMessageW(c->hEnc, CB_GETCURSEL, 0, 0);
    if (idx >= 0 && idx < QP_TABLE_ENC_COUNT && kTableEncs[idx] != r->encoding) {
        r->encoding = kTableEncs[idx];
        c->result = 1;
    }
    idx = (int)SendMessageW(c->hDelim, CB_GETCURSEL, 0, 0);
    if (idx >= 0 && idx < QP_TABLE_DELIM_COUNT && kTableDelims[idx] != r->delimiter) {
        r->delimiter = kTableDelims[idx];
        c->result = 1;
    }
    {
        int hdr = (SendMessageW(c->hHeader, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
        if (hdr != r->has_header) { r->has_header = hdr; c->result = 1; }
    }
    qp_table_invalidate(r->id);
}

/* 一次性摆好所有控件。窗口被拉大缩小时再调一次即可（控件已存在就只挪位置）。 */
static void table_dlg_layout(TableDlgCtx *c)
{
    HWND hwnd = c->hwnd;
    RECT rc;
    int m = DPIX(14), y = DPIX(14), full, bw = DPIX(104), lw = DPIX(64);
    int cx, cw, i;

    qp_dialog_content_rect(hwnd, &rc);
    full = rc.right - m * 2;
    cx = m + lw + DPIX(8);
    cw = rc.right - m - cx;

    if (!c->hList) {
        c->hLblList = mk_label(hwnd,
            L"数据表引用（文件更新会在下次查询时检测；可手动重新加载）：", 0);
        c->hList = mk_list(hwnd, IDC_DT_LIST, 0);
        lv_add_col(c->hList, 0, L"表名", 150);
        lv_add_col(c->hList, 1, L"规模", 110);
        lv_add_col(c->hList, 2, L"文件", 300);

        c->hImport = mk_btn(hwnd, L"导入 CSV/TSV…", IDC_DT_IMPORT);
        c->hRename = mk_btn(hwnd, L"重命名…", IDC_DT_RENAME);
        c->hReload = mk_btn(hwnd, L"重新加载", IDC_DT_RELOAD);
        c->hOpen   = mk_btn(hwnd, L"指定文件…", IDC_DT_OPEN);
        c->hRemove = mk_btn(hwnd, L"删除", IDC_DT_REMOVE);

        c->hLblRead = mk_label(hwnd, L"数据表读取设置（解析结果不正确时可调整）：", 0);
        c->hLblEnc = mk_label_r(hwnd, L"编码：", 0);
        c->hEnc = mk_combo(hwnd, IDC_DT_ENC);
        for (i = 0; i < QP_TABLE_ENC_COUNT; ++i)
            SendMessageW(c->hEnc, CB_ADDSTRING, 0,
                         (LPARAM)qp_table_encoding_name(kTableEncs[i]));
        c->hLblDelim = mk_label_r(hwnd, L"分隔符：", 0);
        c->hDelim = mk_combo(hwnd, IDC_DT_DELIM);
        for (i = 0; i < QP_TABLE_DELIM_COUNT; ++i)
            SendMessageW(c->hDelim, CB_ADDSTRING, 0,
                         (LPARAM)qp_table_delim_name(kTableDelims[i]));
        c->hHeader = mk_check(hwnd, L"第一行是列名（表头）", IDC_DT_HEADER);

        c->hInfo=qp_create(L"Edit",L"",ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL|WS_TABSTOP,
                             WS_EX_CLIENTEDGE,0,0,10,10,hwnd,IDC_DT_INFO);

        {
            wchar_t cap[64];
            swprintf(cap, 64, L"前 %d 行预览：", QP_DT_PREVIEW_ROWS);
            c->hLblPrev = mk_label(hwnd, cap, 0);
        }
        c->hCols = mk_label(hwnd, L"", IDC_DT_COLS);
        c->hPrev = mk_list(hwnd, IDC_DT_PREV, 0);
        mk_btn(hwnd, L"关闭", IDC_DT_OK);
    }

    /* 第一段：列表与按钮 */
    HDWP batch=qp_layout_begin(24);
    batch=qp_dialog_defer(batch,c->hLblList,m,y,full,DPIX(18));
    y += DPIX(22);
    batch=qp_dialog_defer(batch,c->hList,m,y,full,DPIX(112));
    {
        /* 改列宽会连带重排重画整张列表，值没变就别碰它 */
        int want = full - DPIX(272);
        if (want < DPIX(60)) want = DPIX(60);
        if (want != ListView_GetColumnWidth(c->hList, 2))
            ListView_SetColumnWidth(c->hList, 2, want);
    }
    y += DPIX(120);
    {
        HWND b[5];
        b[0] = c->hImport; b[1] = c->hRename; b[2] = c->hReload;
        b[3] = c->hOpen;   b[4] = c->hRemove;
        for (i = 0; i < 5; ++i)
            batch=qp_dialog_defer(batch,b[i],m+(bw+DPIX(8))*i,y,bw,DPIX(26));
    }
    y += DPIX(34);

    /* 第二段：怎么读这张表 */
    batch=qp_dialog_defer(batch,c->hLblRead,m,y,full,DPIX(18));
    y += DPIX(22);
    batch=qp_dialog_defer(batch,c->hLblEnc,m,y+DPIX(4),lw,DPIX(18));
    batch=qp_dialog_defer(batch,c->hEnc,cx,y,DPIX(120),DPIX(24));
    batch=qp_dialog_defer(batch,c->hLblDelim,cx+DPIX(128),y+DPIX(4),lw,DPIX(18));
    batch=qp_dialog_defer(batch,c->hDelim,cx+DPIX(200),y,DPIX(110),DPIX(24));
    batch=qp_dialog_defer(batch,c->hHeader,cx+DPIX(318),y+DPIX(3),cw-DPIX(318),DPIX(20));
    y += DPIX(30);

    /* 第三段：规模 / 列名 / 前几行 */
    c->y_info = y;
    batch=qp_dialog_defer(batch,c->hInfo,m,y,full,DPIX(34));
    y += DPIX(38);
    c->y_cols = y;
    batch=qp_dialog_defer(batch,c->hLblPrev,m,y,full,DPIX(18));
    batch=qp_dialog_defer(batch,c->hCols,m,y+DPIX(20),full,DPIX(18));
    y += DPIX(42);

    c->y_prev = y;
    {
        int avail = rc.bottom - m - DPIX(34) - y;
        batch=qp_dialog_defer(batch,c->hPrev,m,y,full,avail>DPIX(60)?avail:DPIX(60));
    }
    batch=qp_dialog_defer(batch,GetDlgItem(hwnd,IDC_DT_OK),rc.right-m-DPIX(96),rc.bottom-m-DPIX(26),DPIX(96),DPIX(26));
    qp_layout_end(batch);
    qp_dialog_update_view(hwnd);
}

static LRESULT CALLBACK table_dlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    TableDlgCtx *c = (TableDlgCtx *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    int handled = 0;
    LRESULT r;

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        c = (TableDlgCtx *)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)c);
        c->hwnd = hwnd;
        qp_table_set_refs(c->app ? c->app->tables : NULL);
        table_dlg_layout(c);
        qp_theme_children(hwnd);
        table_dlg_fill(c, 0);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        /* 能拉大就得有个下限：再小那一排五个按钮就挤出客户区，预览也没地方放 */
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        RECT rc;
        rc.left = 0; rc.top = 0;
        rc.right = DPIX(DT_MIN_W);
        rc.bottom = DPIX(DT_MIN_H);
        AdjustWindowRectEx(&rc, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE,
                           (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
        mmi->ptMinTrackSize.x = rc.right - rc.left;
        mmi->ptMinTrackSize.y = rc.bottom - rc.top;
        qp_dialog_limit_tracking(hwnd,mmi);
        return 0;
    }

    case WM_ENTERSIZEMOVE:
        if (c) c->sizing = 1;
        break;

    case WM_EXITSIZEMOVE:
        if (c) {
            c->sizing = 0;
            table_dlg_fit_cols(c, 1);   /* 拖完了按最终宽度铺一次 */
        }
        break;

    case WM_SIZE: {
        int cw = (int)LOWORD(lp), ch = (int)HIWORD(lp);
        if (!c || !c->hList) return 0;
        if (cw == c->last_w && ch == c->last_h) return 0;  /* 尺寸没变：一点活都不做 */
        c->last_w = cw;
        c->last_h = ch;
        /* 一条拖拽会来几十上百条 WM_SIZE。重排控件本身不贵，贵的是每次都跟着
           重画一遍，所以把中间过程捂住，最后统一失效一次让系统合并重画。
           收尾这一次必须带 RDW_ERASE：本窗口类没有背景刷、本窗口也没有
           WM_PAINT，整片背景只由 dlg_common 的 WM_ERASEBKGND 来涂；只
           INVALIDATE 不 ERASE 时系统根本不会发 WM_ERASEBKGND，于是拉大窗口
           时新露出来的那圈底没人画，屏幕上留着上一帧的残影（实测：拉大后
           右下角采样到的是纯白，而不是主题背景色）。 */
        SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
        table_dlg_layout(c);
        if (!c->sizing) table_dlg_fit_cols(c, 0);
        SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
        RedrawWindow(hwnd, NULL, NULL,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        return 0;
    }

    case WM_COMMAND:
        qp_combo_dropdown_fix(wp, lp);
        if (!c) break;
        switch (LOWORD(wp)) {
        case IDC_DT_IMPORT:
            table_dlg_import(c);
            return 0;
        case IDC_DT_RENAME: {
            DataTableRef *r2 = table_dlg_ref(c);
            wchar_t name[QP_NAME_MAX];
            if (!r2) return 0;
            qp_strlcpy(name, r2->name, QP_NAME_MAX);
            if (!dlg_input(hwnd, L"重命名数据表", L"新的名称：", name, QP_NAME_MAX))
                return 0;
            qp_trim(name);
            if (!name[0]) return 0;
            qp_strlcpy(r2->name, name, QP_NAME_MAX);
            c->result = 1;
            table_dlg_fill(c, lv_sel(c->hList));
            return 0;
        }
        case IDC_DT_RELOAD: {
            DataTableRef *r2 = table_dlg_ref(c);
            if (!r2) return 0;
            qp_table_invalidate(r2->id);
            table_dlg_fill(c, lv_sel(c->hList));
            return 0;
        }
        case IDC_DT_OPEN: {
            DataTableRef *r2 = table_dlg_ref(c);
            OPENFILENAMEW ofn;
            wchar_t path[MAX_PATH];
            if (!r2) return 0;
            memset(&ofn, 0, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"数据表 (*.csv;*.tsv;*.txt)\0*.csv;*.tsv;*.txt\0"
                              L"所有文件 (*.*)\0*.*\0";
            qp_strlcpy(path, r2->path, MAX_PATH);
            ofn.lpstrFile = path;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrTitle = L"指定该数据表的新位置";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
            if (!GetOpenFileNameW(&ofn)) return 0;
            qp_strlcpy(r2->path, path, QP_PATH_MAX);
            qp_table_invalidate(r2->id);
            c->result = 1;
            table_dlg_fill(c, lv_sel(c->hList));
            return 0;
        }
        case IDC_DT_REMOVE: {
            DataTableRef *r2 = table_dlg_ref(c);
            wchar_t id[QP_ID_MAX], tname[QP_NAME_MAX];
            int used;
            if (!r2) return 0;
            qp_strlcpy(id, r2->id, QP_ID_MAX);
            qp_strlcpy(tname, r2->name, QP_NAME_MAX);
            used = table_dlg_used_by(c->app, id);
            if (qp_msgbox(hwnd, MB_YESNO | MB_ICONWARNING, L"删除数据表",
                          used > 0
                            ? L"有 %d 条查表规则正在引用「%ls」，删除后这些规则"
                              L"将不会生效（无法取值）。\n\n确定删除该数据表的引用吗？"
                              L"（磁盘上的文件不会被删除）"
                            : L"确定不再引用「%ls」吗？（磁盘上的文件不会被删除）",
                          used, tname) != IDYES)
                return 0;
            qp_table_invalidate(id);
            dtref_list_remove(&c->app->tables, id);
            qp_table_set_refs(c->app->tables);
            c->result = 1;
            table_dlg_fill(c, 0);
            return 0;
        }
        case IDC_DT_ENC:
        case IDC_DT_DELIM:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                table_dlg_apply_settings(c);
                table_dlg_load(c);
            }
            return 0;
        case IDC_DT_HEADER:
            table_dlg_apply_settings(c);
            table_dlg_load(c);
            return 0;
        case IDC_DT_OK:
        case IDCANCEL:
            if (c->result) {
                qp_table_set_refs(c->app->tables);
                qp_table_invalidate_all();
            }
            dlg_close(hwnd);
            return 0;
        default:
            break;
        }
        break;

    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        /* 只在「用户真的换了选中行」时重载预览：列表重建期间同步发回来的
           LVN_ITEMCHANGED 不算用户操作（见 table_dlg_fill 的说明）。 */
        if (c && !c->loading && nh && nh->idFrom == IDC_DT_LIST &&
            nh->code == LVN_ITEMCHANGED)
            table_dlg_load(c);
        break;
    }

    case WM_CLOSE:
        dlg_close(hwnd);
        return 0;

    default:
        break;
    }
    r = dlg_common(hwnd, msg, wp, lp, &handled);
    if (handled) return r;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int dlg_data_tables(HWND owner, AppData *app)
{
    TableDlgCtx c;
    HWND dlg;
    memset(&c, 0, sizeof(c));
    c.app = app;
    dlg = make_dlg_ex(L"QuickPhraseTables", table_dlg_proc, L"数据表",
                      760, 560, owner, &c, WS_THICKFRAME | WS_MAXIMIZEBOX);
    return run_modal(owner, dlg, &c.result);
}
