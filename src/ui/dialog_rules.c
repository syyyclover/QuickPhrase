#include "dialogs_internal.h"
#include "rule_layout.h"
/* ================================================================== */
/* 解析规则编辑器                                                      */
/* ================================================================== */

typedef struct {
    HWND hwnd;
    ParseRule *rule;             /* 直接编辑的对象（调用方负责副本语义） */
    const Template *tmpl;
    const Segment *owner_seg;
    int result;
    HWND hType, hDelim, hIndex, hKeyword, hMarkerAfter, hTarget, hFill, hPara, hParaHelp, hTip;
    HWND hParaModeLbl, hParaMode, hParaSepLbl, hParaSep;
    HWND hDelimLbl, hIndexLbl, hKeywordLbl, hMarkerAfterLbl;
    /* 「查表匹配」这一型不用上面那几栏，改用自己的一行：配置按钮 + 摘要 */
    HWND hLookupLbl, hLookupBtn, hLookupDesc;
    HWND lbl[7], ctl[7];         /* 通用行的标签与输入控件（按类型整体显隐） */
    int laying_out;
} ParseDlgCtx;

/* 把模板里可作为目标的变量填进下拉框，选中 sel_id */
void fill_var_combo(HWND combo, const Template *t, const wchar_t *sel_id,
                           const Segment *exclude, int include_static)
{
    const Segment *s;
    int i = 0, sel = -1;
    if (!combo) return;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    if (!t) return;                  /* 没模板就别去摸 t->segments */
    for (s = t->segments; s; s = s->next) {
        wchar_t item[QP_LABEL_MAX + 32];
        if (s == exclude) continue;
        if (s->type == SEG_STATIC && !include_static) continue;
        _snwprintf(item, QP_LABEL_MAX + 32, L"%ls", seg_display_name(s));
        item[QP_LABEL_MAX + 31] = L'\0';
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)item);
        SendMessageW(combo, CB_SETITEMDATA, (WPARAM)i, (LPARAM)s);
        if (sel_id && wcscmp(s->id, sel_id) == 0) sel = i;
        ++i;
    }
    if (sel < 0 && i > 0) sel = 0;
    if (sel >= 0) SendMessageW(combo, CB_SETCURSEL, (WPARAM)sel, 0);
}

const Segment *combo_selected_seg(HWND combo)
{
    int idx = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (idx == CB_ERR) return NULL;
    return (const Segment *)SendMessageW(combo, CB_GETITEMDATA, (WPARAM)idx, 0);
}

static QpRuleLayoutInput parse_layout_input(ParseDlgCtx *c,int width,int height)
{
    int line=qp_line_height(c->hType), full=width-DPIX(28);
    int summary=qp_wrapped_text_height(c->hLookupDesc,full-GetSystemMetrics(SM_CXVSCROLL)-DPIX(8));
    int min_summary=line*2+DPIX(8), max_summary=line*4+DPIX(8);
    if(summary<min_summary) summary=min_summary;
    if(summary>max_summary) summary=max_summary;
    return (QpRuleLayoutInput){g_dpi,width,height,(int)SendMessageW(c->hType,CB_GETCURSEL,0,0),line,
        summary,qp_wrapped_text_height(c->hParaHelp,full-DPIX(22)),qp_wrapped_text_height(c->hTip,full)};
}

static HDWP parse_place(HDWP batch,HWND h,QpRect r)
{ return qp_dialog_defer(batch,h,r.x,r.y,r.w,r.h); }

