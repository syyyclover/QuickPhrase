/*
 * main.c —— 程序入口：单实例、托盘图标、全局热键、主题、数据加载与保存
 */
#include "ui.h"
#include "launch.h"
#include "crash.h"
#include "resource.h"
#include "table.h"

#include <shellapi.h>
#include <shlobj.h>
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

static int create_brushes(void)
{
    HBRUSH background = CreateSolidBrush(theme_bk());
    HBRUSH control = CreateSolidBrush(theme_ctl_bk());
    if (!background || !control) {
        if (background) DeleteObject(background);
        if (control) DeleteObject(control);
        return 0;
    }
    if (g_brushBk) DeleteObject(g_brushBk);
    if (g_brushCtl) DeleteObject(g_brushCtl);
    g_brushBk = background;
    g_brushCtl = control;
    return 1;
}

int app_apply_theme(void)
{
    int want = (g_settings.theme == THEME_DARK) ? 1
             : (g_settings.theme == THEME_LIGHT) ? 0
             : app_system_dark();
    int old = g_dark;
    if (old == want && g_brushBk && g_brushCtl) return 1;
    g_dark = want;
    if (!create_brushes()) { g_dark = old; return 0; }
    if (g_hwndMain) ui_theme_changed();
    return 1;
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
            _snwprintf(tip, 128, L"%ls — %ls 打开", QP_APP_TITLE, hk);
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
    /* A hidden window may be shown at identical geometry (no WM_SIZE). */
    RedrawWindow(g_hwndMain, NULL, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
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
    if (!app_save_data() || !app_save_settings()) {
        s_exiting = 0;
        app_show_main();
        return;
    }
    app_unregister_hotkey();
    app_tray_remove();
    if (g_hwndMain) DestroyWindow(g_hwndMain);
}

/* ================================================================== */
/* 保存                                                                */
/* ================================================================== */

int app_save_data(void)
{
    if (g_app && storage_save(g_app)) return 1;
    qp_msgbox(g_hwndMain, MB_OK | MB_ICONERROR, L"保存失败",
              L"无法保存模板数据，请检查数据目录权限和磁盘空间。当前编辑内容仍保留在内存中。");
    return 0;
}

int app_save_settings(void)
{
    if (settings_save(&g_settings)) return 1;
    qp_msgbox(g_hwndMain, MB_OK | MB_ICONERROR, L"保存失败",
              L"无法保存设置，请检查配置目录权限和磁盘空间。");
    return 0;
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
    QpAutostartSnapshot saved = {0};
    if (!qp_autostart_capture(&saved)) return 0;
    int enabled = saved.enabled;
    qp_autostart_dispose(&saved);
    return enabled;
}

void qp_autostart_dispose(QpAutostartSnapshot *saved)
{
    if (!saved) return;
    free(saved->data);
    memset(saved, 0, sizeof(*saved));
}

int qp_autostart_capture(QpAutostartSnapshot *out)
{
    HKEY key;
    LONG status;
    DWORD size = 0, type = 0;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    status = RegOpenKeyExW(HKEY_CURRENT_USER, QP_RUN_KEY, 0, KEY_QUERY_VALUE, &key);
    if (status == ERROR_FILE_NOT_FOUND) return 1;
    if (status != ERROR_SUCCESS) return 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        status = RegQueryValueExW(key, QP_APP_NAME, NULL, &type, NULL, &size);
        if (status == ERROR_FILE_NOT_FOUND) { RegCloseKey(key); return 1; }
        if (status != ERROR_SUCCESS || size > 65536u) break;
        BYTE *data = calloc((size_t)size + sizeof(wchar_t), 1);
        if (!data) break;
        DWORD capacity = size;
        status = RegQueryValueExW(key, QP_APP_NAME, NULL, &type, data, &capacity);
        if (status == ERROR_SUCCESS) {
            out->data = data; out->size = capacity; out->type = type; out->present = 1;
            if ((type == REG_SZ || type == REG_EXPAND_SZ) && capacity % sizeof(wchar_t) == 0) {
                wchar_t exe[32768], expanded[32768];
                const wchar_t *command = (const wchar_t *)data;
                if (type == REG_EXPAND_SZ) {
                    DWORD n = ExpandEnvironmentStringsW(command, expanded, 32768);
                    command = n && n <= 32768 ? expanded : L"";
                }
                while (*command == L' ' || *command == L'\t') ++command;
                const wchar_t *end;
                if (*command == L'"') { ++command; end = wcschr(command, L'"'); }
                else end = command + wcscspn(command, L" \t");
                if (end && exe_path(exe, 32768))
                    out->enabled = (size_t)(end - command) == wcslen(exe) &&
                        _wcsnicmp(command, exe, (size_t)(end - command)) == 0;
            }
            RegCloseKey(key);
            return 1;
        }
        free(data);
        if (status != ERROR_MORE_DATA) break;
    }
    RegCloseKey(key);
    return 0;
}

