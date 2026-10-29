/*
 * installer.c —— 口径生成器 安装程序
 *   把内嵌的 QuickPhrase.exe / uninstall.exe 释放到目标目录，
 *   按需创建桌面与开始菜单快捷方式、开机启动项，并写入卸载信息。
 *   全部只写 HKCU 与用户目录，因此不需要管理员权限。
 */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "setup_res.h"

static HINSTANCE g_inst;
static HWND  g_wnd, g_hPath, g_hBrowse, g_hProg, g_hStatus, g_hInstall, g_hCancel;
static HWND  g_chkDesktop, g_chkStart, g_chkAuto, g_chkLaunch;
static HFONT g_font, g_fontTitle;
static HICON g_icon;
static HBRUSH g_bkBrush;
static int   g_dpi = 96;
static int   g_done;        /* 1 = 安装完成，按钮变“完成” */
static int   g_busy;
static wchar_t g_installDir[MAX_PATH];

#define DPI(v) MulDiv((v), g_dpi, 96)
#define CW 560
#define CH 408

#define CLR_BK    RGB(250, 251, 252)
#define CLR_TEXT  RGB(28, 32, 38)
#define CLR_DIM   RGB(96, 104, 116)
#define CLR_TITLE RGB(40, 74, 118)

/* ============================ 小工具 ============================ */

static void path_join(wchar_t *out, size_t cap, const wchar_t *a, const wchar_t *b)
{
    size_t n;
    if (out != a) {
        out[0] = L'\0';
        wcsncat(out, a, cap - 1);
    }
    n = wcslen(out);
    if (n && out[n - 1] != L'\\' && n + 1 < cap) {
        out[n] = L'\\';
        out[n + 1] = L'\0';
    }
    wcsncat(out, b, cap - wcslen(out) - 1);
}

