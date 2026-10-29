#ifndef QP_DIALOGS_INTERNAL_H
#define QP_DIALOGS_INTERNAL_H
/*
 * ui_dialogs.c —— 模板编辑器、解析/条件规则编辑器、设置、输入框、关于
 *
 * 这些窗口不使用 .rc 里的对话框模板，而是用普通窗口 + 手工布局，
 * 这样可以按 DPI 缩放、按片段类型动态显隐属性行，并统一套用深浅主题。
 */
#include "ui.h"
int qp_dialog_fit_fixed(HWND hwnd,HWND owner);
int qp_dialog_fit_resizable(HWND hwnd,HWND owner,int min_width,int min_height);
void qp_dialog_content_rect(HWND hwnd,RECT *r);
int qp_dialog_require_size(HWND hwnd,int width,int height);
HDWP qp_dialog_defer(HDWP queue,HWND child,int x,int y,int width,int height);
void qp_dialog_update_view(HWND hwnd);
void qp_dialog_limit_tracking(HWND hwnd,MINMAXINFO *mmi);
/* UI-private preflight: preserve active input before committing a model change. */
int sess_capture_current(void);
#include "resource.h"
#include "regex.h"
#include "preview.h"
#include "table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wctype.h>

#ifndef EM_SETCUEBANNER
#  define EM_SETCUEBANNER 0x1501
#endif
#ifndef LVS_EX_DOUBLEBUFFER
#  define LVS_EX_DOUBLEBUFFER 0x00010000
#endif

void ensure_class(const wchar_t *cls, WNDPROC proc);
HWND make_dlg_ex(const wchar_t *cls, WNDPROC proc, const wchar_t *title,
                        int cw, int ch, HWND owner, void *param, DWORD extra);
HWND make_dlg(const wchar_t *cls, WNDPROC proc, const wchar_t *title,
                     int cw, int ch, HWND owner, void *param);
void dlg_close(HWND hwnd);
int run_modal(HWND owner, HWND dlg, const int *result);
LRESULT dlg_common(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, int *handled);
HWND mk_label(HWND p, const wchar_t *text, int id);
HWND mk_label_r(HWND p, const wchar_t *text, int id);
HWND mk_wrapped_label(HWND p, const wchar_t *text, int id);
HWND mk_edit(HWND p, int id, DWORD extra);
HWND mk_btn(HWND p, const wchar_t *text, int id);
HWND mk_check(HWND p, const wchar_t *text, int id);
HWND mk_combo(HWND p, int id);
HWND mk_combo_group(HWND p, int id);
HWND mk_radio(HWND p, const wchar_t *text, int id, int first_in_group);
HWND mk_combo_edit(HWND p, int id);
HWND mk_list(HWND p, int id, DWORD extra);
void lv_add_col(HWND h, int idx, const wchar_t *title, int width);
void lv_set_row(HWND h, int row, int col, const wchar_t *text);
int lv_sel(HWND h);
void lv_select(HWND h, int i);
void fill_var_combo(HWND combo, const Template *t, const wchar_t *sel_id,
                           const Segment *exclude, int include_static);
const Segment *combo_selected_seg(HWND combo);
int dlg_parse_rule(HWND owner, ParseRule *rule, const Template *t,
                          const Segment *owner_seg);
int dlg_cond_rule(HWND owner, ConditionRule *rule, const Template *t,
                         const Segment *owner_seg);
int dlg_option(HWND owner,const wchar_t *title,wchar_t *buf,size_t cap,
               int *custom,int custom_available);
#endif
