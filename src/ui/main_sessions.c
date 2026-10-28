#include "ui_internal.h"
/* ================================================================== */
/* 并行填写：会话 + 右侧顶部标签条                                     */
/*                                                                    */
/* 同一个口径可以同时开好几份填写，互不干扰 —— 一边盯 A 群的管控、一边  */
/* 发 B 群的通知，切到别的口径再切回来也不用重填。                      */
/*                                                                    */
/* 生命周期：只有「当前这一份」持有 EvalContext，切走时先把手填值抄进会 */
/* 话自己的快照再销毁上下文。于是 Session 里绝不保存 Template/Segment   */
/* 指针 —— 模板一被编辑就会整对象替换，切数据目录时更是先把整份数据释放 */
/* 掉才通知界面，存指针必然悬空。变量 id 列表在激活时一次抓好，保存取值 */
/* 时只读 EvalContext，不再回头碰片段。                                */
/* ================================================================== */





static Session **g_sess;
static int       g_sessCount;
static unsigned  g_sessTick;
Session  *g_curSess;        /* 当前这一份；其取值活在 g_ctx 里 */

/* 标签颜色：深浅两种主题下都够醒目，顺序即默认分配顺序 */
static const COLORREF s_sessPalette[QP_SESS_MAX] = {
    RGB(88, 148, 214), RGB(96, 176, 120), RGB(224, 152, 72),
    RGB(198, 108, 176), RGB(214, 96, 96), RGB(88, 176, 190),
    RGB(180, 168, 88), RGB(120, 132, 214), RGB(140, 146, 158)
};
static const wchar_t *s_sessColorName[QP_SESS_MAX] = {
    L"蓝", L"绿", L"橙", L"紫", L"红", L"青", L"黄", L"靛", L"灰"
};

static COLORREF sess_color_of(const Session *s)
{
    int i = s ? s->color : 0;
    if (i < 0 || i >= QP_SESS_MAX) i = 0;
    return s_sessPalette[i];
}

static void sess_drop_values(Session *s)
{
    int i;
    if (!s) return;
    if (s->vals) {
        for (i = 0; i < s->count; ++i) free(s->vals[i]);
        free(s->vals);
        s->vals = NULL;
    }
    free(s->ids);
    s->ids = NULL;
    for(int i=0;i<s->count;++i) {
        if(s->choice_values)free(s->choice_values[i]);
        if(s->choice_bases)free(s->choice_bases[i]);
    }
    free(s->choice_values);free(s->choice_bases);free(s->choice_flags);
    s->choice_values=s->choice_bases=NULL;s->choice_flags=NULL;
    s->count = 0;
}

static void sess_free(Session *s)
{
    if (!s) return;
    sess_drop_values(s);
    free(s);
}

/* ------------------- 当前模板下的「第几份」查询 ------------------- */

static int sess_visible_count(void)
{
    int i, n = 0;
    if (!g_curTmpl) return 0;
    for (i = 0; i < g_sessCount; ++i)
        if (wcscmp(g_sess[i]->tmpl_id, g_curTmpl->id) == 0) ++n;
    return n;
}

static Session *sess_visible_at(int vis)
{
    int i, n = 0;
    if (!g_curTmpl || vis < 0) return NULL;
    for (i = 0; i < g_sessCount; ++i) {
        if (wcscmp(g_sess[i]->tmpl_id, g_curTmpl->id) != 0) continue;
        if (n == vis) return g_sess[i];
        ++n;
    }
    return NULL;
}

static int sess_visible_index(const Session *s)
{
    int i, n = 0;
    if (!s || !g_curTmpl) return -1;
    for (i = 0; i < g_sessCount; ++i) {
        if (wcscmp(g_sess[i]->tmpl_id, g_curTmpl->id) != 0) continue;
        if (g_sess[i] == s) return n;
        ++n;
    }
    return -1;
}

static int sess_has_input(const Session *s)
{
    int i;
    if (!s) return 0;
    if (s == g_curSess && g_ctx) {
        for (i = 0; i < g_ctx->count; ++i)
            if (g_ctx->fields[i].user_touched && g_ctx->fields[i].user_value[0]) return 1;
        return 0;
    }
    for (i = 0; i < s->count; ++i)
        if (s->vals && s->vals[i] && s->vals[i][0]) return 1;
    return 0;
}

/* ---------------------- 取值的保存与恢复 -------------------------- */

