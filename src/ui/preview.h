/*
 * preview.h —— 口径预览控件（普通文本 / 表格两种呈现，自绘）
 *
 * 主界面和模板编辑器共用同一个控件，避免两处各写一套、各长各的样子：
 *   · 表格：按 \t 分成真正的列，列宽取该列最宽单元格与列头的大者，配列名表头、
 *     浅网格线、隔行底色；表头固定不随竖向滚动走，横竖都能滚。
 *   · 文本：按控件宽度折行；「该有值却空着」的位置（固定内容里的 {变量} 展开成
 *     空、或某条解析规则的目标没取到值）用红字标出「失败」。
 * 这个控件只负责「看着清楚」，复制走的仍是引擎渲染出来的原文（真实 Tab）。
 */
#ifndef QP_PREVIEW_H
#define QP_PREVIEW_H

#include "app.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QP_PREVIEW_TEXT   0
#define QP_PREVIEW_TABLE  1

/* 注册窗口类（进程内调用一次即可）。成功返回 1 */
int  qp_preview_register_class(HINSTANCE inst);
/* 创建预览控件。返回 NULL 表示失败（调用方应能降级） */
HWND qp_preview_create(HWND parent, int id, DWORD exstyle);
/* Compatibility: only selects an already-published compatible model.
   Checked content setters below publish mode and content together. */
void qp_preview_set_mode(HWND h, int mode);
int  qp_preview_mode(HWND h);
/* 文本模式的内容。marks 是「失败位置」（相对 text 的字符下标，需按升序给出），
   没有就传 NULL / 0。 */
int qp_preview_set_text(HWND h, const wchar_t *text, const int *marks, int nmarks);
/* 表格模式的内容。rendered 是引擎渲染出来的表格原文（真实 Tab）；
   format 是模板的 tableOutputFormat，用来生成列名表头（可传 NULL）。 */
void qp_preview_set_table(HWND h, const wchar_t *rendered, const wchar_t *format);
int qp_preview_set_table_marked(HWND h, const wchar_t *rendered, const wchar_t *format,
                                 const int *marks, int nmarks);
/* 清空 */
void qp_preview_clear(HWND h);
/* 换主题后重画 */
void qp_preview_theme_changed(HWND h);

#ifdef __cplusplus
}
#endif

#endif /* QP_PREVIEW_H */