int qp_autostart_restore(const QpAutostartSnapshot *saved)
{
    HKEY key;
    LONG status;
    if (!saved || saved->size > 65536u || (saved->size && !saved->data)) return 0;
    status = RegCreateKeyExW(HKEY_CURRENT_USER, QP_RUN_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL);
    if (status != ERROR_SUCCESS) return 0;
    if (saved->present)
        status = RegSetValueExW(key, QP_APP_NAME, 0, saved->type, saved->data, (DWORD)saved->size);
    else status = RegDeleteValueW(key, QP_APP_NAME);
    RegCloseKey(key);
    return status == ERROR_SUCCESS || (!saved->present && status == ERROR_FILE_NOT_FOUND);
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
    int show_main;

    (void)hPrev;
    (void)nShow;
    g_hInst = hInst;

    /* 最早装上崩溃钩子：连单实例判断、配置读取阶段出事也要留下现场。
       此时数据目录还没确定，日志先落在默认目录；下面 set_data_dir 之后会再调一次
       把路径改到用户自定义的数据目录。 */
    qp_crash_init();

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
    qp_crash_init();                                /* 日志改到真正的数据目录 */
    g_settings.autostart = app_get_autostart();

    g_app = storage_load(NULL);
    if (!g_app) {
        MessageBoxW(NULL, L"无法读取模板数据，文件可能损坏或无法访问。原文件已保留，请检查数据目录后重试。", QP_APP_TITLE,
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    /* 数据表的引用清单挂在 AppData 上；引擎查表、摘要、体检都从这里找表 */
    qp_table_set_refs(g_app->tables);

    g_dark = (g_settings.theme == THEME_DARK) ? 1
           : (g_settings.theme == THEME_LIGHT) ? 0 : app_system_dark();
    if (!create_brushes()) {
        MessageBoxW(NULL,L"无法创建界面绘制资源，程序未启动。请关闭不需要的窗口后重试。",QP_APP_TITLE,MB_OK|MB_ICONERROR);
        qp_table_set_refs(NULL);
        appdata_free(g_app);g_app=NULL;
        if(g_fontUI)DeleteObject(g_fontUI);
        if(g_fontBold)DeleteObject(g_fontBold);
        if(g_fontMono)DeleteObject(g_fontMono);
        if(s_mutex)CloseHandle(s_mutex);
        return 1;
    }

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
    {
        wchar_t exe[QP_PATH_MAX];
        DWORD n=GetModuleFileNameW(NULL,exe,QP_PATH_MAX);
        if(n && n<QP_PATH_MAX) SHChangeNotify(SHCNE_UPDATEITEM,SHCNF_PATHW,exe,NULL);
    }
    if (g_settings.hotkey_vk && !app_register_hotkey())
        app_tray_tip(L"全局热键未生效",
                     L"无法启用设定的组合键。请在「选项」中尝试其他组合键。");

    show_main = qp_should_show_main(lpCmdLine);
    if (show_main)
        app_show_main();
    if (created && show_main)
        app_tray_tip(QP_APP_TITLE,
                     L"已内置两个示例模板，可直接使用或在「编辑模板」中修改。");

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        /* 记下最近派发的消息：崩了以后日志里能看出当时在处理哪条消息 */
        qp_crash_note_message(msg.hwnd, msg.message, msg.wParam);
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