/* Build a complete candidate while the context's template is still alive. */
static int sess_snapshot(Session *out, const EvalContext *ctx)
{
    if (!ctx) return 1;
    out->count = ctx->count;
    if (out->count > 0) {
        out->ids = calloc((size_t)out->count, sizeof(*out->ids));
        out->vals = calloc((size_t)out->count, sizeof(*out->vals));
        if (!out->ids || !out->vals) goto fail;
        int choices=0;
        for(int i=0;i<out->count;++i)if(ctx->fields[i].choice_tracked)choices=1;
        if(choices) {
            out->choice_values=calloc((size_t)out->count,sizeof(*out->choice_values));
            out->choice_bases=calloc((size_t)out->count,sizeof(*out->choice_bases));
            out->choice_flags=calloc((size_t)out->count,1);
            if(!out->choice_values || !out->choice_bases || !out->choice_flags)goto fail;
        }
        for (int i = 0; i < out->count; ++i) {
            const EvalField *f = &ctx->fields[i];
            qp_strlcpy(out->ids[i], f->seg ? f->seg->id : L"", QP_ID_MAX);
            if (f->user_touched) {
                out->vals[i] = qp_wcsdup(f->user_value);
                if (!out->vals[i]) goto fail;
            }
            if(f->choice_tracked) {
                out->choice_flags[i]=(unsigned char)(f->custom_selected?2:1);
                out->choice_values[i]=qp_wcsdup(f->custom_value);
                out->choice_bases[i]=qp_wcsdup(f->choice_base);
                if(!out->choice_values[i] || !out->choice_bases[i])goto fail;
            }
        }
    }
    return 1;
fail:
    sess_drop_values(out);
    return 0;
}

static void sess_publish_values(Session *dst, Session *owned)
{
    sess_drop_values(dst);
    dst->ids = owned->ids; dst->vals = owned->vals; dst->count = owned->count;
    dst->choice_values=owned->choice_values;dst->choice_bases=owned->choice_bases;dst->choice_flags=owned->choice_flags;
    owned->choice_values=owned->choice_bases=NULL;owned->choice_flags=NULL;
    owned->ids = NULL; owned->vals = NULL; owned->count = 0;
}

static int sess_snapshot_current(const Session *s)
{
    if (!g_ctx) return 1;
    if (!s || s->count != g_ctx->count ||
        (s->count > 0 && (!s->ids || !s->vals))) return 0;
    for (int i = 0; i < s->count; ++i) {
        const EvalField *f = &g_ctx->fields[i];
        if (wcscmp(s->ids[i], f->seg ? f->seg->id : L"")) return 0;
        if (f->user_touched) {
            if (!s->vals[i] || wcscmp(s->vals[i], f->user_value)) return 0;
        } else if (s->vals[i]) return 0;
        unsigned flag=f->choice_tracked?(f->custom_selected?2u:1u):0u;
        if((s->choice_flags?s->choice_flags[i]:0)!=flag)return 0;
        if(flag && (!s->choice_values || !s->choice_values[i] || !s->choice_bases || !s->choice_bases[i] ||
            wcscmp(s->choice_values[i],f->custom_value) || wcscmp(s->choice_bases[i],f->choice_base)))return 0;
    }
    return 1;
}

static int sess_save(Session *s)
{
    Session candidate = {0};
    if (!g_ctx) return 1;
    if (!s) return 0;
    if (sess_snapshot_current(s)) return 1;
    if (!sess_snapshot(&candidate, g_ctx)) return 0;
    sess_publish_values(s, &candidate);
    return 1;
}

/* Preflight a model replacement before its settings/file commit. A subsequent
   deactivate is allocation-free while the input remains unchanged. */
int sess_capture_current(void)
{ return sess_save(g_curSess); }

/* 口径 A：模板改了选项之后，会话里存的旧值必须向新选项集对齐。
   值里已不存在的选项丢掉、仍存在的保留；自主补充文字保留（新模板还有补充项
   时）。认不出的旧值原样留着 —— 宁可显示不一致，也不静默吞掉用户手填的内容。
   返回 0 表示内存不足（调用方走 fail 路径）。 */
static void sess_clear_choice(EvalField *f)
{
    f->user_touched = 0;
    f->user_value[0] = L'\0';
    f->choice_tracked = 0;
    f->custom_selected = 0;
    f->choice_base[0] = L'\0';
    f->custom_value[0] = L'\0';
}

static int sess_reconcile_choices(EvalContext *ctx)
{
    if (!ctx) return 1;
    for (int i = 0; i < ctx->count; ++i) {
        EvalField *f = &ctx->fields[i];
        const Segment *seg = f->seg;
        wchar_t value[QP_VALUE_MAX], custom_in[QP_VALUE_MAX], custom_out[QP_VALUE_MAX];
        const wchar_t *sep;
        unsigned char *checked;
        int rc, n;

        if (!seg || (seg->type != SEG_SELECT && seg->type != SEG_MULTI_SELECT)) continue;
        if (!f->user_touched) continue;         /* 没填过：模板默认值本来就对 */
        n = seg->option_count;
        if (n <= 0) { sess_clear_choice(f); continue; }

        /* 多选的旧值优先取 choice_base（不含补充文字）；没有就退回 user_value */
        if (seg->type == SEG_MULTI_SELECT && f->choice_tracked && f->choice_base[0])
            qp_strlcpy(value, f->choice_base, QP_VALUE_MAX);
        else
            qp_strlcpy(value, f->user_value, QP_VALUE_MAX);
        qp_strlcpy(custom_in, f->custom_value, QP_VALUE_MAX);

        checked = (unsigned char *)calloc((size_t)n, 1);
        if (!checked) return 0;
        sep = multi_sep(seg);
        rc = qp_choice_realign(seg, value, custom_in, sep, checked, custom_out, QP_VALUE_MAX);
        if (rc > 0) {
            if (!qp_choice_set(ctx, i, checked, n, custom_out, sep)) { free(checked); return 0; }
        } else if (rc == 0) {
            sess_clear_choice(f);               /* 旧值一项都对不上了 */
        }
        /* rc < 0：认不出旧值，保持原样 */
        free(checked);
    }
    return 1;
}

