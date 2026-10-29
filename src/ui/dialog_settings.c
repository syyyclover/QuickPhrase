#include "dialogs_internal.h"
/* ================================================================== */
/* 设置                                                                */
/* ================================================================== */

typedef struct {
    HWND hwnd;
    int result;
    unsigned mods, vk;
    HWND hHotkey, hAuto, hTheme, hSep, hDataDir;
} SetCtx;

/* 更改数据目录：选择新目录 → 迁移数据 → 保存设置 → 重载界面 */
static void change_data_dir(HWND hwnd, SetCtx *c)
{
    BROWSEINFOW bi;
    PIDLIST_ABSOLUTE pidl;
    wchar_t dir[QP_PATH_MAX] = L"", old[QP_PATH_MAX], np[QP_PATH_MAX];
    int has_data;
    AppData *nd = NULL;
    Settings next_settings;

    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = hwnd;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpszTitle = L"选择模板数据目录（存放 data.json，不迁移外部数据表文件）";
    pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    if (!SHGetPathFromIDListW(pidl, dir) || !dir[0]) {
        CoTaskMemFree(pidl);
        return;
    }
    CoTaskMemFree(pidl);

    storage_get_data_dir(old, QP_PATH_MAX);
    if (wcscmp(old, dir) == 0) return;   /* 相同目录，无需操作 */

    /* 临时切到新目录，检查是否已有数据文件 */
    storage_set_data_dir(dir);
    if (!storage_ensure_dir()) {
        storage_set_data_dir(old);
        qp_msgbox(hwnd, MB_OK | MB_ICONERROR, L"切换失败", L"无法访问所选目录。");
        return;
    }
    has_data = storage_get_data_path(np, QP_PATH_MAX) &&
               GetFileAttributesW(np) != INVALID_FILE_ATTRIBUTES;

    if (has_data) {
        char *json = storage_read_text(np);
        nd = json ? appdata_from_json(json) : NULL;
        free(json);
        if (!nd) {
            storage_set_data_dir(old);
            qp_msgbox(hwnd, MB_OK | MB_ICONERROR, L"切换失败",
                      L"所选目录中的数据无法读取或格式无效。当前数据目录未改变。");
            return;
        }
        /* 旧目录可能仍有上次保存失败的编辑，切换前必须先成功持久化。 */
        storage_set_data_dir(old);
        if (!storage_save(g_app)) {
            appdata_free(nd);
            qp_msgbox(hwnd, MB_OK | MB_ICONERROR, L"切换失败",
                      L"当前数据尚未保存，无法切换目录。请检查原目录权限和磁盘空间后重试。");
            return;
        }
        storage_set_data_dir(dir);
    } else {
        /* 新目录没有数据：把当前数据保存过去 */
        if (!storage_save(g_app)) {
            qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, QP_APP_TITLE,
                      L"无法写入所选目录，请检查权限后重试。");
            storage_set_data_dir(old);      /* 还原 */
            return;
        }
    }

    /* 更新设置并保存（settings.json 位于默认目录） */
    next_settings = g_settings;
    qp_strlcpy(next_settings.data_dir, dir, QP_PATH_MAX);
    if (!sess_capture_current()) {
        appdata_free(nd);
        storage_set_data_dir(old);
        qp_msgbox(hwnd, MB_OK | MB_ICONERROR, L"切换失败",
                  L"内存不足，当前输入和数据目录未改变。请关闭不需要的窗口后重试。");
        return;
    }
    if (!settings_save(&next_settings)) {
        appdata_free(nd);
        storage_set_data_dir(old);
        qp_msgbox(hwnd, MB_OK | MB_ICONERROR, L"切换失败",
                  L"无法保存新的数据目录设置。当前数据和目录未改变。");
        return;
    }

    /* 重载数据并刷新界面 */
    if (!ui_reset_selection()) {
        appdata_free(nd);
        storage_set_data_dir(old);
        if (!settings_save(&g_settings))
            qp_msgbox(hwnd, MB_OK | MB_ICONERROR, L"恢复设置失败",
                      L"当前数据仍保留，但未能恢复原数据目录设置。请检查配置目录权限后重试。");
        return;
    }
    g_settings = next_settings;
    {
        if (nd) {
            appdata_free(g_app);
            g_app = nd;
            qp_table_set_refs(g_app->tables);   /* 换目录后重新指向新的表清单 */
            qp_table_invalidate_all();
        }
    }
    ui_rebuild_tree(NULL);

    /* 更新对话框中的路径显示 */
    SetWindowTextW(c->hDataDir, dir);

    qp_msgbox(hwnd, MB_OK | MB_ICONINFORMATION, QP_APP_TITLE,
              has_data ? L"已切换到所选目录，并使用其中的数据。"
                       : L"已把当前数据保存到新目录并切换过去。");
}

