/*
 * preview.c —— 口径预览控件的实现（自绘）
 *
 * 一个窗口两种呈现方式：
 *   文本：按宽度折行；「没匹配上」的位置插入红底红字的「失败」提示，
 *         一屏里一眼就能挑出哪几处规则没命中。这里的「失败」是**插进去的提示**，
 *         不是去认正文里的字 —— 正文里本来写着「失败」也不会被标色
 *         （哪几处该标由引擎给的 marks 决定，单测见 tests/test_textview.c）。
 *         文本可以像普通文本框一样**拖选**：拖动选中、双击选词、Ctrl+A 全选、
 *         Ctrl+C / 右键菜单复制选中的那段。
 *   表格：真正的列对齐 + 列名表头 + 浅网格 + 隔行底色；空单元格标红「未匹配」。
 *
 * 每段文字的位置、折行与命中都交给纯 C 的 textview.c 算（那边可以单测），
 * 这里只负责画。每个控件实例的状态都挂在 GWLP_USERDATA 上，所以主窗口和
 * 模板编辑器可以各有一个、互不干扰。绘制一律双缓冲，滚动时不闪。
 */
#include "preview.h"
#include "textview.h"
#include "ui.h"          /* qp_copy_to_clipboard */

#include <stdlib.h>
#include <string.h>

#ifndef GET_WHEEL_DELTA_WPARAM
#  define GET_WHEEL_DELTA_WPARAM(wp) ((short)HIWORD(wp))
#endif

static const wchar_t *CLS_PREVIEW = L"QuickPhrasePreview";

/* 文本模式失败处的提示语 / 表格模式空单元格的提示语 */
static const wchar_t *PV_TEXT_MARK  = L"失败";
static const wchar_t *PV_TABLE_MARK = L"未匹配";

#define PV_MAX_MARKS  512        /* 一次最多标这么多处，再多也没人看得过来 */

typedef struct { int off; int len; int missing; } PvCell;

typedef struct {
    int            mode;         /* QP_PREVIEW_TEXT / QP_PREVIEW_TABLE */

    /* ---- 文本模式 ---- */
    wchar_t       *disp;         /* 正文 + 插入的失败提示（显示串） */
    unsigned char *kind;         /* 逐字符：0 = 正文，1 = 失败提示 */
    int           *sourceMap;    /* 显示字符对应的原文下标，提示为 -1 */
    int           *glyphWidths;  /* 绘制、折行、命中共用的字体度量 */
    HFONT          measuredFont;
    int            measuredDpi, measureDirty;
    int            dispCap;
    int            dispLen;
    int           *lineOff;      /* 每个视觉行的起始下标（disp 内） */
    int           *lineLen;
    int            nlines, lineCap;

    /* ---- 表格模式 ---- */
    wchar_t       *raw;          /* 引擎渲染出来的原文 */
    PvCell        *cells;        /* [rows*cols]，off<0 = 该行没这一列 */
    int           *colw;
    wchar_t       *hdr;          /* [PV_MAX_COLS][QP_LABEL_MAX] */
    int            hdrCount, rows, cols;
    int            rowH, headH, totalW;

    /* ---- 公共 ---- */
    int            lineH;
    int            wAscii, wWide;
    int            scrollX, scrollY;

    /* ---- 文本模式的鼠标拖选 ---- */
    int            selAnchor;    /* 按下时的显示串下标             */
    int            selEnd;       /* 拖到的显示串下标；-1 = 没有选区 */
    int            selecting;    /* 正按着左键拖                   */
    int            modelValid, modelMode, modelNmarks;
    int            modelMarks[PV_MAX_MARKS];
    wchar_t       *modelFormat;   /* Owned table format for no-change detection. */
} Preview;

#define PV_MAX_COLS 64

static int pv_padx(void)    { return DPIX(8); }
static int pv_pady(void)    { return DPIX(6); }
static int pv_cellpad(void) { return DPIX(7); }
static int pv_maxcolw(void) { return DPIX(440); }

static COLORREF pv_err_bg(void)
{
    return g_dark ? RGB(74, 42, 42) : RGB(255, 229, 229);
}

/* ------------------------------------------------------------------ */
/* 实例状态                                                            */
/* ------------------------------------------------------------------ */

static Preview *pv_of(HWND hwnd)
{
    return hwnd ? (Preview *)GetWindowLongPtrW(hwnd, GWLP_USERDATA) : NULL;
}

static void pv_free_model(Preview *p)
{
    if (!p) return;
    free(p->disp);    p->disp = NULL;
    free(p->kind);    p->kind = NULL;
    free(p->sourceMap); p->sourceMap = NULL;
    free(p->glyphWidths); p->glyphWidths = NULL;
    free(p->lineOff); p->lineOff = NULL;
    free(p->lineLen); p->lineLen = NULL;
    free(p->raw);     p->raw = NULL;
    free(p->cells);   p->cells = NULL;
    free(p->colw);    p->colw = NULL;
    free(p->hdr);     p->hdr = NULL;
    free(p->modelFormat); p->modelFormat = NULL;
    p->modelValid = 0;
    p->dispCap = p->dispLen = p->nlines = p->lineCap = 0;
    p->rows = p->cols = p->hdrCount = 0;
    p->totalW = 0;
    p->scrollX = p->scrollY = 0;
}

