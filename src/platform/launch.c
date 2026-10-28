/* launch.c —— 命令行启动策略（不依赖 Windows API） */
#include "launch.h"

#include <stddef.h>
#include <wctype.h>

static int qp_cmdline_has(const wchar_t *cmdline, const wchar_t *flag)
{
    size_t flag_len;

    if (!cmdline || !flag || !*flag) return 0;
    flag_len = wcslen(flag);
    for (; *cmdline; ++cmdline) {
        size_t i = 0;
        while (i < flag_len && cmdline[i] &&
               towlower((wint_t)cmdline[i]) == towlower((wint_t)flag[i]))
            ++i;
        if (i == flag_len) return 1;
    }
    return 0;
}

int qp_should_show_main(const wchar_t *cmdline)
{
    return !qp_cmdline_has(cmdline, L"--tray") &&
           !qp_cmdline_has(cmdline, L"/tray");
}
