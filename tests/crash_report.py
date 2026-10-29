from source_helpers import read_source
"""Contract check for the crash reporter (src/platform/crash.c).

「偶发一次闪退」的价值全在事后能不能查到现场。这条契约钉住四件事，都是少了就
白装钩子、而且不会有人立刻发现的：

1. 两个钩子都真的装上了：SEH（SetUnhandledExceptionFilter）和 abort()
   （signal(SIGABRT)）。CRT 的堆损坏检测和 __stack_chk_fail 走的是后一条。
2. 钩子接进了 wWinMain，而且**在数据目录确定之后又接了一次** —— 否则日志会一直
   落在默认目录，用户自定义数据目录时根本找不到。
3. 消息循环记录「最近派发的消息」：日志里能看出崩的时候在处理哪条消息，这是唯一
   能区分「崩在查表」「崩在重建列表」的线索。
4. 处理完必须让进程照常结束，不能把异常吞掉 —— 吞掉的话进程带着坏状态继续跑，
   比直接退出更糟。

Run with:  python3 tests/crash_report.py
"""
import re
import sys

crash = read_source("crash.c")
main = read_source("main.c")

failures = []


def fail(msg):
    failures.append(msg)
    print("FAIL: " + msg)


def strip_comments(text):
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def body(text, name):
    match = re.search(r"%s\([^;]+?\)\n\{" % name, text)
    if not match:
        fail("找不到生产函数 " + name)
        return ""
    return text[match.start():text.index("\n}", match.end()) + 2]


code = strip_comments(crash)

# ---- 1. 两个钩子 ----
init = strip_comments(body(crash, "qp_crash_init"))
if not init:
    sys.exit(1)
if "SetUnhandledExceptionFilter(" not in init:
    fail("qp_crash_init 没有装 SetUnhandledExceptionFilter：访问违例这类 SEH 异常"
         "不会被记录")
if "signal(SIGABRT" not in init:
    fail("qp_crash_init 没有装 SIGABRT 钩子：abort()（含 CRT 堆损坏检测与"
         "__stack_chk_fail）不会被记录")
if "storage_get_data_dir(" not in init:
    fail("qp_crash_init 没有把日志路径指到数据目录，用户找不到 crash.log")

# ---- 2. 接进 wWinMain，且数据目录确定后再接一次 ----
winmain = strip_comments(body(main, "wWinMain"))
if not winmain:
    fail("找不到 wWinMain")
else:
    calls = winmain.count("qp_crash_init()")
    if calls == 0:
        fail("wWinMain 没有调用 qp_crash_init，钩子根本没装上")
    elif calls < 2:
        fail("wWinMain 只调了一次 qp_crash_init：数据目录是 storage_set_data_dir "
             "之后才确定的，少了第二次日志会一直落在默认目录")
    else:
        set_dir = winmain.find("storage_set_data_dir(")
        if set_dir < 0:
            fail("wWinMain 里找不到 storage_set_data_dir")
        else:
            second = winmain.rfind("qp_crash_init()")
            if second < set_dir:
                fail("第二次 qp_crash_init 不在 storage_set_data_dir 之后，"
                     "日志路径没跟着自定义数据目录走")

# ---- 3. 消息循环记录最近的消息 ----
if "qp_crash_note_message(" not in winmain:
    fail("wWinMain 的消息循环没有记录最近派发的消息：崩溃日志里就没有"
         "「当时在处理哪条消息」这条最关键的线索")
note = strip_comments(body(crash, "qp_crash_note_message"))
if note and ("s_msg" not in note or "s_hwnd" not in note):
    fail("qp_crash_note_message 没有把消息号/窗口句柄存下来")

# ---- 4. 记完必须让进程照常结束 ----
filt = strip_comments(body(crash, "crash_filter"))
if filt and "EXCEPTION_EXECUTE_HANDLER" not in filt:
    fail("crash_filter 没有返回 EXCEPTION_EXECUTE_HANDLER：异常被吞掉的话进程会"
         "带着坏状态继续跑，比直接退出更糟")
sig = strip_comments(body(crash, "crash_signal"))
if sig:
    if "SIG_DFL" not in sig:
        fail("crash_signal 没有把信号恢复成 SIG_DFL，abort() 就不再是 abort() 了")
    if "raise(" not in sig:
        fail("crash_signal 没有重新 raise：默认动作不会执行，进程可能继续跑")

# ---- 5. 日志必须写的是「可读的一行」，而不是靠 locale 转换 ----
if "CreateFileW(" not in code or "WriteFile(" not in code:
    fail("日志不是用 CreateFileW/WriteFile 写的：崩溃路径上不该依赖标准库流")

if failures:
    print("\n%d 项约束不满足" % len(failures))
    sys.exit(1)
print("崩溃记录：SEH/abort 钩子已装上并接进消息循环，记完照常结束进程")