int sess_activate(Session *s, const Template *t)
{
    Session next_values = {0}, previous_values = {0};
    EvalContext *next;
    int previous_needed = 0;
    if (!s || !t) return 0;
    next = eval_create(t);
    if (!next) return 0;
    if (s == g_curSess && g_ctx) {
        for (int i = 0; i < g_ctx->count; ++i) {
            const EvalField *f = &g_ctx->fields[i];
            int index = eval_index_of_id(next, f->seg ? f->seg->id : L"");
            if (index >= 0 && f->user_touched) eval_set_user_value(next, index, f->user_value);
            if(index>=0 && qp_choice_custom_index(next->fields[index].seg)>=0) {
                EvalField *to=&next->fields[index];
                to->choice_tracked=f->choice_tracked;to->custom_selected=f->custom_selected;
                qp_strlcpy(to->custom_value,f->custom_value,QP_VALUE_MAX);
                qp_strlcpy(to->choice_base,f->choice_base,QP_VALUE_MAX);
            }
        }
    } else {
        for (int i = 0; s->ids && s->vals && i < s->count; ++i) {
            int index = eval_index_of_id(next, s->ids[i]);
            if (index >= 0 && s->vals[i]) eval_set_user_value(next, index, s->vals[i]);
            if(index>=0 && s->choice_flags && s->choice_flags[i] && qp_choice_custom_index(next->fields[index].seg)>=0) {
                EvalField *to=&next->fields[index];
                to->choice_tracked=1;to->custom_selected=s->choice_flags[i]==2;
                qp_strlcpy(to->custom_value,s->choice_values && s->choice_values[i]?s->choice_values[i]:L"",QP_VALUE_MAX);
                qp_strlcpy(to->choice_base,s->choice_bases && s->choice_bases[i]?s->choice_bases[i]:L"",QP_VALUE_MAX);
            }
        }
    }
    /* 迁移过来的旧值必须向新模板的选项集对齐（口径 A），再求值 */
    if (!sess_reconcile_choices(next)) goto fail;
    if (!qp_eval_run_checked(next) || !sess_snapshot(&next_values, next)) goto fail;
    if (g_curSess && g_curSess != s && !sess_snapshot_current(g_curSess)) {
        if (!sess_snapshot(&previous_values, g_ctx)) goto fail;
        previous_needed = 1;
    }
    if (previous_needed) sess_publish_values(g_curSess, &previous_values);
    sess_publish_values(s, &next_values);
    eval_destroy(g_ctx);
    g_ctx = next; g_curSess = s;
    s->used = ++g_sessTick;
    if (!s->used) s->used = ++g_sessTick;
    return 1;
fail:
    sess_drop_values(&next_values); sess_drop_values(&previous_values);
    eval_destroy(next);
    return 0;
}

int sess_deactivate(void)
{
    if (!sess_capture_current()) return 0;
    eval_destroy(g_ctx); g_ctx = NULL; g_curSess = NULL;
    return 1;
}

/* ------------------------- 新建 / 挑选 ---------------------------- */

/* 默认名取「第 N 份」里最小没被占用的 N：关掉中间一份后不会冒出重名 */
static void sess_default_name(const wchar_t *tmpl_id, wchar_t *out, size_t cap)
{
    int n, i;
    for (n = 1; n <= QP_SESS_MAX + 1; ++n) {
        wchar_t want[QP_NAME_MAX];
        int taken = 0;
        _snwprintf(want, QP_NAME_MAX, L"第 %d 份", n);
        want[QP_NAME_MAX - 1] = L'\0';
        for (i = 0; i < g_sessCount; ++i)
            if (wcscmp(g_sess[i]->tmpl_id, tmpl_id) == 0 &&
                wcscmp(g_sess[i]->name, want) == 0) { taken = 1; break; }
        if (!taken) { qp_strlcpy(out, want, cap); return; }
    }
    qp_strlcpy(out, L"新的一份", cap);
}

/* 颜色也挑当前模板里没用过的，两份并行时一眼能分开 */
static int sess_pick_color(const wchar_t *tmpl_id)
{
    int c, i;
    for (c = 0; c < QP_SESS_MAX; ++c) {
        int taken = 0;
        for (i = 0; i < g_sessCount; ++i)
            if (wcscmp(g_sess[i]->tmpl_id, tmpl_id) == 0 && g_sess[i]->color == c)
                { taken = 1; break; }
        if (!taken) return c;
    }
    return 0;
}

