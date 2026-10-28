#include "editor_internal.h"
/* ================================================================== */
/* 模板编辑器                                                          */
/* ================================================================== */

/* 设计尺寸 = 能缩到的最小尺寸；窗口可以往大拉，多出来的空间给列表和预览。
   长模板（上百个片段）在这里编辑时，能拉大 + 预览能拖高才看得过来。 */




/* ------------------- 编辑器的可变几何（与主窗口同一套做法） -------- */

Segment *ed_cur_seg(EditorCtx *c)
{
    return seg_list_at(c->t->segments, c->cur);
}

static EvalContext *ed_preview_context(EditorCtx *c)
{
    EvalContext *ctx=c->previewCtx;
    const Segment *s=c->t->segments;
    int i=0;
    if(ctx) {
        while(s && i<ctx->count && ctx->fields[i].seg==s){s=s->next;++i;}
        if(s || i!=ctx->count){eval_destroy(ctx);ctx=NULL;}
    }
    if(!ctx)ctx=eval_create(c->t);
    else eval_reset(ctx);
    c->previewCtx=ctx;
    return ctx;
}

static void ed_update_preview(EditorCtx *c)
{
    wchar_t out[QP_OUTPUT_MAX];
    int marks[256];
    int nmarks = 0;
    EvalContext *ctx;
    int table, batch;

    if (!c || !c->hPreview) return;
    out[0] = L'\0';
    table = (c->previewMode == OUTPUT_TABLE) &&
            c->t->table_output_enabled && c->t->table_output_format[0];
    batch = c->t->batch_mode ? 1 : 0;

    ctx = ed_preview_context(c);
    if (ctx) {
        QpRenderResult result=qp_eval_render_checked(ctx,table?OUTPUT_TABLE:OUTPUT_TEXT,batch,out,QP_OUTPUT_MAX,marks,256);
        nmarks=result.mark_count;
        if(result.status==QP_RENDER_NOMEM) {
            qp_set_text_if_diff(c->hPreviewLbl,L"预览更新失败（内存不足，原预览保持）");
            return;
        }
        qp_set_text_if_diff(c->hPreviewLbl,result.status==QP_RENDER_TRUNCATED?L"预览不完整（部分内容达到长度上限）":L"默认值预览");
    } else {
        qp_set_text_if_diff(c->hPreviewLbl,L"预览更新失败（内存不足，原预览保持）");
        return;
    }
    /* 表格 / 文本共用一个自绘控件（与主界面同款样式） */
    int published=table
        ? qp_preview_set_table_marked(c->hPreview,out,c->t->table_output_format,marks,nmarks)
        : qp_preview_set_text(c->hPreview,out,marks,nmarks);
    if(!published)qp_set_text_if_diff(c->hPreviewLbl,L"预览更新失败（内存不足，原预览保持）");
}

/* 片段列表里显示的摘要；隐藏 / 多行 / 时间格式 / 引用都带个标记，
   一眼能看出这个变量的形态，不用逐个点开属性区确认 */
static void ed_seg_summary(const Segment *s, const Template *t, wchar_t *out, size_t cap)
{
    wchar_t *p;
    size_t n;
    if (!out || cap == 0) return;
    out[0] = L'\0';
    if (!s) return;
    if (s->type == SEG_STATIC) {
        qp_strlcpy(out, s->content, cap);
    } else {
        _snwprintf(out, cap, L"【%ls】%ls%ls%ls", seg_display_name(s),
                   s->hidden ? L"（隐藏）" : L"",
                   s->no_output ? L"（不计入）" : L"",
                   (s->type == SEG_TEXT && s->multiline) ? L"（多行）" : L"");
        out[cap - 1] = L'\0';
        n = wcslen(out);
        if (s->type == SEG_DATETIME && n + 1 < cap) {
            _snwprintf(out + n, cap - n, L"（%ls）",
                       s->time_format[0] ? s->time_format : qp_time_format_at(0));
            out[cap - 1] = L'\0';
            n = wcslen(out);
        }
        if (s->ref_segment_id[0] && n + 1 < cap) {
            wchar_t sum[256];
            seg_ref_summary(s, t, sum, 256);
            _snwprintf(out + n, cap - n, L"（%ls）", sum);
            out[cap - 1] = L'\0';
        }
    }
    /* 列表里不显示换行 */
    for (p = out; *p; ++p) if (*p == L'\r' || *p == L'\n') *p = L' ';
}

static void ed_fill_seglist(EditorCtx *c, int select_index)
{
    Segment *s;
    int i = 0;
    SendMessageW(c->hSegList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(c->hSegList);
    for (s = c->t->segments; s; s = s->next, ++i) {
        wchar_t num[16], summary[QP_CONTENT_MAX];
        _snwprintf(num, 16, L"%d", i + 1);
        ed_seg_summary(s, c->t, summary, QP_CONTENT_MAX);
        lv_set_row(c->hSegList, i, 0, num);
        lv_set_row(c->hSegList, i, 1, seg_type_name(s->type));
        lv_set_row(c->hSegList, i, 2, summary);
    }
    SendMessageW(c->hSegList, WM_SETREDRAW, TRUE, 0);
    if (i > 0) lv_select(c->hSegList, select_index);
}

static void ed_fill_optlist(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    int i;
    ListView_DeleteAllItems(c->hOptList);
    if (!s) return;
    for (i = 0; i < s->option_count; ++i) {
        wchar_t label[QP_VALUE_MAX+32];
        if(i==qp_choice_custom_index(s)) {
            _snwprintf(label,QP_VALUE_MAX+32,L"%ls（自主补充）",s->options[i]);
            label[QP_VALUE_MAX+31]=0;
            lv_set_row(c->hOptList,i,0,label);
        } else lv_set_row(c->hOptList,i,0,s->options[i]);
    }
}

static void ed_fill_rulelists(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    ParseRule *pr;
    ConditionRule *cr;
    int i = 0;

    ListView_DeleteAllItems(c->hParseList);
    ListView_DeleteAllItems(c->hCondList);
    if (!s) return;
    for (pr = s->parse_rules; pr; pr = pr->next, ++i) {
        wchar_t txt[512];
        parse_rule_summary(pr, c->t, txt, 512);
        lv_set_row(c->hParseList, i, 0, txt);
    }
    i = 0;
    for (cr = s->condition_rules; cr; cr = cr->next, ++i) {
        wchar_t txt[512];
        cond_rule_summary(cr, c->t, txt, 512);
        lv_set_row(c->hCondList, i, 0, txt);
    }
}

/* 时间格式的实时效果：拿此刻的时间套一遍格式，改一个字就能看到结果 */
static void ed_update_fmt_sample(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    wchar_t sample[160], line[192];
    if (!s || s->type != SEG_DATETIME) {
        qp_set_text_if_diff(c->hFmtSample, L"");
        return;
    }
    qp_format_time_now(s->time_format, sample, 160);
    _snwprintf(line, 192, L"效果：%ls", sample);
    line[191] = L'\0';
    qp_set_text_if_diff(c->hFmtSample, line);
}

/* 时间格式：从下拉里选一个，或者自己敲，两条路都落到这里 */
static void ed_commit_format(EditorCtx *c, int from_list)
{
    Segment *s = ed_cur_seg(c);
    wchar_t buf[QP_FORMAT_MAX];
    DWORD start=0, end=0;
    if (!s || c->loading || s->type != SEG_DATETIME) return;
    if(!from_list)SendMessageW(c->hFmt,CB_GETEDITSEL,(WPARAM)&start,(LPARAM)&end);
    buf[0] = L'\0';
    if (from_list) {
        /* CBN_SELCHANGE 时编辑框里还是旧文字，得从列表项取 */
        int sel = (int)SendMessageW(c->hFmt, CB_GETCURSEL, 0, 0);
        if (sel != CB_ERR &&
            SendMessageW(c->hFmt, CB_GETLBTEXTLEN, (WPARAM)sel, 0) < QP_FORMAT_MAX)
            SendMessageW(c->hFmt, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)buf);
    } else {
        qp_get_text(c->hFmt, buf, QP_FORMAT_MAX);
    }
    qp_strlcpy(s->time_format, buf, QP_FORMAT_MAX);
    ++c->loading;
    ed_update_fmt_sample(c);
    ed_layout(c);
    ed_update_preview(c);
    /* Native editable ComboBox reflow can reset its child edit selection.
       Keep the user's post-keystroke caret/range; preset selection stays native. */
    if(!from_list)SendMessageW(c->hFmt,CB_SETEDITSEL,0,MAKELPARAM(start,end));
    --c->loading;
}

