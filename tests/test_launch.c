/*
 * test_launch.c —— 启动策略自测（不依赖 Windows API，本机 gcc 即可运行）
 */
#include "../src/launch.h"

#include <stdio.h>

static int g_pass;
static int g_fail;

static void check(int cond, const char *what)
{
    if (cond) { ++g_pass; printf("  [ok]   %s\n", what); }
    else      { ++g_fail; printf("  [FAIL] %s\n", what); }
}

int main(void)
{
    printf("启动策略自测\n==========\n");

    check(qp_should_show_main(L"") == 1, "无参数启动显示主界面");
    check(qp_should_show_main(L"--show") == 1, "--show 启动显示主界面");
    check(qp_should_show_main(L"--tray") == 0, "--tray 启动静默驻留");
    check(qp_should_show_main(L"/tray") == 0, "/tray 启动静默驻留");

    printf("\n结果: %d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
