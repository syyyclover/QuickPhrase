#include "ui_internal.h"
#include <dwmapi.h>

/* State belongs to each window's context; owner/editor transitions may nest. */
void qp_prepare_present(HWND hwnd, int *pending)
{
    BOOL cloak = TRUE;
    if (!*pending)
        *pending = SUCCEEDED(DwmSetWindowAttribute(hwnd, DWMWA_CLOAK,
                                                   &cloak, sizeof(cloak)));
    /* Windows 7 does not support app cloaking; retain its normal paint path. */
}

void qp_finish_present(HWND hwnd, int *pending)
{
    BOOL cloak = FALSE;
    if (!*pending) return;
    RedrawWindow(hwnd, NULL, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    GdiFlush();
    DwmFlush();
    if (SUCCEEDED(DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak)))) {
        *pending = 0;
        KillTimer(hwnd, QP_TIMER_PRESENT);
    } else {
        /* Keep the recovery path if the compositor rejects the first release. */
        SetTimer(hwnd, QP_TIMER_PRESENT, 100, NULL);
    }
}
