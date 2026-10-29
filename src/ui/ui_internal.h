#ifndef QP_UI_INTERNAL_H
#define QP_UI_INTERNAL_H
/*
 * ui.c —— 主窗口、模板树、动态生成区、实时预览、复制、主题
 */
#include "ui.h"
#include "resource.h"
#include "preview.h"
#include "table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commdlg.h>
#include <uxtheme.h>

#ifndef TVS_EX_DOUBLEBUFFER
#  define TVS_EX_DOUBLEBUFFER 0x0004
#endif
#ifndef EM_SETCUEBANNER
#  define EM_SETCUEBANNER 0x1501
#endif
#ifndef LVS_EX_DOUBLEBUFFER
#  define LVS_EX_DOUBLEBUFFER 0x00010000
#endif

#define QP_ROW_CTL_H   DPIX(24)     /* 普通控件高度       */
#define QP_MULTILINE_H DPIX(88)     /* 多行文本框高度     */
#define QP_ROW_GAP     DPIX(6)      /* 行间距             */
#define QP_SESS_MAX 9              /* 每个口径最多同时开几份（对应 Ctrl+1~9） */
#define QP_TABS_H DPIX(27)
#define QP_SPLIT_HIT DPIX(10)      /* 竖分隔条命中宽度 */
#define QP_HSPLIT_HIT DPIX(8)      /* 横分隔条命中高度（顶掉原来的 6px 间距） */
#define QP_TREE_W_DEF 230          /* 双击竖条恢复到这个宽度 */
typedef struct {
    int   field_index;             /* 在 g_ctx->fields 中的下标 */
    HWND  hLabel;
    HWND  hCtl;                    /* Edit / ComboBox / 只读 Edit */
    HWND  hBtn;                    /* multi-select 的 ▼ / datetime 的「现在」 */
    int   y;                       /* 逻辑 y（未减去滚动量）      */
    int   h;                       /* 控件高度（多行文本框更高）  */
    int   btnW;                    /* hBtn 宽度；0 = 用默认 DPIX(26) */
    int   extraIndex;              /* select：为规则值临时追加的项，-1 = 无 */
    int   readonly;                /* text：当前是否因被规则接管而只读 */
} FieldCtl;
typedef struct { int kind; wchar_t id[QP_ID_MAX]; } NodeRef;
typedef enum { APPMODE_DEFAULT = 0, APPMODE_ALLOWDARK = 1, APPMODE_FORCEDARK = 2,
               APPMODE_FORCELIGHT = 3 } QpAppMode;
