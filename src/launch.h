/* launch.h —— 不依赖 Windows API 的启动策略 */
#ifndef QP_LAUNCH_H
#define QP_LAUNCH_H

#include <wchar.h>

int qp_should_show_main(const wchar_t *cmdline);

#endif
