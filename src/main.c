/*
 * main.c —— 程序入口：单实例、托盘图标、全局热键、主题、数据加载与保存
 */
#include "ui.h"
#include "resource.h"

#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------- 全局状态 ---------------------------- */
HINSTANCE g_hInst;
HWND      g_hwndMain;
AppData  *g_app;
Settings  g_settings;
int       g_dark;
int       g_dpi = 96;
HFONT     g_fontUI, g_fontBold, g_fontMono;
HBRUSH    g_brushBk, g_brushCtl;
UINT      g_msgShowWindow;
UINT      g_msgRequestExit;

static HACCEL s_accel;
static HANDLE s_mutex;
static int    s_trayAdded;
static int    s_hotkeyOn;
static int    s_exiting;

#define QP_TRAY_ID   1
#define QP_HOTKEY_ID 1
#define QP_MUTEX_NAME L"QuickPhrase.SingleInstance.Mutex"
#define QP_SHOW_MSG   L"QuickPhrase.ShowMainWindow"
#define QP_EXIT_MSG   L"QuickPhrase.RequestExit"
#define QP_RUN_KEY    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

/* ================================================================== */
/* 字体与主题                                                          */
/* ================================================================== */

static HFONT make_font(const wchar_t *face, int pt, int weight)
{
    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -MulDiv(pt, g_dpi, 72);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfOutPrecision = OUT_TT_PRECIS;
    qp_strlcpy(lf.lfFaceName, face, LF_FACESIZE);
    return CreateFontIndirectW(&lf);
}

static void create_fonts(void)
{
    NONCLIENTMETRICSW ncm;
    if (g_fontUI) DeleteObject(g_fontUI);
    if (g_fontBold) DeleteObject(g_fontBold);
    if (g_fontMono) DeleteObject(g_fontMono);

    /* 优先跟随系统界面字体，取不到再退回微软雅黑 */
    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        LOGFONTW lf = ncm.lfMessageFont;
        lf.lfHeight = -MulDiv(9, g_dpi, 72);
        lf.lfQuality = CLEARTYPE_QUALITY;
        g_fontUI = CreateFontIndirectW(&lf);
        lf.lfWeight = FW_BOLD;
        lf.lfHeight = -MulDiv(10, g_dpi, 72);
        g_fontBold = CreateFontIndirectW(&lf);
    }
    if (!g_fontUI) g_fontUI = make_font(L"Microsoft YaHei UI", 9, FW_NORMAL);
    if (!g_fontBold) g_fontBold = make_font(L"Microsoft YaHei UI", 10, FW_BOLD);
    /* 预览区使用等宽字体，便于对齐编号、括号 */
    g_fontMono = make_font(L"Consolas", 10, FW_NORMAL);
    if (!g_fontMono) g_fontMono = make_font(L"Courier New", 10, FW_NORMAL);
}

int app_system_dark(void)
{
    HKEY key;
    DWORD val = 1, sz = sizeof(val), type = REG_DWORD;
    int dark = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        if (RegQueryValueExW(key, L"AppsUseLightTheme", NULL, &type,
                             (BYTE *)&val, &sz) == ERROR_SUCCESS)
            dark = (val == 0);
        RegCloseKey(key);
    }
    return dark;
}

static void create_brushes(void)
{
    if (g_brushBk) DeleteObject(g_brushBk);
    if (g_brushCtl) DeleteObject(g_brushCtl);
    g_brushBk = CreateSolidBrush(theme_bk());
    g_brushCtl = CreateSolidBrush(theme_ctl_bk());
}

void app_apply_theme(void)
{
    int want = (g_settings.theme == THEME_DARK) ? 1
             : (g_settings.theme == THEME_LIGHT) ? 0
             : app_system_dark();
    g_dark = want;
    create_brushes();
    if (g_hwndMain) ui_theme_changed();
}

/* ================================================================== */
/* 托盘                                                                */
/* ================================================================== */

static void tray_fill(NOTIFYICONDATAW *nid)
{
    memset(nid, 0, sizeof(*nid));
    nid->cbSize = sizeof(*nid);
    nid->hWnd = g_hwndMain;
    nid->uID = QP_TRAY_ID;
}