static Session *sess_create(const wchar_t *tmpl_id)
{
    Session **grown;
    Session *s = (Session *)calloc(1, sizeof(Session));
    if (!s) return NULL;
    qp_strlcpy(s->tmpl_id, tmpl_id, QP_ID_MAX);
    sess_default_name(tmpl_id, s->name, QP_NAME_MAX);
    s->color = sess_pick_color(tmpl_id);
    grown = (Session **)realloc(g_sess, (size_t)(g_sessCount + 1) * sizeof(Session *));
    if (!grown) { free(s); return NULL; }
    g_sess = grown;
    g_sess[g_sessCount++] = s;
    return s;
}

/* Only a never-activated candidate may be discarded after a failed switch. */
void sess_discard_pending(Session *s)
{
    if (!s || s == g_curSess || s->used) return;
    for (int i = 0; i < g_sessCount; ++i) if (g_sess[i] == s) {
        memmove(g_sess + i, g_sess + i + 1, (size_t)(g_sessCount - i - 1) * sizeof(*g_sess));
        --g_sessCount;
        sess_free(s);
        return;
    }
}

/* 选中某个模板时用哪一份：上次用过的那一份；一份都没有就开一份 */
Session *sess_pick_for(const wchar_t *tmpl_id)
{
    Session *best = NULL;
    int i;
    for (i = 0; i < g_sessCount; ++i) {
        Session *s = g_sess[i];
        if (wcscmp(s->tmpl_id, tmpl_id) != 0) continue;
        if (!best || s->used > best->used) best = s;
    }
    return best ? best : sess_create(tmpl_id);
}

/* 数据被整体替换（切数据目录 / 覆盖导入）或模板被删掉后，扔掉无主的会话 */
void sess_prune(void)
{
    int i, n = 0, lost = 0;
    for (i = 0; i < g_sessCount; ++i) {
        Session *s = g_sess[i];
        if (g_app && tmpl_list_find(g_app->templates, s->tmpl_id)) {
            g_sess[n++] = s;
            continue;
        }
        if (g_curSess == s) { g_curSess = NULL; lost = 1; }
        sess_free(s);
    }
    g_sessCount = n;
    if (lost) ui_select_template(NULL);     /* 连当前这份的模板都没了 */
}

void sess_free_all(void)
{
    int i;
    for (i = 0; i < g_sessCount; ++i) sess_free(g_sess[i]);
    free(g_sess);
    g_sess = NULL;
    g_sessCount = 0;
    g_curSess = NULL;
}

/* ================================================================== */
/* 标签条（自绘：与多选弹层同一套画法，深色下不会露出系统浅色外观）    */
/* ================================================================== */

HWND g_hTabs;
int  g_tabsShown = -1;      /* 上次 ShowWindow 的状态，避免反复闪 */




static TabBox g_tabBox[QP_SESS_MAX];
static int    g_tabCount;          /* 已量好的标签数 */
static int    g_tabPlusX, g_tabPlusW;
static int    g_tabHot = -1;       /* 悬停的标签，-1 = 无 */
static int    g_tabHotX;           /* 悬停在该标签的 × 上 */
static int    g_tabPlusHot;
static int    g_tabMenuVis = -1;   /* 右键菜单作用于第几份，-1 = 当前那份 */

static int tabs_close_w(void)  { return DPIX(16); }
static COLORREF tabs_hot_bk(void)
{
    return g_dark ? RGB(58, 62, 70) : RGB(226, 236, 248);
}
static COLORREF tabs_idle_bk(void)
{
    return g_dark ? RGB(40, 43, 48) : RGB(235, 238, 242);
}

/* 量出每个标签的位置。标签数最多 9 个，每次用到时重量一遍就够，省掉一份
   随时可能过期的缓存。 */
void tabs_measure(void)
{
    RECT rc;
    HDC hdc;
    HFONT old;
    int i, n, avail, total = 0;
    int gap = DPIX(3), plusW = DPIX(26), minW = DPIX(46), maxW = DPIX(190);

    g_tabCount = 0;
    g_tabPlusX = g_tabPlusW = 0;
    if (!g_hTabs || !g_curTmpl) return;
    n = sess_visible_count();
    if (n > QP_SESS_MAX) n = QP_SESS_MAX;
    GetClientRect(g_hTabs, &rc);
    avail = rc.right - rc.left - DPIX(2);
    if (avail <= 0) return;

    hdc = GetDC(g_hTabs);
    if (!hdc) return;
    old = (HFONT)SelectObject(hdc, g_fontUI);
    for (i = 0; i < n; ++i) {
        Session *s = sess_visible_at(i);
        SIZE sz;
        int w;
        sz.cx = sz.cy = 0;
        if (s) GetTextExtentPoint32W(hdc, s->name, (int)wcslen(s->name), &sz);
        /* 左边距 + 圆点 + 间隙 + 文字 + 间隙 + × + 右边距 */
        w = DPIX(7) + DPIX(8) + DPIX(6) + sz.cx + DPIX(6) + tabs_close_w() + DPIX(4);
        if (w < minW) w = minW;
        if (w > maxW) w = maxW;
        g_tabBox[i].w = w;
        total += w + gap;
    }
    SelectObject(hdc, old);
    ReleaseDC(g_hTabs, hdc);
    g_tabCount = n;

    /* 放不下就等比压窄（到下限为止）。最多 9 份，压到下限也还看得见圆点。 */
    if (n > 0 && total + plusW > avail) {
        int floorW = DPIX(34);
        int each = (avail - plusW - n * gap) / n;
        if (each < floorW) each = floorW;
        for (i = 0; i < n; ++i)
            if (g_tabBox[i].w > each) g_tabBox[i].w = each;
    }

    total = 0;
    for (i = 0; i < n; ++i) {
        g_tabBox[i].x = DPIX(1) + total;
        total += g_tabBox[i].w + gap;
    }
    g_tabPlusX = DPIX(1) + total;
    g_tabPlusW = (n < QP_SESS_MAX) ? plusW : 0;   /* 开满了就不给「+」 */
}

