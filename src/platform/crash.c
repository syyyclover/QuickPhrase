/*
 * crash.c —— 未处理异常的落地记录
 *
 * 「用着用着偶发一次闪退，连后台进程也退出」这种问题最怕的是查不到：进程整个没了，
 * 托盘也没了，事后什么都没有。这里在启动时挂两个钩子，把崩溃时的最小现场写进数据
 * 目录的 crash.log：
 *
 *   1) SetUnhandledExceptionFilter —— 访问违例、栈溢出这类 SEH 异常；
 *   2) signal(SIGABRT)             —— abort()；CRT 的堆损坏检测和
 *                                     __stack_chk_fail 走的也是这条。
 *
 * 另外尽力写一份 minidump（同目录 crash-<pid>.dmp）：dbghelp.dll 是运行时
 * LoadLibrary 取的，取不到就只留文本日志 —— 这样不引静态导入、也不占体积。
 *
 * 两条刻意的实现约束，都是为了这个 exe 的体积预算（只剩几 KB）：
 *   - 日志只写 ASCII，且自己拼字符串。崩溃路径上不能依赖 locale（默认 "C"
 *     locale 下 fwprintf 写中文会转换失败、什么都落不下）；而窄字符 stdio 那一套
 *     （FILE / 格式化）在 GUI 程序里本来没被链进来，为了写一行日志把它拖进来要占
 *     好几 KB。
 *   - 只缓存数据目录（一个静态缓冲），文件名在崩溃时用栈上的临时缓冲拼。静态数组
 *     在这个工具链下会被放进 .data（要占文件体积）而不是 .bss，多留几个就是几 KB。
 *
 * 已知覆盖不到的情况：栈溢出时处理函数本身也可能没有栈可用；挂了调试器时系统不会
 * 调用 SetUnhandledExceptionFilter。两者都不算「钩子没装上」。
 */
#include "crash.h"
#include "app.h"

#include <dbghelp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

static wchar_t s_dir[QP_PATH_MAX];    /* 数据目录，init 时缓存 */
static volatile LONG     s_busy;      /* 只写一次，挡住「写日志时又崩」的递归 */
static volatile LONG     s_msg;       /* 最近派发的消息，作为「崩在哪一步」的线索 */
static volatile LONG_PTR s_hwnd;
static volatile LONG_PTR s_wp;

/* 模块名只留 ASCII：崩溃路径上不碰 locale 转换 */
static void module_of(void *address, char *out, size_t cap, unsigned long *offset)
{
    HMODULE mod = NULL;
    wchar_t full[QP_PATH_MAX];
    const wchar_t *base;
    size_t i = 0;

    out[0] = '\0';
    *offset = 0;
    if (!address) return;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)address, &mod)) return;
    if (!GetModuleFileNameW(mod, full, QP_PATH_MAX)) return;
    base = wcsrchr(full, L'\\');
    base = base ? base + 1 : full;
    for (; base[i] && i + 1 < cap; ++i)
        out[i] = (char)(base[i] < 128 ? base[i] : '?');
    out[i] = '\0';
    *offset = (unsigned long)((const char *)address - (const char *)mod);
}

static char *put_str(char *p, const char *s)
{
    while (*s) *p++ = *s++;
    return p;
}

static char *put_dec(char *p, unsigned long v, int width)
{
    char tmp[12];
    int n = 0;
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n < width) tmp[n++] = '0';
    while (n) *p++ = tmp[--n];
    return p;
}

static char *put_hex(char *p, unsigned long long v, int width, int upper)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[18];
    int n = 0;
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = digits[(unsigned)(v & 0xFu)]; v >>= 4; }
    while (n < width) tmp[n++] = '0';
    while (n) *p++ = tmp[--n];
    return p;
}

