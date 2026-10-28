#include "ui_internal.h"
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

/* ================================================================== */
/* 模块内全局                                                          */
/* ================================================================== */

HWND g_hTree, g_hSearch, g_hTitle, g_hHost, g_hPreview;
HWND g_hBtnCopy, g_hBtnReset, g_hBtnEdit, g_hHint, g_hPreviewLabel;
HWND g_hPreviewTextMode, g_hPreviewTableMode;

Template   *g_curTmpl;      /* 当前选中的模板（指向 g_app 中的对象） */
EvalContext *g_ctx;         /* 当前模板的求值上下文                  */
OutputMode   g_previewMode = OUTPUT_TEXT;

/* 填写区一行的度量（都随 DPI 缩放） */




/* 动态字段控件 */


FieldCtl *g_fields;
int       g_fieldCount;
int       g_hostScroll;     /* 字段区滚动偏移 */
int       g_hostContentH;
int       g_unstableWarned; /* 提示行里现在挂着「规则相互覆盖」那句 */
int       g_syncFields;     /* >0：正在把引擎值写进控件，别回读 */

/* 多选下拉弹层（自绘 + 鼠标捕获，不改变焦点与激活，详见下方章节注释） */
HWND  g_popup;
int   g_popupField = -1;
HWND  g_popupOwnerCtl;          /* 锚点控件（只读 Edit）        */
const Segment *g_popupSeg;
unsigned char *g_popupChecked;  /* g_popupCount 字节，1 = 已勾选 */
int   g_popupCount;
int   g_popupHot = -1;          /* 鼠标/键盘所在行，-1 = 无      */
int   g_popupTop;               /* 首个可见行                    */
int   g_popupRows;              /* 可见行数                      */
int   g_popupItemH;             /* 行高                          */
int   g_popupSbDrag;            /* 正在拖动自绘滚动条            */
int   g_popupSbGrabDy;          /* 滑块内抓取偏移                */

/* 拖拽（模板树 / 列表） */
HTREEITEM g_dragItem;
int        g_dragging;

/* 分隔条拖动 */
int g_splitDrag;
int g_splitGrabDx;          /* 按下点相对分隔条左沿的偏移 */
int g_splitLastX = -1;      /* 上次绘制的分隔条位置       */
/* 横分隔条（字段区 ↔ 口径预览）：与竖条同一套做法，见下方 WM_LBUTTONDOWN */
int g_hsplitDrag;
int g_hsplitGrabDy;         /* 按下点相对横条上沿的偏移   */
int g_hsplitLastY = -1;     /* 上次绘制的横条位置         */

  /* kind: 0=分组 1=模板 */

const wchar_t CLS_MAIN[]  = L"QuickPhraseMain";
const wchar_t CLS_HOST[]  = L"QuickPhraseFieldHost";
const wchar_t CLS_POPUP[] = L"QuickPhraseMultiPopup";
const wchar_t CLS_TABS[]  = L"QuickPhraseSessionTabs";

void ui_layout(HWND hwnd);
void ui_build_fields(void);
void ui_clear_fields(void);
int ui_select_template(Template *t);
void popup_close(void);
void tabs_refresh(void);
