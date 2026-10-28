/* Real HWND/GDI restoration-frame contract. No user profile, visible windows,
   clipboard or interactive message loop is used by this test. */
#include "ui_internal.h"
#include <stdio.h>

static int failures, checks;

static void check(int yes, const char *message)
{
    ++checks;
    if (!yes) { ++failures; printf("FAIL: %s\n", message); }
}

static void check_frame(HWND hwnd, const char *name)
{
    RECT rc, clip = {17, 9, 521, 123}, before, after;
    GetClientRect(hwnd, &rc);
    BITMAPINFO info = {0};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = rc.right;
    info.bmiHeader.biHeight = -rc.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    HDC screen = GetDC(NULL), dc = CreateCompatibleDC(screen);
    BYTE *pixels = NULL;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, (void **)&pixels, NULL, 0);
    ReleaseDC(NULL, screen);
    check(dc && bitmap && pixels, "allocate real 32-bit restoration target");
    if (!dc || !bitmap || !pixels) return;
    HGDIOBJ old = SelectObject(dc, bitmap);
    size_t size = (size_t)rc.right * rc.bottom * 4;
    BYTE *reference = malloc(size);
    check(reference != NULL, "allocate reference pixels");
    if (!reference) goto done;
    for (int clipped = 0; clipped < 2; ++clipped) {
        HRGN region = clipped ? CreateRectRgnIndirect(&clip) : NULL;
        SelectClipRgn(dc, region);
        if (region) DeleteObject(region);
        GetClipBox(dc, &before);
        memset(pixels, 0, size);
        SendMessageW(hwnd, WM_PRINTCLIENT, (WPARAM)dc, PRF_CLIENT);
        GdiFlush();
        memcpy(reference, pixels, size);
        int painted = 0, ink = 0;
        COLORREF foreground = theme_text();
        for (size_t i = 0; i < size; i += 4) {
            if (reference[i] || reference[i+1] || reference[i+2]) painted = 1;
            if (reference[i] == GetBValue(foreground) &&
                reference[i+1] == GetGValue(foreground) &&
                reference[i+2] == GetRValue(foreground)) ++ink;
        }
        check(painted, "reference renders into black target");
        check(ink > 0, "reference contains foreground glyph pixels, not just a background");
        InvalidateRect(hwnd, NULL, TRUE);
        int pending = GetUpdateRect(hwnd, NULL, FALSE);
        for (int repeat = 0; repeat < 20; ++repeat) {
            memset(pixels, 0, size);
            LRESULT erased = SendMessageW(hwnd, WM_ERASEBKGND, (WPARAM)dc, 0);
            GdiFlush();
            int same = !memcmp(reference, pixels, size);
            if (!same && repeat == 0)
                printf("%s theme=%d clipped=%d: erase claimed=%lld but frame differs\n",
                       name, g_dark, clipped, (long long)erased);
            check(erased && same, "erase frame equals complete client frame before WM_PAINT");
            GetClipBox(dc, &after);
            check(EqualRect(&before, &after), "erase preserves supplied DC clipping");
            check(GetUpdateRect(hwnd, NULL, FALSE) == pending, "erase does not validate pending normal paint");
        }
    }
    free(reference);
done:
    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc);
}

int wmain(void)
{
    INITCOMMONCONTROLSEX ic = {sizeof(ic), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&ic);
    g_hInst = GetModuleHandleW(NULL);
    settings_defaults(&g_settings);
    g_dpi = 96;
    g_fontUI = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                          0, 0, 0, 0, L"Segoe UI");
    g_fontBold = g_fontUI;
    g_fontMono = g_fontUI;
    check(ui_register_classes(), "register production window classes");
    HWND owner = CreateWindowExW(0, L"Static", L"isolated restoration test", WS_POPUP,
                                 0, 0, 720, 240, NULL, NULL, g_hInst, NULL);
    HWND preview = qp_preview_create(owner, 1, 0);
    HWND tabs = CreateWindowExW(0, CLS_TABS, L"", WS_CHILD, 0, 0, 640, 160,
                                owner, NULL, g_hInst, NULL);
    check(owner && preview && tabs, "create isolated hidden HWNDs");
    g_hTabs = tabs;
    g_curTmpl = tmpl_create(L"Restoration fixture");
    g_curSess = g_curTmpl ? sess_pick_for(g_curTmpl->id) : NULL;
    check(g_curSess != NULL, "create actual tab session model");
    if (g_curSess) qp_strlcpy(g_curSess->name, L"Restoration session", QP_NAME_MAX);
    SetWindowPos(preview, NULL, 0, 0, 640, 160, SWP_NOZORDER | SWP_NOACTIVATE);
    for (g_dark = 0; g_dark < 2; ++g_dark) {
        g_brushBk = CreateSolidBrush(theme_bk());
        g_brushCtl = CreateSolidBrush(theme_ctl_bk());
        check(qp_preview_set_text(preview, L"Restoration frame\nSecond line", NULL, 0), "publish real text model");
        check_frame(preview, "text preview");
        check(qp_preview_set_table_marked(preview, L"A\tB\nC\tD", L"one\ttwo", NULL, 0), "publish real table model");
        check_frame(preview, "table preview");
        check_frame(tabs, "session tabs");
        DeleteObject(g_brushBk); DeleteObject(g_brushCtl);
    }
    DestroyWindow(owner);
    g_hTabs = NULL;
    sess_free_all();
    tmpl_free(g_curTmpl); g_curTmpl = NULL;
    DeleteObject(g_fontUI);
    printf("Native restoration frames: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