/* 折行结果要有地方放：最多「一个字符一行」，所以容量按显示串长度留 */
static int pv_reserve_lines(Preview *p, int want)
{
    if (want < 1) want = 1;
    if (want <= p->lineCap) return 1;
    {
        int *no = (int *)realloc(p->lineOff, (size_t)want * sizeof(int));
        int *nl;
        if (!no) return 0;
        p->lineOff = no;
        nl = (int *)realloc(p->lineLen, (size_t)want * sizeof(int));
        if (!nl) return 0;
        p->lineLen = nl;
        p->lineCap = want;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* 字体度量（等宽字体：只量一个窄字符与一个宽字符，其余按区间估）        */
/* ------------------------------------------------------------------ */

static void pv_measure_font(Preview *p, HDC dc)
{
    SIZE sz;
    TEXTMETRICW tm;
    HFONT old = (HFONT)SelectObject(dc, g_fontMono);

    memset(&tm, 0, sizeof(tm));
    if (GetTextMetricsW(dc, &tm) && tm.tmHeight > 0) p->lineH = tm.tmHeight;
    if (p->lineH < DPIX(14)) p->lineH = DPIX(15);

    p->wAscii = DPIX(7);
    p->wWide  = DPIX(14);
    if (GetTextExtentPoint32W(dc, L"0", 1, &sz) && sz.cx > 0) p->wAscii = sz.cx;
    if (GetTextExtentPoint32W(dc, L"中", 1, &sz) && sz.cx > 0) p->wWide = sz.cx;
    if (p->wWide <= p->wAscii) p->wWide = p->wAscii * 2;
    SelectObject(dc, old);
}

/* ------------------------------------------------------------------ */
/* 文本模式：拼显示串 + 折行（算法在 textview.c，那边有单测）            */
/* ------------------------------------------------------------------ */

/* 把正文与「未匹配」提示拼成一条显示串。提示只出现在 marks 指定的位置上，
   正文里本来写着「失败」也不会被当成提示。 */
static int pv_build_disp(Preview *p, const wchar_t *text, const int *marks, int nmarks)
{
    int need;

    if (!text) text = L"";
    if (nmarks < 0) nmarks = 0;
    if (nmarks > PV_MAX_MARKS) nmarks = PV_MAX_MARKS;

    need = (int)qp_tv_capacity(text, nmarks, PV_TEXT_MARK);
    if (need > p->dispCap) {
        wchar_t *disp = (wchar_t *)malloc((size_t)need * sizeof(wchar_t));
        unsigned char *kind = (unsigned char *)malloc((size_t)need);
        int *map = (int *)malloc((size_t)need * sizeof(int));
        int *widths = (int *)malloc((size_t)need * sizeof(int));
        if (!disp || !kind || !map || !widths) {
            free(disp); free(kind); free(map); free(widths);
            return 0;
        }
        free(p->disp); free(p->kind); free(p->sourceMap); free(p->glyphWidths);
        p->disp = disp; p->kind = kind;
        p->sourceMap = map; p->glyphWidths = widths;
        p->dispCap = need;
    }
    if (!p->disp || !p->kind || !p->sourceMap || !p->glyphWidths) return 0;
    p->dispLen = qp_tv_build_mapped(text, marks, nmarks, PV_TEXT_MARK,
                             p->disp, p->dispCap, p->kind, p->sourceMap);
    p->measureDirty = 1;
    return 1;
}

/* 按可用宽度折行（宽字符按两倍宽估，宁可早断也不溢出右边界） */
static void pv_wrap(Preview *p, int availW)
{
    if (availW < DPIX(24)) availW = DPIX(24);
    if (!pv_reserve_lines(p, p->dispLen + 2)) { p->nlines = 0; return; }
    p->nlines = qp_tv_wrap_measured(p->disp ? p->disp : L"", p->dispLen,
                           p->glyphWidths, availW,
                           p->lineOff, p->lineLen, p->lineCap);
}

/* ------------------------------------------------------------------ */
/* 表格模式：原文 → 行列 / 列宽 / 表头                                   */
/* ------------------------------------------------------------------ */

static void pv_scan_rows(const wchar_t *s, int *rows_out, int *cols_out)
{
    const wchar_t *p = s;
    int rows = 0, cols = 0;
    if (!s || !*s) { *rows_out = 0; *cols_out = 0; return; }
    while (*p) {
        const wchar_t *nl = wcschr(p, L'\n');
        size_t len = nl ? (size_t)(nl - p) : wcslen(p);
        int c = 1;
        size_t i;
        for (i = 0; i < len; ++i) if (p[i] == L'\t') ++c;
        ++rows;
        if (c > cols) cols = c;
        if (!nl) break;
        p = nl + 1;
    }
    if (cols > PV_MAX_COLS) cols = PV_MAX_COLS;
    *rows_out = rows;
    *cols_out = cols;
}

static void pv_fill_cells(Preview *p)
{
    const wchar_t *p0 = p->raw;
    int r = 0, i;
    for (i = 0; i < p->rows * p->cols; ++i) {
        p->cells[i].off = -1;
        p->cells[i].len = 0;
        p->cells[i].missing = 0;
    }
    while (*p0 && r < p->rows) {
        const wchar_t *nl = wcschr(p0, L'\n');
        size_t len = nl ? (size_t)(nl - p0) : wcslen(p0);
        if (len && p0[len - 1] == L'\r') --len;
        size_t start = 0, k;
        int c = 0;
        for (k = 0; k <= len; ++k) {
            if (k == len || p0[k] == L'\t') {
                if (c < p->cols) {
                    p->cells[r * p->cols + c].off = (int)(p0 - p->raw) + (int)start;
                    p->cells[r * p->cols + c].len = (int)(k - start);
                }
                ++c;
                start = k + 1;
            }
        }
        ++r;
        if (!nl) break;
        p0 = nl + 1;
    }
}

/* 列名：格式串里第 i 个 {变量名}。个数与列数对不上就不冒充表头 */
static void pv_build_header(Preview *p, const wchar_t *format)
{
    int i, n = 0;
    p->hdrCount = 0;
    if (!format || !format[0] || !p->hdr) return;
    for (i = 0; i < PV_MAX_COLS; ++i) {
        wchar_t name[QP_LABEL_MAX];
        if (!qp_placeholder_at(format, i, name, QP_LABEL_MAX)) break;
        qp_strlcpy(p->hdr + (size_t)i * QP_LABEL_MAX, name, QP_LABEL_MAX);
        ++n;
    }
    p->hdrCount = (n == p->cols) ? n : 0;
}

/* ------------------------------------------------------------------ */
/* 滚动条                                                              */
/* ------------------------------------------------------------------ */

static void pv_update_scroll(HWND hwnd)
{
    Preview *p = pv_of(hwnd);
    RECT rc;
    SCROLLINFO si;
    int cw, ch, contentH, contentW, viewH;

    if (!p) return;
    GetClientRect(hwnd, &rc);
    cw = rc.right - rc.left;
    ch = rc.bottom - rc.top;

    if (p->mode == QP_PREVIEW_TABLE) {
        viewH = ch - p->headH;
        contentH = p->rows * p->rowH;
        contentW = p->totalW;
    } else {
        viewH = ch;
        contentH = p->nlines * p->lineH;
        contentW = 0;                    /* 文本模式折行，不需要横向滚动 */
    }
    if (viewH < 1) viewH = 1;

    if (p->scrollY > contentH - viewH) p->scrollY = contentH - viewH;
    if (p->scrollY < 0) p->scrollY = 0;
    if (p->scrollX > contentW - cw) p->scrollX = contentW - cw;
    if (p->scrollX < 0) p->scrollX = 0;

    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = contentH > 0 ? contentH - 1 : 0;
    si.nPage = (UINT)viewH;
    si.nPos = p->scrollY;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);

    ShowScrollBar(hwnd, SB_HORZ, p->mode == QP_PREVIEW_TABLE && contentW > cw);
    if (p->mode == QP_PREVIEW_TABLE) {
        si.nMax = contentW > 0 ? contentW - 1 : 0;
        si.nPage = (UINT)(cw > 0 ? cw : 1);
        si.nPos = p->scrollX;
        SetScrollInfo(hwnd, SB_HORZ, &si, TRUE);
    }
}

/* 内容或尺寸变了之后重新排版 */
static void pv_relayout(HWND hwnd)
{
    Preview *p = pv_of(hwnd);
    RECT rc;
    HDC dc;
    if (!p) return;
    GetClientRect(hwnd, &rc);
    dc = GetDC(hwnd);
    if (dc) {
        pv_measure_font(p, dc);
        if (p->mode == QP_PREVIEW_TEXT) {
            if (p->glyphWidths && (p->measureDirty || p->measuredFont != g_fontMono || p->measuredDpi != g_dpi)) {
                int i;
                HFONT old = (HFONT)SelectObject(dc, g_fontMono);
                for (i = 0; i < p->dispLen; ) {
                    SIZE sz;
                    int units = i + 1 < p->dispLen && p->disp[i] >= 0xD800 && p->disp[i] <= 0xDBFF &&
                                p->disp[i + 1] >= 0xDC00 && p->disp[i + 1] <= 0xDFFF ? 2 : 1;
                    int width = units == 2 ? p->wWide : qp_tv_char_w(p->disp[i], p->wAscii, p->wWide);
                    if (GetTextExtentPoint32W(dc, p->disp + i, units, &sz) && sz.cx > 0) width = sz.cx;
                    p->glyphWidths[i] = width;
                    if (units == 2) p->glyphWidths[i + 1] = 0;
                    i += units;
                }
                SelectObject(dc, old);
                p->measureDirty = 0;
                p->measuredFont = g_fontMono;
                p->measuredDpi = g_dpi;
            }
            pv_wrap(p, rc.right - rc.left - pv_padx() * 2);
        }
        ReleaseDC(hwnd, dc);
    }
    pv_update_scroll(hwnd);
}

/* ------------------------------------------------------------------ */
/* 绘制                                                                */
/* ------------------------------------------------------------------ */

static COLORREF pv_sel_bg(void)
{
    /* 选中底色：浅色主题用淡蓝，深色主题用暗蓝，和字段里选字的感觉一致 */
    return g_dark ? RGB(46, 70, 104) : RGB(205, 224, 248);
}

static void pv_paint_text(HDC mem, Preview *p, const RECT *rc)
{
    int ch = rc->bottom - rc->top;
    int first, last, ln, y;
    int selLo = 0, selHi = 0, hasSel = 0;

    if (p->selEnd >= 0 && p->selEnd != p->selAnchor) {
        qp_tv_sel_range(p->selAnchor, p->selEnd, &selLo, &selHi);
        hasSel = (selLo < selHi);
    }

    if (p->dispLen <= 0) {
        RECT t = *rc;
        t.left += pv_padx();
        t.top += pv_pady();
        SetTextColor(mem, theme_dim());
        DrawTextW(mem, L"（当前输出为空）", -1, &t,
                  DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        return;
    }
    first = p->scrollY / (p->lineH > 0 ? p->lineH : 1);
    last  = (p->scrollY + ch) / (p->lineH > 0 ? p->lineH : 1) + 1;
    if (first < 0) first = 0;
    if (last > p->nlines) last = p->nlines;

    for (ln = first; ln < last; ++ln) {
        int s = p->lineOff[ln];
        int e = s + p->lineLen[ln];
        int x = rc->left + pv_padx();
        int k = s;
        y = ln * p->lineH - p->scrollY + rc->top + pv_pady();

        /* 选中底色先铺，再画字：这样提示的小色块与选中底色不会互相打架 */
        if (hasSel && selLo < e && selHi > s) {
            int a = (selLo > s) ? selLo : s;
            int b = (selHi < e) ? selHi : e;
            RECT bg;
            HBRUSH br = CreateSolidBrush(pv_sel_bg());
            bg.left   = x + qp_tv_x_measured(p->disp, p->lineOff, ln, a, p->glyphWidths);
            bg.right  = x + qp_tv_x_measured(p->disp, p->lineOff, ln, b, p->glyphWidths);
            bg.top    = y - DPIX(1);
            bg.bottom = y + p->lineH + DPIX(1);
            if (bg.right <= bg.left) bg.right = bg.left + DPIX(3);   /* 空行也给个可见光标 */
            if (br) { FillRect(mem, &bg, br); DeleteObject(br); }
        }

        while (k < e) {
            int kind = p->kind[k];
            int j = k;
            int wseg;
            while (j < e && p->kind[j] == kind) ++j;
            wseg = qp_tv_x_measured(p->disp, p->lineOff, ln, j, p->glyphWidths) -
                   qp_tv_x_measured(p->disp, p->lineOff, ln, k, p->glyphWidths);
            if (kind) {
                /* 没匹配上的位置：浅红底 + 红字插进去，一眼就能挑出来 */
                RECT bg;
                HBRUSH br = CreateSolidBrush(pv_err_bg());
                bg.left = x - DPIX(1);
                bg.right = x + wseg + DPIX(1);
                bg.top = y - DPIX(1);
                bg.bottom = y + p->lineH + DPIX(1);
                if (br) { FillRect(mem, &bg, br); DeleteObject(br); }
                SetTextColor(mem, theme_error());
            } else {
                SetTextColor(mem, theme_text());
            }
            ExtTextOutW(mem, x, y, ETO_CLIPPED, rc, p->disp + k, j - k, p->glyphWidths + k);
            x += wseg;
            k = j;
        }
    }
}

static void pv_paint_table(HDC mem, Preview *p, const RECT *rc)
{
    int ch = rc->bottom - rc->top;
    int areaTop = rc->top + p->headH;
    int r, c, x, y, firstRow, lastRow;
    HBRUSH zebra = NULL, hdrBg = NULL;
    HPEN grid = NULL, oldPen;
    int empty = (p->rows <= 0 || p->cols <= 0 || !p->cells || !p->colw);

    if (empty) {
        RECT t = *rc;
        t.left += pv_padx();
        t.top += pv_pady();
        SetTextColor(mem, theme_dim());
        DrawTextW(mem, L"（当前表格为空）", -1, &t,
                  DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        return;
    }

    zebra = CreateSolidBrush(g_dark ? RGB(38, 41, 46) : RGB(249, 250, 252));
    hdrBg = CreateSolidBrush(g_dark ? RGB(52, 58, 66) : RGB(228, 234, 242));
    grid  = CreatePen(PS_SOLID, 1, theme_line());

    /* 表头：固定在顶上，不跟着竖向滚动走 */
    {
        RECT hr = *rc;
        int hx = rc->left - p->scrollX;
        hr.bottom = areaTop;
        if (hdrBg) FillRect(mem, &hr, hdrBg);
        for (c = 0; c < p->cols; ++c) {
            RECT cell;
            cell.left   = hx + pv_cellpad();
            cell.right  = hx + p->colw[c] - pv_cellpad();
            cell.top    = rc->top + DPIX(3);
            cell.bottom = areaTop;
            if (cell.right > rc->left && cell.left < rc->right) {
                wchar_t gen[32];
                const wchar_t *txt;
                if (c < p->hdrCount) {
                    txt = p->hdr + (size_t)c * QP_LABEL_MAX;
                } else {
                    _snwprintf(gen, 32, L"列 %d", c + 1);
                    gen[31] = L'\0';
                    txt = gen;
                }
                SetTextColor(mem, theme_text());
                DrawTextW(mem, txt, -1, &cell,
                          DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            }
            hx += p->colw[c];
        }
    }

    /* 数据行：只画落在可视区里的那几行 */
    {
        /* Direct fallback may use a caller-owned print DC. Never replace its
           clip; if DC state cannot be saved, keep the caller's clip intact. */
        int saved=SaveDC(mem);
        if(saved)IntersectClipRect(mem,rc->left,areaTop,rc->right,rc->bottom);
        firstRow = p->scrollY / (p->rowH > 0 ? p->rowH : 1);
        lastRow  = (p->scrollY + (ch - p->headH)) / (p->rowH > 0 ? p->rowH : 1) + 1;
        if (firstRow < 0) firstRow = 0;
        if (lastRow > p->rows) lastRow = p->rows;

        for (r = firstRow; r < lastRow; ++r) {
            y = areaTop + r * p->rowH - p->scrollY;
            if ((r & 1) && zebra) {
                RECT rr;
                rr.left = rc->left; rr.right = rc->right;
                rr.top = y; rr.bottom = y + p->rowH;
                FillRect(mem, &rr, zebra);
            }
            x = rc->left - p->scrollX;
            for (c = 0; c < p->cols; ++c) {
                const PvCell *cell = &p->cells[(size_t)r * p->cols + c];
                RECT cellrc;
                cellrc.left   = x + pv_cellpad();
                cellrc.right  = x + p->colw[c] - pv_cellpad();
                cellrc.top    = y + DPIX(4);
                cellrc.bottom = y + p->rowH - DPIX(4);
                if (cellrc.right > rc->left && cellrc.left < rc->right) {
                    if (cell->len > 0) {
                        SetTextColor(mem, cell->missing ? theme_error() : theme_text());
                        DrawTextW(mem, p->raw + cell->off, cell->len, &cellrc,
                                  DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX |
                                  DT_END_ELLIPSIS);
                    } else if (cell->missing) {
                        /* 没取到值 = 匹配失败：红字标出来 */
                        SetTextColor(mem, theme_error());
                        DrawTextW(mem, PV_TABLE_MARK, -1, &cellrc,
                                  DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX |
                                  DT_END_ELLIPSIS);
                    }
                }
                x += p->colw[c];
            }
        }

        if (grid) {
            oldPen = (HPEN)SelectObject(mem, grid);
            for (r = firstRow; r <= lastRow; ++r) {
                int yy = areaTop + r * p->rowH - p->scrollY;
                MoveToEx(mem, rc->left, yy, NULL);
                LineTo(mem, rc->right, yy);
            }
            x = rc->left - p->scrollX;
            for (c = 0; c <= p->cols; ++c) {
                MoveToEx(mem, x, areaTop, NULL);
                LineTo(mem, x, ch);
                if (c < p->cols) x += p->colw[c];
            }
            MoveToEx(mem, rc->left, areaTop - 1, NULL);
            LineTo(mem, rc->right, areaTop - 1);
            SelectObject(mem, oldPen);
        }
        if(saved)RestoreDC(mem,saved);
    }

    if (zebra) DeleteObject(zebra);
    if (hdrBg) DeleteObject(hdrBg);
    if (grid) DeleteObject(grid);
}

static void pv_draw(HWND hwnd,HDC hdc)
{
    Preview *p = pv_of(hwnd);
    HDC mem;
    HBITMAP bmp, oldBmp;
    HFONT oldFont;
    HBRUSH bk;
    RECT rc;
    int cw, ch;

    if (!hdc) return;
    GetClientRect(hwnd, &rc);
    cw = rc.right - rc.left;
    ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) { return; }

    mem = CreateCompatibleDC(hdc);
    bmp = mem ? CreateCompatibleBitmap(hdc, cw, ch) : NULL;
    if (!bmp) {
        if (mem) DeleteDC(mem);
        mem = hdc;
        oldBmp = NULL;
    } else oldBmp = (HBITMAP)SelectObject(mem, bmp);
    oldFont = (HFONT)SelectObject(mem, g_fontMono);
    SetBkMode(mem, TRANSPARENT);

    bk = g_brushCtl;
    if (!bk) {
        bk = (HBRUSH)GetStockObject(DC_BRUSH);
        SetDCBrushColor(mem, theme_ctl_bk());
    }
    FillRect(mem, &rc, bk);

    if (!p) {
        /* 理论上不会走到（状态随窗口创建） */
    } else if (p->mode == QP_PREVIEW_TABLE) {
        pv_paint_table(mem, p, &rc);
    } else {
        pv_paint_text(mem, p, &rc);
    }

    if (bmp) BitBlt(hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldFont);
    if (bmp) {
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
    }
}

static void pv_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc=BeginPaint(hwnd,&ps);
    pv_draw(hwnd,dc);
    EndPaint(hwnd,&ps);
}

/* ------------------------------------------------------------------ */
/* 窗口过程                                                            */
/* ------------------------------------------------------------------ */

static void pv_scroll_by(HWND hwnd, int dy, int dx)
{
    Preview *p = pv_of(hwnd);
    int oy, ox;
    if (!p) return;
    oy = p->scrollY;
    ox = p->scrollX;
    p->scrollY += dy;
    p->scrollX += dx;
    pv_update_scroll(hwnd);
    if (p->scrollY != oy || p->scrollX != ox) InvalidateRect(hwnd, NULL, FALSE);
}

/* ------------------------------------------------------------------ */
/* 文本模式的拖选                                                      */
/* ------------------------------------------------------------------ */

static int pv_has_sel(const Preview *p)
{
    return p && p->selEnd >= 0 && p->selEnd != p->selAnchor;
}

/* 屏幕点 → 显示串下标 */
static int pv_index_at_point(Preview *p, int px, int py)
{
    int rel, line;
    if (!p || p->nlines <= 0 || p->lineH <= 0) return 0;
    rel = py + p->scrollY - pv_pady();
    line = (rel >= 0) ? rel / p->lineH : 0;
    if (line < 0) line = 0;
    if (line >= p->nlines) line = p->nlines - 1;
    return qp_tv_index_measured(p->disp, p->lineOff, p->lineLen, line,
                          px - pv_padx(), p->glyphWidths);
}

/* 双击选词：中英文都没有空格分词，就取「一段连续的非空白」 */
static void pv_select_word(Preview *p, int idx)
{
    int lo = idx, hi = idx;
    if (!p || p->dispLen <= 0) return;
    while (lo > 0 && p->disp[lo - 1] != L' ' && p->disp[lo - 1] != L'\n') --lo;
    while (hi < p->dispLen && p->disp[hi] != L' ' && p->disp[hi] != L'\n') ++hi;
    p->selAnchor = lo;
    p->selEnd = hi;
}

/* 把选区（显示串上的一段）送进剪贴板；没有选区就不动剪贴板 */
static int pv_copy_selection(HWND hwnd, Preview *p)
{
    int lo = 0, hi = 0;
    wchar_t *buf;
    int ok;
    if (!p || !pv_has_sel(p) || !p->disp || !p->raw) return 0;
    qp_tv_sel_range(p->selAnchor, p->selEnd, &lo, &hi);
    if (lo < 0) lo = 0;
    if (hi > p->dispLen) hi = p->dispLen;
    if (hi <= lo) return 0;
    buf = (wchar_t *)malloc((wcslen(p->raw) + 1) * sizeof(wchar_t));
    if (!buf) return 0;
    if (!qp_tv_copy_raw(p->raw, p->sourceMap, p->dispLen, lo, hi, buf, (int)wcslen(p->raw) + 1)) {
        free(buf);
        return 0;
    }
    ok = qp_copy_to_clipboard(hwnd, buf);
    free(buf);
    return ok;
}

static void pv_select_all(HWND hwnd, Preview *p)
{
    if (!p || p->dispLen <= 0) return;
    p->selAnchor = 0;
    p->selEnd = p->dispLen;
    InvalidateRect(hwnd, NULL, FALSE);
}

/* 右键菜单：选中了什么就给「复制选中」，否则给「全选」 */
static void pv_context_menu(HWND hwnd, Preview *p)
{
    enum { IDM_COPY_SEL = 1, IDM_SEL_ALL = 2 };
    HMENU m = CreatePopupMenu();
    POINT pt;
    int cmd;
    if (!m || !p) { if (m) DestroyMenu(m); return; }
    AppendMenuW(m, MF_STRING | (pv_has_sel(p) ? 0 : MF_GRAYED), IDM_COPY_SEL,
                L"复制选中(&C)");
    AppendMenuW(m, MF_STRING, IDM_SEL_ALL, L"全选(&A)");
    GetCursorPos(&pt);
    cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                              pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(m);
    if (cmd == IDM_COPY_SEL) pv_copy_selection(hwnd, p);
    else if (cmd == IDM_SEL_ALL) pv_select_all(hwnd, p);
}

static LRESULT CALLBACK pv_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Preview *p = pv_of(hwnd);

    switch (msg) {
    case WM_NCCREATE: {
        Preview *np = (Preview *)calloc(1, sizeof(Preview));
        if (!np) return FALSE;
        np->mode = QP_PREVIEW_TEXT;
        np->selEnd = -1;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)np);
        return TRUE;
    }

    case WM_PAINT:
        pv_paint(hwnd);
        return 0;

    /* Restore/animation can request erase before the next WM_PAINT. Publish a
       complete buffered frame to that supplied DC instead of claiming a blank
       surface was erased. Keep both paths identical, including text/selection. */
    case WM_ERASEBKGND:
    case WM_PRINTCLIENT: {
        HDC dc=(HDC)wp;
        int saved=SaveDC(dc);
        pv_draw(hwnd,dc);
        if(saved)RestoreDC(dc,saved);
        return msg == WM_ERASEBKGND;
    }

    case WM_SIZE:
        pv_relayout(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_LBUTTONDOWN: {
        SetFocus(hwnd);                  /* 拿到焦点，滚轮与 Ctrl+C 才会送到这里 */
        if (!p) return 0;
        if (p->mode == QP_PREVIEW_TEXT) {
            int idx = pv_index_at_point(p, (short)LOWORD(lp), (short)HIWORD(lp));
            /* 点一下只把光标挪过去（选区收起）；要拖选就按住不动地拖 */
            p->selAnchor = idx;
            p->selEnd = idx;
            p->selecting = 1;
            SetCapture(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_MOUSEMOVE:
        if (p && p->selecting && p->mode == QP_PREVIEW_TEXT) {
            int idx = pv_index_at_point(p, (short)LOWORD(lp), (short)HIWORD(lp));
            if (idx != p->selEnd) {
                p->selEnd = idx;
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;

    case WM_LBUTTONUP:
        if (p && p->selecting) {
            p->selecting = 0;
            if (GetCapture() == hwnd) ReleaseCapture();
        }
        return 0;

    case WM_LBUTTONDBLCLK:
        if (p && p->mode == QP_PREVIEW_TEXT) {
            pv_select_word(p, pv_index_at_point(p, (short)LOWORD(lp), (short)HIWORD(lp)));
            p->selecting = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_CAPTURECHANGED:
        if (p) p->selecting = 0;
        return 0;

    case WM_RBUTTONUP:
        if (p && p->mode == QP_PREVIEW_TEXT) pv_context_menu(hwnd, p);
        return 0;

    case WM_KEYDOWN:
        if (p && p->mode == QP_PREVIEW_TEXT) {
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                if (wp == 'A') { pv_select_all(hwnd, p); return 0; }
                if (wp == 'C' || wp == VK_INSERT) {
                    /* 有选区就复制选区；没选区时留给「一键复制」按钮那条路 */
                    if (pv_has_sel(p)) { pv_copy_selection(hwnd, p); return 0; }
                    return 0;
                }
            }
        }
        break;

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int step = (p && p->lineH > 0) ? p->lineH : DPIX(16);
        if (p && p->mode == QP_PREVIEW_TABLE && p->rowH > 0) step = p->rowH;
        pv_scroll_by(hwnd, delta > 0 ? -3 * step : 3 * step, 0);
        return 0;
    }

    case WM_VSCROLL: {
        SCROLLINFO si;
        int pos, step;
        if (!p) return 0;
        step = (p->lineH > 0) ? p->lineH : DPIX(16);
        if (p->mode == QP_PREVIEW_TABLE && p->rowH > 0) step = p->rowH;
        pos = p->scrollY;
        memset(&si, 0, sizeof(si));
        si.cbSize = sizeof(si);
        si.fMask = SIF_TRACKPOS | SIF_PAGE;
        GetScrollInfo(hwnd, SB_VERT, &si);
        switch (LOWORD(wp)) {
        case SB_LINEUP:   pos -= step; break;
        case SB_LINEDOWN: pos += step; break;
        case SB_PAGEUP:   pos -= (int)si.nPage; break;
        case SB_PAGEDOWN: pos += (int)si.nPage; break;
        case SB_TOP:      pos = 0; break;
        case SB_BOTTOM:   pos = 1 << 28; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        default: return 0;
        }
        p->scrollY = pos;
        pv_update_scroll(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_HSCROLL: {
        SCROLLINFO si;
        int pos, step;
        if (!p) return 0;
        step = DPIX(24);
        pos = p->scrollX;
        memset(&si, 0, sizeof(si));
        si.cbSize = sizeof(si);
        si.fMask = SIF_TRACKPOS | SIF_PAGE;
        GetScrollInfo(hwnd, SB_HORZ, &si);
        switch (LOWORD(wp)) {
        case SB_LINELEFT:  pos -= step; break;
        case SB_LINERIGHT: pos += step; break;
        case SB_PAGELEFT:  pos -= (int)si.nPage; break;
        case SB_PAGERIGHT: pos += (int)si.nPage; break;
        case SB_LEFT:      pos = 0; break;
        case SB_RIGHT:     pos = 1 << 28; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        default: return 0;
        }
        p->scrollX = pos;
        pv_update_scroll(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_NCDESTROY:
        if (p) {
            Preview *pp = p;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            pv_free_model(pp);
            free(pp);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

int qp_preview_register_class(HINSTANCE inst)
{
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;               /* 双击选词 */
    wc.lpfnWndProc = pv_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = CLS_PREVIEW;
    return RegisterClassExW(&wc) ? 1 : 0;
}

HWND qp_preview_create(HWND parent, int id, DWORD exstyle)
{
    /* 两个滚动条都建出来，用不用由 ShowScrollBar 决定（文本模式折行，
       不需要横向滚动）。
       WS_GROUP：自己起一个对话框组，这样「文本 / 表格」那两个单选的
       方向键导航不会绕到后面的按钮上；不给 WS_TABSTOP，不进 Tab 顺序。
       两个滚动条都建出来，用不用由 ShowScrollBar 决定。 */
    HWND h = CreateWindowExW(exstyle, CLS_PREVIEW, L"",
                             WS_CHILD | WS_VISIBLE | WS_GROUP | WS_VSCROLL |
                             WS_HSCROLL | WS_CLIPCHILDREN,
                             0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, g_hInst, NULL);
    if (h) {
        Preview *p = pv_of(h);
        if (p) p->scrollX = p->scrollY = 0;
        SendMessageW(h, WM_SETFONT, (WPARAM)g_fontUI, TRUE);
    }
    return h;
}

void qp_preview_set_mode(HWND h, int mode)
{
    Preview *p = pv_of(h);
    if (!p || p->mode == mode || !p->modelValid || p->modelMode != mode) return;
    p->mode = mode;
    p->scrollX = p->scrollY = 0;
    p->selAnchor = 0;
    p->selEnd = -1;
    p->selecting = 0;
    pv_relayout(h);
    InvalidateRect(h, NULL, FALSE);
}

int qp_preview_mode(HWND h)
{
    Preview *p = pv_of(h);
    return p ? p->mode : QP_PREVIEW_TEXT;
}

int qp_preview_set_text(HWND h, const wchar_t *text, const int *marks, int nmarks)
{
    Preview *p = pv_of(h);
    if (!p) return 0;
    if (!text) text=L"";
    if (!marks || nmarks<0) nmarks=0;
    if (nmarks>PV_MAX_MARKS) nmarks=PV_MAX_MARKS;
    if (p->modelValid && p->modelMode==OUTPUT_TEXT && p->raw &&
        !wcscmp(p->raw,text) && p->modelNmarks==nmarks &&
        (!nmarks || !memcmp(p->modelMarks,marks,(size_t)nmarks*sizeof(*marks)))) {
        if(p->measureDirty || p->measuredFont!=g_fontMono || p->measuredDpi!=g_dpi) {
            pv_relayout(h);InvalidateRect(h,NULL,FALSE);
        }
        return 1;
    }
    {
        wchar_t *raw = qp_wcsdup(text ? text : L"");
        if (!raw) return 0;
        if (!pv_build_disp(p, text, marks, nmarks)) { free(raw); return 0; }
        free(p->raw);
        p->raw = raw;
    }
    p->mode=OUTPUT_TEXT;p->modelValid=1; p->modelMode=OUTPUT_TEXT; p->modelNmarks=nmarks;
    if(nmarks)memcpy(p->modelMarks,marks,(size_t)nmarks*sizeof(*marks));
    /* 换了内容，旧选区就没意义了 */
    p->selAnchor = 0;
    p->selEnd = -1;
    p->selecting = 0;
    p->scrollY = 0;
    pv_relayout(h);
    InvalidateRect(h, NULL, FALSE);
    return 1;
}

int qp_preview_set_table_marked(HWND h, const wchar_t *rendered, const wchar_t *format,
                                 const int *marks, int nmarks)
{
    Preview *p = pv_of(h);
    HDC dc;
    int i, c;
    if (!p) return 0;

    if (!rendered) rendered = L"";
    if (!format) format=L"";
    if (!marks && nmarks>=0)nmarks=0;
    if (nmarks>PV_MAX_MARKS)nmarks=PV_MAX_MARKS;
    int same=p->modelValid && p->modelMode==OUTPUT_TABLE && p->raw && p->modelFormat &&
        !wcscmp(p->raw,rendered) && !wcscmp(p->modelFormat,format) && p->modelNmarks==nmarks &&
        (nmarks<=0 || !memcmp(p->modelMarks,marks,(size_t)nmarks*sizeof(*marks)));
    if(same && p->measuredFont==g_fontMono && p->measuredDpi==g_dpi)return 1;
    Preview *original=p, candidate=*p;
    p=&candidate;
    p->raw=NULL;p->cells=NULL;p->colw=NULL;p->hdr=NULL;p->modelFormat=NULL;
    {
        wchar_t *raw = (wchar_t *)malloc((wcslen(rendered) + 1) * sizeof(wchar_t));
        if (!raw) return 0;
        wcscpy(raw, rendered);
        p->raw = raw;
        p->modelFormat=malloc((wcslen(format)+1)*sizeof(wchar_t));
        if(!p->modelFormat){free(raw);return 0;}
        wcscpy(p->modelFormat,format);
    }

    pv_scan_rows(p->raw, &p->rows, &p->cols);
    if (!p->rows && marks && nmarks > 0) p->rows = p->cols = 1;
    if (p->rows <= 0 || p->cols <= 0) {
        p->rows = p->cols = 0;
        p->hdrCount = 0;
        p->totalW = 0;
        p->scrollX = p->scrollY = 0;
        goto commit;
    }

    {
    PvCell *cells = (PvCell *)calloc((size_t)p->rows * p->cols, sizeof(PvCell));
    int *colw = (int *)calloc((size_t)p->cols, sizeof(int));
    wchar_t *hdr = (wchar_t *)calloc((size_t)PV_MAX_COLS * QP_LABEL_MAX, sizeof(wchar_t));
    if (!cells || !colw || !hdr) {
        free(cells); free(colw); free(hdr);
        free(p->raw);free(p->modelFormat);
        return 0;
    }
    p->cells = cells; p->colw = colw; p->hdr = hdr;
    }
    pv_fill_cells(p);
    if (!p->raw[0]) { p->cells[0].off = 0; p->cells[0].len = 0; }
    for (i = 0; i < p->rows * p->cols; ++i) {
        PvCell *cell = &p->cells[i];
        int k;
        cell->missing = nmarks < 0 && cell->len == 0;
        for (k = 0; marks && k < nmarks; ++k)
            if (cell->off >= 0 && marks[k] >= cell->off && marks[k] <= cell->off + cell->len)
                cell->missing = 1;
    }
    pv_build_header(p, format);

    /* 列宽必须从 0 重新起算。p->colw 是 realloc 出来的，里面还留着上一次算完
       （已经加过一遍 pv_cellpad() * 2 内边距）的宽度；下面的测量又只「取更大
       值」，于是每刷新一次就再宽一个内边距，几次以后整张表撑到列宽上限——
       用户看到的就是「预览文字后面莫名多出一段空格」。 */
    for (c = 0; c < p->cols; ++c) p->colw[c] = 0;

    p->rowH = DPIX(22);
    dc = GetDC(h);
    if (dc) {
        HFONT old = (HFONT)SelectObject(dc, g_fontMono);
        TEXTMETRICW tm;
        memset(&tm, 0, sizeof(tm));
        if (GetTextMetricsW(dc, &tm) && tm.tmHeight > 0)
            p->rowH = tm.tmHeight + DPIX(4) * 2;
        if (p->rowH < DPIX(20)) p->rowH = DPIX(20);

        for (i = 0; i < p->rows * p->cols; ++i) {
            const PvCell *cell = &p->cells[i];
            SIZE sz;
            int w;
            c = i % p->cols;
            if (cell->len > 0) {
                if (!GetTextExtentPoint32W(dc, p->raw + cell->off, cell->len, &sz))
                    sz.cx = cell->len * DPIX(8);
                w = sz.cx;
            } else if (cell->missing) {
                if (!GetTextExtentPoint32W(dc, PV_TABLE_MARK, 3, &sz)) sz.cx = DPIX(42);
                w = sz.cx;
            } else w = 0;
            if (w > p->colw[c]) p->colw[c] = w;
        }
        for (c = 0; c < p->hdrCount && c < p->cols; ++c) {
            const wchar_t *hname = p->hdr + (size_t)c * QP_LABEL_MAX;
            SIZE sz;
            if (GetTextExtentPoint32W(dc, hname, (int)wcslen(hname), &sz) &&
                sz.cx > p->colw[c])
                p->colw[c] = sz.cx;
        }
        SelectObject(dc, old);
        ReleaseDC(h, dc);
    }
    p->totalW = 0;
    for (c = 0; c < p->cols; ++c) {
        p->colw[c] += pv_cellpad() * 2;
        if (p->colw[c] < DPIX(60)) p->colw[c] = DPIX(60);
        if (p->colw[c] > pv_maxcolw()) p->colw[c] = pv_maxcolw();
        p->totalW += p->colw[c];
    }
    p->headH = p->rowH;
commit:
    p->mode=OUTPUT_TABLE;p->modelValid=1;p->modelMode=OUTPUT_TABLE;p->modelNmarks=nmarks;
    if(nmarks>0)memcpy(p->modelMarks,marks,(size_t)nmarks*sizeof(*marks));
    p->measuredFont=g_fontMono;p->measuredDpi=g_dpi;
    if(!same){p->scrollX=p->scrollY=0;p->selAnchor=0;p->selEnd=-1;p->selecting=0;}
    free(original->raw);free(original->cells);free(original->colw);
    free(original->hdr);free(original->modelFormat);
    *original=*p;
    pv_update_scroll(h);
    InvalidateRect(h, NULL, FALSE);
    return 1;
}

void qp_preview_set_table(HWND h, const wchar_t *rendered, const wchar_t *format)
{
    qp_preview_set_table_marked(h, rendered, format, NULL, -1);
}

void qp_preview_clear(HWND h)
{
    Preview *p = pv_of(h);
    if (!p) return;
    pv_free_model(p);
    if (h) InvalidateRect(h, NULL, FALSE);
}

void qp_preview_theme_changed(HWND h)
{
    if (!h) return;
    InvalidateRect(h, NULL, TRUE);
}
