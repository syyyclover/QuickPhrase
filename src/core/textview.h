/*
 * textview.h —— 预览用的纯文本排版内核（纯 C，不依赖任何 Windows API）
 *
 * 把「预览控件怎么摆这段文本」这件事从窗口代码里拆出来，好处是可以在本机
 * 直接单元测试（见 tests/test_textview.c）：
 *   · 把原文与「未匹配提示」拼成一条显示串，并逐字符标出哪些是自己插的提示；
 *   · 按可用像素宽折行；
 *   · 显示串下标 ↔ 屏幕位置互算（鼠标拖选命中要用）。
 *
 * 关键约定：提示只出现在调用方给的位置上，**绝不**因为正文里出现了「失败」
 * 这两个字就把它当成提示 —— 正文永远是 kind = 0。
 */
#ifndef QP_TEXTVIEW_H
#define QP_TEXTVIEW_H

#include <stddef.h>
#include <wchar.h>

/* 正文里的 \t 在预览里折成几个空格 */
#define QP_TV_TAB_COLS 4

/* 等宽字体下这个字符是否按「两格宽」算（CJK / 假名 / 谚文 / 全角） */
int qp_tv_char_wide(wchar_t c);

/* 单字符的估算宽度（像素） */
int qp_tv_char_w(wchar_t c, int w_ascii, int w_wide);

/* 把原文与提示拼成显示串。
   marks[i] 是「该插提示」的位置（相对 text 的字符下标，必须按升序、可重复），
   在每个这样的位置插入 mark 文本。kind 与 disp 逐字符对应：0 = 正文，1 = 提示。
   超界的位置夹到末尾，负数忽略。返回显示串长度（不含结尾 0）；
   disp_cap 不够就截断，并且始终保证结尾 0。 */
int qp_tv_build(const wchar_t *text, const int *marks, int nmarks,
                const wchar_t *mark, wchar_t *disp, int disp_cap,
                unsigned char *kind);

/* 所需容量含结束符和 Tab 展开；映射中 -1 表示提示，其余为原文下标。 */
size_t qp_tv_capacity(const wchar_t *text, int nmarks, const wchar_t *mark);
int qp_tv_build_mapped(const wchar_t *text, const int *marks, int nmarks,
                       const wchar_t *mark, wchar_t *disp, int disp_cap,
                       unsigned char *kind, int *source_map);
/* 显示选区转成原文：保留 Tab/换行，不复制插入的提示。 */
int qp_tv_copy_raw(const wchar_t *raw, const int *source_map, int disp_len,
                   int a, int b, wchar_t *out, int cap);

/* 按可用宽度 avail（像素）折行：优先在空格处断，其次逐字符断。
   每行的起始下标与长度收进 line_off / line_len（两者容量都是 line_cap）。
   返回行数（最多 line_cap 行，多出来的内容丢掉）。 */
int qp_tv_wrap(const wchar_t *disp, int disp_len, int w_ascii, int w_wide,
               int avail, int *line_off, int *line_len, int line_cap);
/* widths 为逐 UTF-16 code unit 的实际像素宽；代理对第二项为 0。 */
int qp_tv_wrap_measured(const wchar_t *disp, int disp_len, const int *widths,
                        int avail, int *line_off, int *line_len, int line_cap);
int qp_tv_x_measured(const wchar_t *disp, const int *line_off, int line, int idx,
                     const int *widths);
int qp_tv_index_measured(const wchar_t *disp, const int *line_off, const int *line_len,
                         int line, int x, const int *widths);

/* 显示串下标 idx → 它在本行内的像素偏移（idx 会被夹到本行范围内） */
int qp_tv_x_of(const wchar_t *disp, const int *line_off, int line, int idx,
               int w_ascii, int w_wide);

/* (行号, 行内像素 x) → 显示串下标（鼠标命中）。
   x 落在某个字符的前半段算这个字符，后半段算下一个。 */
int qp_tv_index_at(const wchar_t *disp, const int *line_off, const int *line_len,
                   int line, int x, int w_ascii, int w_wide);

/* 取两个下标之间的选区（自动排好先后），用于画选中底色 */
void qp_tv_sel_range(int a, int b, int *lo, int *hi);

#endif /* QP_TEXTVIEW_H */
