from source_helpers import read_source
"""Contract check for the 主窗口的背景擦除（CLS_MAIN）。

主窗口的窗口类也没有背景刷（`hbrBackground = NULL`），客户区底子只由
`main_draw_client` 画 —— 而且它要画的不只是背景，还有两条分隔条。

系统在「带 ERASE 标志地失效」时会先发 WM_ERASEBKGND：从任务栏还原、放大缩小、
拉边框都走这条路。这条消息只有两种错误做法：

- 回一句「已经擦过了」却什么都不画 → 新露出来的那圈底先黑一帧，等 WM_PAINT
  到了才补上（就是「填写框/窗口瞬间变黑」）。
- 只 FillRect 涂背景、不画分隔条 → 分隔条先消失一帧再出现，拖动时表现为边框
  抽搐（这是历史上真实修过的毛病）。

唯一同时避开两者的做法：擦除趟和绘制趟走**同一个** `main_draw_client`，两趟
像素完全相同。所以这条契约钉住「WM_ERASEBKGND 必须调 main_draw_client」。

Run with:  python3 tests/window_erase_repaint.py
"""
import re
import sys

source = read_source("ui.c")

failures = []


def fail(msg):
    failures.append(msg)
    print("FAIL: " + msg)


def strip_comments(text):
    """断言前先剥掉 /* ... */ 注释，避免注释里的字样把用例喂绿。"""
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def body(name):
    match = re.search(r"%s\([^;]+?\)\n\{" % name, source)
    if not match:
        fail("找不到生产函数 " + name)
        return ""
    return source[match.start():source.index("\n}", match.end()) + 2]


proc = body("main_proc")
if not proc:
    sys.exit(1)
proc_code = strip_comments(proc)


def branch(case):
    found = re.search(r"case %s:(.*?)(?=\n    case |\n    default:)" % case, proc_code, re.S)
    if not found:
        fail("main_proc 里找不到 %s 分支" % case)
        return None
    return found.group(1)


# ---- 1. 擦除趟必须画出与绘制趟相同的像素 ----
erase = branch("WM_ERASEBKGND")
if erase is not None:
    if "main_draw_client(" not in erase:
        fail("WM_ERASEBKGND 没有走 main_draw_client：只回「已经擦过了」不画任何"
             "像素，从任务栏还原／放大缩小时新露出来的那圈底会先黑一帧；"
             "只 FillRect 涂背景又会漏掉分隔条，拖动时边框抽搐")
    if "FillRect(" in erase:
        fail("WM_ERASEBKGND 自己 FillRect 了：背景与分隔条会分两趟画，中间态"
             "会露到屏幕上，应当整体交给 main_draw_client")
    if "return 1" not in erase and "return TRUE" not in erase:
        fail("WM_ERASEBKGND 处理完应当返回 1（已经擦过了），否则系统还会用窗口类"
             "背景刷再涂一遍")

# ---- 2. 绘制趟用的是同一个合成函数 ----
paint = branch("WM_PAINT")
if paint is not None and "main_draw_client(" not in paint:
    fail("WM_PAINT 没有走 main_draw_client —— 擦除趟与绘制趟已经分家")

# ---- 3. 那个合成函数必须认 clip（擦除趟传的是 GetClipBox 的矩形）----
draw = strip_comments(body("qp_draw_window_frame"))
if not draw:
    fail("找不到 main_draw_client")
else:
    if "clip" not in draw:
        fail("main_draw_client 不再按传入的 clip 矩形作画，擦除趟传 GetClipBox "
             "就会画错位置")
    if "BitBlt(" not in draw:
        fail("main_draw_client 不是「离屏画完再整块贴上」—— 擦除趟会露出中间态")
    if "FillRect(" not in draw:
        fail("main_draw_client 没有涂背景")
    if "splitter_x()" not in draw:
        fail("main_draw_client 没有画分隔条本体：只涂背景的那一趟会让分隔条"
             "先消失一帧")

if failures:
    print("\n%d 项约束不满足" % len(failures))
    sys.exit(1)
print("主窗口：背景擦除与绘制走同一份合成（还原/缩放不再黑闪，也不抽搐）")