/* 命中：>=0 为标签下标，-1 空白，-2 「+」。*onClose 置 1 表示落在 × 上 */
static int tabs_hit(int x, int y, int *onClose)
{
    int i;
    (void)y;
    if (onClose) *onClose = 0;
    if (g_tabPlusW && x >= g_tabPlusX && x < g_tabPlusX + g_tabPlusW) return -2;
    for (i = 0; i < g_tabCount; ++i) {
        if (x < g_tabBox[i].x || x >= g_tabBox[i].x + g_tabBox[i].w) continue;
        if (onClose && g_tabCount > 1 &&
            x >= g_tabBox[i].x + g_tabBox[i].w - tabs_close_w() - DPIX(4))
            *onClose = 1;
        return i;
    }
    return -1;
}

/* 画一个 ×（关闭）或 +（新开）：两根线，够清楚也不抢眼 */
static void tabs_draw_glyph(HDC dc, int cx, int cy, int r, int plus, COLORREF clr)
{
    HPEN pen = CreatePen(PS_SOLID, DPIX(1) > 1 ? DPIX(1) : 1, clr);
    HGDIOBJ old;
    if (!pen) return;
    old = SelectObject(dc, pen);
    if (plus) {
        MoveToEx(dc, cx - r, cy, NULL);     LineTo(dc, cx + r + 1, cy);
        MoveToEx(dc, cx, cy - r, NULL);     LineTo(dc, cx, cy + r + 1);
    } else {
        MoveToEx(dc, cx - r, cy - r, NULL); LineTo(dc, cx + r + 1, cy + r + 1);
        MoveToEx(dc, cx + r, cy - r, NULL); LineTo(dc, cx - r - 1, cy + r + 1);
    }
    SelectObject(dc, old);
    DeleteObject(pen);
}

static void tabs_draw(HWND hwnd,HDC hdc)
{
    HDC mem;
    HBITMAP bmp, oldBmp;
    HFONT oldFont;
    HBRUSH idleBrush, hotBrush, selBrush;
    RECT rc;
    int i, cw, ch, cur;

    if (!hdc) return;
    GetClientRect(hwnd, &rc);
    cw = rc.right - rc.left;
    ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) { return; }

    mem = CreateCompatibleDC(hdc);
    bmp = mem ? CreateCompatibleBitmap(hdc, cw, ch) : NULL;
    if (!bmp) {
        if(mem)DeleteDC(mem);
        mem=hdc;
        oldBmp=NULL;
    } else oldBmp=(HBITMAP)SelectObject(mem,bmp);
    oldFont = (HFONT)SelectObject(mem, g_fontUI);
    SetBkMode(mem, TRANSPARENT);
    FillRect(mem, &rc, g_brushBk);

    tabs_measure();
    cur = sess_visible_index(g_curSess);
    idleBrush = CreateSolidBrush(tabs_idle_bk());
    hotBrush  = CreateSolidBrush(tabs_hot_bk());
    selBrush  = CreateSolidBrush(theme_ctl_bk());

    for (i = 0; i < g_tabCount; ++i) {
        Session *s = sess_visible_at(i);
        RECT tab, txt;
        int sel = (i == cur), cy, d = DPIX(8);
        if (!s) continue;
        tab.left   = g_tabBox[i].x;
        tab.right  = tab.left + g_tabBox[i].w;
        tab.top    = DPIX(2);
        tab.bottom = ch - DPIX(2);
        cy = (tab.top + tab.bottom) / 2;

        FillRect(mem, &tab, sel ? selBrush : (i == g_tabHot ? hotBrush : idleBrush));
        if (sel) {
            /* 选中的那份底部压一条本份颜色的横杠，和圆点呼应 */
            RECT bar = tab;
            HBRUSH cb = CreateSolidBrush(sess_color_of(s));
            bar.top = bar.bottom - (DPIX(2) > 1 ? DPIX(2) : 1);
            if (cb) { FillRect(mem, &bar, cb); DeleteObject(cb); }
        }

        {   /* 颜色圆点 */
            HBRUSH cb = CreateSolidBrush(sess_color_of(s));
            HPEN   cp = CreatePen(PS_SOLID, 1, sess_color_of(s));
            HGDIOBJ ob = cb ? SelectObject(mem, cb) : NULL;
            HGDIOBJ op = cp ? SelectObject(mem, cp) : NULL;
            if (cb && cp)
                Ellipse(mem, tab.left + DPIX(7), cy - d / 2,
                             tab.left + DPIX(7) + d, cy - d / 2 + d);
            if (ob) SelectObject(mem, ob);
            if (op) SelectObject(mem, op);
            if (cb) DeleteObject(cb);
            if (cp) DeleteObject(cp);
        }

        txt = tab;
        txt.left  += DPIX(7) + d + DPIX(6);
        txt.right -= (g_tabCount > 1 ? tabs_close_w() + DPIX(4) : DPIX(6));
        if (txt.right > txt.left) {
            SetTextColor(mem, sel ? theme_text() : theme_dim());
            DrawTextW(mem, s->name, -1, &txt,
                      DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        }

        /* × 只在选中或悬停时露出来，平时不添乱；只有一份时不给关 */
        if (g_tabCount > 1 && (sel || i == g_tabHot)) {
            int bw = tabs_close_w();
            int hotX = (i == g_tabHot && g_tabHotX);
            RECT cb2;
            cb2.right  = tab.right - DPIX(4);
            cb2.left   = cb2.right - bw;
            cb2.top    = cy - bw / 2;
            cb2.bottom = cb2.top + bw;
            if (hotX) {
                HBRUSH xb = CreateSolidBrush(g_dark ? RGB(104, 72, 72)
                                                    : RGB(240, 214, 214));
                if (xb) { FillRect(mem, &cb2, xb); DeleteObject(xb); }
            }
            tabs_draw_glyph(mem, (cb2.left + cb2.right) / 2, cy, DPIX(3),
                            0, hotX ? theme_text() : theme_dim());
        }
    }

    if (g_tabPlusW) {
        RECT pr;
        pr.left   = g_tabPlusX;
        pr.right  = pr.left + g_tabPlusW;
        pr.top    = DPIX(2);
        pr.bottom = ch - DPIX(2);
        if (g_tabPlusHot) FillRect(mem, &pr, hotBrush);
        tabs_draw_glyph(mem, (pr.left + pr.right) / 2, (pr.top + pr.bottom) / 2,
                        DPIX(5), 1, g_tabPlusHot ? theme_text() : theme_dim());
    }

    if(bmp)BitBlt(hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldFont);
    if(bmp) {
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
    }
    if (idleBrush) DeleteObject(idleBrush);
    if (hotBrush)  DeleteObject(hotBrush);
    if (selBrush)  DeleteObject(selBrush);
}

