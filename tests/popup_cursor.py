from source_helpers import read_source
"""Contract check for the 多选弹层's cursor ownership.

弹层是「全程持有鼠标捕获」的窗口（SetCapture(g_popup)），而系统在捕获期间不再
把 WM_SETCURSOR 发给任何窗口 —— 光标的形状只能由捕获方自己摆。所以弹层必须
两头都管住：

1. popup_open 里 SetCapture 之后要立刻把光标摆成箭头。否则「自主补充」这条路径
   （popup_close → 模态输入框 → popup_open）会把光标留在输入框关闭后的状态，
   没人接手时指针在弹层上直接消失。
2. popup_proc 要自己处理 WM_SETCURSOR 并返回 TRUE。漏了它，DefWindowProc 会把
   消息转给父窗口（主窗口），主窗口按「分隔条命中」的逻辑回一个左右/上下箭头 ——
   弹层压在分隔条那一列上时，光标就变成缩放箭头。

Run with:  python3 tests/popup_cursor.py
"""
import re
import sys

source = read_source("ui.c")

failures = []


def fail(msg):
    failures.append(msg)
    print("FAIL: " + msg)


def strip_comments(text):
    """断言前先剥掉 /* ... */ 注释：修复说明里就写着 WM_SETCURSOR / IDC_ARROW，
       不剥的话「撤掉代码」这个用例照样绿，测试就成了摆设。"""
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def body(name):
    """抽出某个函数的完整函数体（含首尾花括号）"""
    match = re.search(r"%s\([^;]+?\)\n\{" % name, source)
    if not match:
        fail("找不到生产函数 " + name)
        return ""
    return source[match.start():source.index("\n}", match.end()) + 2]


proc = body("popup_proc")
if not proc:
    sys.exit(1)
proc_code = strip_comments(proc)

# ---- 1. popup_proc 必须自己处理 WM_SETCURSOR ----
if "case WM_SETCURSOR" not in proc_code:
    fail("popup_proc 没有处理 WM_SETCURSOR：捕获期间系统不发这条消息给别的窗口，"
         "弹层上没人摆光标，指针会停在「未设置」状态（自主补充填写完后指针消失）")
else:
    branch = re.search(r"case WM_SETCURSOR:(.*?)(?=\n    case |\n    default:)",
                       proc_code, re.S)
    if not branch:
        fail("popup_proc 的 WM_SETCURSOR 分支取不到函数体")
    else:
        text = branch.group(1)
        if "SetCursor(" not in text:
            fail("popup_proc 的 WM_SETCURSOR 没有调 SetCursor")
        if "IDC_ARROW" not in text:
            fail("popup_proc 的 WM_SETCURSOR 没有摆回箭头光标")
        if "return TRUE" not in text and "return 1" not in text:
            fail("popup_proc 的 WM_SETCURSOR 没有返回 TRUE —— 交回 DefWindowProc "
                 "会被转给父窗口，按分隔条命中回一个缩放箭头")

# ---- 2. popup_open 里 SetCapture 之后必须把光标摆回来 ----
opener = strip_comments(body("popup_open"))
if not opener:
    fail("找不到 popup_open")
else:
    cap_at = opener.find("SetCapture(g_popup)")
    if cap_at < 0:
        fail("popup_open 没有 SetCapture(g_popup)：弹层靠捕获接管鼠标输入")
    elif "SetCursor(" not in opener[cap_at:]:
        fail("popup_open 在 SetCapture 之后没有摆光标：捕获期间系统不再发 "
             "WM_SETCURSOR，重开弹层时没人接手，指针会消失")

if failures:
    print("\n%d 项约束不满足" % len(failures))
    sys.exit(1)
print("多选弹层：光标归属（捕获期显式摆光标 + WM_SETCURSOR）符合约定")