/* 引用来源下拉：第 0 项固定「不引用」，其余是本模板里除自己以外的变量。
   会形成环的来源照样列出来，选中时再拦——列表里凭空少几项，人只会以为是 bug。
   顺手做一件事：来源已经被删掉（悬空）时，把引用清掉，界面与数据保持一个说法。 */
static void ed_fill_ref_combo(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    const Segment *p;
    int i = 1, sel = 0;
    SendMessageW(c->hRef, CB_RESETCONTENT, 0, 0);
    SendMessageW(c->hRef, CB_ADDSTRING, 0, (LPARAM)L"（不引用）");
    SendMessageW(c->hRef, CB_SETITEMDATA, 0, (LPARAM)NULL);
    if (s) {
        for (p = c->t->segments; p; p = p->next) {
            if (p == s || p->type == SEG_STATIC) continue;
            SendMessageW(c->hRef, CB_ADDSTRING, 0, (LPARAM)seg_display_name(p));
            SendMessageW(c->hRef, CB_SETITEMDATA, (WPARAM)i, (LPARAM)p);
            if (wcscmp(s->ref_segment_id, p->id) == 0) sel = i;
            ++i;
        }
        if (s->ref_segment_id[0] && sel == 0) s->ref_segment_id[0] = L'\0';
    }
    SendMessageW(c->hRef, CB_SETCURSEL, (WPARAM)sel, 0);
}

/* 取法下拉 + 两个参数框：按当前取法填内容与占位提示 */
static void ed_load_ref_params(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    int save = c->loading;
    wchar_t buf[32];
    c->loading = 1;
    if (s && s->ref_segment_id[0]) {
        SendMessageW(c->hRefMode, CB_SETCURSEL, (WPARAM)s->ref_mode, 0);
        if (s->ref_mode == REF_SPLIT) {
            SendMessageW(c->hRefA, EM_SETCUEBANNER, 0, (LPARAM)L"分隔符");
            SendMessageW(c->hRefB, EM_SETCUEBANNER, 0, (LPARAM)L"第几段");
            SetWindowTextW(c->hRefA, s->ref_delimiter);
            _snwprintf(buf, 32, L"%d", s->ref_index > 0 ? s->ref_index : 1);
            SetWindowTextW(c->hRefB, buf);
        } else if (s->ref_mode == REF_SUBSTR) {
            SendMessageW(c->hRefA, EM_SETCUEBANNER, 0, (LPARAM)L"从第几个字");
            SendMessageW(c->hRefB, EM_SETCUEBANNER, 0, (LPARAM)L"取几个（空=到末尾）");
            _snwprintf(buf, 32, L"%d", s->ref_start > 0 ? s->ref_start : 1);
            SetWindowTextW(c->hRefA, buf);
            if (s->ref_len > 0) {
                _snwprintf(buf, 32, L"%d", s->ref_len);
                SetWindowTextW(c->hRefB, buf);
            } else {
                SetWindowTextW(c->hRefB, L"");
            }
        } else {
            SetWindowTextW(c->hRefA, L"");
            SetWindowTextW(c->hRefB, L"");
        }
    } else {
        SendMessageW(c->hRefMode, CB_SETCURSEL, 0, 0);
        SetWindowTextW(c->hRefA, L"");
        SetWindowTextW(c->hRefB, L"");
    }
    c->loading = save;
}

/* 批量源变量下拉：第一项固定「（未选）」，其后列出本模板的「文本输入」变量
   —— 多行粘贴只有文本变量装得下。选中当前源；源已删除 / 改成别的类型时回落到
   「（未选）」（保存时再把悬空 id 一并清掉，与引用悬空同一口径）。 */
static void ed_fill_batch_combo(EditorCtx *c)
{
    Segment *s;
    int i = 1, sel = 0;
    SendMessageW(c->hBatchSrc, CB_RESETCONTENT, 0, 0);
    SendMessageW(c->hBatchSrc, CB_ADDSTRING, 0, (LPARAM)L"（未选）");
    SendMessageW(c->hBatchSrc, CB_SETITEMDATA, 0, (LPARAM)NULL);
    for (s = c->t->segments; s; s = s->next) {
        wchar_t item[QP_LABEL_MAX + 32];
        if (s->type != SEG_TEXT) continue;
        _snwprintf(item, QP_LABEL_MAX + 32, L"%ls", seg_display_name(s));
        item[QP_LABEL_MAX + 31] = L'\0';
        SendMessageW(c->hBatchSrc, CB_ADDSTRING, 0, (LPARAM)item);
        SendMessageW(c->hBatchSrc, CB_SETITEMDATA, (WPARAM)i, (LPARAM)s);
        if (c->t->batch_source_id[0] && wcscmp(s->id, c->t->batch_source_id) == 0)
            sel = i;
        ++i;
    }
    SendMessageW(c->hBatchSrc, CB_SETCURSEL, (WPARAM)sel, 0);
}

/* 表格格式按变量名展开；保存前已有重名检查，这里保留每个可引用变量供插入。 */
static void ed_fill_table_combo(EditorCtx *c)
{
    Segment *s;
    const Segment *selected = combo_selected_seg(c->hTableInsert);
    int i = 0, sel = -1;
    SendMessageW(c->hTableInsert, CB_RESETCONTENT, 0, 0);
    for (s = c->t->segments; s; s = s->next) {
        if (s->type == SEG_STATIC || !s->label[0]) continue;
        SendMessageW(c->hTableInsert, CB_ADDSTRING, 0, (LPARAM)s->label);
        SendMessageW(c->hTableInsert, CB_SETITEMDATA, (WPARAM)i, (LPARAM)s);
        if (s == selected) sel = i;
        ++i;
    }
    if (sel < 0 && i > 0) sel = 0;
    if (sel >= 0) SendMessageW(c->hTableInsert, CB_SETCURSEL, (WPARAM)sel, 0);
}

static void ed_commit_table(EditorCtx *c)
{
    wchar_t format[QP_OUTPUT_MAX];
    c->t->table_output_enabled =
        SendMessageW(c->hTableOutput, BM_GETCHECK, 0, 0) == BST_CHECKED;
    qp_get_text(c->hTableFmt, format, QP_OUTPUT_MAX);
    qp_strlcpy(c->t->table_output_format, format, QP_OUTPUT_MAX);
}