static void write_log(DWORD code, void *address, const char *tag)
{
    SYSTEMTIME st;
    wchar_t path[QP_PATH_MAX];
    char mod[64];
    char line[512];
    char *p = line;
    unsigned long offset;
    HANDLE f;
    DWORD written;

    GetLocalTime(&st);
    module_of(address, mod, sizeof(mod), &offset);
    *p++ = '[';
    p = put_dec(p, st.wYear, 4);   *p++ = '-';
    p = put_dec(p, st.wMonth, 2);  *p++ = '-';
    p = put_dec(p, st.wDay, 2);    *p++ = ' ';
    p = put_dec(p, st.wHour, 2);   *p++ = ':';
    p = put_dec(p, st.wMinute, 2); *p++ = ':';
    p = put_dec(p, st.wSecond, 2); *p++ = ']';
    p = put_str(p, " tag=");       p = put_str(p, tag ? tag : "?");
    p = put_str(p, " pid=");       p = put_dec(p, GetCurrentProcessId(), 0);
    p = put_str(p, " tid=");       p = put_dec(p, GetCurrentThreadId(), 0);
    p = put_str(p, " code=0x");    p = put_hex(p, code, 8, 1);
    p = put_str(p, " addr=0x");
    p = put_hex(p, (unsigned long long)(ULONG_PTR)address, 0, 0);
    p = put_str(p, " module=");    p = put_str(p, mod[0] ? mod : "?");
    p = put_str(p, "+0x");         p = put_hex(p, offset, 0, 0);
    p = put_str(p, " lastmsg=0x"); p = put_hex(p, (unsigned long)(s_msg & 0xFFFFu), 4, 1);
    p = put_str(p, " hwnd=0x");
    p = put_hex(p, (unsigned long long)(ULONG_PTR)s_hwnd, 0, 0);
    p = put_str(p, " wp=0x");
    p = put_hex(p, (unsigned long long)(ULONG_PTR)s_wp, 0, 0);
    *p++ = '\r'; *p++ = '\n';

    _snwprintf(path, QP_PATH_MAX, L"%ls\\crash.log", s_dir);
    path[QP_PATH_MAX - 1] = L'\0';
    f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    WriteFile(f, line, (DWORD)(p - line), &written, NULL);
    CloseHandle(f);
}

static void try_dump(EXCEPTION_POINTERS *info)
{
    typedef BOOL (WINAPI *QpMwd)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                 PMINIDUMP_EXCEPTION_INFORMATION,
                                 PMINIDUMP_USER_STREAM_INFORMATION,
                                 PMINIDUMP_CALLBACK_INFORMATION);
    wchar_t path[QP_PATH_MAX];
    HMODULE dbg;
    QpMwd mwd;
    HANDLE file;
    MINIDUMP_EXCEPTION_INFORMATION mei;

    dbg = LoadLibraryW(L"dbghelp.dll");
    if (!dbg) return;
    mwd = (QpMwd)(void *)GetProcAddress(dbg, "MiniDumpWriteDump");
    if (!mwd) { FreeLibrary(dbg); return; }
    _snwprintf(path, QP_PATH_MAX, L"%ls\\crash-%lu.dmp", s_dir,
               (unsigned long)GetCurrentProcessId());
    path[QP_PATH_MAX - 1] = L'\0';
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) { FreeLibrary(dbg); return; }
    memset(&mei, 0, sizeof(mei));
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = info;
    mei.ClientPointers = FALSE;
    mwd(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal,
        &mei, NULL, NULL);
    CloseHandle(file);
    FreeLibrary(dbg);
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *info)
{
    if (InterlockedExchange(&s_busy, 1) == 0) {
        EXCEPTION_RECORD *rec = info ? info->ExceptionRecord : NULL;
        write_log(rec ? rec->ExceptionCode : 0,
                  rec ? rec->ExceptionAddress : NULL, "SEH");
        try_dump(info);
    }
    return EXCEPTION_EXECUTE_HANDLER;   /* 让进程按默认方式结束 */
}

static void crash_signal(int sig)
{
    if (InterlockedExchange(&s_busy, 1) == 0)
        write_log((DWORD)sig, NULL, sig == SIGABRT ? "SIGABRT" : "signal");
    signal(sig, SIG_DFL);
    raise(sig);                         /* 交回默认动作：进程照常结束 */
}

void qp_crash_init(void)
{
    if (!storage_get_data_dir(s_dir, QP_PATH_MAX) || !s_dir[0])
        qp_strlcpy(s_dir, L".", QP_PATH_MAX);
    SetUnhandledExceptionFilter(crash_filter);
    signal(SIGABRT, crash_signal);
}

void qp_crash_note_message(HWND hwnd, UINT msg, WPARAM wp)
{
    s_hwnd = (LONG_PTR)hwnd;
    s_wp = (LONG_PTR)wp;
    s_msg = (LONG)msg;
}
