#include "ui_internal.h"
/* ================================================================== */
/* 字段构建与刷新                                                      */
/* ================================================================== */

void ui_clear_fields(void)
{
    int i;
    ++g_syncFields;
    ui_choice_cancel_pending();
    popup_close();
    for (i = 0; i < g_fieldCount; ++i) {
        if (g_fields[i].hLabel) DestroyWindow(g_fields[i].hLabel);
        if (g_fields[i].hCtl)   DestroyWindow(g_fields[i].hCtl);
        if (g_fields[i].hBtn)   DestroyWindow(g_fields[i].hBtn);
    }
    free(g_fields);
    g_fields = NULL;
    g_fieldCount = 0;
    g_hostScroll = 0;
    g_hostContentH = 0;
    --g_syncFields;
    if (g_hHost) {
        host_update_scroll();
        InvalidateRect(g_hHost, NULL, TRUE);
    }
}

/* 填写界面是否给该片段建控件：固定句不需要；勾了「在填写界面隐藏」的
   变量也不需要（只靠变量匹配/默认值填充，但照常参与输出）。 */
static int seg_needs_input(const Segment *s)
{
    return s && s->type != SEG_STATIC && !s->hidden;
}

void ui_build_fields(void)
{
    int i, count = 0, y = DPIX(10);
    wchar_t label[QP_LABEL_MAX + 8];

    ui_clear_fields();
    if (!g_ctx || !g_hHost) return;

    for (i = 0; i < g_ctx->count; ++i)
        if (seg_needs_input(g_ctx->fields[i].seg)) ++count;
    if (count == 0) {
        g_hostContentH = 0;
        host_update_scroll();
        InvalidateRect(g_hHost, NULL, TRUE);
        return;
    }

    g_fields = (FieldCtl *)calloc((size_t)count, sizeof(FieldCtl));
    if (!g_fields) return;

    /* 控件先在 (0,0) 建出来再统一摆位，期间关掉重绘，避免左上角闪一片控件 */
    SendMessageW(g_hHost, WM_SETREDRAW, FALSE, 0);

    for (i = 0; i < g_ctx->count; ++i) {
        const Segment *seg = g_ctx->fields[i].seg;
        FieldCtl *f;
        int ctlId;
        if (!seg_needs_input(seg)) continue;

        f = &g_fields[g_fieldCount];
        f->field_index = i;
        f->y = y;
        f->h = (seg->type == SEG_TEXT && seg->multiline) ? QP_MULTILINE_H : QP_ROW_CTL_H;
        f->extraIndex = -1;
        ctlId = IDC_FIELD_BASE + g_fieldCount * IDC_FIELD_STRIDE;

        _snwprintf(label, QP_LABEL_MAX + 8, L"%ls：", seg_display_name(seg));
        label[QP_LABEL_MAX + 7] = L'\0';
        f->hLabel = qp_create(L"Static", label, SS_RIGHT | SS_ENDELLIPSIS, 0,
                              0, 0, 10, 10, g_hHost, ctlId + 2);

        if (seg->type == SEG_TEXT) {
            /* 多行：自动换行 + 竖滚动条，回车直接换行（ES_WANTRETURN），
               主循环里的 IsDialogMessageW 会把回车让给它。 */
            DWORD st = seg->multiline
                         ? (ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL |
                            WS_VSCROLL | WS_TABSTOP)
                         : (ES_AUTOHSCROLL | WS_TABSTOP);
            f->hCtl = qp_create(L"Edit", g_ctx->fields[i].value, st, WS_EX_CLIENTEDGE,
                                0, 0, 10, 10, g_hHost, ctlId);
            if (seg->placeholder[0])
                SendMessageW(f->hCtl, EM_SETCUEBANNER, TRUE, (LPARAM)seg->placeholder);
        } else if (seg->type == SEG_SELECT) {
            int k;
            f->hCtl = qp_create(L"ComboBox", L"",
                                CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0,
                                0, 0, 10, DPIX(300), g_hHost, ctlId);
            for (k = 0; k < seg->option_count; ++k)
                SendMessageW(f->hCtl, CB_ADDSTRING, 0, (LPARAM)seg->options[k]);
        } else if (seg->type == SEG_DATETIME) {
            /* 时间：单行框（内容仍可手改）+「现在」刷新成此刻。
               按钮复用多选 ▼ 的那个位置（ctlId + 1），不用动 STRIDE。 */
            f->hCtl = qp_create(L"Edit", g_ctx->fields[i].value,
                                ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE,
                                0, 0, 10, 10, g_hHost, ctlId);
            f->hBtn = qp_create(L"Button", L"现在", BS_PUSHBUTTON | WS_TABSTOP, 0,
                                0, 0, 10, 10, g_hHost, ctlId + 1);
            f->btnW = DPIX(46);
        } else { /* SEG_MULTI_SELECT */
            f->hCtl = qp_create(L"Edit", g_ctx->fields[i].value,
                                ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP,
                                WS_EX_CLIENTEDGE, 0, 0, 10, 10, g_hHost, ctlId);
            SetWindowSubclass(f->hCtl, multi_edit_proc, (UINT_PTR)i, 0);
            if (seg->placeholder[0])
                SendMessageW(f->hCtl, EM_SETCUEBANNER, TRUE, (LPARAM)seg->placeholder);
            f->hBtn = qp_create(L"Button", L"▼", BS_PUSHBUTTON | WS_TABSTOP, 0,
                                0, 0, 10, 10, g_hHost, ctlId + 1);
        }
        ++g_fieldCount;
        y += f->h + QP_ROW_GAP;
    }

    g_hostContentH = y + DPIX(4);
    host_clamp_scroll();
    host_reposition();
    host_update_scroll();
    SendMessageW(g_hHost, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(g_hHost, NULL, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

/* 把求值结果同步到控件（跳过正在输入的控件，避免光标跳动）。
   同时统一处理「被规则/引用接管 → 该控件只读」：文本与时间用
   EM_SETREADONLY（文字颜色不变，照常刷新），单选/多选没有只读态，
   只能整体禁用。三者语义一致：界面上看到的永远是最终生效的值。 */
void ui_sync_controls(void)
{
    HWND focus = GetFocus();
    int i;
    if (!g_ctx) return;
    /* 下面往 Edit 里写字会引出 EN_CHANGE。那不是人在敲键盘，而是我们自己把引擎
       的值写回去，绝不能再当成「手动填写」回读一遍：否则规则填进去的值会顺手
       变成手填值，等规则下一轮不再命中时，它就赖在框里不肯让位给默认值。
       （单选走 CB_SETCURSEL，本来就不会发 CBN_SELCHANGE，两者由此一致。） */
    ++g_syncFields;
    for (i = 0; i < g_fieldCount; ++i) {
        FieldCtl *f = &g_fields[i];
        const EvalField *ef = &g_ctx->fields[f->field_index];
        const Segment *seg = ef->seg;
        int ro;
        if (!seg) continue;
        ro = ef->rule_driven ? 1 : 0;
        if (seg->type == SEG_SELECT) {
            /* 控件必须严格跟随引擎里的值：值为空时也要清掉选中态，
               否则界面上还留着上一次匹配到的文字，看着「已填」，
               实际预览里那一段是空的（点一下才恢复正常）。 */
            int custom=qp_choice_custom_index(seg)>=0 && ef->custom_selected && !ro && ef->value[0];
            wchar_t custom_label[QP_VALUE_MAX+16];
            if(custom) {
                _snwprintf(custom_label,QP_VALUE_MAX+16,L"自主补充：%ls",ef->value);
                custom_label[QP_VALUE_MAX+15]=0;
            }
            const wchar_t *display=custom?custom_label:ef->value;
            int idx = custom?f->extraIndex:ef->value[0]
                        ? (int)SendMessageW(f->hCtl, CB_FINDSTRINGEXACT, (WPARAM)-1,
                                            (LPARAM)ef->value)
                        : CB_ERR;
            if(custom && idx>=0) {
                wchar_t old[QP_VALUE_MAX+16];
                int length=(int)SendMessageW(f->hCtl,CB_GETLBTEXTLEN,(WPARAM)idx,0);
                if(length<0 || length>=QP_VALUE_MAX+16)idx=CB_ERR;
                else {
                    SendMessageW(f->hCtl,CB_GETLBTEXT,(WPARAM)idx,(LPARAM)old);
                    if(wcscmp(old,display))idx=CB_ERR;
                }
            }
            if (f->extraIndex >= 0 && idx != f->extraIndex) {
                /* 上次为显示而临时加入的项已经过期 → 撤掉。
                   临时项总是加在末尾，删除它不会影响正式选项的索引。 */
                SendMessageW(f->hCtl, CB_DELETESTRING, (WPARAM)f->extraIndex, 0);
                f->extraIndex = -1;
                if (!custom && ef->value[0])
                    idx = (int)SendMessageW(f->hCtl, CB_FINDSTRINGEXACT, (WPARAM)-1,
                                            (LPARAM)ef->value);
            }
            if (idx == CB_ERR && ef->value[0]) {
                /* 规则推导出的值不在选项列表中 → 临时加入以便显示。
                   只保留「一个」临时项：解析规则的 fill_value 为空时推导值会
                   跟着输入逐字变化，旧实现每次刷新都 CB_ADDSTRING，下拉列表
                   会随敲键无限膨胀。 */
                idx = (int)SendMessageW(f->hCtl, CB_ADDSTRING, 0, (LPARAM)display);
                if (idx >= 0) f->extraIndex = idx;
            }
            if (idx < 0) idx = -1;                    /* CB_ERR / 空值 → 不选中 */
            if ((int)SendMessageW(f->hCtl, CB_GETCURSEL, 0, 0) != idx)
                SendMessageW(f->hCtl, CB_SETCURSEL, (WPARAM)idx, 0);
        } else if (seg->type == SEG_TEXT || seg->type == SEG_DATETIME) {
            /* 被变量匹配接管的文本框转为只读：手动输入已经不作数了（规则值
               优先），再让人敲字只会造成「框里是我打的、预览是规则算的」错觉。
               只读时也照常刷新，框里始终就是最终生效的值。 */
            if (f->readonly != ro) {
                SendMessageW(f->hCtl, EM_SETREADONLY, (WPARAM)ro, 0);
                InvalidateRect(f->hCtl, NULL, TRUE);
            }
            if (ro || f->hCtl != focus)
                qp_set_text_if_diff(f->hCtl, ef->value);
        } else {
            /* 多选的只读框现在全程持有焦点（弹层不抢焦点），必须照常刷新，
               否则勾选了也看不到文字变化。 */
            qp_set_text_if_diff(f->hCtl, ef->value);
        }
        /* 单选 / 多选没有「只读」态，被接管时只能整体禁用，否则点得动、
           下一次刷新又被规则值顶回去，白点一下（v2.1 修） */
        if (f->readonly != ro &&
            (seg->type == SEG_SELECT || seg->type == SEG_MULTI_SELECT)) {
            if (ro) {
                if (g_popupField == f->field_index) popup_close();
                if (focus == f->hCtl || focus == f->hBtn) {
                    SetFocus(g_hwndMain);
                    focus = GetFocus();
                }
            }
            EnableWindow(f->hCtl, ro ? FALSE : TRUE);
            if (f->hBtn) EnableWindow(f->hBtn, ro ? FALSE : TRUE);
        }
        /* 时间的「现在」按钮同理：值由规则说话时刷新它没有意义 */
        if (f->readonly != ro && seg->type == SEG_DATETIME && f->hBtn) {
            if (ro && focus == f->hBtn) { SetFocus(g_hwndMain); focus = GetFocus(); }
            EnableWindow(f->hBtn, ro ? FALSE : TRUE);
        }
        f->readonly = ro;
    }
    --g_syncFields;
}

/* 表格配置不完整时绝不把主窗口留在一个渲染为空的表格模式。这个函数没有
   UI 或全局状态依赖，供本地宿主测试直接执行。 */
static OutputMode ui_effective_output_mode(OutputMode requested, int table_enabled,
                                           const wchar_t *table_format)
{
    if (requested != OUTPUT_TABLE || !table_enabled || !table_format || !table_format[0])
        return OUTPUT_TEXT;
    return OUTPUT_TABLE;
}

static int ui_table_output_enabled(void)
{
    return g_curTmpl && g_curTmpl->table_output_enabled;
}

static int ui_table_mode_available(void)
{
    return ui_table_output_enabled() && g_curTmpl->table_output_format[0];
}

void ui_update_preview_mode_labels(void)
{
    int table = g_previewMode == OUTPUT_TABLE;
    if (g_hPreviewLabel)
        SetWindowTextW(g_hPreviewLabel, table ? L"输出预览（表格）" : L"输出预览（文本）");
    if (g_hBtnCopy)
        SetWindowTextW(g_hBtnCopy, table ? L"一键复制表格" : L"一键复制文本");
}

void ui_sync_preview_mode_controls(void)
{
    int has_template = g_curTmpl != NULL;
    int table_enabled = ui_table_output_enabled();
    int table_available = ui_table_mode_available();

    g_previewMode = ui_effective_output_mode(g_previewMode, table_enabled,
                                             g_curTmpl ? g_curTmpl->table_output_format : L"");
    if (g_hPreviewTextMode) {
        SendMessageW(g_hPreviewTextMode, BM_SETCHECK,
                     g_previewMode == OUTPUT_TEXT ? BST_CHECKED : BST_UNCHECKED, 0);
        EnableWindow(g_hPreviewTextMode, has_template);
    }
    if (g_hPreviewTableMode) {
        SendMessageW(g_hPreviewTableMode, BM_SETCHECK,
                     g_previewMode == OUTPUT_TABLE ? BST_CHECKED : BST_UNCHECKED, 0);
        EnableWindow(g_hPreviewTableMode, table_available);
    }
    ui_update_preview_mode_labels();
}

static void ui_set_preview_hint(void)
{
    if (!g_hHint || !g_ctx) return;
    if (g_previewMode == OUTPUT_TABLE) {
        qp_set_text_if_diff(g_hHint,
            L"表格模式：红字「未匹配」表示对应变量的解析未命中，不代表空单元格。Ctrl+Enter 复制后可粘贴到 Excel。");
    } else if (g_fieldCount == 0) {
        qp_set_text_if_diff(g_hHint, L"文本模式：无需填写，Ctrl+Enter 可直接复制");
    } else {
        qp_set_text_if_diff(g_hHint, L"文本模式：填写后自动套用规则，Ctrl+Enter 可直接复制");
    }
}

/* 预览与复制只走这一条最终渲染路径，避免批量开关或输出模式让两处内容分叉。
   marks 只给预览用（复制传 NULL）——它记的是「该有值却空着」的位置，不影响
   渲染出来的文本本身，所以预览与复制永远是同一份内容。 */
const wchar_t *qp_render_status_hint(QpRenderStatus status)
{
    if(status==QP_RENDER_TRUNCATED)
        return L"结果不完整：部分变量或输出达到长度上限。请缩短源文本、减少匹配结果或分批处理后再复制。";
    if(status==QP_RENDER_NOMEM)
        return L"内存不足，输出未更新，原预览保持。请关闭不需要的窗口后重试。";
    return L"当前输出不可用。请选择模板并检查输出设置后重试。";
}

QpRenderResult ui_render_current_checked(wchar_t *out,size_t cap,int *marks,int mark_cap)
{
    if(out && cap)out[0]=0;
    if(!g_ctx)return (QpRenderResult){QP_RENDER_INVALID,0,0};
    OutputMode mode=ui_effective_output_mode(g_previewMode,ui_table_output_enabled(),
        g_curTmpl?g_curTmpl->table_output_format:L"");
    g_previewMode=mode;
    int batch=g_ctx->tmpl && g_ctx->tmpl->batch_mode;
    return qp_eval_render_checked(g_ctx,mode,batch,out,cap,marks,mark_cap);
}

size_t ui_render_current(wchar_t *out,size_t cap,int *marks,int mark_cap,int *mark_count)
{
    QpRenderResult result=ui_render_current_checked(out,cap,marks,mark_cap);
    if(mark_count)*mark_count=result.mark_count;
    return result.written;
}

/* 按当前模式把渲染结果喂给预览控件：文本模式带「失败位置」，表格模式带格式串。
   文本与表格只用这一个控件，切模式不会换控件，也就不会有位置/焦点漂移。 */
int ui_preview_show_result(const wchar_t *out, const int *marks, int nmarks)
{
    if (!g_hPreview) return 0;
    if (g_previewMode == OUTPUT_TABLE && ui_table_mode_available()) {
        return qp_preview_set_table_marked(g_hPreview, out ? out : L"",
                             g_curTmpl ? g_curTmpl->table_output_format : NULL, marks, nmarks);
    } else {
        return qp_preview_set_text(g_hPreview, out ? out : L"", marks, nmarks);
    }
}

void ui_refresh_current(void)
{
    wchar_t out[QP_OUTPUT_MAX];
    int marks[256];
    int nmarks = 0;
    if (!g_ctx) {
        ui_sync_preview_mode_controls();
        if (g_hPreview) qp_preview_clear(g_hPreview);
        return;
    }
    ui_sync_preview_mode_controls();
    QpRenderResult result=ui_render_current_checked(out,QP_OUTPUT_MAX,marks,256);
    nmarks=result.mark_count;
    if(result.status!=QP_RENDER_OK) {
        if(result.status==QP_RENDER_TRUNCATED) {
            ui_sync_controls();
            if(!ui_preview_show_result(out,marks,nmarks))result.status=QP_RENDER_NOMEM;
        }
        EnableWindow(g_hBtnCopy,FALSE);
        qp_set_text_if_diff(g_hHint,qp_render_status_hint(result.status));
        return;
    }
    ui_sync_controls();
    if(!ui_preview_show_result(out,marks,nmarks)) {
        EnableWindow(g_hBtnCopy,FALSE);
        qp_set_text_if_diff(g_hHint,qp_render_status_hint(QP_RENDER_NOMEM));
        return;
    }
    EnableWindow(g_hBtnCopy,TRUE);
    /* 规则互相覆盖、迭代跑满仍没收敛时说一句：否则界面只是安静地拿着中间态，
       人看到的结果说不清是哪一轮的（v2.1 修） */
    if (g_ctx->unstable) {
        qp_set_text_if_diff(g_hHint, L"规则相互覆盖，未能收敛，已按最后一轮结果生成");
        g_unstableWarned = 1;
    } else {
        g_unstableWarned = 0;
        ui_set_preview_hint();
    }
}

int ui_select_template(Template *t)
{
    wchar_t title[QP_NAME_MAX + 32];

    if (t == g_curTmpl && g_ctx) {
        qp_set_text_if_diff(g_hTitle, t ? t->name : L"未选择模板");
        return 1;
    }
    if (t) {
        Session *session = sess_pick_for(t->id);
        if (!session || !sess_activate(session, t)) {
            sess_discard_pending(session);
            qp_msgbox(g_hwndMain, MB_OK | MB_ICONERROR, L"无法切换模板",
                      L"内存不足，当前输入已保留。请关闭不需要的窗口后重试。");
            return 0;
        }
    } else if (!sess_deactivate()) {
        qp_msgbox(g_hwndMain, MB_OK | MB_ICONERROR, L"无法结束当前填写",
                  L"内存不足，当前输入已保留。请关闭不需要的窗口后重试。");
        return 0;
    }
    ui_clear_fields();
    g_previewMode = OUTPUT_TEXT; /* 每次切换模板都回到普通文本，避免状态串模板 */
    g_curTmpl = t;
    ui_sync_preview_mode_controls();

    if (!t) {
        SetWindowTextW(g_hTitle, L"未选择模板");
        if (g_hPreview) qp_preview_clear(g_hPreview);
        SetWindowTextW(g_hHint, L"在左侧选择模板，开始生成输出文本。");
        EnableWindow(g_hBtnCopy, FALSE);
        EnableWindow(g_hBtnReset, FALSE);
        EnableWindow(g_hBtnEdit, FALSE);
        tabs_refresh();
        ui_layout(g_hwndMain);
        return 1;
    }

    _snwprintf(title, QP_NAME_MAX + 32, L"%ls", t->name);
    title[QP_NAME_MAX + 31] = L'\0';
    SetWindowTextW(g_hTitle, title);
    EnableWindow(g_hBtnCopy, TRUE);
    EnableWindow(g_hBtnReset, TRUE);
    EnableWindow(g_hBtnEdit, TRUE);

    /* 切回来时接着上次那一份填，内容都还在 */
    ui_build_fields();
    tabs_refresh();
    ui_layout(g_hwndMain);
    ui_refresh_current();
    return 1;
}

Template *ui_current_template(void) { return g_curTmpl; }

/* 清空当前模板选择（数据目录切换 / 数据整体替换后调用，避免悬空指针） */
int ui_reset_selection(void)
{
    return ui_select_template(NULL);
}
