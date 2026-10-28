from source_helpers import read_source, INCLUDE_FLAGS, CORE_SOURCES
"""Contract checks for the 数据表 dialog's repaint and notification discipline.

两处都是「拖窗口边框时出现 UI 绘制错误」的直接来源，而且都不是靠肉眼看代码能
长期守住的，所以在这里钉成硬约束：

1. 背景谁来画。QuickPhraseTables 这个窗口类没有背景刷（ensure_class 里
   hbrBackground = NULL），数据表窗口自己也没有 WM_PAINT —— 整片背景只由
   dlg_common 的 WM_ERASEBKGND 涂。而 WM_ERASEBKGND 只在「带 ERASE 标志地
   失效」时才会被系统发出来，所以 WM_SIZE 收尾的那次 RedrawWindow 必须带
   RDW_ERASE；漏了它，拉大窗口时新露出来的那圈底就没人画，屏幕上留着上一帧的
   残影（实测：拉大后右下角采样到纯白，而不是主题背景色）。
   （若哪天给这个窗口补了 WM_PAINT，这条约束才允许放宽。）

2. 程序化重建列表必须闸住 loading。ListView 的 DeleteAllItems / InsertItem /
   SetItemState 都会**同步**给父窗口发 LVN_ITEMCHANGED，而 WM_NOTIFY 收到它
   就 table_dlg_load() 一次 —— 不闸住的话，刷新一次列表会把预览列表拆了重建
   N+1 遍，屏幕上一片闪。闸门还必须恢复成进来时的值（嵌套调用会抹掉外层的闸）。

Run with:  python3 tests/table_dialog_repaint.py
"""
import pathlib
import re
import sys

repo = pathlib.Path(__file__).resolve().parents[1]
source = read_source("ui_dialogs.c")

failures = []


def fail(msg):
    failures.append(msg)
    print("FAIL: " + msg)


def strip_comments(text):
    """去掉 /* ... */ 注释再断言 —— 否则「注释里提到 RDW_ERASE」会被当成
       「代码里带了 RDW_ERASE」，测试就成了摆设（第一版就踩了这个坑）。"""
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def body(name, kind="static"):
    """抽出某个函数的完整函数体（含首尾花括号）"""
    pattern = (r"%s [^\n]+\b%s\([^;]+?\)\n\{" % (kind, name)) if kind else \
              (r"%s\([^;]+?\)\n\{" % name)
    match = re.search(pattern, source)
    if not match:
        fail("找不到生产函数 " + name)
        return ""
    return source[match.start():source.index("\n}", match.end()) + 2]


proc = body("table_dlg_proc")
if not proc:
    sys.exit(1)

# ---- 1. 背景必须有人擦 ----
size_branch = re.search(r"case WM_SIZE:\s*\{(.*?)\n    \}", proc, re.S)
if not size_branch:
    fail("table_dlg_proc 里找不到 WM_SIZE 分支")
else:
    branch = strip_comments(size_branch.group(1))
    if "WM_SETREDRAW" not in branch:
        fail("WM_SIZE 没有把中间过程捂起来（拖动时会一条消息重画一遍）")
    if "RedrawWindow" not in branch:
        fail("WM_SIZE 收尾没有 RedrawWindow，让出来的空档没人重画")
    paints_itself = re.search(r"case WM_PAINT", proc) is not None
    if "RDW_ERASE" not in branch and not paints_itself:
        fail("WM_SIZE 的重绘没有 RDW_ERASE，而本窗口类没有背景刷、也没有 WM_PAINT —— "
             "拉大窗口时新露出来的那圈底不会被重画（拖边框时的 UI 绘制错误）")
    if "RDW_INVALIDATE" not in branch:
        fail("WM_SIZE 的重绘没有 RDW_INVALIDATE")
    if "RDW_ALLCHILDREN" not in branch:
        fail("WM_SIZE 的重绘没有 RDW_ALLCHILDREN，子控件会停在旧位置/旧配色")

# ---- 2. 程序化重建列表必须闸住 loading ----
fill = strip_comments(body("table_dlg_fill"))
if not fill:
    fail("找不到 table_dlg_fill")
else:
    if "int was = c->loading;" not in fill:
        fail("table_dlg_fill 没有保存进来时的 loading —— 恢复时只能写回原值，"
             "直接写 0 会把外层（比如 table_dlg_load）的闸抹掉")
    if "c->loading = 1;" not in fill:
        fail("table_dlg_fill 重建列表时没有抬 loading：DeleteAllItems / 每插一行都会"
             "同步发 LVN_ITEMCHANGED，预览列表会被拆了重建 N+1 遍")
    if "c->loading = was;" not in fill:
        fail("table_dlg_fill 没有把 loading 恢复成进来时的值")
    first_del = fill.find("ListView_DeleteAllItems(c->hList)")
    raise_at = fill.find("c->loading = 1;")
    restore_at = fill.find("c->loading = was;")
    if -1 in (first_del, raise_at, restore_at) or not (raise_at < first_del < restore_at):
        fail("table_dlg_fill 里 loading 的抬起/恢复没有把整个列表重建夹在中间")
    if fill.count("table_dlg_load(c)") != 1:
        fail("table_dlg_fill 里 table_dlg_load 应该只在最后统一调一次")

notify_branch = re.search(r"case WM_NOTIFY:\s*\{(.*?)\n    \}", proc, re.S)
if not notify_branch:
    fail("table_dlg_proc 里找不到 WM_NOTIFY 分支")
else:
    branch = strip_comments(notify_branch.group(1))
    if "LVN_ITEMCHANGED" not in branch:
        fail("WM_NOTIFY 没有处理 LVN_ITEMCHANGED，选表不会刷新预览")
    if "!c->loading" not in branch:
        fail("WM_NOTIFY 的 LVN_ITEMCHANGED 没有用 !c->loading 闸住：列表重建期间"
             "同步发回来的通知会被当成用户换行，反复重载预览")

if failures:
    print("\n%d 项约束不满足" % len(failures))
    sys.exit(1)
print("数据表窗口：背景重绘与列表重建的通知纪律符合约定")
