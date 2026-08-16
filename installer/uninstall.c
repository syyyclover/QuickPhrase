/*
 * uninstall.c —— 口径生成器 卸载程序
 *   两阶段执行：第一阶段确认并把自身复制到 %TEMP%，
 *   第二阶段从 %TEMP% 运行，删除安装目录（含自己原来的位置）、
 *   快捷方式、启动项与注册表信息，可选删除用户数据。
 */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "setup_res.h"

#define STAGE2_FLAG L"--stage2"

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

static int reg_get_str(const wchar_t *sub, const wchar_t *name, wchar_t *out, DWORD cap)
{
    HKEY k;
    DWORD sz = cap * sizeof(wchar_t), type = REG_SZ;
    int ok = 0;
    out[0] = L'\0';
    if (RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return 0;
    if (RegQueryValueExW(k, name, NULL, &type, (BYTE *)out, &sz) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        out[cap - 1] = L'\0';
        ok = out[0] != L'\0';
    }
    RegCloseKey(k);
    return ok;
}

static void reg_del_value(const wchar_t *sub, const wchar_t *name)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, name);
        RegCloseKey(k);
    }
}

/* 删除目录下所有文件（不递归子目录，本程序不产生子目录）并移除目录本身 */
static int wipe_dir(const wchar_t *dir)
{
    WIN32_FIND_DATAW fd;
    wchar_t pattern[MAX_PATH], item[MAX_PATH];
    HANDLE h;
    path_join(pattern, MAX_PATH, dir, L"*");
    h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            path_join(item, MAX_PATH, dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                wipe_dir(item);
            } else {
                SetFileAttributesW(item, FILE_ATTRIBUTE_NORMAL);
                if (!DeleteFileW(item)) {
                    Sleep(300);
                    DeleteFileW(item);
                }
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(dir) != 0;
}

static void stop_running(void)
{
    UINT m = RegisterWindowMessageW(QP_EXIT_MSG);
    if (m) {
        PostMessageW(HWND_BROADCAST, m, 0, 0);
        Sleep(900);
    }
}

/* 让自己在退出后被删除 */
static void self_delete_later(void)
{
    wchar_t self[MAX_PATH], cmd[MAX_PATH + 96], sys[MAX_PATH];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    if (!GetModuleFileNameW(NULL, self, MAX_PATH)) return;
    if (!GetSystemDirectoryW(sys, MAX_PATH)) return;
    path_join(sys, MAX_PATH, sys, L"cmd.exe");
    _snwprintf(cmd, MAX_PATH + 96,
               L"\"%ls\" /c ping -n 3 127.0.0.1 >nul & del /f /q \"%ls\"", sys, self);
    cmd[MAX_PATH + 95] = L'\0';
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW | DETACHED_PROCESS, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

/* 第二阶段：真正的删除动作 */
static int stage2(const wchar_t *dir, int silent, int purge)
{
    wchar_t buf[MAX_PATH], appdata[MAX_PATH];
    int failed = 0;

    Sleep(900);   /* 等第一阶段进程与主程序退出 */
    stop_running();

    /* 快捷方式 */
    if (reg_get_str(QP_REG_APP, L"DesktopShortcut", buf, MAX_PATH)) DeleteFileW(buf);
    if (reg_get_str(QP_REG_APP, L"StartMenuShortcut", buf, MAX_PATH)) DeleteFileW(buf);

    /* 开机启动项 */
    reg_del_value(QP_REG_RUN, L"QuickPhrase");

    /* 程序文件 */
    if (dir && dir[0] && GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES) {
        int tries;
        for (tries = 0; tries < 3; tries++) {
            if (wipe_dir(dir)) break;
            Sleep(500);
        }
        if (GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES) failed = 1;
    }

    /* 用户数据 */
    if (purge && SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appdata))) {
        path_join(buf, MAX_PATH, appdata, L"QuickPhrase");
        if (GetFileAttributesW(buf) != INVALID_FILE_ATTRIBUTES) wipe_dir(buf);
    }

    /* 注册表 */
    RegDeleteKeyW(HKEY_CURRENT_USER, QP_REG_UNINST);
    RegDeleteKeyW(HKEY_CURRENT_USER, QP_REG_APP);

    if (!silent) {
        if (failed)
            MessageBoxW(NULL,
                        L"卸载基本完成，但安装目录中仍有文件无法删除"
                        L"（可能正被占用）。\n请稍后手动删除该目录。",
                        QP_PRODUCT, MB_OK | MB_ICONWARNING);
        else
            MessageBoxW(NULL, L"口径生成器已卸载完成。", QP_PRODUCT,
                        MB_OK | MB_ICONINFORMATION);
    }
    self_delete_later();
    return failed;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmdline, int nShow)
{
    int argc = 0, i;
    LPWSTR *argv;
    int silent = 0, purge = 0, is_stage2 = 0;
    wchar_t dir[MAX_PATH] = L"";
    wchar_t self[MAX_PATH], tmpdir[MAX_PATH], tmpexe[MAX_PATH];
    wchar_t cmd[MAX_PATH * 2 + 64];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    (void)hInst; (void)hPrev; (void)nShow;

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (i = 1; argv && i < argc; i++) {
        if (!_wcsicmp(argv[i], STAGE2_FLAG)) {
            is_stage2 = 1;
            if (i + 1 < argc) {
                dir[0] = L'\0';
                wcsncat(dir, argv[++i], MAX_PATH - 1);
            }
        } else if (!_wcsicmp(argv[i], L"/S") || !_wcsicmp(argv[i], L"--silent")) {
            silent = 1;
        } else if (!_wcsicmp(argv[i], L"/PURGE")) {
            purge = 1;
        }
    }
    if (argv) LocalFree(argv);

    if (is_stage2)
        return stage2(dir, silent, purge);

    /* ---------------- 第一阶段 ---------------- */
    if (!reg_get_str(QP_REG_APP, L"InstallDir", dir, MAX_PATH)) {
        /* 注册表里没有就用自身所在目录 */
        if (GetModuleFileNameW(NULL, dir, MAX_PATH)) {
            wchar_t *p = wcsrchr(dir, L'\\');
            if (p) *p = L'\0';
        }
    }

    if (!silent) {
        if (MessageBoxW(NULL,
                        L"确定要卸载「口径生成器 QuickPhrase」吗？",
                        QP_PRODUCT, MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;
        {
            int r = MessageBoxW(NULL,
                               L"是否同时删除模板与设置数据？\n\n"
                               L"「是」：一并删除 %APPDATA%\\QuickPhrase（模板全部丢失）\n"
                               L"「否」：保留数据，重装后模板仍在\n"
                               L"「取消」：不卸载",
                               QP_PRODUCT, MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL) return 0;
            purge = (r == IDYES);
        }
    }

    stop_running();

    /* 把自己复制到 %TEMP% 后再执行删除，否则无法删掉安装目录 */
    if (!GetModuleFileNameW(NULL, self, MAX_PATH)) return 1;
    if (!GetTempPathW(MAX_PATH, tmpdir)) return 1;
    _snwprintf(tmpexe, MAX_PATH, L"%lsqp_uninst_%lu.exe", tmpdir,
               (unsigned long)GetCurrentProcessId());
    tmpexe[MAX_PATH - 1] = L'\0';
    if (!CopyFileW(self, tmpexe, FALSE)) {
        /* 复制失败时退化为原地删除：安装目录会残留 uninstall.exe */
        return stage2(dir, silent, purge);
    }

    _snwprintf(cmd, MAX_PATH * 2 + 64, L"\"%ls\" %ls \"%ls\"%ls%ls",
               tmpexe, STAGE2_FLAG, dir, silent ? L" /S" : L"",
               purge ? L" /PURGE" : L"");
    cmd[MAX_PATH * 2 + 63] = L'\0';

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, tmpdir, &si, &pi)) {
        DeleteFileW(tmpexe);
        return stage2(dir, silent, purge);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
