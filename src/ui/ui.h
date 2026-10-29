/*
 * ui.h —— 主窗口 / 生成界面 / 各对话框 的对外接口
 *   ui.c          主窗口、模板树、动态字段区、实时预览、复制、主题
 *   ui_dialogs.c  模板编辑对话框、规则编辑对话框、设置对话框、输入框
 */
#ifndef QP_UI_H
#define QP_UI_H

#include "app.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------- ui.c -------------------------------- */
int    ui_register_classes(void);
HWND   ui_create_main_window(void);
HACCEL ui_create_accelerators(void);

/* 重建左侧模板树；select_template_id 非空时重建后选中该模板 */
void ui_rebuild_tree(const wchar_t *select_template_id);
/* 重新求值当前模板并刷新控件 / 预览 */
void ui_refresh_current(void);
void ui_focus_search(void);
void ui_copy_current(void);
void ui_reset_current(void);
void ui_edit_current(void);
int ui_reset_selection(void);
/* 主题切换后刷新界面 */
void ui_theme_changed(void);
Template *ui_current_template(void);

/* ------------------------- ui_dialogs.c --------------------------- */
/* 模板编辑器：直接编辑传入的副本，返回 1 = 用户点了保存 */
int  dlg_edit_template(HWND owner, Template *working, int is_new);
int  dlg_settings(HWND owner);
int  dlg_input(HWND owner, const wchar_t *title, const wchar_t *prompt,
               wchar_t *buf, size_t cap);
void dlg_about(HWND owner);

/* 数据表（名单 / 联系表）管理：直接改 app->tables，返回 1 = 有改动（需存盘） */
int  dlg_data_tables(HWND owner, AppData *app);
/* 查表匹配规则：配置一张表怎么查、查到的东西填到哪些变量。1 = 用户点了确定 */
int  dlg_lookup_rule(HWND owner, ParseRule *rule, const Template *t);

/* --------------------- 公共小工具（ui.c 实现） -------------------- */
HWND qp_create(const wchar_t *cls, const wchar_t *text, DWORD style, DWORD ex,
               int x, int y, int w, int h, HWND parent, int id);
/* 批量摆位：把一批位置变化攒成一次 EndDeferWindowPos 提交。位置没变的控件直接
   跳过；尺寸没变、只是挪位置的让系统搬像素而不重绘（否则标签会「闪一下」）。 */
HDWP qp_defer(HDWP dwp, HWND h, int x, int y, int w, int ht);
/* 返回私有布局队列；必须配对 qp_layout_end，不能传给原生 EndDeferWindowPos。 */
HDWP qp_layout_begin(int capacity);
void qp_layout_end(HDWP batch);
void qp_show_if_needed(HWND h, int visible);
void qp_set_column_width_if_needed(HWND h, int col, int width);
void qp_set_scroll_info_if_needed(HWND h, int bar, const SCROLLINFO *info);
int qp_line_height(HWND h);
int qp_wrapped_text_height(HWND h, int width);
void qp_paint_background(HWND h);
void qp_draw_background(HWND h,HDC dc);
void qp_draw_window_frame(HWND hwnd,HDC dc,const RECT *area,int editorY);
/* Delay discrete window transitions until their full client frame is ready. */
enum { QP_TIMER_PRESENT = 3 };
void qp_prepare_present(HWND hwnd, int *pending);
void qp_finish_present(HWND hwnd, int *pending);
void qp_buffer_static(HWND h);
void qp_get_text(HWND h, wchar_t *buf, size_t cap);
void qp_set_text_if_diff(HWND h, const wchar_t *text);
int  qp_msgbox(HWND owner, UINT type, const wchar_t *title, const wchar_t *fmt, ...);
int  qp_copy_to_clipboard(HWND owner, const wchar_t *text);
void qp_theme_children(HWND parent);
void qp_apply_font(HWND parent);
/* 统一处理 WM_CTLCOLOR*；*handled=1 时返回值即窗口过程应返回的画刷 */
LRESULT qp_handle_ctlcolor(UINT msg, WPARAM wp, LPARAM lp, int *handled);
/* 让列表/树控件跟随主题（含 Win10+ 深色滚动条） */
void qp_theme_list(HWND h);
/* 模态窗口的通用消息循环；窗口销毁后返回 close_code */
int  qp_modal_loop(HWND owner, HWND modal, const int *result_flag);
void qp_center_window(HWND hwnd, HWND owner);
int qp_window_work_area(HWND owner, RECT *work);
const wchar_t *qp_render_status_hint(QpRenderStatus status);
/* ListView 拖拽排序辅助：返回拖到的目标索引，-1 表示取消 */
int  qp_lv_drag_reorder(HWND hlv, int from_index);
/* ComboBox 下拉时临时拉高窗口，收起后还原（在 WM_COMMAND 开头调用） */
int  qp_combo_dropdown_fix(WPARAM wp, LPARAM lp);

#ifdef __cplusplus
}
#endif

#endif /* QP_UI_H */
