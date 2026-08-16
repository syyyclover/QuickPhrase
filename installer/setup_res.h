/* setup_res.h —— 安装程序 / 卸载程序共用的资源与常量定义 */
#ifndef QP_SETUP_RES_H
#define QP_SETUP_RES_H

#define IDI_SETUP        101

/* 内嵌的负载（RT_RCDATA） */
#define IDR_APP_EXE      1000
#define IDR_UNINST_EXE   1001

/* 控件 */
#define IDC_PATH         2001
#define IDC_BROWSE       2002
#define IDC_CHK_DESKTOP  2003
#define IDC_CHK_STARTMNU 2004
#define IDC_CHK_AUTORUN  2005
#define IDC_CHK_LAUNCH   2006
#define IDC_PROGRESS     2007
#define IDC_STATUS       2008
#define IDC_INSTALL      2009
#define IDC_CANCEL       2010

/* 注册表位置（仅 HKCU，无需管理员权限） */
#define QP_REG_APP    L"Software\\QuickPhrase"
#define QP_REG_UNINST L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\QuickPhrase"
#define QP_REG_RUN    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

#define QP_PRODUCT   L"口径生成器 QuickPhrase"
#define QP_SHORT     L"口径生成器"
#define QP_VERSION   L"1.2.0"
#define QP_EXE_NAME  L"QuickPhrase.exe"
#define QP_UNI_NAME  L"uninstall.exe"
#define QP_EXIT_MSG  L"QuickPhrase.RequestExit"

#endif /* QP_SETUP_RES_H */