void app_tray_add(void)
{
    NOTIFYICONDATAW nid;
    wchar_t tip[128];
    if (s_trayAdded || !g_hwndMain) return;
    tray_fill(&nid);
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_QP_TRAY;
    nid.hIcon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXSMICON),
                                  GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!nid.hIcon) nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    {
        wchar_t hk[64];
        hk[0] = L'\0';
        if (g_settings.hotkey_vk) {
            if (g_settings.hotkey_mods & MOD_CONTROL) wcscat(hk, L"Ctrl+");
            if (g_settings.hotkey_mods & MOD_ALT) wcscat(hk, L"Alt+");
            if (g_settings.hotkey_mods & MOD_SHIFT) wcscat(hk, L"Shift+");
            if (g_settings.hotkey_mods & MOD_WIN) wcscat(hk, L"Win+");
            {
                wchar_t k[32];
                UINT sc = MapVirtualKeyW(g_settings.hotkey_vk, MAPVK_VK_TO_VSC);
                if (GetKeyNameTextW((LONG)(sc << 16), k, 32) > 0) wcscat(hk, k);
            }
        }
        if (hk[0])
            _snwprintf(tip, 128, L"%ls — %ls 唤出", QP_APP_TITLE, hk);
        else
            _snwprintf(tip, 128, L"%ls", QP_APP_TITLE);
        tip[127] = L'\0';
    }
    qp_strlcpy(nid.szTip, tip, sizeof(nid.szTip) / sizeof(wchar_t));
    if (Shell_NotifyIconW(NIM_ADD, &nid)) s_trayAdded = 1;
}

void app_tray_remove(void)
{
    NOTIFYICONDATAW nid;
    if (!s_trayAdded) return;
    tray_fill(&nid);
    Shell_NotifyIconW(NIM_DELETE, &nid);
    s_trayAdded = 0;
}

void app_tray_tip(const wchar_t *title, const wchar_t *text)
{
    NOTIFYICONDATAW nid;
    if (!s_trayAdded) return;
    tray_fill(&nid);
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    qp_strlcpy(nid.szInfoTitle, title ? title : QP_APP_TITLE,
               sizeof(nid.szInfoTitle) / sizeof(wchar_t));
    qp_strlcpy(nid.szInfo, text ? text : L"",
               sizeof(nid.szInfo) / sizeof(wchar_t));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void app_tray_show_menu(void)
{
    HMENU m = CreatePopupMenu();
    POINT pt;
    if (!m) return;
    AppendMenuW(m, MF_STRING, IDM_TRAY_OPEN, L"打开主界面(&O)");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_TRAY_SETTINGS, L"选项(&S)…");
    AppendMenuW(m, MF_STRING | (app_get_autostart() ? MF_CHECKED : 0),
                IDM_TRAY_AUTOSTART, L"开机自启动(&A)");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_TRAY_EXIT, L"退出(&X)");
    SetMenuDefaultItem(m, IDM_TRAY_OPEN, FALSE);

    GetCursorPos(&pt);
    /* 让菜单能正常获得焦点并在点击外部时收起 */
    SetForegroundWindow(g_hwndMain);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RIGHTALIGN | TPM_BOTTOMALIGN,
                   pt.x, pt.y, 0, g_hwndMain, NULL);
    PostMessageW(g_hwndMain, WM_NULL, 0, 0);
    DestroyMenu(m);
}

/* ================================================================== */
/* 热键                                                                */
/* ================================================================== */

int app_register_hotkey(void)
{
    if (!g_hwndMain || !g_settings.hotkey_vk) return 0;
    app_unregister_hotkey();
    if (RegisterHotKey(g_hwndMain, QP_HOTKEY_ID,
                       g_settings.hotkey_mods | MOD_NOREPEAT, g_settings.hotkey_vk)) {
        s_hotkeyOn = 1;
        if (s_trayAdded) { app_tray_remove(); app_tray_add(); }
        return 1;
    }
    /* 某些系统不支持 MOD_NOREPEAT，退一步再试 */
    if (RegisterHotKey(g_hwndMain, QP_HOTKEY_ID,
                       g_settings.hotkey_mods, g_settings.hotkey_vk)) {
        s_hotkeyOn = 1;
        if (s_trayAdded) { app_tray_remove(); app_tray_add(); }
        return 1;
    }
    return 0;
}