static void tabs_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc=BeginPaint(hwnd,&ps);
    tabs_draw(hwnd,dc);
    EndPaint(hwnd,&ps);
}

void tabs_refresh(void)
{
    int want = g_curTmpl ? 1 : 0;
    if (!g_hTabs) return;
    if (g_tabsShown != want) {
        ShowWindow(g_hTabs, want ? SW_SHOWNA : SW_HIDE);
        g_tabsShown = want;
    }
    g_tabHot = -1;
    g_tabHotX = 0;
    g_tabPlusHot = 0;
    tabs_measure();
    InvalidateRect(g_hTabs, NULL, TRUE);
}

static void tabs_set_hot(int tab, int onClose, int plus)
{
    if (tab == g_tabHot && onClose == g_tabHotX && plus == g_tabPlusHot) return;
    g_tabHot = tab;
    g_tabHotX = onClose;
    g_tabPlusHot = plus;
    if (g_hTabs) InvalidateRect(g_hTabs, NULL, FALSE);
}

/* ================================================================== */
/* 标签条的动作                                                        */
/* ================================================================== */

static void ui_focus_first_field(void)
{
    if (g_fieldCount > 0 && g_fields && g_fields[0].hCtl) SetFocus(g_fields[0].hCtl);
}

/* 切走当前这份、换上另一份。字段控件整批重建：切换时焦点可能还停在上一份
   的输入框里，而 ui_sync_controls 会跳过持有焦点的框，不重建就会留下一格
   显示着上一份的内容。 */
static int sess_rebind(Session *s)
{
    if (!s || !g_curTmpl) return 0;
    if (!sess_activate(s, g_curTmpl)) {
        sess_discard_pending(s);
        qp_msgbox(g_hwndMain, MB_OK | MB_ICONERROR, L"无法切换填写会话",
                  L"内存不足，当前输入已保留。请关闭不需要的窗口后重试。");
        return 0;
    }
    ui_clear_fields();
    ui_build_fields();
    ui_refresh_current();
    tabs_refresh();
    ui_layout(g_hwndMain);
    return 1;
}

void sess_switch_to(int vis)
{
    Session *s = sess_visible_at(vis);
    if (!s || s == g_curSess) return;
    popup_close();
    if (!sess_rebind(s)) return;
    ui_focus_first_field();
    SetWindowTextW(g_hHint, L"已切换填写会话，其他会话的内容已保留。");
}

void sess_cycle(int delta)
{
    int n = sess_visible_count(), cur;
    if (n <= 1) return;
    cur = sess_visible_index(g_curSess);
    if (cur < 0) cur = 0;
    sess_switch_to(((cur + delta) % n + n) % n);
}