static void hotkey_text(unsigned mods, unsigned vk, wchar_t *out, size_t cap)
{
    wchar_t key[64] = L"";
    out[0] = L'\0';
    if (mods & MOD_CONTROL) qp_strlcpy(out + wcslen(out), L"Ctrl + ", cap - wcslen(out));
    if (mods & MOD_ALT)     qp_strlcpy(out + wcslen(out), L"Alt + ", cap - wcslen(out));
    if (mods & MOD_SHIFT)   qp_strlcpy(out + wcslen(out), L"Shift + ", cap - wcslen(out));
    if (mods & MOD_WIN)     qp_strlcpy(out + wcslen(out), L"Win + ", cap - wcslen(out));
    if (vk) {
        UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        LONG lp = (LONG)(sc << 16);
        switch (vk) {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
        case VK_PRIOR:  case VK_NEXT:   case VK_LEFT: case VK_RIGHT:
        case VK_UP:     case VK_DOWN:
            lp |= (1L << 24);       /* 扩展键 */
            break;
        default: break;
        }
        if (GetKeyNameTextW(lp, key, 64) <= 0)
            _snwprintf(key, 64, L"VK_%02X", vk);
        qp_strlcpy(out + wcslen(out), key, cap - wcslen(out));
    } else {
        qp_strlcpy(out + wcslen(out), L"（未设置）", cap - wcslen(out));
    }
}