typedef int  (WINAPI *FnSetPreferredAppMode)(int);
typedef BOOL (WINAPI *FnAllowDarkModeForApp)(BOOL);
typedef BOOL (WINAPI *FnAllowDarkModeForWindow)(HWND, BOOL);
typedef void (WINAPI *FnFlushMenuThemes)(void);
typedef HRESULT (WINAPI *FnDwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
typedef void (WINAPI *FnRtlGetNtVersionNumbers)(DWORD *, DWORD *, DWORD *);
typedef struct {
    wchar_t   tmpl_id[QP_ID_MAX];  /* 归属模板（只存 id，见上方注释） */
    wchar_t   name[QP_NAME_MAX];   /* 标签名，可重命名               */
    int       color;               /* 调色板下标                     */
    unsigned  used;                /* 最近一次激活的序号             */
    wchar_t (*ids)[QP_ID_MAX];     /* 变量 id 列表（激活时抓取）     */
    wchar_t **vals;                /* 与 ids 一一对应；NULL = 没手填过 */
    wchar_t **choice_values, **choice_bases;
    unsigned char *choice_flags;   /* 0 = 无状态，1 = 普通选项，2 = 选中自主补充 */
    int       count;
} Session;
const wchar_t *multi_sep(const Segment *seg);
void ui_choice_single(int index);
void ui_choice_single_queue(int index,HWND anchor);
void ui_choice_dispatch(void);
void ui_choice_cancel_pending(void);
void ui_choice_multi(int index,const unsigned char *checked,int count);
typedef struct { int x, w; } TabBox;
extern HWND g_hTree, g_hSearch, g_hTitle, g_hHost, g_hPreview;
extern HWND g_hBtnCopy, g_hBtnReset, g_hBtnEdit, g_hHint, g_hPreviewLabel;
extern HWND g_hPreviewTextMode, g_hPreviewTableMode;
extern Template   *g_curTmpl;
extern EvalContext *g_ctx;
extern OutputMode   g_previewMode;
extern FieldCtl *g_fields;
extern int       g_fieldCount;
extern int       g_hostScroll;
extern int       g_hostContentH;
extern int       g_unstableWarned;
extern int       g_syncFields;
extern HWND  g_popup;
extern int   g_popupField;
extern HWND  g_popupOwnerCtl;
extern const Segment *g_popupSeg;
extern unsigned char *g_popupChecked;
extern int   g_popupCount;
extern int   g_popupHot;
extern int   g_popupTop;
extern int   g_popupRows;
extern int   g_popupItemH;
extern int   g_popupSbDrag;
extern int   g_popupSbGrabDy;
extern HTREEITEM g_dragItem;
extern int        g_dragging;
extern int g_splitDrag;
extern int g_splitGrabDx;
extern int g_splitLastX;
extern int g_hsplitDrag;
extern int g_hsplitGrabDy;
extern int g_hsplitLastY;
extern const wchar_t CLS_MAIN[];
extern const wchar_t CLS_HOST[];
extern const wchar_t CLS_POPUP[];
extern const wchar_t CLS_TABS[];

extern FnFlushMenuThemes        s_flushMenuThemes;
extern Session  *g_curSess;
extern HWND g_hTabs;
extern int  g_tabsShown;
void dark_apply_window(HWND hwnd);
void popup_close(void);
void popup_move_hot(int delta);
void popup_toggle(int row);
LRESULT CALLBACK popup_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
void popup_open(int field_index, HWND anchor);
void host_clamp_scroll(void);
void host_update_scroll(void);
void host_reposition(void);
LRESULT CALLBACK host_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK multi_edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                        UINT_PTR id, DWORD_PTR ref);
int sess_activate(Session *s, const Template *t);
int sess_deactivate(void);
int sess_capture_current(void);
int ui_preview_show_result(const wchar_t *out,const int *marks,int nmarks);
void sess_discard_pending(Session *s);
Session *sess_pick_for(const wchar_t *tmpl_id);
void sess_prune(void);
void sess_free_all(void);
void tabs_measure(void);
void tabs_refresh(void);
void sess_switch_to(int vis);
void sess_cycle(int delta);
void sess_new(void);
void sess_duplicate(int vis);
void sess_close(int vis);
void sess_rename(int vis);
void sess_set_color(int vis, int color);
int sess_target_vis(void);
LRESULT CALLBACK tabs_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
void ui_clear_fields(void);
void ui_build_fields(void);
void ui_sync_controls(void);
void ui_update_preview_mode_labels(void);
void ui_sync_preview_mode_controls(void);
size_t ui_render_current(wchar_t *out, size_t cap,
                                int *marks, int mark_cap, int *mark_count);
QpRenderResult ui_render_current_checked(wchar_t *out,size_t cap,int *marks,int mark_cap);
int ui_select_template(Template *t);
NodeRef *tree_ref(HTREEITEM item);
Template *tree_selected_template(void);
void ui_new_template(void);
void ui_insert_demo_template(void);
void ui_duplicate_template(void);
void ui_delete_template(void);
void ui_new_group(void);
void ui_rename_group(void);
void ui_delete_group(void);
void ui_move_template_to_group(const wchar_t *tmpl_id, const wchar_t *group_id);
void ui_export(void);
void ui_import(void);
void ui_build_menu(HWND hwnd);
void ui_tree_context_menu(int x, int y);
int ui_tree_width_for(int clientW);
int splitter_x(void);
void ui_preview_bounds(int H, int *lo, int *hi);
int hsplitter_y(int H);
int hsplit_hit(int x, int y);
void ui_layout(HWND hwnd);
#endif
