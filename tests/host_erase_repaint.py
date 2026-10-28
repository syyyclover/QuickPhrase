from source_helpers import read_source
"""Contract check for the 填写区宿主窗口（CLS_HOST）的背景擦除。

CLS_HOST 注册时 hbrBackground = NULL，整片背景只由它自己的 WM_PAINT
（qp_paint_background）画。而系统在「带 ERASE 标志地失效」时会先发一条
WM_ERASEBKGND —— 从任务栏还原、放大缩小、拉边框都走这条路。这条消息如果只
回一个「已经擦过了」却不画任何像素，新露出来的那圈底就没人画，屏幕上先是黑
（或上一帧的残影），等 WM_PAINT 到了才补上，肉眼看就是「填写框瞬间变黑」。

所以 WM_ERASEBKGND 必须真的把背景涂出来，而且涂的必须是 WM_PAINT 会涂的同一
份（qp_draw_background 就是 qp_paint_background 内部用的那一个 FillRect）。
宿主窗口自己没有任何分隔条，两趟画出来的像素完全相同，不存在「先擦掉再画上」
的闪烁问题。

Run with:  python3 tests/host_erase_repaint.py
"""
import re
import sys

source = read_source("ui.c")
# qp_draw_background / qp_paint_background 住在 text_paint.c，不在 ui.c 的拼接里
paint_source = read_source("text_paint.c")

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


def paint_body(name):
    match = re.search(r"%s\([^;]+?\)\n\{" % name, paint_source)
    if not match:
        fail("text_paint.c 里找不到 " + name)
        return ""
    return paint_source[match.start():paint_source.index("\n}", match.end()) + 2]


proc = body("host_proc")
if not proc:
    sys.exit(1)
proc_code = strip_comments(proc)

# ---- 1. 擦除必须真的画背景 ----
erase = re.search(r"case WM_ERASEBKGND:(.*?)(?=\n    case |\n    default:)", proc_code, re.S)
if not erase:
    fail("host_proc 里找不到 WM_ERASEBKGND 分支")
else:
    text = erase.group(1)
    if "qp_draw_background(" not in text:
        fail("WM_ERASEBKGND 只回了「已经擦过了」却没画任何像素：CLS_HOST 的窗口类"
             "没有背景刷，从任务栏还原／放大缩小时新露出来的那圈底没人画，"
             "填写框会先黑一帧再等 WM_PAINT 补上")
    if "return 1" not in text and "return TRUE" not in text:
        fail("WM_ERASEBKGND 处理完应当返回 1（已经擦过了），否则系统还会用窗口类"
             "背景刷再涂一遍")

# ---- 2. 擦与画必须是同一份背景 ----
paint = re.search(r"case WM_PAINT:(.*?)(?=\n    case |\n    default:)", proc_code, re.S)
if not paint:
    fail("host_proc 里找不到 WM_PAINT 分支")
elif "qp_paint_background(" not in paint.group(1):
    fail("WM_PAINT 没有走 qp_paint_background")
else:
    draw = strip_comments(paint_body("qp_draw_background"))
    if "FillRect(" not in draw or "g_brushBk" not in draw:
        fail("qp_draw_background 不再是用主题背景刷 FillRect —— 擦除趟与绘制趟"
             "画出来的不再是同一份像素，会引入新的闪烁")
    if "qp_draw_background(" not in strip_comments(paint_body("qp_paint_background")):
        fail("qp_paint_background 不再复用 qp_draw_background：两趟背景已经分家")

if failures:
    print("\n%d 项约束不满足" % len(failures))
    sys.exit(1)
print("填写区宿主窗口：背景擦除与绘制走同一份背景（还原/缩放不再黑闪）")