static LRESULT CALLBACK hotkey_edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                         UINT_PTR id, DWORD_PTR ref)
{
    SetCtx *c = (SetCtx *)ref;
    (void)id;
    switch (msg) {
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    case WM_CHAR:
    case WM_SYSCHAR:
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        unsigned vk = (unsigned)wp;
        unsigned mods = 0;
        wchar_t txt[128];
        if (vk == VK_TAB) break;                       /* 让 Tab 正常切换焦点 */
        if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
        if (GetKeyState(VK_MENU) & 0x8000)    mods |= MOD_ALT;
        if (GetKeyState(VK_SHIFT) & 0x8000)   mods |= MOD_SHIFT;
        if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000))
            mods |= MOD_WIN;
        if (vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT ||
            vk == VK_LWIN || vk == VK_RWIN) {
            hotkey_text(mods, 0, txt, 128);
            SetWindowTextW(hwnd, txt);
            return 0;
        }
        if (vk == VK_BACK || vk == VK_DELETE) {
            c->mods = 0; c->vk = 0;
            SetWindowTextW(hwnd, L"（未设置）");
            return 0;
        }
        c->mods = mods;
        c->vk = vk;
        hotkey_text(mods, vk, txt, 128);
        SetWindowTextW(hwnd, txt);
        return 0;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        wchar_t txt[128];
        hotkey_text(c->mods, c->vk, txt, 128);
        SetWindowTextW(hwnd, txt);
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        return 0;
    default:
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK settings_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    SetCtx *c = (SetCtx *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    int handled = 0;
    LRESULT r;

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        int m = DPIX(16), lw = DPIX(90), y = DPIX(16), rowh = DPIX(30);
        int cx, cw;
        RECT rc;
        HWND l1, l2, l3, l4, tip;
        wchar_t txt[600];

        c = (SetCtx *)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)c);
        c->hwnd = hwnd;
        GetClientRect(hwnd, &rc);
        cx = m + lw + DPIX(8);
        cw = rc.right - m - cx;

        l1 = mk_label_r(hwnd, L"全局热键：", 0);
        c->hHotkey = mk_edit(hwnd, IDC_SET_HOTKEY, ES_READONLY);
        SetWindowSubclass(c->hHotkey, hotkey_edit_proc, 1, (DWORD_PTR)c);
        hotkey_text(c->mods, c->vk, txt, 600);
        SetWindowTextW(c->hHotkey, txt);

        tip = mk_wrapped_label(hwnd, L"在上方按下组合键；须包含 Ctrl、Alt、Shift 或 Win。",
                       0);

        l2 = mk_label_r(hwnd, L"主题：", 0);
        c->hTheme = mk_combo(hwnd, IDC_SET_THEME);
        SendMessageW(c->hTheme, CB_ADDSTRING, 0, (LPARAM)L"浅色");
        SendMessageW(c->hTheme, CB_ADDSTRING, 0, (LPARAM)L"深色");
        SendMessageW(c->hTheme, CB_ADDSTRING, 0, (LPARAM)L"跟随系统");
        SendMessageW(c->hTheme, CB_SETCURSEL, (WPARAM)g_settings.theme, 0);

        l3 = mk_label_r(hwnd, L"多选分隔符：", 0);
        c->hSep = mk_edit(hwnd, IDC_SET_SEP, 0);
        SetWindowTextW(c->hSep, g_settings.default_separator);

        c->hAuto = mk_check(hwnd, L"开机自动启动（登录后驻留托盘）", IDC_SET_AUTOSTART);
        SendMessageW(c->hAuto, BM_SETCHECK,
                     app_get_autostart() ? BST_CHECKED : BST_UNCHECKED, 0);

        l4 = mk_label_r(hwnd, L"数据目录：", 0);
        c->hDataDir = mk_edit(hwnd, IDC_SET_DATADIR, ES_READONLY);
        storage_get_data_dir(txt, 600);
        SetWindowTextW(c->hDataDir, txt);

        mk_btn(hwnd, L"更改…", IDC_SET_DATADIR + 1);
        mk_btn(hwnd, L"打开", IDC_SET_DATADIR + 2);
        mk_btn(hwnd, L"确定", IDOK);
        mk_btn(hwnd, L"取消", IDCANCEL);
        SendMessageW(GetDlgItem(hwnd, IDOK), BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);

        MoveWindow(l1, m, y + DPIX(4), lw, DPIX(18), TRUE);
        MoveWindow(c->hHotkey, cx, y, DPIX(200), DPIX(24), TRUE);
        y += DPIX(26);
        { int height=qp_wrapped_text_height(tip,cw);
          MoveWindow(tip,cx,y,cw,height,TRUE); y+=height+DPIX(8); }
        MoveWindow(l2, m, y + DPIX(4), lw, DPIX(18), TRUE);
        MoveWindow(c->hTheme, cx, y, DPIX(140), DPIX(24), TRUE);
        y += rowh;
        MoveWindow(l3, m, y + DPIX(4), lw, DPIX(18), TRUE);
        MoveWindow(c->hSep, cx, y, DPIX(80), DPIX(24), TRUE);
        y += rowh;
        MoveWindow(c->hAuto, cx, y, cw, DPIX(24), TRUE);
        y += rowh;
        MoveWindow(l4, m, y + DPIX(4), lw, DPIX(18), TRUE);
        MoveWindow(c->hDataDir, cx, y, cw - DPIX(128), DPIX(24), TRUE);
        MoveWindow(GetDlgItem(hwnd, IDC_SET_DATADIR + 1), rc.right - m - DPIX(126), y,
                   DPIX(62), DPIX(24), TRUE);
        MoveWindow(GetDlgItem(hwnd, IDC_SET_DATADIR + 2), rc.right - m - DPIX(62), y,
                   DPIX(62), DPIX(24), TRUE);
        MoveWindow(GetDlgItem(hwnd, IDOK), rc.right - m - DPIX(188),
                   rc.bottom - m - DPIX(26), DPIX(88), DPIX(26), TRUE);
        MoveWindow(GetDlgItem(hwnd, IDCANCEL), rc.right - m - DPIX(92),
                   rc.bottom - m - DPIX(26), DPIX(88), DPIX(26), TRUE);

        qp_theme_children(hwnd);
        SetFocus(c->hHotkey);
        return 0;
    }
    case WM_COMMAND:
        qp_combo_dropdown_fix(wp, lp);
        if (!c) break;
        if (LOWORD(wp) == IDC_SET_DATADIR + 1) {   /* 更改… */
            change_data_dir(hwnd, c);
            return 0;
        }
        if (LOWORD(wp) == IDC_SET_DATADIR + 2) {   /* 打开 */
            wchar_t dir[600];
            storage_ensure_dir();
            if (storage_get_data_dir(dir, 600))
                ShellExecuteW(hwnd, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        if (LOWORD(wp) == IDOK) {
            wchar_t sep[QP_SEP_MAX];
            int theme = (int)SendMessageW(c->hTheme, CB_GETCURSEL, 0, 0);
            int autostart = SendMessageW(c->hAuto, BM_GETCHECK, 0, 0) == BST_CHECKED;

            if (c->vk && !c->mods) {
                qp_msgbox(hwnd, MB_OK | MB_ICONWARNING, L"提示",
                          L"全局热键需要至少包含 Ctrl / Alt / Shift / Win 之一，"
                          L"否则会影响其他程序的正常输入。");
                SetFocus(c->hHotkey);
                return 0;
            }
            qp_get_text(c->hSep, sep, QP_SEP_MAX);
            if (!sep[0]) qp_strlcpy(sep, L"、", QP_SEP_MAX);

            Settings candidate = g_settings;
            QpSettingsResult result;
            candidate.hotkey_mods = c->mods;
            candidate.hotkey_vk = c->vk;
            candidate.autostart = autostart;
            if (theme != CB_ERR) candidate.theme = (ThemeMode)theme;
            qp_strlcpy(candidate.default_separator, sep, QP_SEP_MAX);
            if (!qp_settings_commit(&candidate, &result)) {
                if (result.rollback_failures)
                    qp_msgbox(hwnd, MB_OK | MB_ICONERROR, L"设置未保存",
                              L"设置修改失败，部分系统设置未能恢复。请重新检查热键、主题和开机启动项，再重试。当前窗口保持打开。");
                else
                    qp_msgbox(hwnd, MB_OK | MB_ICONERROR, L"设置未保存",
                              L"无法应用或保存设置，原设置已恢复。请检查热键是否可用、配置目录权限和磁盘空间后重试。");
                return 0;
            }
            c->result = 1;
            dlg_close(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) { c->result = 0; dlg_close(hwnd); return 0; }
        break;
    case WM_CLOSE:
        if (c) c->result = 0;
        dlg_close(hwnd);
        return 0;
    default:
        break;
    }
    r = dlg_common(hwnd, msg, wp, lp, &handled);
    if (handled) return r;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int dlg_settings(HWND owner)
{
    SetCtx c;
    HWND dlg;
    memset(&c, 0, sizeof(c));
    c.mods = g_settings.hotkey_mods;
    c.vk = g_settings.hotkey_vk;
    dlg = make_dlg(L"QuickPhraseSettings", settings_proc, L"选项", 520, 288, owner, &c);
    return run_modal(owner, dlg, &c.result);
}