static int sess_room_check(void)
{
    if (sess_visible_count() < QP_SESS_MAX) return 1;
    qp_msgbox(g_hwndMain, MB_OK | MB_ICONINFORMATION, QP_APP_TITLE,
              L"同一模板最多同时打开 %d 个填写会话。", QP_SESS_MAX);
    return 0;
}

void sess_new(void)
{
    Session *s;
    if (!g_curTmpl || !sess_room_check()) return;
    popup_close();
    s = sess_create(g_curTmpl->id);
    if (!s) return;
    if (!sess_rebind(s)) return;
    ui_focus_first_field();
    SetWindowTextW(g_hHint, L"已新建填写会话；各会话的输入相互独立。");
}

/* 复制某一份（连同已填内容），并切到副本上 */
void sess_duplicate(int vis)
{
    Session *src = sess_visible_at(vis), *dst;
    int i;
    if (!src || !g_curTmpl || !sess_room_check()) return;
    popup_close();
    if (src == g_curSess && !sess_save(src)) {
        qp_msgbox(g_hwndMain, MB_OK | MB_ICONERROR, L"无法复制填写会话",
                  L"内存不足，当前输入已保留。请关闭不需要的窗口后重试。");
        return;
    }
    dst = sess_create(g_curTmpl->id);
    if (!dst) return;
    if (src->count > 0 && src->ids) {
        dst->ids  = (wchar_t (*)[QP_ID_MAX])calloc((size_t)src->count, sizeof(*dst->ids));
        dst->vals = (wchar_t **)calloc((size_t)src->count, sizeof(*dst->vals));
        if (dst->ids && dst->vals) {
            dst->count = src->count;
            for (i = 0; i < src->count; ++i) {
                qp_strlcpy(dst->ids[i], src->ids[i], QP_ID_MAX);
                dst->vals[i] = (src->vals && src->vals[i])
                                 ? qp_wcsdup(src->vals[i]) : NULL;
                if (src->vals && src->vals[i] && !dst->vals[i]) {
                    sess_discard_pending(dst);
                    qp_msgbox(g_hwndMain, MB_OK | MB_ICONERROR, L"无法复制填写会话",
                              L"内存不足，未创建副本。原填写会话未改变。");
                    return;
                }
            }
        } else {
            sess_discard_pending(dst);
            qp_msgbox(g_hwndMain, MB_OK | MB_ICONERROR, L"无法复制填写会话",
                      L"内存不足，未创建副本。原填写会话未改变。");
            return;
        }
    }
    if(src->choice_flags) {
        dst->choice_flags=calloc((size_t)src->count,1);
        dst->choice_values=calloc((size_t)src->count,sizeof(*dst->choice_values));
        dst->choice_bases=calloc((size_t)src->count,sizeof(*dst->choice_bases));
        if(!dst->choice_flags || !dst->choice_values || !dst->choice_bases)goto choice_failure;
        for(i=0;i<src->count;++i)if(src->choice_flags[i]) {
            dst->choice_flags[i]=src->choice_flags[i];
            dst->choice_values[i]=qp_wcsdup(src->choice_values && src->choice_values[i]?src->choice_values[i]:L"");
            dst->choice_bases[i]=qp_wcsdup(src->choice_bases && src->choice_bases[i]?src->choice_bases[i]:L"");
            if(!dst->choice_values[i] || !dst->choice_bases[i])goto choice_failure;
        }
    }
    if (!sess_rebind(dst)) return;
    ui_focus_first_field();
    SetWindowTextW(g_hHint, L"已复制填写会话；副本的输入与原会话相互独立。");
    return;
choice_failure:
    sess_discard_pending(dst);
    qp_msgbox(g_hwndMain,MB_OK|MB_ICONERROR,L"无法复制填写会话",
              L"内存不足，未创建副本。原填写会话未改变。");
}

void sess_close(int vis)
{
    Session *s = sess_visible_at(vis);
    int i, at = -1, wasCur;

    if (!s || !g_curTmpl) return;
    if (sess_visible_count() <= 1) {
        SetWindowTextW(g_hHint, L"至少保留一份；如需清空内容，请点击「重置」");
        return;
    }
    if (sess_has_input(s) &&
        qp_msgbox(g_hwndMain, MB_YESNO | MB_ICONWARNING, L"关闭这一份",
                  L"「%ls」已填写内容，关闭后无法恢复。确定关闭吗？",
                  s->name) != IDYES)
        return;

    for (i = 0; i < g_sessCount; ++i) if (g_sess[i] == s) { at = i; break; }
    if (at < 0) return;
    popup_close();
    wasCur = (s == g_curSess);
    if (wasCur) {
        Session *replacement = sess_visible_at(vis > 0 ? vis - 1 : 1);
        if (!sess_rebind(replacement)) return;
    }
    for (i = at; i + 1 < g_sessCount; ++i) g_sess[i] = g_sess[i + 1];
    --g_sessCount;
    sess_free(s);

    if (wasCur) {
        /* 接着显示左边那一份（关掉第一份时显示新的第一份） */
        tabs_refresh();
        ui_focus_first_field();
    } else {
        tabs_refresh();
    }
    SetWindowTextW(g_hHint, L"已关闭填写会话。");
}