/* 开关只控制表格作者工具；关闭时预览必须立即回到普通文本。 */
static void ed_sync_table(EditorCtx *c)
{
    int on = SendMessageW(c->hTableOutput, BM_GETCHECK, 0, 0) == BST_CHECKED;
    EnableWindow(c->hTableFmtLbl, on);
    EnableWindow(c->hTableFmt, on);
    EnableWindow(c->hTableInsert, on);
    EnableWindow(c->hTableInsertBtn, on);
    EnableWindow(c->hTableCellBtn, on);
    EnableWindow(c->hPreviewTable, on);
    if (!on) {
        c->previewMode = OUTPUT_TEXT;
        SendMessageW(c->hPreviewText, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(c->hPreviewTable, BM_SETCHECK, BST_UNCHECKED, 0);
    }
    if (c->loading) return;
    ed_commit_table(c);
    ed_update_preview(c);
}

static void ed_table_format_changed(EditorCtx *c)
{
    if (c->loading) return;
    ed_commit_table(c);                 /* 先写 working clone，再刷新预览 */
    ed_update_preview(c);
}

static void ed_insert_table_text(EditorCtx *c, const wchar_t *text)
{
    DWORD start=0, end=0;
    size_t length=(size_t)GetWindowTextLengthW(c->hTableFmt), added=wcslen(text);
    SendMessageW(c->hTableFmt,EM_GETSEL,(WPARAM)&start,(LPARAM)&end);
    if(start>length)start=(DWORD)length;
    if(end>length)end=(DWORD)length;
    if(length-(end-start)+added >= QP_OUTPUT_MAX) {
        qp_msgbox(c->hwnd,MB_OK|MB_ICONWARNING,L"无法插入",
                  L"表格格式已达到长度上限。请缩短格式内容后重试。");
        return;
    }
    SendMessageW(c->hTableFmt, EM_REPLACESEL, TRUE, (LPARAM)text);
    ed_table_format_changed(c);
    SetFocus(c->hTableFmt);
}

static void ed_insert_table_variable(EditorCtx *c)
{
    const Segment *s = combo_selected_seg(c->hTableInsert);
    wchar_t text[QP_LABEL_MAX + 3];
    if (!s || !s->label[0]) return;
    _snwprintf(text, QP_LABEL_MAX + 3, L"{%ls}", s->label);
    text[QP_LABEL_MAX + 2] = L'\0';
    ed_insert_table_text(c,text);
}

static void ed_load_props(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    c->loading = 1;
    if (s) {
        SendMessageW(c->hType, CB_SETCURSEL, (WPARAM)s->type, 0);
        SetWindowTextW(c->hLabel, s->label);
        SetWindowTextW(c->hContent, s->content);
        SetWindowTextW(c->hPlace, s->placeholder);
        SetWindowTextW(c->hDef, s->default_value);
        SetWindowTextW(c->hSep, s->separator);
        SendMessageW(c->hHidden, BM_SETCHECK,
                     s->hidden ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(c->hNoOutput, BM_SETCHECK,
                     s->no_output ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(c->hMultiline, BM_SETCHECK,
                     s->multiline ? BST_CHECKED : BST_UNCHECKED, 0);
        SetWindowTextW(c->hFmt, s->time_format);
    } else {
        SendMessageW(c->hType, CB_SETCURSEL, (WPARAM)-1, 0);
        SetWindowTextW(c->hLabel, L"");
        SetWindowTextW(c->hContent, L"");
        SetWindowTextW(c->hPlace, L"");
        SetWindowTextW(c->hDef, L"");
        SetWindowTextW(c->hSep, L"");
        SendMessageW(c->hHidden, BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessageW(c->hNoOutput, BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessageW(c->hMultiline, BM_SETCHECK, BST_UNCHECKED, 0);
        SetWindowTextW(c->hFmt, L"");
    }
    ed_update_fmt_sample(c);
    ed_fill_ref_combo(c);
    ed_fill_batch_combo(c);          /* 变量增删 / 改类型后，源下拉随之刷新 */
    ed_fill_table_combo(c);          /* 变量改名、增删或改类型后，插入列表同步 */
    ed_load_ref_params(c);
    ed_fill_optlist(c);
    ed_fill_rulelists(c);
    ed_layout(c);
    c->loading = 0;
}

static void ed_commit_props(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    /* 一个缓冲喂所有属性框：容量取最大的那个（默认值可以是多行大段文字） */
    wchar_t buf[QP_VALUE_MAX > QP_CONTENT_MAX ? QP_VALUE_MAX : QP_CONTENT_MAX];
    if (!s || c->loading) return;
    qp_get_text(c->hLabel, buf, QP_LABEL_MAX);
    qp_strlcpy(s->label, buf, QP_LABEL_MAX);
    qp_get_text(c->hContent, buf, QP_CONTENT_MAX);
    qp_strlcpy(s->content, buf, QP_CONTENT_MAX);
    qp_get_text(c->hPlace, buf, QP_PLACEHOLD_MAX);
    qp_strlcpy(s->placeholder, buf, QP_PLACEHOLD_MAX);
    qp_get_text(c->hDef, buf, QP_VALUE_MAX);
    qp_strlcpy(s->default_value, buf, QP_VALUE_MAX);
    qp_get_text(c->hSep, buf, QP_SEP_MAX);
    qp_strlcpy(s->separator, buf, QP_SEP_MAX);
    if (s->type == SEG_DATETIME) {
        qp_get_text(c->hFmt, buf, QP_FORMAT_MAX);
        qp_strlcpy(s->time_format, buf, QP_FORMAT_MAX);
    }
    s->hidden = (s->type != SEG_STATIC &&
                 SendMessageW(c->hHidden, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    s->no_output = (s->type != SEG_STATIC &&
                    SendMessageW(c->hNoOutput, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    s->multiline = (s->type == SEG_TEXT &&
                    SendMessageW(c->hMultiline, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    ed_update_preview(c);
}

/* 片段列表某行的文字随属性变化刷新 */
static void ed_refresh_cur_row(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    wchar_t summary[QP_CONTENT_MAX];
    if (!s) return;
    ed_seg_summary(s, c->t, summary, QP_CONTENT_MAX);
    lv_set_row(c->hSegList, c->cur, 1, seg_type_name(s->type));
    lv_set_row(c->hSegList, c->cur, 2, summary);
}

/* ------------------------------ 引用 ------------------------------ */

/* 换引用来源。会成环的选择直接拒绝：A 引用 B、B 又引用 A，两边都算不出值，
   与「条件项指向不存在的变量一律不成立」同一个态度——宁可不生效，不能乱生效。 */
static void ed_ref_changed(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    const Segment *pick;
    if (!s || c->loading) return;
    pick = combo_selected_seg(c->hRef);
    if (pick && tmpl_ref_would_cycle(c->t, s, pick->id)) {
        qp_msgbox(c->hwnd, MB_OK | MB_ICONWARNING, L"不能这样引用",
                  L"变量「%ls」已直接或间接引用「%ls」。此操作将形成循环引用，"
                  L"无法求值；请选择其他来源。", seg_display_name(pick),
                  seg_display_name(s));
        ed_fill_ref_combo(c);            /* 退回原来的选择 */
        return;
    }
    qp_strlcpy(s->ref_segment_id, pick ? pick->id : L"", QP_ID_MAX);
    if (s->ref_segment_id[0]) {
        if (!s->ref_delimiter[0]) qp_strlcpy(s->ref_delimiter, L"-", QP_SEP_MAX);
        if (s->ref_index < 1) s->ref_index = 1;
        if (s->ref_start < 1) s->ref_start = 1;
    }
    ed_load_ref_params(c);
    ed_layout(c);                        /* 「取法」那一行随之出现 / 收起 */
    ed_refresh_cur_row(c);
    ed_update_preview(c);
}

static void ed_ref_mode_changed(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    int sel = (int)SendMessageW(c->hRefMode, CB_GETCURSEL, 0, 0);
    if (!s || c->loading || sel == CB_ERR) return;
    s->ref_mode = (RefMode)sel;
    ed_load_ref_params(c);
    ed_layout(c);                        /* 两个参数框按取法显隐 */
    ed_refresh_cur_row(c);
    ed_update_preview(c);
}

static void ed_ref_param_changed(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    wchar_t buf[64];
    if (!s || c->loading || !s->ref_segment_id[0]) return;
    if (s->ref_mode == REF_SPLIT) {
        qp_get_text(c->hRefA, buf, QP_SEP_MAX);
        qp_strlcpy(s->ref_delimiter, buf, QP_SEP_MAX);
        qp_get_text(c->hRefB, buf, 64);
        s->ref_index = _wtoi(buf);
        if (s->ref_index < 1) s->ref_index = 1;
    } else if (s->ref_mode == REF_SUBSTR) {
        qp_get_text(c->hRefA, buf, 64);
        s->ref_start = _wtoi(buf);
        if (s->ref_start < 1) s->ref_start = 1;
        qp_get_text(c->hRefB, buf, 64);
        s->ref_len = _wtoi(buf);          /* 空 / 0 = 一直取到末尾 */
        if (s->ref_len < 0) s->ref_len = 0;
    }
    ed_refresh_cur_row(c);
    ed_update_preview(c);
}

static void ed_add_segment(EditorCtx *c, SegmentType type)
{
    Segment *s = seg_create(type);
    int at;
    if (!s) return;
    if (type != SEG_STATIC) {
        wchar_t name[QP_LABEL_MAX];
        int n = seg_list_count(c->t->segments) + 1;
        _snwprintf(name, QP_LABEL_MAX, L"变量%d", n);
        name[QP_LABEL_MAX - 1] = L'\0';
        qp_strlcpy(s->label, name, QP_LABEL_MAX);
    }
    if (type == SEG_MULTI_SELECT)
        qp_strlcpy(s->separator, g_settings.default_separator[0]
                                    ? g_settings.default_separator : L"、", QP_SEP_MAX);
    at = c->cur >= 0 ? c->cur + 1 : seg_list_count(c->t->segments);
    seg_list_insert_at(&c->t->segments, s, at);
    c->cur = at;
    if(c->narrow)c->activePage=1;
    ed_fill_seglist(c, at);
    ed_load_props(c);
    ed_update_preview(c);
    if (type == SEG_STATIC) SetFocus(c->hContent);
    else SetFocus(c->hLabel);
}

static void ed_del_segment(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    wchar_t id[QP_ID_MAX];
    if (!s) return;
    if (qp_msgbox(c->hwnd, MB_YESNO | MB_ICONQUESTION, L"删除片段",
                  L"确定删除第 %d 个片段吗？取消模板编辑可放弃本次删除。", c->cur + 1) != IDYES)
        return;
    qp_strlcpy(id, s->id, QP_ID_MAX);
    s = seg_list_remove_at(&c->t->segments, c->cur);
    seg_free(s);
    /* 别人引用了这一个的话，引用一并撤掉：悬空引用本来就不生效，
       留着只会让人以为还跟着来源在变（与保存时清悬空引用同一口径） */
    for (s = c->t->segments; s; s = s->next)
        if (wcscmp(s->ref_segment_id, id) == 0) s->ref_segment_id[0] = L'\0';
    if (c->cur >= seg_list_count(c->t->segments)) c->cur = seg_list_count(c->t->segments) - 1;
    ed_fill_seglist(c, c->cur);
    ed_load_props(c);
    ed_update_preview(c);
}

static void ed_move_segment(EditorCtx *c, int delta)
{
    int n = seg_list_count(c->t->segments);
    int to = c->cur + delta;
    if (c->cur < 0 || to < 0 || to >= n) return;
    if (!seg_list_move(&c->t->segments, c->cur, to)) return;
    c->cur = to;
    ed_fill_seglist(c, to);
    ed_load_props(c);
    ed_update_preview(c);
}

static void ed_change_type(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    int sel = (int)SendMessageW(c->hType, CB_GETCURSEL, 0, 0);
    if (!s || sel == CB_ERR || sel == (int)s->type) return;
    s->type = (SegmentType)sel;
    if (s->type == SEG_STATIC) {
        s->hidden = 0;                    /* 固定句本来就不出现在填写界面 */
        s->no_output = 0;                 /* 固定句必须重新参与输出 */
        s->ref_segment_id[0] = L'\0';     /* 固定句没有「值」，引用无从谈起 */
    }
    if (s->type != SEG_TEXT) s->multiline = 0;  /* 多行只对文本输入有意义 */
    if (s->type == SEG_DATETIME) {
        /* 格式就是时间变量的全部配置；留着默认值只会变成第二个真相 */
        if (!s->time_format[0])
            qp_strlcpy(s->time_format, qp_time_format_at(0), QP_FORMAT_MAX);
        s->default_value[0] = L'\0';
    }
    if (s->type != SEG_STATIC && !s->label[0]) {
        wchar_t name[QP_LABEL_MAX];
        _snwprintf(name, QP_LABEL_MAX, L"变量%d", c->cur + 1);
        name[QP_LABEL_MAX - 1] = L'\0';
        qp_strlcpy(s->label, name, QP_LABEL_MAX);
    }
    if (s->type == SEG_MULTI_SELECT && !s->separator[0])
        qp_strlcpy(s->separator, g_settings.default_separator[0]
                                    ? g_settings.default_separator : L"、", QP_SEP_MAX);
    ed_load_props(c);
    ed_refresh_cur_row(c);
    ed_update_preview(c);
}

/* ------------------------------ 选项 ------------------------------ */

static void ed_opt_add(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    wchar_t buf[QP_VALUE_MAX] = L"";
    int custom=0;
    if (!s) return;
    if (!dlg_option(c->hwnd,L"添加选项",buf,QP_VALUE_MAX,&custom,qp_choice_custom_index(s)<0)) return;
    qp_trim(buf);
    if(custom && !buf[0])qp_strlcpy(buf,L"自主补充",QP_VALUE_MAX);
    if (!buf[0]) return;
    if (!qp_seg_add_option(s, buf)) {
        qp_msgbox(c->hwnd, MB_OK | MB_ICONERROR, L"无法添加选项",
                  L"内存不足，选项未添加。请关闭不需要的窗口后重试。");
        return;
    }
    if(custom)s->custom_option=s->option_count;
    ed_fill_optlist(c);
    lv_select(c->hOptList, s->option_count - 1);
    ed_update_preview(c);
}

static void ed_opt_edit(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    int i = lv_sel(c->hOptList);
    wchar_t buf[QP_VALUE_MAX];
    if (!s || i < 0 || i >= s->option_count) return;
    int ci=qp_choice_custom_index(s),custom=i==ci;
    qp_strlcpy(buf, s->options[i], QP_VALUE_MAX);
    if (!dlg_option(c->hwnd,L"修改选项",buf,QP_VALUE_MAX,&custom,ci<0 || ci==i)) return;
    qp_trim(buf);
    if(custom && !buf[0])qp_strlcpy(buf,L"自主补充",QP_VALUE_MAX);
    if (!buf[0]) return;
    wchar_t *value = qp_wcsdup(buf);
    if (!value) {
        qp_msgbox(c->hwnd, MB_OK | MB_ICONERROR, L"无法修改选项",
                  L"内存不足，原选项未改变。请关闭不需要的窗口后重试。");
        return;
    }
    free(s->options[i]);
    s->options[i] = value;
    if(custom)s->custom_option=i+1;
    else if(ci==i)s->custom_option=0;
    ed_fill_optlist(c);
    lv_select(c->hOptList, i);
    ed_update_preview(c);
}

static void ed_opt_del(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    int i = lv_sel(c->hOptList);
    if (!s || i < 0) return;
    seg_remove_option(s, i);
    ed_fill_optlist(c);
    lv_select(c->hOptList, i);
    ed_update_preview(c);
}

static void ed_opt_move(EditorCtx *c, int delta)
{
    Segment *s = ed_cur_seg(c);
    int i = lv_sel(c->hOptList);
    if (!s || i < 0) return;
    if (!seg_move_option(s, i, delta)) return;
    ed_fill_optlist(c);
    lv_select(c->hOptList, i + delta);
    ed_update_preview(c);
}

/* --------------------------- 解析规则 ----------------------------- */

static void ed_parse_add(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    ParseRule *r;
    if (!s) return;
    r = parse_rule_create();
    if (!r) return;
    qp_strlcpy(r->delimiter, L"-", QP_SEP_MAX);
    r->segment_index = 1;
    if (dlg_parse_rule(c->hwnd, r, c->t, s)) {
        parse_rule_append(&s->parse_rules, r);
        ed_fill_rulelists(c);
        lv_select(c->hParseList, parse_rule_count(s->parse_rules) - 1);
        ed_update_preview(c);
    } else {
        parse_rule_free(r);
    }
}

static void ed_parse_edit(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    int i = lv_sel(c->hParseList);
    ParseRule *orig, *copy;
    if (!s || i < 0) return;
    orig = parse_rule_at(s->parse_rules, i);
    if (!orig) return;
    copy = parse_rule_clone(orig);
    if (!copy) return;
    if (dlg_parse_rule(c->hwnd, copy, c->t, s)) {
        qp_parse_rule_take(orig, copy);
        ed_fill_rulelists(c);
        lv_select(c->hParseList, i);
        ed_update_preview(c);
    } else {
        parse_rule_free(copy);
    }
}

static void ed_parse_del(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    int i = lv_sel(c->hParseList);
    if (!s || i < 0) return;
    parse_rule_remove_at(&s->parse_rules, i);
    ed_fill_rulelists(c);
    lv_select(c->hParseList, i);
    ed_update_preview(c);
}

static void ed_parse_move(EditorCtx *c, int delta)
{
    Segment *s = ed_cur_seg(c);
    int i = lv_sel(c->hParseList);
    if (!s || i < 0) return;
    if (!parse_rule_move(&s->parse_rules, i, i + delta)) return;
    ed_fill_rulelists(c);
    lv_select(c->hParseList, i + delta);
    ed_update_preview(c);
}

/* --------------------------- 条件规则 ----------------------------- */

static void ed_cond_add(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    ConditionRule *r;
    if (!s) return;
    r = cond_rule_create();
    if (!r) return;
    if (dlg_cond_rule(c->hwnd, r, c->t, s)) {
        cond_rule_append(&s->condition_rules, r);
        ed_fill_rulelists(c);
        lv_select(c->hCondList, cond_rule_count(s->condition_rules) - 1);
        ed_update_preview(c);
    } else {
        cond_rule_free(r);
    }
}

static void ed_cond_edit(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    int i = lv_sel(c->hCondList);
    ConditionRule *orig, *copy;
    if (!s || i < 0) return;
    orig = cond_rule_at(s->condition_rules, i);
    if (!orig) return;
    copy = cond_rule_clone(orig);
    if (!copy) return;
    if (dlg_cond_rule(c->hwnd, copy, c->t, s)) {
        qp_cond_rule_take(orig, copy);
        ed_fill_rulelists(c);
        lv_select(c->hCondList, i);
        ed_update_preview(c);
    } else {
        cond_rule_free(copy);
    }
}

static void ed_cond_del(EditorCtx *c)
{
    Segment *s = ed_cur_seg(c);
    int i = lv_sel(c->hCondList);
    if (!s || i < 0) return;
    cond_rule_remove_at(&s->condition_rules, i);
    ed_fill_rulelists(c);
    lv_select(c->hCondList, i);
    ed_update_preview(c);
}

static void ed_cond_move(EditorCtx *c, int delta)
{
    Segment *s = ed_cur_seg(c);
    int i = lv_sel(c->hCondList);
    if (!s || i < 0) return;
    if (!cond_rule_move(&s->condition_rules, i, i + delta)) return;
    ed_fill_rulelists(c);
    lv_select(c->hCondList, i + delta);
    ed_update_preview(c);
}

/* ---------------------------- 布局 -------------------------------- */



static void ed_fill_groups(EditorCtx *c)
{
    Group *g;
    int i = 1, sel = 0;
    SendMessageW(c->hGroup, CB_RESETCONTENT, 0, 0);
    SendMessageW(c->hGroup, CB_ADDSTRING, 0, (LPARAM)L"（未分组）");
    SendMessageW(c->hGroup, CB_SETITEMDATA, 0, (LPARAM)NULL);
    for (g = g_app ? g_app->groups : NULL; g; g = g->next, ++i) {
        SendMessageW(c->hGroup, CB_ADDSTRING, 0, (LPARAM)g->name);
        SendMessageW(c->hGroup, CB_SETITEMDATA, (WPARAM)i, (LPARAM)g);
        if (wcscmp(c->t->group_id, g->id) == 0) sel = i;
    }
    SendMessageW(c->hGroup, CB_SETCURSEL, (WPARAM)sel, 0);
}

/* 批量开关 + 源下拉：勾选才启用源下拉；把当前选择写回模板。
   loading 时只摆状态、不回写（与其它属性同一纪律）。 */
static void ed_sync_batch(EditorCtx *c)
{
    int on = SendMessageW(c->hBatch, BM_GETCHECK, 0, 0) == BST_CHECKED;
    EnableWindow(c->hBatchSrc, on);
    EnableWindow(c->hBatchSrcLbl, on);
    if (c->loading) return;
    c->t->batch_mode = on ? 1 : 0;
    if (on) {
        const Segment *src = combo_selected_seg(c->hBatchSrc);
        qp_strlcpy(c->t->batch_source_id, src ? src->id : L"", QP_ID_MAX);
    }
    ed_update_preview(c);
}


/*
 * 保存前的体检：把「配好了但永远不会生效」的规则摆出来。
 * 只提醒不拦人 —— 判定再保守也可能有我想不到的用法，最终由你说了算。
 * 返回 1 = 继续保存，0 = 回去修改（顺手把光标停在第一条问题那一段上）。
 */
#define ED_LINT_SHOW 6              /* 一次最多列这么多条，再多也看不下去 */

static int ed_check_lint(HWND hwnd, EditorCtx *c)
{
    LintIssue *issues;
    wchar_t *msg;
    size_t cap, used;
    int n, i, shown, ret;

    issues = (LintIssue *)calloc(QP_LINT_MAX, sizeof(LintIssue));
    if (!issues) return 1;          /* 内存都没了，别为了体检挡住保存 */
    n = tmpl_lint(c->t, issues, QP_LINT_MAX);
    if (n <= 0) { free(issues); return 1; }

    cap = (size_t)(ED_LINT_SHOW + 2) * QP_RESULT_MAX;
    msg = (wchar_t *)calloc(cap, sizeof(wchar_t));
    if (!msg) { free(issues); return 1; }

    qp_strlcpy(msg, L"以下配置可能无法生效：\n\n", cap);
    used = wcslen(msg);
    shown = (n < ED_LINT_SHOW) ? n : ED_LINT_SHOW;
    for (i = 0; i < shown; ++i) {
        _snwprintf(msg + used, cap - used, L"· %ls\n", issues[i].text);
        msg[cap - 1] = L'\0';
        used += wcslen(msg + used);
    }
    if (n > shown) {
        _snwprintf(msg + used, cap - used, L"…另外还有 %d 条同类问题。\n", n - shown);
        msg[cap - 1] = L'\0';
        used += wcslen(msg + used);
    }
    qp_strlcpy(msg + used, L"\n是否仍然保存？选择「否」可返回修改。", cap - used);

    ret = MessageBoxW(hwnd, msg, L"模板体检",
                      MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (ret != IDYES) {
        /* 停到第一条问题所在的片段：省得自己数「第 7 段」是哪一段 */
        int at = issues[0].seg_index;
        if (at >= 0 && at < seg_list_count(c->t->segments)) {
            c->cur = at;
            ed_fill_seglist(c, at);
            ed_load_props(c);
        }
    }
    free(msg);
    free(issues);
    return ret == IDYES;
}

static LRESULT CALLBACK editor_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    EditorCtx *c = (EditorCtx *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    int handled = 0;
    LRESULT r;

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        HWND props;
        int fi;
        c = (EditorCtx *)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)c);
        c->hwnd = hwnd;
        c->loading = 1;

        /* 顶部 */
        qp_create(L"Static", L"模板名称：", SS_RIGHT, 0, 0, 0, 10, 10, hwnd,
                  IDC_ED_NAME + 1000);
        c->hName = mk_edit(hwnd, IDC_ED_NAME, 0);
        qp_create(L"Static", L"分组：", SS_RIGHT, 0, 0, 0, 10, 10, hwnd,
                  IDC_ED_GROUP + 1000);
        c->hGroup = mk_combo(hwnd, IDC_ED_GROUP);
        /* 批量：一句话→表格一行时，把源变量的多行粘贴逐行重复整模板输出 */
        c->hBatch = mk_check(hwnd, L"批量输出（一行一份）",
                             IDC_ED_BATCH);
        c->hBatchSrcLbl = mk_label_r(hwnd, L"源变量：", 0);
        c->hBatchSrc = mk_combo(hwnd, IDC_ED_BATCH_SRC);
        c->hPageSegments = qp_create(L"Button", L"片段", BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_GROUP | WS_TABSTOP,
                                    0,0,0,10,10,hwnd,IDC_ED_PAGE_SEGMENTS);
        c->hPageProps = qp_create(L"Button", L"属性", BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_TABSTOP,
                                 0,0,0,10,10,hwnd,IDC_ED_PAGE_PROPS);

        /* 左侧 */
        c->hSegLbl = mk_label(hwnd, L"片段（按顺序拼接；双击编辑，拖动排序）", 0);
        c->hAddStatic = mk_btn(hwnd, L"+固定", IDC_ED_SEG_ADD_STATIC);
        SetWindowLongPtrW(c->hAddStatic,GWL_STYLE,GetWindowLongPtrW(c->hAddStatic,GWL_STYLE)|WS_GROUP);
        c->hAddText = mk_btn(hwnd, L"+输入", IDC_ED_SEG_ADD_TEXT);
        c->hAddSelect = mk_btn(hwnd, L"+单选", IDC_ED_SEG_ADD_SELECT);
        c->hAddMulti = mk_btn(hwnd, L"+多选", IDC_ED_SEG_ADD_MULTI);
        c->hAddTime = mk_btn(hwnd, L"+时间", IDC_ED_SEG_ADD_TIME);
        c->hSegDel = mk_btn(hwnd, L"删除", IDC_ED_SEG_DEL);
        c->hSegUp = mk_btn(hwnd, L"↑", IDC_ED_SEG_UP);
        c->hSegDown = mk_btn(hwnd, L"↓", IDC_ED_SEG_DOWN);
        c->hSegList = mk_list(hwnd, IDC_ED_SEGLIST, 0);
        lv_add_col(c->hSegList, 0, L"#", 32);
        lv_add_col(c->hSegList, 1, L"类型", 64);
        lv_add_col(c->hSegList, 2, L"内容 / 变量", 200);

        /* 右侧属性：一个有独立裁剪边界的子窗口，无需资源 ID。 */
        ensure_class(L"QuickPhraseEditorProps", ed_props_proc);
        c->hPropsHost = CreateWindowExW(WS_EX_CONTROLPARENT,
            L"QuickPhraseEditorProps", L"片段属性",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, 0, 0, 0, hwnd, NULL, g_hInst, c);
        if (!c->hPropsHost) return -1;
        props = c->hPropsHost;
        c->hPropTitle = mk_label(props, L"片段属性", 0);
        SendMessageW(c->hPropTitle, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
        c->hTableOutput = mk_check(props, L"启用表格输出", IDC_ED_TABLE_OUTPUT);
        c->hTableFmtLbl = mk_label_r(props, L"表格格式：", 0);
        c->hTableFmt = mk_edit(props, IDC_ED_TABLE_FORMAT,
                               ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | WS_VSCROLL);
        c->hTableInsert = mk_combo(props, IDC_ED_TABLE_INSERT);
        c->hTableInsertBtn = mk_btn(props, L"插入", IDC_ED_TABLE_INSERT_BTN);
        c->hTableCellBtn = mk_btn(props, L"插入单元格分隔符", IDC_ED_TABLE_CELL_BTN);
        c->hPreviewText = mk_radio(props, L"文本预览", IDC_ED_PREVIEW_TEXT, 1);
        c->hPreviewTable = mk_radio(props, L"表格预览", IDC_ED_PREVIEW_TABLE, 0);
        c->hTypeLbl = mk_label_r(props, L"类型：", 0);
        c->hType = mk_combo_group(props, IDC_ED_SEG_TYPE);
        SendMessageW(c->hType, CB_ADDSTRING, 0, (LPARAM)L"固定文本");
        SendMessageW(c->hType, CB_ADDSTRING, 0, (LPARAM)L"文本输入");
        SendMessageW(c->hType, CB_ADDSTRING, 0, (LPARAM)L"下拉单选");
        SendMessageW(c->hType, CB_ADDSTRING, 0, (LPARAM)L"下拉多选");
        SendMessageW(c->hType, CB_ADDSTRING, 0, (LPARAM)L"时间日期");
        c->hLabelLbl = mk_label_r(props, L"变量名：", 0);
        c->hLabel = mk_edit(props, IDC_ED_SEG_LABEL, 0);
        c->hContentLbl = mk_label_r(props, L"固定内容：", 0);
        c->hContent = mk_edit(props, IDC_ED_SEG_CONTENT,
                              ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | WS_VSCROLL);
        c->hHint = mk_wrapped_label(props,
                            L"可以写 {变量名} 直接引用其他变量的值；{{ 输出一个 {",
                            IDC_ED_SEG_HINT);
        c->hPlaceLbl = mk_label_r(props, L"占位提示：", 0);
        c->hPlace = mk_edit(props, IDC_ED_SEG_PLACE, 0);
        c->hDefLbl = mk_label_r(props, L"默认值：", 0);
        c->hDef = mk_edit(props, IDC_ED_SEG_DEFAULT, 0);
        c->hSepLbl = mk_label_r(props, L"分隔符：", 0);
        c->hSep = mk_edit(props, IDC_ED_SEG_SEP, 0);
        c->hFmtLbl = mk_label_r(props, L"时间格式：", 0);
        c->hFmt = mk_combo_edit(props, IDC_ED_SEG_FORMAT);
        SendMessageW(c->hFmt, CB_LIMITTEXT, (WPARAM)(QP_FORMAT_MAX - 1), 0);
        for (fi = 0; fi < qp_time_format_count(); ++fi)
            SendMessageW(c->hFmt, CB_ADDSTRING, 0, (LPARAM)qp_time_format_at(fi));
        c->hFmtSample = mk_wrapped_label(props, L"", IDC_ED_SEG_FMT_SAMPLE);
        c->hHidden = qp_create(L"Button", L"隐藏输入控件（仍参与规则、引用及输出）",
                              BS_AUTOCHECKBOX|BS_MULTILINE|WS_TABSTOP,0,0,0,10,10,props,
                              IDC_ED_SEG_HIDDEN);
        c->hNoOutput = qp_create(L"Button", L"不加入输出文本（仍参与规则、引用及占位符）",
                                BS_AUTOCHECKBOX|BS_MULTILINE|WS_TABSTOP,0,0,0,10,10,props,
                                IDC_ED_SEG_NOOUTPUT);
        c->hMultiline = qp_create(L"Button", L"多行文本框（大段文字，可换行）",
                                 BS_AUTOCHECKBOX|BS_MULTILINE|WS_TABSTOP,0,0,0,10,10,props,
                                 IDC_ED_SEG_MULTILINE);
        c->hRefLbl = mk_label_r(props, L"引用：", 0);
        c->hRef = mk_combo(props, IDC_ED_SEG_REF);
        c->hRefModeLbl = mk_label_r(props, L"取值方式：", 0);
        c->hRefMode = mk_combo(props, IDC_ED_SEG_REF_MODE);
        SendMessageW(c->hRefMode, CB_ADDSTRING, 0, (LPARAM)L"整体");
        SendMessageW(c->hRefMode, CB_ADDSTRING, 0, (LPARAM)L"按分隔符取第 N 段");
        SendMessageW(c->hRefMode, CB_ADDSTRING, 0, (LPARAM)L"从第 N 个字起");
        c->hRefA = mk_edit(props, IDC_ED_SEG_REF_A, 0);
        c->hRefB = mk_edit(props, IDC_ED_SEG_REF_B, 0);

        c->hOptLbl = mk_label(props, L"选项", 0);
        c->hOptAdd = mk_btn(props, L"添加", IDC_ED_OPT_ADD);
        c->hOptEdit = mk_btn(props, L"修改", IDC_ED_OPT_EDIT);
        c->hOptDel = mk_btn(props, L"删除", IDC_ED_OPT_DEL);
        c->hOptUp = mk_btn(props, L"↑", IDC_ED_OPT_UP);
        c->hOptDown = mk_btn(props, L"↓", IDC_ED_OPT_DOWN);
        c->hOptList = mk_list(props, IDC_ED_OPTLIST, LVS_NOCOLUMNHEADER);
        lv_add_col(c->hOptList, 0, L"选项", 300);

        c->hParseLbl = mk_wrapped_label(props, L"解析规则（从当前变量提取信息，填写目标变量）", 0);
        c->hParseAdd = mk_btn(props, L"添加", IDC_ED_PARSE_ADD);
        c->hParseEdit = mk_btn(props, L"修改", IDC_ED_PARSE_EDIT);
        c->hParseDel = mk_btn(props, L"删除", IDC_ED_PARSE_DEL);
        c->hParseUp = mk_btn(props, L"↑", IDC_ED_PARSE_UP);
        c->hParseDown = mk_btn(props, L"↓", IDC_ED_PARSE_DOWN);
        c->hParseList = mk_list(props, IDC_ED_PARSELIST, LVS_NOCOLUMNHEADER);
        lv_add_col(c->hParseList, 0, L"规则", 300);

        c->hCondLbl = mk_wrapped_label(props, L"条件规则（按其他变量的值决定本片段输出）", 0);
        c->hCondAdd = mk_btn(props, L"添加", IDC_ED_COND_ADD);
        c->hCondEdit = mk_btn(props, L"修改", IDC_ED_COND_EDIT);
        c->hCondDel = mk_btn(props, L"删除", IDC_ED_COND_DEL);
        c->hCondUp = mk_btn(props, L"↑", IDC_ED_COND_UP);
        c->hCondDown = mk_btn(props, L"↓", IDC_ED_COND_DOWN);
        c->hCondList = mk_list(props, IDC_ED_CONDLIST, LVS_NOCOLUMNHEADER);
        lv_add_col(c->hCondList, 0, L"规则", 300);
        EnumChildWindows(c->hPropsHost, ed_prop_hook_focus, (LPARAM)c);
        ed_prop_hook_focus(c->hGroup, (LPARAM)c);
        ed_prop_hook_focus(c->hBatchSrc, (LPARAM)c);

        /* 底部 */
        c->hPreviewLbl = mk_label(hwnd, L"默认值预览", 0);
        /* 与主界面同一个自绘预览控件：文本失败处标红、表格按列对齐 */
        c->hPreview = qp_preview_create(hwnd, IDC_ED_PREVIEW, WS_EX_CLIENTEDGE);
        mk_btn(hwnd, L"保存", IDOK);
        mk_btn(hwnd, L"取消", IDCANCEL);
        SendMessageW(GetDlgItem(hwnd, IDOK), BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);

        SetWindowTextW(c->hName, c->t->name);
        ed_fill_groups(c);
        SendMessageW(c->hBatch, BM_SETCHECK,
                     c->t->batch_mode ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(c->hTableOutput, BM_SETCHECK,
                     c->t->table_output_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        SetWindowTextW(c->hTableFmt, c->t->table_output_format);
        c->previewMode = OUTPUT_TEXT;
        SendMessageW(c->hPreviewText, BM_SETCHECK, BST_CHECKED, 0);
        c->cur = seg_list_count(c->t->segments) > 0 ? 0 : -1;
        ed_layout(c);
        ed_fill_seglist(c, c->cur < 0 ? 0 : c->cur);
        ed_load_props(c);            /* 内部 ed_fill_batch_combo 选中当前源 */
        ed_sync_batch(c);            /* 按开关状态启用 / 禁用源下拉（loading 中不回写） */
        ed_sync_table(c);            /* 按表格开关启用格式与表格预览（loading 中不回写） */
        ed_update_preview(c);
        qp_theme_children(hwnd);
        c->loading = 0;
        SetFocus(c->hName);
        SendMessageW(c->hName, EM_SETSEL, 0, -1);
        return 0;
    }

    case WM_WINDOWPOSCHANGING:
        /* IsZoomed already reflects the requested state before the first erase.
           Ordinary border resizing keeps its live painting path. */
        if (c && IsWindowVisible(hwnd) && !IsIconic(hwnd) &&
            c->maximized != !!IsZoomed(hwnd)) {
            c->maximized = !!IsZoomed(hwnd);
            qp_prepare_present(hwnd, &c->restore_cloaked);
        }
        break;

    case WM_WINDOWPOSCHANGED: {
        WINDOWPOS *pos = (WINDOWPOS *)lp;
        LRESULT result = DefWindowProcW(hwnd, msg, wp, lp);
        if (c && !IsIconic(hwnd) && IsWindowVisible(hwnd) &&
            ((pos->flags & SWP_SHOWWINDOW) || !(pos->flags & SWP_NOSIZE)))
            qp_finish_present(hwnd, &c->restore_cloaked);
        return result;
    }

    case WM_TIMER:
        if (c && wp == QP_TIMER_PRESENT) {
            qp_finish_present(hwnd, &c->restore_cloaked);
            return 0;
        }
        break;

    case WM_GETMINMAXINFO: {
        /* 最小尺寸就是原来写死的那套设计尺寸：再小属性区放不下三个规则列表 */
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        RECT rc;
        rc.left = 0; rc.top = 0;
        rc.right = DPIX(ED_MIN_TRACK_W);
        rc.bottom = DPIX(ED_MIN_TRACK_H);
        AdjustWindowRectEx(&rc, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE,
                           (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
        mmi->ptMinTrackSize.x = rc.right - rc.left;
        mmi->ptMinTrackSize.y = rc.bottom - rc.top;
        RECT work;
        qp_window_work_area(GetWindow(hwnd,GW_OWNER),&work);
        if(mmi->ptMinTrackSize.x>work.right-work.left)mmi->ptMinTrackSize.x=work.right-work.left;
        if(mmi->ptMinTrackSize.y>work.bottom-work.top)mmi->ptMinTrackSize.y=work.bottom-work.top;
        return 0;
    }

    case WM_SIZE:
        if (c) {
            RECT current;
            if(wp==SIZE_MINIMIZED) return 0;
            GetClientRect(hwnd,&current);
            if(current.right==c->lastClientW && current.bottom==c->lastClientH) return 0;
            c->lastClientW=current.right; c->lastClientH=current.bottom;
            if (wp == SIZE_RESTORED) {
                /* 记住客户区尺寸，与主窗口的 win_w/win_h 同一口径（绝对像素、
                   不做 DPI 折算）。最大化时不记，否则下次还原成一扇大窗。 */
                g_settings.editor_w = current.right - current.left;
                g_settings.editor_h = current.bottom - current.top;
            }
            ed_layout(c);
            RedrawWindow(hwnd, NULL, NULL,
                         RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        }
        return 0;

    /* 拖完边框才落盘一次：拖动途中每格都写一次文件没有意义 */
    case WM_EXITSIZEMOVE:
        if (c) app_save_settings();
        return 0;

    case WM_ERASEBKGND:
    case WM_PRINTCLIENT:
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = msg == WM_PAINT ? BeginPaint(hwnd, &ps) : (HDC)wp;
        /* Erase/print must publish the same background and splitter as paint,
           without validating the pending paint region or changing the caller DC. */
        if (msg != WM_PAINT) GetClientRect(hwnd, &ps.rcPaint);
        if (hdc) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            qp_draw_window_frame(hwnd, hdc, &ps.rcPaint,
                                 ed_splitter_y(rc.bottom) + ED_SPLIT_HIT / 2);
        }
        if (msg == WM_PAINT) EndPaint(hwnd, &ps);
        return msg == WM_ERASEBKGND;
    }

    case WM_SETCURSOR: {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        if (ed_split_hit(hwnd, pt.x, pt.y)) {
            SetCursor(LoadCursorW(NULL, IDC_SIZENS));
            return TRUE;
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        RECT rc;
        if (!c) break;
        GetClientRect(hwnd, &rc);
        if (ed_split_hit(hwnd, x, y)) {
            c->splitDrag = 1;
            /* 记住抓取偏移，分隔条不会跳到光标下 */
            c->splitGrabDy = y - ed_splitter_y(rc.bottom - rc.top);
            SetCapture(hwnd);
        }
        return 0;
    }

    /* 双击分隔条恢复默认高度：与主窗口那两条分隔条的交互保持一致 */
    case WM_LBUTTONDBLCLK:
        if (c && ed_split_hit(hwnd, (short)LOWORD(lp), (short)HIWORD(lp))) {
            g_settings.editor_preview_height = 0;
            ed_layout(c);
            app_save_settings();
            return 0;
        }
        break;

    case WM_MOUSEMOVE:
        if (c && c->splitDrag) {
            RECT rc;
            int y = (short)HIWORD(lp);
            int H, lo, hi, h;
            GetClientRect(hwnd, &rc);
            H = rc.bottom - rc.top;
            ed_preview_bounds(H, &lo, &hi);
            /* 往上拖 = 预览变高；公式与 ed_splitter_y 互为反函数 */
            h = ed_splitter_base(H) - (y - c->splitGrabDy);
            if (h < lo) h = lo;
            if (h > hi) h = hi;
            if (h != g_settings.editor_preview_height) {
                g_settings.editor_preview_height = h;
                ed_layout(c);
            }
        }
        return 0;

    case WM_LBUTTONUP:
        if (c && c->splitDrag) {
            c->splitDrag = 0;
            ReleaseCapture();
            app_save_settings();
        }
        return 0;

    case WM_CANCELMODE:
        if (c) c->splitDrag = 0;
        break;

    case WM_COMMAND: {
        WORD id = LOWORD(wp), code = HIWORD(wp);
        qp_combo_dropdown_fix(wp, lp);
        if (!c) break;
        switch (id) {
        case IDC_ED_PAGE_SEGMENTS:
        case IDC_ED_PAGE_PROPS:
            if(code==BN_CLICKED) { c->activePage=id==IDC_ED_PAGE_PROPS; ed_layout(c); }
            return 0;
        case IDC_ED_SEG_ADD_STATIC: ed_add_segment(c, SEG_STATIC); return 0;
        case IDC_ED_SEG_ADD_TEXT:   ed_add_segment(c, SEG_TEXT); return 0;
        case IDC_ED_SEG_ADD_SELECT: ed_add_segment(c, SEG_SELECT); return 0;
        case IDC_ED_SEG_ADD_MULTI:  ed_add_segment(c, SEG_MULTI_SELECT); return 0;
        case IDC_ED_SEG_ADD_TIME:   ed_add_segment(c, SEG_DATETIME); return 0;
        case IDC_ED_SEG_DEL:        ed_del_segment(c); return 0;
        case IDC_ED_SEG_UP:         ed_move_segment(c, -1); return 0;
        case IDC_ED_SEG_DOWN:       ed_move_segment(c, +1); return 0;
        case IDC_ED_SEG_TYPE:
            if (code == CBN_SELCHANGE) ed_change_type(c);
            return 0;
        case IDC_ED_SEG_LABEL:
        case IDC_ED_SEG_CONTENT:
        case IDC_ED_SEG_PLACE:
        case IDC_ED_SEG_DEFAULT:
        case IDC_ED_SEG_SEP:
            if (code == EN_CHANGE && !c->loading) {
                ed_commit_props(c);
                if (id == IDC_ED_SEG_LABEL) ed_fill_table_combo(c);
                ed_refresh_cur_row(c);
            }
            return 0;
        case IDC_ED_SEG_HIDDEN:
        case IDC_ED_SEG_MULTILINE:
        case IDC_ED_SEG_NOOUTPUT:
            if (code == BN_CLICKED && !c->loading) {
                ed_commit_props(c);
                ed_refresh_cur_row(c);
            }
            return 0;
        case IDC_ED_BATCH:
            if (code == BN_CLICKED) ed_sync_batch(c);
            return 0;
        case IDC_ED_BATCH_SRC:
            if (code == CBN_SELCHANGE) ed_sync_batch(c);
            return 0;
        case IDC_ED_TABLE_OUTPUT:
            if (code == BN_CLICKED) ed_sync_table(c);
            return 0;
        case IDC_ED_TABLE_FORMAT:
            if (code == EN_CHANGE) ed_table_format_changed(c);
            return 0;
        case IDC_ED_TABLE_INSERT_BTN:
            if (code == BN_CLICKED) ed_insert_table_variable(c);
            return 0;
        case IDC_ED_TABLE_CELL_BTN:
            if (code == BN_CLICKED) ed_insert_table_text(c,L"\\t");
            return 0;
        case IDC_ED_PREVIEW_TEXT:
            if (code == BN_CLICKED && !c->loading) {
                c->previewMode = OUTPUT_TEXT;
                ed_update_preview(c);
            }
            return 0;
        case IDC_ED_PREVIEW_TABLE:
            if (code == BN_CLICKED && !c->loading &&
                c->t->table_output_enabled) {
                c->previewMode = OUTPUT_TABLE;
                ed_update_preview(c);
            }
            return 0;
        case IDC_ED_SEG_FORMAT:
            /* 下拉里选一个（SELCHANGE）或自己敲（EDITCHANGE），两条路都算改格式 */
            if (code == CBN_SELCHANGE || code == CBN_EDITCHANGE) {
                ed_commit_format(c, code == CBN_SELCHANGE);
                ed_refresh_cur_row(c);
            }
            return 0;
        case IDC_ED_SEG_REF:
            if (code == CBN_SELCHANGE) ed_ref_changed(c);
            return 0;
        case IDC_ED_SEG_REF_MODE:
            if (code == CBN_SELCHANGE) ed_ref_mode_changed(c);
            return 0;
        case IDC_ED_SEG_REF_A:
        case IDC_ED_SEG_REF_B:
            if (code == EN_CHANGE && !c->loading) ed_ref_param_changed(c);
            return 0;
        case IDC_ED_OPT_ADD:   ed_opt_add(c); return 0;
        case IDC_ED_OPT_EDIT:  ed_opt_edit(c); return 0;
        case IDC_ED_OPT_DEL:   ed_opt_del(c); return 0;
        case IDC_ED_OPT_UP:    ed_opt_move(c, -1); return 0;
        case IDC_ED_OPT_DOWN:  ed_opt_move(c, +1); return 0;
        case IDC_ED_PARSE_ADD:  ed_parse_add(c); return 0;
        case IDC_ED_PARSE_EDIT: ed_parse_edit(c); return 0;
        case IDC_ED_PARSE_DEL:  ed_parse_del(c); return 0;
        case IDC_ED_PARSE_UP:   ed_parse_move(c, -1); return 0;
        case IDC_ED_PARSE_DOWN: ed_parse_move(c, +1); return 0;
        case IDC_ED_COND_ADD:   ed_cond_add(c); return 0;
        case IDC_ED_COND_EDIT:  ed_cond_edit(c); return 0;
        case IDC_ED_COND_DEL:   ed_cond_del(c); return 0;
        case IDC_ED_COND_UP:    ed_cond_move(c, -1); return 0;
        case IDC_ED_COND_DOWN:  ed_cond_move(c, +1); return 0;
        case IDOK: {
            wchar_t name[QP_NAME_MAX];
            Group *g;
            Segment *a, *b;
            int gi, ia, ib;
            qp_get_text(c->hName, name, QP_NAME_MAX);
            qp_trim(name);
            if (!name[0]) {
                qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"提示", L"请填写模板名称。");
                SetFocus(c->hName);
                return 0;
            }
            if (seg_list_count(c->t->segments) == 0) {
                qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"提示",
                          L"模板至少需要一个片段。");
                return 0;
            }
            ed_commit_props(c);
            ed_commit_table(c);
            /* 变量重名拦下来：规则和引用都是靠变量名认人的，两个同名变量在下拉里
               长得一模一样，指错了也看不出来（引擎按 id 找不到时还会退回按名字找，
               只会命中第一个）。改一个名字比事后排查便宜得多。 */
            for (a = c->t->segments, ia = 0; a; a = a->next, ++ia) {
                if (a->type == SEG_STATIC || !a->label[0]) continue;
                for (b = a->next, ib = ia + 1; b; b = b->next, ++ib) {
                    if (b->type == SEG_STATIC) continue;
                    if (wcscmp(a->label, b->label) != 0) continue;
                    qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"变量名重复",
                              L"第 %d 个和第 %d 个片段的变量名称均为「%ls」。\n"
                              L"名称用于占位符及兼容引用，重复名称会造成歧义。请修改其中一个名称。",
                              ia + 1, ib + 1, a->label);
                    c->cur = ib;
                    ed_fill_seglist(c, ib);
                    ed_load_props(c);
                    SetFocus(c->hLabel);
                    return 0;
                }
            }
            /* 指向已删除变量的引用（导入的老数据也可能有）直接清掉：它永远不会
               生效，留在存档里只会让下次打开的人以为还跟着来源在变 */
            for (a = c->t->segments; a; a = a->next) {
                if (a->type == SEG_STATIC ||
                    (a->ref_segment_id[0] &&
                     !tmpl_find_segment(c->t, a->ref_segment_id)))
                    a->ref_segment_id[0] = L'\0';
            }
            /* 体检：能证明不会生效的规则，保存前提一句（不拦人） */
            if (!ed_check_lint(hwnd, c)) return 0;
            /* 批量源变量指向已删除 / 非文本变量时，按引用悬空同一口径把 id 清掉
               （开关留着：运行时无有效源会自动退化为单行，不会出错；体检已在上面
               提醒过了，用户执意保存就不再留个悬空指针在存档里） */
            if (c->t->batch_source_id[0]) {
                const Segment *bs = tmpl_find_segment(c->t, c->t->batch_source_id);
                if (!bs || bs->type != SEG_TEXT) c->t->batch_source_id[0] = L'\0';
            }
            qp_strlcpy(c->t->name, name, QP_NAME_MAX);
            gi = (int)SendMessageW(c->hGroup, CB_GETCURSEL, 0, 0);
            g = (gi > 0) ? (Group *)SendMessageW(c->hGroup, CB_GETITEMDATA, (WPARAM)gi, 0)
                         : NULL;
            qp_strlcpy(c->t->group_id, g ? g->id : L"", QP_ID_MAX);
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
    }

    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (!c) break;
        if (nh->idFrom == IDC_ED_SEGLIST) {
            if(nh->code==NM_DBLCLK && c->narrow) {
                c->activePage=1;ed_layout(c);return 0;
            }
            if (nh->code == LVN_ITEMCHANGED) {
                NMLISTVIEW *n = (NMLISTVIEW *)lp;
                if ((n->uChanged & LVIF_STATE) && (n->uNewState & LVIS_SELECTED) &&
                    n->iItem != c->cur) {
                    c->cur = n->iItem;
                    ed_load_props(c);
                }
            } else if (nh->code == LVN_BEGINDRAG) {
                NMLISTVIEW *n = (NMLISTVIEW *)lp;
                int to = qp_lv_drag_reorder(c->hSegList, n->iItem);
                if (to >= 0 && seg_list_move(&c->t->segments, n->iItem, to)) {
                    c->cur = to;
                    ed_fill_seglist(c, to);
                    ed_load_props(c);
                    ed_update_preview(c);
                }
                return 0;
            }
        } else if (nh->code == NM_DBLCLK) {
            if (nh->idFrom == IDC_ED_OPTLIST) { ed_opt_edit(c); return 0; }
            if (nh->idFrom == IDC_ED_PARSELIST) { ed_parse_edit(c); return 0; }
            if (nh->idFrom == IDC_ED_CONDLIST) { ed_cond_edit(c); return 0; }
        }
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

/* 还原上次记住的编辑器尺寸：存的是客户区绝对像素，这里夹进屏幕工作区 */


int dlg_edit_template(HWND owner, Template *working, int is_new)
{
    EditorCtx c;
    HWND dlg;
    memset(&c, 0, sizeof(c));
    c.t = working;
    c.is_new = is_new;
    c.cur = -1;
    c.splitLastY = -1;                /* 还没画过分隔条 */
    dlg = make_dlg_ex(L"QuickPhraseEditor", editor_proc,
                      is_new ? L"新建模板" : L"编辑模板",
                      ED_MIN_W, ED_MIN_H, owner, &c,
                      WS_THICKFRAME | WS_MAXIMIZEBOX);
    ed_restore_size(dlg, owner);
    int accepted=run_modal(owner, dlg, &c.result);
    eval_destroy(c.previewCtx);
    return accepted;
}
