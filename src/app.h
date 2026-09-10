/*
 * app.h —— 各 UI 模块共享的全局状态与跨模块函数
 */
#ifndef QP_APP_H
#define QP_APP_H

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>

#include "engine.h"
#include "storage.h"

/* 自定义消息 */
#define WM_QP_TRAY      (WM_APP + 1)
#define WM_QP_REFRESH   (WM_APP + 2)   /* 数据变化后重建模板树 */

/* 定时器 */
#define TIMER_COPY_FEEDBACK  1
#define TIMER_SAVE_PLACEMENT 2

#define QP_APP_NAME    L"QuickPhrase"
#define QP_APP_TITLE   L"口径生成器"
#define QP_APP_VERSION L"3.1.0"

/* ---------------------- 全局状态（定义在 main.c） ------------------ */
extern HINSTANCE g_hInst;
extern HWND      g_hwndMain;
extern AppData  *g_app;
extern Settings  g_settings;
extern int       g_dark;        /* 当前是否深色主题 */
extern int       g_dpi;         /* 主窗口 DPI */
extern HFONT     g_fontUI;
extern HFONT     g_fontBold;
extern HFONT     g_fontMono;
extern HBRUSH    g_brushBk;     /* 当前主题窗口背景 */
extern HBRUSH    g_brushCtl;    /* 当前主题控件背景（输入框） */
extern UINT      g_msgShowWindow; /* RegisterWindowMessage：唤出已运行实例 */
extern UINT      g_msgRequestExit;/* RegisterWindowMessage：卸载程序请求退出 */

#define DPIX(v) MulDiv((v), g_dpi, 96)

/* 主题色 */
#define CLR_DARK_BK      RGB(32, 34, 38)
#define CLR_DARK_CTL     RGB(45, 48, 54)
#define CLR_DARK_TEXT    RGB(228, 230, 234)
#define CLR_DARK_DIM     RGB(150, 156, 165)
#define CLR_DARK_ACCENT  RGB(96, 140, 190)
#define CLR_LIGHT_BK     RGB(246, 247, 249)
#define CLR_LIGHT_CTL    RGB(255, 255, 255)
#define CLR_LIGHT_TEXT   RGB(28, 32, 38)
#define CLR_LIGHT_DIM    RGB(96, 104, 116)
#define CLR_LIGHT_ACCENT RGB(58, 102, 152)

COLORREF theme_bk(void);
COLORREF theme_ctl_bk(void);
COLORREF theme_text(void);
COLORREF theme_dim(void);
COLORREF theme_accent(void);
COLORREF theme_line(void);      /* 分隔条 / 细分隔线（ui.c 实现，编辑器也用） */

/* ---------------------- main.c ------------------------------------ */
void app_tray_add(void);
void app_tray_remove(void);
void app_tray_show_menu(void);
void app_tray_tip(const wchar_t *title, const wchar_t *text);
int  app_register_hotkey(void);
void app_unregister_hotkey(void);
void app_show_main(void);
void app_hide_main(void);
void app_request_exit(void);
void app_save_data(void);
void app_save_settings(void);
int  app_get_autostart(void);
int  app_set_autostart(int enable);
void app_apply_theme(void);          /* 重新计算 g_dark 并刷新界面 */
int  app_system_dark(void);          /* 读注册表 AppsUseLightTheme */

/* ---------------------- ui.c / ui_dialogs.c ----------------------- */
/* 界面相关声明见 ui.h */

#endif /* QP_APP_H */