void sess_rename(int vis)
{
    Session *s = sess_visible_at(vis);
    wchar_t name[QP_NAME_MAX];
    if (!s) return;
    qp_strlcpy(name, s->name, QP_NAME_MAX);
    if (!dlg_input(g_hwndMain, L"重命名这一份", L"标签名称：", name, QP_NAME_MAX))
        return;
    qp_trim(name);
    if (!name[0]) return;
    qp_strlcpy(s->name, name, QP_NAME_MAX);
    tabs_refresh();
}

void sess_set_color(int vis, int color)
{
    Session *s = sess_visible_at(vis);
    if (!s || color < 0 || color >= QP_SESS_MAX) return;
    s->color = color;
    tabs_refresh();
}

/* 菜单命令作用于谁：右键菜单点出来的那份，否则当前这份 */
int sess_target_vis(void)
{
    return g_tabMenuVis >= 0 ? g_tabMenuVis : sess_visible_index(g_curSess);
}

static void tabs_context_menu(int vis, int x, int y)
{
    HMENU m = CreatePopupMenu();
    Session *s = sess_visible_at(vis);
    int cmd;
    if (!m) return;

    g_tabMenuVis = vis;
    AppendMenuW(m, MF_STRING, IDM_SESS_NEW, L"新开一份\tCtrl+T");
    if (s) {
        HMENU cm = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, IDM_SESS_DUP, L"复制这一份（连同已填内容）");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, IDM_SESS_RENAME, L"重命名…");
        if (cm) {
            int i;
            for (i = 0; i < QP_SESS_MAX; ++i)
                AppendMenuW(cm, MF_STRING, IDM_SESS_COLOR_BASE + i, s_sessColorName[i]);
            CheckMenuRadioItem(cm, IDM_SESS_COLOR_BASE,
                               IDM_SESS_COLOR_BASE + QP_SESS_MAX - 1,
                               IDM_SESS_COLOR_BASE + s->color, MF_BYCOMMAND);
            AppendMenuW(m, MF_POPUP, (UINT_PTR)cm, L"颜色");
        }
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING | (sess_visible_count() > 1 ? 0 : MF_GRAYED),
                    IDM_SESS_CLOSE, L"关闭这一份\tCtrl+W");
    }
    /* 自己接命令再转发：菜单的 WM_COMMAND 是投递的，等它自己走一圈的话
       g_tabMenuVis 早被清掉了，命令就会落到「当前那份」身上 */
    cmd = (int)TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_RETURNCMD,
                              x, y, 0, g_hwndMain, NULL);
    DestroyMenu(m);
    if (cmd) SendMessageW(g_hwndMain, WM_COMMAND, (WPARAM)cmd, 0);
    g_tabMenuVis = -1;
}

LRESULT CALLBACK tabs_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        tabs_paint(hwnd);
        return 0;

    /* The restore erase pass must contain the labels too, not a blank surface. */
    case WM_ERASEBKGND:
    case WM_PRINTCLIENT: {
        HDC dc=(HDC)wp;
        int saved=SaveDC(dc);
        tabs_draw(hwnd,dc);
        if(saved)RestoreDC(dc,saved);
        return msg == WM_ERASEBKGND;
    }

    case WM_SIZE:
        /* 变窄时标签要压一压，整条重画（只补新露出的一块会留下旧标签） */
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;

    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme;
        int onClose = 0, hit;
        tabs_measure();
        hit = tabs_hit((short)LOWORD(lp), (short)HIWORD(lp), &onClose);
        tabs_set_hot(hit >= 0 ? hit : -1, onClose, hit == -2);
        memset(&tme, 0, sizeof(tme));
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE:
        tabs_set_hot(-1, 0, 0);
        return 0;

    case WM_LBUTTONDOWN: {
        int onClose = 0, hit;
        tabs_measure();
        hit = tabs_hit((short)LOWORD(lp), (short)HIWORD(lp), &onClose);
        if (hit == -2)            sess_new();
        else if (hit >= 0 && onClose) sess_close(hit);
        else if (hit >= 0)        sess_switch_to(hit);
        else                      popup_close();
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        int onClose = 0, hit;
        tabs_measure();
        hit = tabs_hit((short)LOWORD(lp), (short)HIWORD(lp), &onClose);
        if (hit >= 0 && !onClose) sess_rename(hit);
        return 0;
    }

    case WM_MBUTTONUP: {
        int hit;
        tabs_measure();
        hit = tabs_hit((short)LOWORD(lp), (short)HIWORD(lp), NULL);
        if (hit >= 0) sess_close(hit);
        return 0;
    }

    case WM_RBUTTONUP: {
        POINT pt;
        int hit;
        tabs_measure();
        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        hit = tabs_hit(pt.x, pt.y, NULL);
        ClientToScreen(hwnd, &pt);
        tabs_context_menu(hit, pt.x, pt.y);
        return 0;
    }

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
