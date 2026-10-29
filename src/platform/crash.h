/*
 * crash.h —— 未处理异常的落地记录（实现在 crash.c）
 */
#ifndef QP_CRASH_H
#define QP_CRASH_H

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* 安装钩子并把日志路径指到当前数据目录。幂等，可以在数据目录确定前后各调一次。 */
void qp_crash_init(void);

/* 记录最近派发的窗口消息；消息循环里逐条调用（只写几个全局变量，不做 I/O）。 */
void qp_crash_note_message(HWND hwnd, UINT msg, WPARAM wp);

#endif /* QP_CRASH_H */