static void parse_dlg_layout(ParseDlgCtx *c,int fit)
{
    RECT client, frame;
    QpRuleLayout layout;
    QpRuleLayoutInput in;
    HDWP batch;
    if(!c || !c->hTip || c->laying_out) return;
    c->laying_out=1;
    qp_dialog_content_rect(c->hwnd,&client);
    in=parse_layout_input(c,client.right,client.bottom);
    qp_rule_layout(&in,&layout);
    if(qp_dialog_require_size(c->hwnd,DPIX(440),layout.minimum_height)) {
        qp_dialog_content_rect(c->hwnd,&client);
        in=parse_layout_input(c,client.right,client.bottom);
        qp_rule_layout(&in,&layout);
    } else if((fit && !IsZoomed(c->hwnd)) || client.bottom<layout.minimum_height) {
        int height=layout.minimum_height>DPIX(240)?layout.minimum_height:DPIX(240);
        frame=(RECT){0,0,client.right,height};
        AdjustWindowRectEx(&frame,(DWORD)GetWindowLongPtrW(c->hwnd,GWL_STYLE),FALSE,
                           (DWORD)GetWindowLongPtrW(c->hwnd,GWL_EXSTYLE));
        SetWindowPos(c->hwnd,NULL,0,0,frame.right-frame.left,frame.bottom-frame.top,
                     SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        GetClientRect(c->hwnd,&client);
        in=parse_layout_input(c,client.right,client.bottom);
        qp_rule_layout(&in,&layout);
    }
    batch=qp_layout_begin(24);
    for(int k=0;k<7;++k) {
        int show=(layout.visible_rows&(1u<<k))!=0;
        qp_show_if_needed(c->lbl[k],show); qp_show_if_needed(c->ctl[k],show);
        if(show) { batch=parse_place(batch,c->lbl[k],layout.labels[k]); batch=parse_place(batch,c->ctl[k],layout.fields[k]); }
    }
    int lookup=in.kind==4;
    qp_show_if_needed(c->hLookupLbl,lookup); qp_show_if_needed(c->hLookupBtn,lookup);
    qp_show_if_needed(c->hLookupDesc,lookup);
    if(lookup) {
        batch=parse_place(batch,c->hLookupLbl,layout.lookup_label);
        batch=parse_place(batch,c->hLookupBtn,layout.lookup_button);
        batch=parse_place(batch,c->hLookupDesc,layout.summary);
    }
    batch=parse_place(batch,c->hPara,layout.paragraph);
    batch=parse_place(batch,c->hParaHelp,layout.paragraph_help);
    batch=parse_place(batch,c->hParaModeLbl,layout.paragraph_mode_label);
    batch=parse_place(batch,c->hParaMode,layout.paragraph_mode);
    batch=parse_place(batch,c->hParaSepLbl,layout.paragraph_separator_label);
    batch=parse_place(batch,c->hParaSep,layout.paragraph_separator);
    batch=parse_place(batch,c->hTip,layout.tip);
    batch=parse_place(batch,GetDlgItem(c->hwnd,IDOK),layout.ok);
    batch=parse_place(batch,GetDlgItem(c->hwnd,IDCANCEL),layout.cancel);
    qp_layout_end(batch);
    qp_dialog_update_view(c->hwnd);
    c->laying_out=0;
    RedrawWindow(c->hwnd,NULL,NULL,RDW_INVALIDATE|RDW_ERASE);
}

static void parse_dlg_sync_enable(ParseDlgCtx *c)
{
    int sel = (int)SendMessageW(c->hType, CB_GETCURSEL, 0, 0);
    int is_split = (sel == 0), is_between = (sel == 2), is_regex = (sel == 3);
    int is_lookup = (sel == 4);
    /* 「逐段处理」对所有类型都可用：查表匹配的键源同样可以是多段文本，
       逐段各查一次、命中结果分行填进各自的目标变量。 */
    qp_set_text_if_diff(c->hPara,L"逐段处理");
    qp_set_text_if_diff(c->hParaHelp,
                   is_lookup
                     ? L"键源按换行逐段查表；命中结果可换行或连续填写。连续填写的分隔符留空表示直接连接。"
                     : L"源按换行逐段提取；命中结果可换行或连续填写。连续填写的分隔符留空表示直接连接。");
    int paragraph=SendMessageW(c->hPara,BM_GETCHECK,0,0)==BST_CHECKED;
    int continuous=SendMessageW(c->hParaMode,CB_GETCURSEL,0,0)==1;
    EnableWindow(c->hParaModeLbl,paragraph);
    EnableWindow(c->hParaMode,paragraph);
    EnableWindow(c->hParaSepLbl,paragraph && continuous);
    EnableWindow(c->hParaSep,paragraph && continuous);
    if (is_lookup) {
        wchar_t txt[QP_RESULT_MAX];
        lookup_rule_summary(c->rule, c->tmpl, txt, QP_RESULT_MAX);
        qp_set_text_if_diff(c->hLookupDesc, txt);
    }

    /* 分隔符：只有「按分隔符切割」用得上 */
    EnableWindow(c->hDelim, is_split);
    EnableWindow(c->hDelimLbl, is_split);
    /* 第几段 / 捕获组：同一个输入框，切割与正则两种类型都用 */
    EnableWindow(c->hIndex, is_split || is_regex);
    EnableWindow(c->hIndexLbl, is_split || is_regex);
    SetWindowTextW(c->hIndexLbl, is_regex ? L"捕获组：" : L"取第几段：");
    SendMessageW(c->hIndex, EM_SETCUEBANNER, TRUE,
                 (LPARAM)(is_regex ? L"0 = 整体匹配，1 = 第 1 个括号"
                                   : L"从 1 开始"));
    /* 后标记：只有「取两标记之间」用得上 */
    EnableWindow(c->hMarkerAfter, is_between);
    EnableWindow(c->hMarkerAfterLbl, is_between);
    /* 关键词一栏：切割=可选命中词、包含=必填词、取两标记之间=前标记、
       正则=正则表达式本身。不禁用，只按类型改标签、提示语与提示行。 */
    SetWindowTextW(c->hKeywordLbl,
                   is_between ? L"前标记："
                              : (is_regex ? L"正则表达式：" : L"命中关键词："));
    SendMessageW(c->hKeyword, EM_SETLIMITTEXT,
                 is_regex ? QP_PATTERN_MAX - 1 : QP_KEYWORD_MAX - 1, 0);
    SendMessageW(c->hKeyword, EM_SETCUEBANNER, TRUE,
                 (LPARAM)(is_regex ? L"如 设备编号[:：]\\s*([A-Za-z0-9\\-]+)"
                          : is_between ? L"留空表示从头开始取"
                                       : L"留空表示提取到非空内容时匹配成功"));
    SetWindowTextW(c->hTip,
        is_lookup
          ? L"查表匹配：以一个值作为键，在数据表的指定列中查找对应行，"
            L"并把该行的若干列写入模板变量（与表格软件的 VLOOKUP 类似）。"
            L"文件更新会在下次查询时检测；也可在数据表管理中重新加载。"
            L"勾选「逐段处理」可对多行键源逐段各查一次。"
        : is_regex
          ? L"例：正则「设备编号[:：]\\s*([A-Za-z0-9\\-]+)」取第 1 个捕获组 → 把编号填到目标变量。"
            L"支持 . * + ? {m,n} [] [^] () (?:) | ^ $ \\d \\w \\s 与 \\1 反向引用；"
            L"匹配大小写不敏感；勾选「逐段处理」可对多段文本逐段各取一次。"
        : is_between
          ? L"例：源文本中「网络设备」之后、「经核查」之前 → 取出设备名并填到目标变量。"
            L"勾选「逐段处理」可对多段文本逐段各取一次。"
          : is_split
            ? L"例：设备编号按「-」切割取第 8 段，命中「DBS」→ 设备类型填「存储服务器」。"
            : L"例：内容中包含「硬件」→ 将目标变量填为指定值。");
    parse_dlg_layout(c,1);
}

static LRESULT CALLBACK parse_dlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ParseDlgCtx *c = (ParseDlgCtx *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    int handled = 0;
    LRESULT r;

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        wchar_t buf[32];

        c = (ParseDlgCtx *)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)c);
        c->hwnd = hwnd;

        c->lbl[0] = mk_label_r(hwnd, L"规则类型：", 0);
        c->hType = mk_combo(hwnd, IDC_RL_TYPE);
        SendMessageW(c->hType, CB_ADDSTRING, 0, (LPARAM)L"按分隔符切割取第 N 段");
        SendMessageW(c->hType, CB_ADDSTRING, 0, (LPARAM)L"包含关键词");
        SendMessageW(c->hType, CB_ADDSTRING, 0, (LPARAM)L"取两标记之间");
        SendMessageW(c->hType, CB_ADDSTRING, 0, (LPARAM)L"正则表达式");
        SendMessageW(c->hType, CB_ADDSTRING, 0, (LPARAM)L"查表匹配（名单 / 联系表）");
        SendMessageW(c->hType, CB_SETCURSEL,
                     c->rule->type == PARSE_CONTAINS ? 1 :
                     c->rule->type == PARSE_BETWEEN ? 2 :
                     c->rule->type == PARSE_REGEX ? 3 :
                     c->rule->type == PARSE_LOOKUP ? 4 : 0, 0);

        c->lbl[1] = c->hDelimLbl = mk_label_r(hwnd, L"分隔符：", 0);
        c->hDelim = mk_edit(hwnd, IDC_RL_DELIM, 0);
        SetWindowTextW(c->hDelim, c->rule->delimiter);

        c->lbl[2] = c->hIndexLbl = mk_label_r(hwnd, L"取第几段：", 0);
        c->hIndex = mk_edit(hwnd, IDC_RL_INDEX, ES_NUMBER);
        /* 同一个框：切割时填第几段，正则时填捕获组序号 */
        if (c->rule->type == PARSE_REGEX)
            _snwprintf(buf, 32, L"%d", c->rule->capture_group > 0
                                          ? c->rule->capture_group : 0);
        else
            _snwprintf(buf, 32, L"%d",
                       c->rule->segment_index > 0 ? c->rule->segment_index : 1);
        SetWindowTextW(c->hIndex, buf);

        /* 命中关键词 / 前标记 / 正则表达式（同一控件，按类型改标签） */
        c->lbl[3] = c->hKeywordLbl = mk_label_r(hwnd, L"命中关键词：", 0);
        c->hKeyword = mk_edit(hwnd, IDC_RL_KEYWORD, 0);
        SendMessageW(c->hKeyword, EM_SETLIMITTEXT, QP_PATTERN_MAX - 1, 0);
        SetWindowTextW(c->hKeyword, c->rule->type == PARSE_REGEX ? c->rule->pattern
                                                                 : c->rule->keyword);

        /* 后标记：只在「取两标记之间」时可用 */
        c->lbl[4] = c->hMarkerAfterLbl = mk_label_r(hwnd, L"后标记：", 0);
        c->hMarkerAfter = mk_edit(hwnd, IDC_RL_MARKER_AFTER, 0);
        SetWindowTextW(c->hMarkerAfter, c->rule->marker_after);
        SendMessageW(c->hMarkerAfter, EM_SETCUEBANNER, TRUE, (LPARAM)L"留空表示取到末尾");

        c->lbl[5] = mk_label_r(hwnd, L"填充到变量：", 0);
        c->hTarget = mk_combo(hwnd, IDC_RL_TARGET);
        fill_var_combo(c->hTarget, c->tmpl, c->rule->target_segment_id, c->owner_seg, 0);

        c->lbl[6] = mk_label_r(hwnd, L"填充值：", 0);
        c->hFill = mk_edit(hwnd, IDC_RL_FILL, 0);
        SetWindowTextW(c->hFill, c->rule->fill_value);
        SendMessageW(c->hFill, EM_SETCUEBANNER, TRUE, (LPARAM)L"留空表示填入取到的原文");

        /* 逐段处理：自带文字，左侧与输入框对齐，对三种规则都可用 */
        c->hPara = mk_check(hwnd, L"逐段处理",
                            IDC_RL_PARA);
        c->hParaHelp=mk_wrapped_label(hwnd,L"",IDC_RL_PARA_HELP);
        SendMessageW(c->hPara, BM_SETCHECK,
                     c->rule->per_paragraph ? BST_CHECKED : BST_UNCHECKED, 0);
        c->hParaModeLbl=mk_label_r(hwnd,L"填写：",0);
        c->hParaMode=mk_combo(hwnd,IDC_RL_PARA_MODE);
        SendMessageW(c->hParaMode,CB_ADDSTRING,0,(LPARAM)L"换行填写");
        SendMessageW(c->hParaMode,CB_ADDSTRING,0,(LPARAM)L"连续填写");
        SendMessageW(c->hParaMode,CB_SETCURSEL,c->rule->paragraph_inline?1:0,0);
        c->hParaSepLbl=mk_label_r(hwnd,L"分隔符：",0);
        c->hParaSep=mk_edit(hwnd,IDC_RL_PARA_SEP,0);
        SendMessageW(c->hParaSep,EM_SETLIMITTEXT,QP_SEP_MAX-1,0);
        SetWindowTextW(c->hParaSep,c->rule->paragraph_separator);

        /* 查表匹配专用的一行：配置按钮 + 当前配置摘要（借用第 1 行的位置） */
        c->hLookupLbl = mk_label_r(hwnd, L"查表设置：", IDC_RL_LOOKUP_LBL);
        c->hLookupBtn = mk_btn(hwnd, L"配置查表…", IDC_RL_LOOKUP_CFG);
        c->hLookupDesc=qp_create(L"Edit",L"",ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL|WS_TABSTOP,
                                    WS_EX_CLIENTEDGE,0,0,10,10,hwnd,IDC_RL_LOOKUP_DESC);

        c->hTip = mk_wrapped_label(hwnd, L"", IDC_RL_TIP);
        mk_btn(hwnd, L"确定", IDOK);
        mk_btn(hwnd, L"取消", IDCANCEL);
        SendMessageW(GetDlgItem(hwnd, IDOK), BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);

        {
            c->ctl[0] = c->hType;    c->ctl[1] = c->hDelim;       c->ctl[2] = c->hIndex;
            c->ctl[3] = c->hKeyword; c->ctl[4] = c->hMarkerAfter; c->ctl[5] = c->hTarget;
            c->ctl[6] = c->hFill;
        }

        parse_dlg_sync_enable(c);
        qp_theme_children(hwnd);
        SetFocus(c->hType);
        return 0;
    }
    case WM_SIZE:
        if(wp!=SIZE_MINIMIZED) parse_dlg_layout(c,0);
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi=(MINMAXINFO *)lp;
        RECT frame={0,0,DPIX(440),DPIX(240)};
        if(c && c->hTip) {
            QpRuleLayout layout;
            QpRuleLayoutInput in=parse_layout_input(c,DPIX(440),DPIX(240));
            qp_rule_layout(&in,&layout); frame.bottom=layout.minimum_height;
        }
        AdjustWindowRectEx(&frame,(DWORD)GetWindowLongPtrW(hwnd,GWL_STYLE),FALSE,
                           (DWORD)GetWindowLongPtrW(hwnd,GWL_EXSTYLE));
        mmi->ptMinTrackSize.x=frame.right-frame.left;
        mmi->ptMinTrackSize.y=frame.bottom-frame.top;
        qp_dialog_limit_tracking(hwnd,mmi);
        return 0;
    }
    case WM_COMMAND:
        qp_combo_dropdown_fix(wp, lp);
        if (!c) break;
        if ((LOWORD(wp)==IDC_RL_PARA && HIWORD(wp)==BN_CLICKED) ||
            (LOWORD(wp)==IDC_RL_PARA_MODE && HIWORD(wp)==CBN_SELCHANGE)) {
            parse_dlg_sync_enable(c);
            return 0;
        }
        if (LOWORD(wp) == IDC_RL_TYPE && HIWORD(wp) == CBN_SELCHANGE) {
            parse_dlg_sync_enable(c);
            return 0;
        }
        if (LOWORD(wp) == IDC_RL_LOOKUP_CFG) {
            dlg_lookup_rule(hwnd, c->rule, c->tmpl);
            c->rule->type = PARSE_LOOKUP;
            SendMessageW(c->hType, CB_SETCURSEL, 4, 0);
            parse_dlg_sync_enable(c);
            return 0;
        }
        if (LOWORD(wp) == IDOK) {
            wchar_t buf[QP_VALUE_MAX];
            const Segment *target = combo_selected_seg(c->hTarget);
            int sel = (int)SendMessageW(c->hType, CB_GETCURSEL, 0, 0);
            int is_split = (sel == 0), is_contains = (sel == 1);
            int is_between = (sel == 2), is_regex = (sel == 3);
            int is_lookup = (sel == 4);

            /* 查表匹配的落点由「配置查表…」里的输出列决定，不看这一栏 */
            if (!target && !is_lookup) {
                qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"提示",
                          L"请先在模板中添加一个变量作为填充目标。");
                return 0;
            }
            if (is_lookup && !c->rule->lk_table_id[0]) {
                if (qp_msgbox(hwnd, MB_YESNO | MB_ICONQUESTION, L"查表尚未配置",
                              L"该查表规则尚未选择数据表，目前不会生效。\n\n"
                              L"是否现在配置？") == IDYES) {
                    dlg_lookup_rule(hwnd, c->rule, c->tmpl);
                    parse_dlg_sync_enable(c);
                    return 0;
                }
            }
            qp_get_text(c->hDelim, buf, QP_VALUE_MAX);
            if (is_split && !buf[0]) {
                qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"提示", L"请填写分隔符。");
                SetFocus(c->hDelim);
                return 0;
            }
            c->rule->type = is_lookup ? PARSE_LOOKUP :
                            is_regex  ? PARSE_REGEX  :
                            is_split  ? PARSE_SPLIT  :
                            is_between ? PARSE_BETWEEN : PARSE_CONTAINS;
            qp_strlcpy(c->rule->delimiter, buf, QP_SEP_MAX);
            qp_get_text(c->hIndex, buf, QP_VALUE_MAX);
            if (is_regex) {
                c->rule->capture_group = _wtoi(buf);
                if (c->rule->capture_group < 0) c->rule->capture_group = 0;
                if (c->rule->capture_group >= QP_REGEX_MAX_GROUPS)
                    c->rule->capture_group = QP_REGEX_MAX_GROUPS - 1;
            } else {
                c->rule->segment_index = _wtoi(buf);
                if (c->rule->segment_index <= 0) c->rule->segment_index = 1;
            }
            /* 关键词一栏 = 命中词（分隔/包含）/ 前标记（取两标记之间）/ 正则本体 */
            qp_get_text(c->hKeyword, buf, QP_VALUE_MAX);
            qp_trim(buf);
            if (is_regex) {
                int epos = -1;
                if (!buf[0]) {
                    qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"提示",
                              L"请填写正则表达式。");
                    SetFocus(c->hKeyword);
                    return 0;
                }
                if (!qp_regex_valid(buf, &epos)) {
                    qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"正则表达式有误",
                              L"正则表达式在位置 %d 附近无法解析，请检查括号、"
                              L"方括号或量词是否配对。", epos + 1);
                    SetFocus(c->hKeyword);
                    return 0;
                }
                qp_strlcpy(c->rule->pattern, buf, QP_PATTERN_MAX);
            } else {
                qp_strlcpy(c->rule->keyword, buf, QP_KEYWORD_MAX);
                if (is_contains && !buf[0]) {
                    qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"提示",
                              L"「包含关键词」规则必须填写关键词。");
                    SetFocus(c->hKeyword);
                    return 0;
                }
            }
            /* 后标记（仅取两标记之间用；其余类型留空不影响） */
            qp_get_text(c->hMarkerAfter, buf, QP_VALUE_MAX);
            qp_trim(buf);
            qp_strlcpy(c->rule->marker_after, buf, QP_KEYWORD_MAX);
            c->rule->per_paragraph =
                (SendMessageW(c->hPara, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
            c->rule->paragraph_inline=SendMessageW(c->hParaMode,CB_GETCURSEL,0,0)==1;
            qp_get_text(c->hParaSep,c->rule->paragraph_separator,QP_SEP_MAX);
            if (target)
                qp_strlcpy(c->rule->target_segment_id, target->id, QP_ID_MAX);
            qp_get_text(c->hFill, buf, QP_VALUE_MAX);
            qp_strlcpy(c->rule->fill_value, buf, QP_VALUE_MAX);
            c->result = 1;
            dlg_close(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) { c->result = 0; dlg_close(hwnd); return 0; }
        break;
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

int dlg_parse_rule(HWND owner, ParseRule *rule, const Template *t,
                          const Segment *owner_seg)
{
    ParseDlgCtx c;
    HWND dlg;
    memset(&c, 0, sizeof(c));
    c.rule = rule;
    c.tmpl = t;
    c.owner_seg = owner_seg;
    dlg = make_dlg_ex(L"QuickPhraseParseRule", parse_dlg_proc, L"解析规则",
                   520, 330, owner, &c,WS_THICKFRAME|WS_MAXIMIZEBOX);
    return run_modal(owner, dlg, &c.result);
}

/* ================================================================== */
/* 条件规则编辑器                                                      */
/*                                                                    */
/* 一条规则 = 若干「条件项」 + 组合方式（全部 / 任意） + 输出内容。      */
/* 列表选中一项，下面的编辑区即时改写它——不再另开一层弹窗。            */
/* ================================================================== */

/* 关系下拉的显示顺序（与枚举值解耦，以后插入新关系不影响存档） */
static const CondOperator kCondOps[] = {
    OP_CONTAINS, OP_EQUALS, OP_STARTS_WITH, OP_ENDS_WITH, OP_IN, OP_EMPTY
};
#define QP_COND_OP_COUNT ((int)(sizeof(kCondOps) / sizeof(kCondOps[0])))

typedef struct {
    HWND hwnd;
    ConditionRule *rule;
    const Template *tmpl;
    const Segment *owner_seg;
    int result;
    int loading;                 /* 1 = 正在往控件里灌值，忽略变更通知 */
    HWND hJoin, hList, hResult;
    HWND hTrigger, hOp, hNegate, hMatch;
    HWND hTrigLbl, hOpLbl, hMatchLbl;
} CondDlgCtx;

static int cond_op_to_index(CondOperator op)
{
    int i;
    for (i = 0; i < QP_COND_OP_COUNT; ++i)
        if (kCondOps[i] == op) return i;
    return 0;
}

static CondOperator cond_op_from_combo(HWND combo)
{
    int i = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (i < 0 || i >= QP_COND_OP_COUNT) return OP_CONTAINS;
    return kCondOps[i];
}

/* 刷新列表里某一行的摘要文字 */
static void cond_row_text(CondDlgCtx *c, int row)
{
    wchar_t txt[QP_MATCH_MAX + QP_LABEL_MAX + 32];
    wchar_t line[QP_MATCH_MAX + QP_LABEL_MAX + 48];
    CondTerm *t = cond_term_at(c->rule->terms, row);
    if (!t) return;
    cond_term_summary(t, c->tmpl, txt, sizeof(txt) / sizeof(txt[0]));
    _snwprintf(line, sizeof(line) / sizeof(line[0]), L"%d.  %ls", row + 1, txt);
    line[sizeof(line) / sizeof(line[0]) - 1] = L'\0';
    lv_set_row(c->hList, row, 0, line);
}

/* 匹配值框的占位提示：跟着「关系」走，一次只说一件事 */
static const wchar_t *cond_cue_for(CondOperator op)
{
    switch (op) {
    case OP_EQUALS:      return L"填写完整匹配值，例如：数据库服务器";
    case OP_STARTS_WITH: return L"填写需要匹配的开头，例如：数据库";
    case OP_ENDS_WITH:   return L"填写需要匹配的结尾，例如：服务器";
    case OP_IN:          return L"填写匹配值；多个条件可逐项添加。";
    case OP_EMPTY:       return L"「为空」不需要匹配值";
    default:             return L"填写变量值中包含的文字，例如：数据库";
    }
}

static void cond_sync_enable(CondDlgCtx *c)
{
    int on = cond_term_at(c->rule->terms, lv_sel(c->hList)) != NULL;
    int needs = on && cond_op_needs_value(cond_op_from_combo(c->hOp));
    SendMessageW(c->hMatch, EM_SETCUEBANNER, TRUE,
                 (LPARAM)cond_cue_for(cond_op_from_combo(c->hOp)));
    EnableWindow(c->hTrigLbl, on);
    EnableWindow(c->hTrigger, on);
    EnableWindow(c->hOpLbl, on);
    EnableWindow(c->hOp, on);
    EnableWindow(c->hNegate, on);
    EnableWindow(c->hMatchLbl, needs);
    EnableWindow(c->hMatch, needs);
    /* 至少留一个条件项，避免出现永远不会命中的空规则 */
    EnableWindow(GetDlgItem(c->hwnd, IDC_RL_TERM_DEL),
                 on && cond_term_count(c->rule->terms) > 1);
}

/* 把选中的条件项灌进编辑区 */
static void cond_load_term(CondDlgCtx *c)
{
    int row = lv_sel(c->hList);
    CondTerm *t = cond_term_at(c->rule->terms, row);

    c->loading = 1;
    if (t) {
        fill_var_combo(c->hTrigger, c->tmpl, t->trigger_segment_id, c->owner_seg, 0);
        SendMessageW(c->hOp, CB_SETCURSEL, (WPARAM)cond_op_to_index(t->op), 0);
        CheckDlgButton(c->hwnd, IDC_RL_NEGATE, t->negate ? BST_CHECKED : BST_UNCHECKED);
        SetWindowTextW(c->hMatch, t->match_value);
    } else {
        SendMessageW(c->hTrigger, CB_RESETCONTENT, 0, 0);
        SendMessageW(c->hOp, CB_SETCURSEL, 0, 0);
        CheckDlgButton(c->hwnd, IDC_RL_NEGATE, BST_UNCHECKED);
        SetWindowTextW(c->hMatch, L"");
    }
    c->loading = 0;

    /* 新条件项还没选变量：直接采纳下拉框里显示的那个，做到「看到的就是存下的」 */
    if (t && !t->trigger_segment_id[0]) {
        const Segment *s = combo_selected_seg(c->hTrigger);
        if (s) {
            qp_strlcpy(t->trigger_segment_id, s->id, QP_ID_MAX);
            cond_row_text(c, row);
        }
    }
    cond_sync_enable(c);
}

/* 把编辑区的内容写回选中的条件项 */
static int cond_commit_term(CondDlgCtx *c)
{
    int row = lv_sel(c->hList);
    CondTerm *t = cond_term_at(c->rule->terms, row);
    const Segment *trig;
    wchar_t buf[QP_MATCH_MAX];

    if (c->loading || !t) return 1;
    qp_get_text(c->hMatch, buf, QP_MATCH_MAX);
    qp_trim(buf);
    if (wcscmp(t->match_value, buf) && !qp_cond_term_set_match(t, buf)) {
        qp_msgbox(c->hwnd, MB_OK | MB_ICONERROR, L"无法修改条件",
                  L"内存不足，条件修改未应用。原条件未改变。请关闭不需要的窗口后重试。");
        return 0;
    }
    trig = combo_selected_seg(c->hTrigger);
    qp_strlcpy(t->trigger_segment_id, trig ? trig->id : L"", QP_ID_MAX);
    t->op = cond_op_from_combo(c->hOp);
    t->negate = (IsDlgButtonChecked(c->hwnd, IDC_RL_NEGATE) == BST_CHECKED);
    cond_row_text(c, row);
    return 1;
}

static void cond_fill_terms(CondDlgCtx *c, int sel)
{
    int n = cond_term_count(c->rule->terms), i;
    ListView_DeleteAllItems(c->hList);
    for (i = 0; i < n; ++i) cond_row_text(c, i);
    if (n > 0) lv_select(c->hList, sel);
    cond_load_term(c);
}

static LRESULT CALLBACK cond_dlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    CondDlgCtx *c = (CondDlgCtx *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    int handled = 0;
    LRESULT r;

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        int m = DPIX(14), y = DPIX(14), rowh = DPIX(28);
        int lw = DPIX(64), cx, cw, full, bw = DPIX(86);
        RECT rc;
        HWND lJoin, lWhen, lSect, lOut, tip, add, del;
        int i;

        c = (CondDlgCtx *)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)c);
        c->hwnd = hwnd;
        GetClientRect(hwnd, &rc);
        full = rc.right - m * 2;
        cx = m + lw + DPIX(8);
        cw = rc.right - m - cx;

        /* ---- 第一行：组合方式 + 增删按钮 ---- */
        lJoin = mk_label(hwnd, L"当以下条件", 0);
        c->hJoin = mk_combo(hwnd, IDC_RL_JOIN);
        SendMessageW(c->hJoin, CB_ADDSTRING, 0, (LPARAM)L"全部满足（且）");
        SendMessageW(c->hJoin, CB_ADDSTRING, 0, (LPARAM)L"任意满足（或）");
        SendMessageW(c->hJoin, CB_SETCURSEL,
                     c->rule->join == COND_JOIN_ANY ? 1 : 0, 0);
        lWhen = mk_label(hwnd, L"时：", 0);
        add = mk_btn(hwnd, L"添加条件", IDC_RL_TERM_ADD);
        del = mk_btn(hwnd, L"删除条件", IDC_RL_TERM_DEL);

        MoveWindow(lJoin, m, y + DPIX(5), DPIX(70), DPIX(18), TRUE);
        MoveWindow(c->hJoin, m + DPIX(74), y, DPIX(126), DPIX(24), TRUE);
        MoveWindow(lWhen, m + DPIX(204), y + DPIX(5), DPIX(28), DPIX(18), TRUE);
        MoveWindow(add, rc.right - m - bw * 2 - DPIX(8), y, bw, DPIX(24), TRUE);
        MoveWindow(del, rc.right - m - bw, y, bw, DPIX(24), TRUE);
        y += DPIX(32);

        /* ---- 条件项列表 ---- */
        c->hList = mk_list(hwnd, IDC_RL_TERMLIST, LVS_NOCOLUMNHEADER);
        lv_add_col(c->hList, 0, L"条件", 300);
        MoveWindow(c->hList, m, y, full, DPIX(116), TRUE);
        ListView_SetColumnWidth(c->hList, 0, full - DPIX(26));
        y += DPIX(126);

        /* ---- 选中条件项的编辑区 ---- */
        lSect = mk_label(hwnd, L"编辑选中的条件：", 0);
        MoveWindow(lSect, m, y, full, DPIX(18), TRUE);
        y += DPIX(24);

        c->hTrigLbl = mk_label_r(hwnd, L"变量：", 0);
        c->hTrigger = mk_combo(hwnd, IDC_RL_TRIGGER);
        MoveWindow(c->hTrigLbl, m, y + DPIX(4), lw, DPIX(18), TRUE);
        MoveWindow(c->hTrigger, cx, y, cw, DPIX(24), TRUE);
        y += rowh;

        c->hOpLbl = mk_label_r(hwnd, L"关系：", 0);
        c->hOp = mk_combo(hwnd, IDC_RL_OP);
        for (i = 0; i < QP_COND_OP_COUNT; ++i)
            SendMessageW(c->hOp, CB_ADDSTRING, 0, (LPARAM)cond_op_name(kCondOps[i]));
        c->hNegate = mk_check(hwnd, L"不满足时才成立（取反）", IDC_RL_NEGATE);
        MoveWindow(c->hOpLbl, m, y + DPIX(4), lw, DPIX(18), TRUE);
        MoveWindow(c->hOp, cx, y, DPIX(120), DPIX(24), TRUE);
        MoveWindow(c->hNegate, cx + DPIX(132), y + DPIX(2), cw - DPIX(132), DPIX(20), TRUE);
        y += rowh;

        c->hMatchLbl = mk_label_r(hwnd, L"匹配值：", 0);
        c->hMatch = mk_edit(hwnd, IDC_RL_MATCH, 0);
        SendMessageW(c->hMatch, EM_SETCUEBANNER, TRUE,
                     (LPARAM)cond_cue_for(OP_CONTAINS));
        MoveWindow(c->hMatchLbl, m, y + DPIX(4), lw, DPIX(18), TRUE);
        MoveWindow(c->hMatch, cx, y, cw, DPIX(24), TRUE);
        y += DPIX(30);

        tip = mk_wrapped_label(hwnd,
                       L"多个条件可逐项添加；勾选「不满足时才成立（取反）」可否定匹配结果。",
                       IDC_RL_TIP);
        { int height=qp_wrapped_text_height(tip,full);
          MoveWindow(tip,m,y,full,height,TRUE); y+=height+DPIX(8); }

        /* ---- 输出内容 ---- */
        lOut = mk_label(hwnd, L"命中时，本片段的内容替换为：", 0);
        MoveWindow(lOut, m, y, full, DPIX(18), TRUE);
        y += DPIX(22);
        c->hResult = mk_edit(hwnd, IDC_RL_RESULT,
                             ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | WS_VSCROLL);
        SetWindowTextW(c->hResult, c->rule->result_value);
        MoveWindow(c->hResult, m, y, full, DPIX(64), TRUE);

        mk_btn(hwnd, L"确定", IDOK);
        mk_btn(hwnd, L"取消", IDCANCEL);
        SendMessageW(GetDlgItem(hwnd, IDOK), BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);
        MoveWindow(GetDlgItem(hwnd, IDOK), rc.right - m - DPIX(180),
                   rc.bottom - m - DPIX(26), bw, DPIX(26), TRUE);
        MoveWindow(GetDlgItem(hwnd, IDCANCEL), rc.right - m - DPIX(88),
                   rc.bottom - m - DPIX(26), bw, DPIX(26), TRUE);

        qp_theme_children(hwnd);
        cond_fill_terms(c, 0);
        SetFocus(c->hList);
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (c && nh && nh->idFrom == IDC_RL_TERMLIST && nh->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *nv = (NMLISTVIEW *)lp;
            if ((nv->uNewState & LVIS_SELECTED) && !(nv->uOldState & LVIS_SELECTED))
                cond_load_term(c);
        }
        break;
    }
    case WM_COMMAND:
        qp_combo_dropdown_fix(wp, lp);
        if (!c) break;
        switch (LOWORD(wp)) {
        case IDC_RL_JOIN:
            if (HIWORD(wp) == CBN_SELCHANGE)
                c->rule->join = (SendMessageW(c->hJoin, CB_GETCURSEL, 0, 0) == 1)
                                ? COND_JOIN_ANY : COND_JOIN_ALL;
            return 0;
        case IDC_RL_TRIGGER:
            if (HIWORD(wp) == CBN_SELCHANGE) cond_commit_term(c);
            return 0;
        case IDC_RL_OP:
            if (HIWORD(wp) == CBN_SELCHANGE) { cond_commit_term(c); cond_sync_enable(c); }
            return 0;
        case IDC_RL_NEGATE:
            if (HIWORD(wp) == BN_CLICKED) cond_commit_term(c);
            return 0;
        case IDC_RL_MATCH:
            if (HIWORD(wp) == EN_CHANGE) cond_commit_term(c);
            return 0;
        case IDC_RL_TERM_ADD:
            cond_term_append(&c->rule->terms, cond_term_create());
            cond_fill_terms(c, cond_term_count(c->rule->terms) - 1);
            SetFocus(c->hTrigger);
            return 0;
        case IDC_RL_TERM_DEL: {
            int row = lv_sel(c->hList);
            if (row >= 0 && cond_term_count(c->rule->terms) > 1) {
                cond_term_remove_at(&c->rule->terms, row);
                cond_fill_terms(c, row);
            }
            return 0;
        }
        case IDOK: {
            wchar_t buf[QP_RESULT_MAX];
            const CondTerm *t;
            const Segment *sg;
            int i = 0, nvar = 0;
            if (!cond_commit_term(c)) return 0;

            /* 模板里一个变量都没有，条件规则无从下手（口径要先有变量） */
            for (sg = c->tmpl->segments; sg; sg = sg->next)
                if (sg != c->owner_seg && sg->type != SEG_STATIC) ++nvar;
            if (nvar == 0) {
                qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"提示",
                          L"请先在模板中添加一个变量作为触发条件。");
                return 0;
            }
            if (!c->rule->terms) {
                qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"提示",
                          L"请至少添加一个条件。");
                return 0;
            }
            for (t = c->rule->terms; t; t = t->next, ++i) {
                if (!tmpl_find_segment(c->tmpl, t->trigger_segment_id)) {
                    qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"提示",
                              L"第 %d 个条件尚未选择变量。", i + 1);
                    lv_select(c->hList, i);
                    SetFocus(c->hTrigger);
                    return 0;
                }
                if (cond_op_needs_value(t->op) && !t->match_value[0]) {
                    qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"提示",
                              L"第 %d 个条件（%ls）需要填写匹配值。\n"
                              L"若需表示「该变量为空」，请将关系改为「为空」。",
                              i + 1, cond_op_name(t->op));
                    lv_select(c->hList, i);
                    SetFocus(c->hMatch);
                    return 0;
                }
            }
            qp_get_text(c->hResult, buf, QP_RESULT_MAX);
            qp_strlcpy(c->rule->result_value, buf, QP_RESULT_MAX);
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

int dlg_cond_rule(HWND owner, ConditionRule *rule, const Template *t,
                         const Segment *owner_seg)
{
    CondDlgCtx c;
    HWND dlg;
    memset(&c, 0, sizeof(c));
    c.rule = rule;
    c.tmpl = t;
    c.owner_seg = owner_seg;
    dlg = make_dlg(L"QuickPhraseCondRule", cond_dlg_proc, L"条件规则",
                   520, 486, owner, &c);
    return run_modal(owner, dlg, &c.result);
}