void app_unregister_hotkey(void)
{
    if (s_hotkeyOn && g_hwndMain) UnregisterHotKey(g_hwndMain, QP_HOTKEY_ID);
    s_hotkeyOn = 0;
}

/* ================================================================== */
/* 显示 / 隐藏 / 退出                                                  */
/* ================================================================== */

void app_show_main(void)
{
    if (!g_hwndMain) return;
    if (!IsWindowVisible(g_hwndMain))
        ShowWindow(g_hwndMain, g_settings.win_maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    else if (IsIconic(g_hwndMain))
        ShowWindow(g_hwndMain, SW_RESTORE);
    BringWindowToTop(g_hwndMain);
    SetForegroundWindow(g_hwndMain);
    SetActiveWindow(g_hwndMain);
}

void app_hide_main(void)
{
    if (!g_hwndMain) return;
    ShowWindow(g_hwndMain, SW_HIDE);
    if (!g_settings.minimize_to_tray_hint) {
        g_settings.minimize_to_tray_hint = 1;
        app_save_settings();
        app_tray_tip(QP_APP_TITLE, L"程序仍在后台运行，双击托盘图标或按全局热键可再次唤出。");
    }
}

void app_request_exit(void)
{
    if (s_exiting) return;
    s_exiting = 1;
    app_save_data();
    app_save_settings();
    app_unregister_hotkey();
    app_tray_remove();
    if (g_hwndMain) DestroyWindow(g_hwndMain);
}

/* ================================================================== */
/* 保存                                                                */
/* ================================================================== */

void app_save_data(void)
{
    if (g_app) storage_save(g_app);
}

void app_save_settings(void)
{
    settings_save(&g_settings);
}

/* ================================================================== */
/* 开机自启动                                                          */
/* ================================================================== */

static int exe_path(wchar_t *out, size_t cap)
{
    DWORD n = GetModuleFileNameW(NULL, out, (DWORD)cap);
    return n > 0 && n < cap;
}

int app_get_autostart(void)
{
    HKEY key;
    wchar_t buf[MAX_PATH * 2];
    DWORD sz = sizeof(buf), type = REG_SZ;
    int on = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, QP_RUN_KEY, 0, KEY_QUERY_VALUE, &key)
        != ERROR_SUCCESS)
        return 0;
    if (RegQueryValueExW(key, QP_APP_NAME, NULL, &type, (BYTE *)buf, &sz) == ERROR_SUCCESS)
        on = 1;
    RegCloseKey(key);
    return on;
}