static int reg_set_str(HKEY root, const wchar_t *sub, const wchar_t *name,
                       const wchar_t *val)
{
    HKEY k;
    LONG r;
    if (RegCreateKeyExW(root, sub, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)
        != ERROR_SUCCESS)
        return 0;
    r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)val,
                       (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static int reg_set_dword(HKEY root, const wchar_t *sub, const wchar_t *name, DWORD val)
{
    HKEY k;
    LONG r;
    if (RegCreateKeyExW(root, sub, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)
        != ERROR_SUCCESS)
        return 0;
    r = RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&val, sizeof(val));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

/* 逐级创建目录 */
static int make_dirs(const wchar_t *path)
{
    wchar_t tmp[MAX_PATH];
    size_t i, n;
    tmp[0] = L'\0';
    wcsncat(tmp, path, MAX_PATH - 1);
    n = wcslen(tmp);
    for (i = 3; i <= n; i++) {
        if (tmp[i] == L'\\' || tmp[i] == L'\0') {
            wchar_t save = tmp[i];
            tmp[i] = L'\0';
            if (GetFileAttributesW(tmp) == INVALID_FILE_ATTRIBUTES)
                CreateDirectoryW(tmp, NULL);
            tmp[i] = save;
        }
    }
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

/* 把内嵌资源写成文件 */
static int extract(int resid, const wchar_t *dest)
{
    HRSRC res = FindResourceW(NULL, MAKEINTRESOURCEW(resid), RT_RCDATA);
    HGLOBAL h;
    const void *p;
    DWORD size, written;
    HANDLE f = INVALID_HANDLE_VALUE;
    wchar_t temp[MAX_PATH];
    int okay;
    if (!res) return 0;
    size = SizeofResource(NULL, res);
    h = LoadResource(NULL, res);
    if (!h || !size) return 0;
    p = LockResource(h);
    if (!p) return 0;
    /* Never truncate an installed binary before the complete replacement is
       safely staged. CREATE_NEW also protects another writer's temporary. */
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        int n = _snwprintf(temp,MAX_PATH,L"%ls.qp-%lu-%u.tmp",dest,
                         GetCurrentProcessId(),attempt);
        temp[MAX_PATH-1]=L'\0';
        if (n < 0 || n >= MAX_PATH) return 0;
        f = CreateFileW(temp,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
        if (f != INVALID_HANDLE_VALUE) break;
        if (GetLastError()!=ERROR_FILE_EXISTS && GetLastError()!=ERROR_ALREADY_EXISTS) return 0;
    }
    if (f == INVALID_HANDLE_VALUE) return 0;
    okay = WriteFile(f,p,size,&written,NULL) && written==size;
    if (okay && !FlushFileBuffers(f)) okay=0;
    if (!CloseHandle(f)) okay=0;
    if (okay && MoveFileExW(temp,dest,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) return 1;
    DeleteFileW(temp);
    return 0;
}

static int make_shortcut(const wchar_t *link, const wchar_t *target,
                         const wchar_t *args, const wchar_t *workdir,
                         const wchar_t *desc)
{
    IShellLinkW *sl = NULL;
    IPersistFile *pf = NULL;
    HRESULT hr;
    int ok = 0;

    hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IShellLinkW, (void **)&sl);
    if (FAILED(hr) || !sl) return 0;
    sl->lpVtbl->SetPath(sl, target);
    if (args && args[0]) sl->lpVtbl->SetArguments(sl, args);
    if (workdir) sl->lpVtbl->SetWorkingDirectory(sl, workdir);
    if (desc) sl->lpVtbl->SetDescription(sl, desc);
    sl->lpVtbl->SetIconLocation(sl, target, 0);
    hr = sl->lpVtbl->QueryInterface(sl, &IID_IPersistFile, (void **)&pf);
    if (SUCCEEDED(hr) && pf) {
        ok = SUCCEEDED(pf->lpVtbl->Save(pf, link, TRUE));
        pf->lpVtbl->Release(pf);
    }
    sl->lpVtbl->Release(sl);
    return ok;
}

static int shell_folder(int csidl, wchar_t *out)
{
    return SUCCEEDED(SHGetFolderPathW(NULL, csidl, NULL, 0, out));
}

static void status(const wchar_t *text, int percent)
{
    SetWindowTextW(g_hStatus, text);
    if (percent >= 0) SendMessageW(g_hProg, PBM_SETPOS, percent, 0);
    UpdateWindow(g_hStatus);
    UpdateWindow(g_hProg);
}

/* 请求已在运行的实例退出，避免覆盖时被占用 */
static void stop_running(void)
{
    UINT m = RegisterWindowMessageW(QP_EXIT_MSG);
    if (m) {
        PostMessageW(HWND_BROADCAST, m, 0, 0);
        Sleep(700);
    }
}

/* ============================ 安装动作 ============================ */

static int do_install(void)
{
    wchar_t exe[MAX_PATH], uni[MAX_PATH], link[MAX_PATH], folder[MAX_PATH];
    wchar_t cmd[MAX_PATH + 32];
    WIN32_FILE_ATTRIBUTE_DATA fad;
    DWORD kb = 0;

    GetWindowTextW(g_hPath, g_installDir, MAX_PATH);
    if (!g_installDir[0]) {
        MessageBoxW(g_wnd, L"请填写安装位置。", QP_PRODUCT, MB_OK | MB_ICONWARNING);
        return 0;
    }

    ShowWindow(g_hProg, SW_SHOW);
    status(L"正在停止已运行的实例…", 5);
    stop_running();

    status(L"正在创建安装目录…", 12);
    if (!make_dirs(g_installDir)) {
        MessageBoxW(g_wnd,
                    L"无法创建安装目录。\n\n"
                    L"本安装程序不申请管理员权限，请换一个当前用户可写的位置"
                    L"（例如默认的 %LOCALAPPDATA%\\Programs\\QuickPhrase）。",
                    QP_PRODUCT, MB_OK | MB_ICONERROR);
        return 0;
    }

    path_join(exe, MAX_PATH, g_installDir, QP_EXE_NAME);
    path_join(uni, MAX_PATH, g_installDir, QP_UNI_NAME);

    status(L"正在释放程序文件…", 30);
    if (!extract(IDR_APP_EXE, exe)) {
        MessageBoxW(g_wnd, L"无法写入 QuickPhrase.exe。请检查安装目录权限、可用空间"
                           L"及文件占用后重试。", QP_PRODUCT, MB_OK | MB_ICONERROR);
        return 0;
    }
    status(L"正在释放卸载程序…", 45);
    if (!extract(IDR_UNINST_EXE, uni)) {
        MessageBoxW(g_wnd, L"写入 uninstall.exe 失败。", QP_PRODUCT,
                    MB_OK | MB_ICONERROR);
        return 0;
    }
    SHChangeNotify(SHCNE_UPDATEITEM,SHCNF_PATHW|SHCNF_FLUSH,exe,NULL);
    SHChangeNotify(SHCNE_UPDATEITEM,SHCNF_PATHW|SHCNF_FLUSH,uni,NULL);

    if (GetFileAttributesExW(exe, GetFileExInfoStandard, &fad))
        kb += (fad.nFileSizeLow + 1023) / 1024;
    if (GetFileAttributesExW(uni, GetFileExInfoStandard, &fad))
        kb += (fad.nFileSizeLow + 1023) / 1024;

    /* 快捷方式 */
    status(L"正在创建快捷方式…", 60);
    reg_set_str(HKEY_CURRENT_USER, QP_REG_APP, L"InstallDir", g_installDir);
    reg_set_str(HKEY_CURRENT_USER, QP_REG_APP, L"Version", QP_VERSION);

    if (SendMessageW(g_chkDesktop, BM_GETCHECK, 0, 0) == BST_CHECKED &&
        shell_folder(CSIDL_DESKTOPDIRECTORY, folder)) {
        wchar_t name[64];
        _snwprintf(name, 64, L"%ls.lnk", QP_SHORT);
        name[63] = L'\0';
        path_join(link, MAX_PATH, folder, name);
        if (make_shortcut(link, exe, L"--show", g_installDir, QP_PRODUCT))
            reg_set_str(HKEY_CURRENT_USER, QP_REG_APP, L"DesktopShortcut", link);
    }
    if (SendMessageW(g_chkStart, BM_GETCHECK, 0, 0) == BST_CHECKED &&
        shell_folder(CSIDL_PROGRAMS, folder)) {
        wchar_t name[64];
        _snwprintf(name, 64, L"%ls.lnk", QP_SHORT);
        name[63] = L'\0';
        path_join(link, MAX_PATH, folder, name);
        if (make_shortcut(link, exe, L"--show", g_installDir, QP_PRODUCT))
            reg_set_str(HKEY_CURRENT_USER, QP_REG_APP, L"StartMenuShortcut", link);
    }

    /* 开机启动 */
    status(L"正在写入注册表…", 78);
    if (SendMessageW(g_chkAuto, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        _snwprintf(cmd, MAX_PATH + 32, L"\"%ls\" --tray", exe);
        cmd[MAX_PATH + 31] = L'\0';
        reg_set_str(HKEY_CURRENT_USER, QP_REG_RUN, L"QuickPhrase", cmd);
    }

    /* 控制面板卸载项 */
    {
        wchar_t s[MAX_PATH + 32];
        reg_set_str(HKEY_CURRENT_USER, QP_REG_UNINST, L"DisplayName", QP_PRODUCT);
        reg_set_str(HKEY_CURRENT_USER, QP_REG_UNINST, L"DisplayVersion", QP_VERSION);
        reg_set_str(HKEY_CURRENT_USER, QP_REG_UNINST, L"Publisher", L"QuickPhrase");
        reg_set_str(HKEY_CURRENT_USER, QP_REG_UNINST, L"InstallLocation", g_installDir);
        _snwprintf(s, MAX_PATH + 32, L"%ls,0", exe); s[MAX_PATH + 31] = L'\0';
        reg_set_str(HKEY_CURRENT_USER, QP_REG_UNINST, L"DisplayIcon", s);
        _snwprintf(s, MAX_PATH + 32, L"\"%ls\"", uni); s[MAX_PATH + 31] = L'\0';
        reg_set_str(HKEY_CURRENT_USER, QP_REG_UNINST, L"UninstallString", s);
        _snwprintf(s, MAX_PATH + 32, L"\"%ls\" /S", uni); s[MAX_PATH + 31] = L'\0';
        reg_set_str(HKEY_CURRENT_USER, QP_REG_UNINST, L"QuietUninstallString", s);
        reg_set_dword(HKEY_CURRENT_USER, QP_REG_UNINST, L"NoModify", 1);
        reg_set_dword(HKEY_CURRENT_USER, QP_REG_UNINST, L"NoRepair", 1);
        reg_set_dword(HKEY_CURRENT_USER, QP_REG_UNINST, L"EstimatedSize", kb);
    }

    status(L"安装完成。", 100);
    return 1;
}

static void finish_and_close(void)
{
    if (SendMessageW(g_chkLaunch, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        wchar_t exe[MAX_PATH];
        path_join(exe, MAX_PATH, g_installDir, QP_EXE_NAME);
        ShellExecuteW(NULL, L"open", exe, L"--show", g_installDir, SW_SHOWNORMAL);
    }
    DestroyWindow(g_wnd);
}

static void browse_dir(void)
{
    BROWSEINFOW bi;
    LPITEMIDLIST pidl;
    wchar_t path[MAX_PATH];
    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = g_wnd;
    bi.lpszTitle = L"选择安装位置";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
    pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    if (SHGetPathFromIDListW(pidl, path)) {
        wchar_t full[MAX_PATH];
        /* 用户选的是父目录时自动补上产品子目录 */
        size_t n = wcslen(path);
        if (n < 11 || _wcsicmp(path + n - 11, L"QuickPhrase") != 0)
            path_join(full, MAX_PATH, path, L"QuickPhrase");
        else {
            full[0] = L'\0';
            wcsncat(full, path, MAX_PATH - 1);
        }
        SetWindowTextW(g_hPath, full);
    }
    CoTaskMemFree(pidl);
}

/* ============================ 界面 ============================ */

static HWND mk(const wchar_t *cls, const wchar_t *text, DWORD style,
               int x, int y, int w, int h, int id, HFONT f)
{
    HWND c = CreateWindowExW(0, cls, text, style | WS_CHILD | WS_VISIBLE,
                             DPI(x), DPI(y), DPI(w), DPI(h), g_wnd,
                             (HMENU)(INT_PTR)id, g_inst, NULL);
    if (c) SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : g_font), TRUE);
    return c;
}

static void build_ui(void)
{
    int y;
    mk(L"Static", QP_PRODUCT, SS_LEFT, 84, 22, CW - 100, 26, 0, g_fontTitle);
    mk(L"Static", L"版本 " QP_VERSION L" · 单文件绿色程序 · 无需管理员权限",
       SS_LEFT, 84, 52, CW - 100, 20, 0, NULL);
    mk(L"Static", L"", SS_ETCHEDHORZ, 0, 84, CW, 1, 0, NULL);

    mk(L"Static", L"安装位置：", SS_LEFT, 20, 104, 100, 18, 0, NULL);
    g_hPath = CreateWindowExW(WS_EX_CLIENTEDGE, L"Edit", g_installDir,
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                              DPI(20), DPI(126), DPI(CW - 40 - 96), DPI(24),
                              g_wnd, (HMENU)(INT_PTR)IDC_PATH, g_inst, NULL);
    SendMessageW(g_hPath, WM_SETFONT, (WPARAM)g_font, TRUE);
    g_hBrowse = mk(L"Button", L"浏览…", BS_PUSHBUTTON | WS_TABSTOP,
                   CW - 20 - 88, 126, 88, 24, IDC_BROWSE, NULL);

    mk(L"Static", L"默认安装在当前用户目录；默认数据目录为 %APPDATA%\\QuickPhrase，可在应用内更改。",
       SS_LEFT | SS_NOPREFIX, 20, 154, CW - 40, 34, 0, NULL);

    y = 190;
    g_chkDesktop = mk(L"Button", L"创建桌面快捷方式",
                      BS_AUTOCHECKBOX | WS_TABSTOP, 20, y, CW - 40, 22,
                      IDC_CHK_DESKTOP, NULL);
    g_chkStart = mk(L"Button", L"添加到开始菜单",
                    BS_AUTOCHECKBOX | WS_TABSTOP, 20, y + 26, CW - 40, 22,
                    IDC_CHK_STARTMNU, NULL);
    g_chkAuto = mk(L"Button", L"开机自动启动（登录后驻留托盘，双击托盘图标打开）",
                   BS_AUTOCHECKBOX | WS_TABSTOP, 20, y + 52, CW - 40, 22,
                   IDC_CHK_AUTORUN, NULL);
    g_chkLaunch = mk(L"Button", L"安装完成后立即运行",
                     BS_AUTOCHECKBOX | WS_TABSTOP, 20, y + 78, CW - 40, 22,
                     IDC_CHK_LAUNCH, NULL);
    SendMessageW(g_chkDesktop, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(g_chkStart, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(g_chkLaunch, BM_SETCHECK, BST_CHECKED, 0);

    mk(L"Static", L"", SS_ETCHEDHORZ, 0, 298, CW, 1, 0, NULL);
    g_hProg = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD,
                              DPI(20), DPI(310), DPI(CW - 40), DPI(14),
                              g_wnd, (HMENU)(INT_PTR)IDC_PROGRESS, g_inst, NULL);
    SendMessageW(g_hProg, PBM_SETRANGE32, 0, 100);
    g_hStatus = mk(L"Static", L"请确认安装位置与选项，然后点击「开始安装」。",
                   SS_LEFT | SS_ENDELLIPSIS, 20, 332, CW - 40, 18, IDC_STATUS, NULL);

    g_hInstall = mk(L"Button", L"开始安装",
                    BS_DEFPUSHBUTTON | WS_TABSTOP, CW - 224, 360, 104, 30,
                    IDC_INSTALL, NULL);
    g_hCancel = mk(L"Button", L"取消", BS_PUSHBUTTON | WS_TABSTOP,
                   CW - 112, 360, 92, 30, IDC_CANCEL, NULL);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_wnd = hwnd;
        build_ui();
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (g_icon)
            DrawIconEx(hdc, DPI(20), DPI(20), g_icon, DPI(48), DPI(48), 0,
                       NULL, DI_NORMAL);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, g_bkBrush);
        return 1;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        SetBkColor(hdc, CLR_BK);
        SetTextColor(hdc, (HWND)lp == g_hStatus ? CLR_DIM : CLR_TEXT);
        return (LRESULT)g_bkBrush;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BROWSE:
            if (!g_busy && !g_done) browse_dir();
            return 0;
        case IDC_INSTALL:
            if (g_busy) return 0;
            if (g_done) { finish_and_close(); return 0; }
            g_busy = 1;
            EnableWindow(g_hInstall, FALSE);
            EnableWindow(g_hCancel, FALSE);
            EnableWindow(g_hPath, FALSE);
            EnableWindow(g_hBrowse, FALSE);
            if (do_install()) {
                g_done = 1;
                SetWindowTextW(g_hInstall, L"完成");
                EnableWindow(g_hInstall, TRUE);
                SetFocus(g_hInstall);
            } else {
                EnableWindow(g_hInstall, TRUE);
                EnableWindow(g_hCancel, TRUE);
                EnableWindow(g_hPath, TRUE);
                EnableWindow(g_hBrowse, TRUE);
                status(L"安装未完成。", 0);
                ShowWindow(g_hProg, SW_HIDE);
            }
            g_busy = 0;
            return 0;
        case IDC_CANCEL:
        case IDCANCEL:
            if (!g_busy) SendMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        default:
            break;
        }
        return 0;

    case WM_CLOSE:
        if (g_busy) return 0;
        if (g_done) { finish_and_close(); return 0; }
        if (MessageBoxW(hwnd, L"确定要退出安装程序吗？", QP_PRODUCT,
                        MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void init_fonts(void)
{
    NONCLIENTMETRICSW ncm;
    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        LOGFONTW lf = ncm.lfMessageFont;
        lf.lfHeight = -MulDiv(9, g_dpi, 72);
        lf.lfQuality = CLEARTYPE_QUALITY;
        g_font = CreateFontIndirectW(&lf);
        lf.lfHeight = -MulDiv(14, g_dpi, 72);
        lf.lfWeight = FW_SEMIBOLD;
        g_fontTitle = CreateFontIndirectW(&lf);
    }
    if (!g_font) g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    if (!g_fontTitle) g_fontTitle = g_font;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int nShow)
{
    WNDCLASSEXW wc;
    MSG msg;
    RECT rc;
    HDC hdc;
    INITCOMMONCONTROLSEX icc;
    wchar_t base[MAX_PATH];

    (void)hPrev; (void)nShow;
    g_inst = hInst;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    memset(&icc, 0, sizeof(icc));
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);

    hdc = GetDC(NULL);
    if (hdc) {
        int d = GetDeviceCaps(hdc, LOGPIXELSX);
        if (d > 0) g_dpi = d;
        ReleaseDC(NULL, hdc);
    }
    init_fonts();
    g_bkBrush = CreateSolidBrush(CLR_BK);
    g_icon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_SETUP), IMAGE_ICON,
                               DPI(48), DPI(48), LR_DEFAULTCOLOR);

    /* 默认装到 %LOCALAPPDATA%\Programs\QuickPhrase：当前用户可写，无需 UAC */
    if (shell_folder(CSIDL_LOCAL_APPDATA, base)) {
        path_join(g_installDir, MAX_PATH, base, L"Programs\\QuickPhrase");
    } else {
        GetCurrentDirectoryW(MAX_PATH, base);
        path_join(g_installDir, MAX_PATH, base, L"QuickPhrase");
    }

    /* 静默安装：/S 直接按默认选项装完退出 */
    if (cmd && (wcsstr(cmd, L"/S") || wcsstr(cmd, L"--silent"))) {
        wchar_t exe[MAX_PATH], uni[MAX_PATH], folder[MAX_PATH], link[MAX_PATH];
        stop_running();
        if (!make_dirs(g_installDir)) return 1;
        path_join(exe, MAX_PATH, g_installDir, QP_EXE_NAME);
        path_join(uni, MAX_PATH, g_installDir, QP_UNI_NAME);
        if (!extract(IDR_APP_EXE, exe) || !extract(IDR_UNINST_EXE, uni)) return 1;
        reg_set_str(HKEY_CURRENT_USER, QP_REG_APP, L"InstallDir", g_installDir);
        reg_set_str(HKEY_CURRENT_USER, QP_REG_APP, L"Version", QP_VERSION);
        if (shell_folder(CSIDL_PROGRAMS, folder)) {
            wchar_t name[64];
            _snwprintf(name, 64, L"%ls.lnk", QP_SHORT); name[63] = L'\0';
            path_join(link, MAX_PATH, folder, name);
            if (make_shortcut(link, exe, L"--show", g_installDir, QP_PRODUCT))
                reg_set_str(HKEY_CURRENT_USER, QP_REG_APP, L"StartMenuShortcut", link);
        }
        {
            wchar_t s[MAX_PATH + 32];
            reg_set_str(HKEY_CURRENT_USER, QP_REG_UNINST, L"DisplayName", QP_PRODUCT);
            reg_set_str(HKEY_CURRENT_USER, QP_REG_UNINST, L"DisplayVersion", QP_VERSION);
            reg_set_str(HKEY_CURRENT_USER, QP_REG_UNINST, L"Publisher", L"QuickPhrase");
            reg_set_str(HKEY_CURRENT_USER, QP_REG_UNINST, L"InstallLocation", g_installDir);
            _snwprintf(s, MAX_PATH + 32, L"%ls,0", exe); s[MAX_PATH + 31] = L'\0';
            reg_set_str(HKEY_CURRENT_USER, QP_REG_UNINST, L"DisplayIcon", s);
            _snwprintf(s, MAX_PATH + 32, L"\"%ls\"", uni); s[MAX_PATH + 31] = L'\0';
            reg_set_str(HKEY_CURRENT_USER, QP_REG_UNINST, L"UninstallString", s);
            _snwprintf(s, MAX_PATH + 32, L"\"%ls\" /S", uni); s[MAX_PATH + 31] = L'\0';
            reg_set_str(HKEY_CURRENT_USER, QP_REG_UNINST, L"QuietUninstallString", s);
            reg_set_dword(HKEY_CURRENT_USER, QP_REG_UNINST, L"NoModify", 1);
            reg_set_dword(HKEY_CURRENT_USER, QP_REG_UNINST, L"NoRepair", 1);
        }
        CoUninitialize();
        return 0;
    }

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = g_bkBrush;
    wc.lpszClassName = L"QuickPhraseSetup";
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_SETUP), IMAGE_ICON,
                                 0, 0, LR_DEFAULTSIZE);
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_SETUP), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    if (!RegisterClassExW(&wc)) return 1;

    rc.left = 0; rc.top = 0; rc.right = DPI(CW); rc.bottom = DPI(CH);
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0);
    {
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
        int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 3;
        HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_APPWINDOW,
                                    wc.lpszClassName, L"口径生成器 安装程序",
                                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                                    WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                                    x, y, w, h, NULL, NULL, hInst, NULL);
        if (!hwnd) return 1;
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(g_wnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}