int app_set_autostart(int enable)
{
    HKEY key;
    LONG r;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, QP_RUN_KEY, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS)
        return 0;
    if (enable) {
        wchar_t exe[MAX_PATH], cmd[MAX_PATH + 32];
        if (!exe_path(exe, MAX_PATH)) { RegCloseKey(key); return 0; }
        _snwprintf(cmd, MAX_PATH + 32, L"\"%ls\" --tray", exe);
        cmd[MAX_PATH + 31] = L'\0';
        r = RegSetValueExW(key, QP_APP_NAME, 0, REG_SZ, (const BYTE *)cmd,
                           (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
    } else {
        r = RegDeleteValueW(key, QP_APP_NAME);
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

/* ================================================================== */
/* 入口                                                                */
/* ================================================================== */

static int cmdline_has(const wchar_t *cmd, const wchar_t *flag)
{
    return cmd && flag && qp_istr(cmd, flag) != NULL;
}

static void init_dpi(void)
{
    HDC hdc = GetDC(NULL);
    if (hdc) {
        int d = GetDeviceCaps(hdc, LOGPIXELSX);
        if (d > 0) g_dpi = d;
        ReleaseDC(NULL, hdc);
    }
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmdLine, int nShow)
{
    INITCOMMONCONTROLSEX icc;
    MSG msg;
    int created = 0;
    int want_show, force_tray;

    (void)hPrev;
    (void)nShow;
    g_hInst = hInst;

    g_msgShowWindow = RegisterWindowMessageW(QP_SHOW_MSG);
    g_msgRequestExit = RegisterWindowMessageW(QP_EXIT_MSG);

    /* 单实例：已有实例时唤出它的窗口后退出 */
    s_mutex = CreateMutexW(NULL, FALSE, QP_MUTEX_NAME);
    if (s_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_msgShowWindow) PostMessageW(HWND_BROADCAST, g_msgShowWindow, 0, 0);
        CloseHandle(s_mutex);
        return 0;
    }

    memset(&icc, 0, sizeof(icc));
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES |
                ICC_BAR_CLASSES | ICC_TAB_CLASSES;
    InitCommonControlsEx(&icc);

    init_dpi();
    create_fonts();

    storage_ensure_dir();
    settings_defaults(&g_settings);
    if (!settings_load(&g_settings)) created = 1;   /* 首次运行 */
    storage_set_data_dir(g_settings.data_dir);      /* 应用自定义数据目录 */
    g_settings.autostart = app_get_autostart();

    g_app = storage_load(NULL);
    if (!g_app) g_app = appdata_create_seed();
    if (!g_app) {
        MessageBoxW(NULL, L"无法初始化数据，程序退出。", QP_APP_TITLE,
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    g_dark = (g_settings.theme == THEME_DARK) ? 1
           : (g_settings.theme == THEME_LIGHT) ? 0 : app_system_dark();
    create_brushes();

    if (!ui_register_classes()) {
        MessageBoxW(NULL, L"窗口类注册失败。", QP_APP_TITLE, MB_OK | MB_ICONERROR);
        return 1;
    }
    g_hwndMain = ui_create_main_window();
    if (!g_hwndMain) {
        MessageBoxW(NULL, L"主窗口创建失败。", QP_APP_TITLE, MB_OK | MB_ICONERROR);
        return 1;
    }
    s_accel = ui_create_accelerators();

    ui_theme_changed();
    ui_rebuild_tree(NULL);

    app_tray_add();
    if (g_settings.hotkey_vk && !app_register_hotkey())
        app_tray_tip(L"全局热键未生效",
                     L"设定的组合键已被其他程序占用，可在「选项」里换一个。");

    force_tray = cmdline_has(lpCmdLine, L"--tray") || cmdline_has(lpCmdLine, L"/tray");
    want_show = cmdline_has(lpCmdLine, L"--show") || cmdline_has(lpCmdLine, L"/show");
    if (!force_tray && (want_show || created)) {
        app_show_main();
        if (created)
            app_tray_tip(QP_APP_TITLE,
                         L"已内置两个示例模板，可直接使用或在「编辑模板」中修改。");
    } else {
        /* 按设计常驻托盘：首次这样启动时提示一次，避免用户以为没启动 */
        if (!g_settings.minimize_to_tray_hint) {
            g_settings.minimize_to_tray_hint = 1;
            app_save_settings();
            app_tray_tip(QP_APP_TITLE,
                         L"已在后台运行：双击托盘图标或按全局热键即可唤出。");
        }
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (g_hwndMain && (msg.hwnd == g_hwndMain || IsChild(g_hwndMain, msg.hwnd))) {
            if (s_accel && TranslateAcceleratorW(g_hwndMain, s_accel, &msg)) continue;
            if (IsDialogMessageW(g_hwndMain, &msg)) continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    app_unregister_hotkey();
    app_tray_remove();
    if (s_accel) DestroyAcceleratorTable(s_accel);
    if (g_fontUI) DeleteObject(g_fontUI);
    if (g_fontBold) DeleteObject(g_fontBold);
    if (g_fontMono) DeleteObject(g_fontMono);
    if (g_brushBk) DeleteObject(g_brushBk);
    if (g_brushCtl) DeleteObject(g_brushCtl);
    appdata_free(g_app);
    g_app = NULL;
    if (s_mutex) CloseHandle(s_mutex);
    return (int)msg.wParam;
}
