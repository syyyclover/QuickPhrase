from source_helpers import read_source, ROOT
"""Contract check for 口径 A：模板改了选项后，旧值向新选项集对齐。

问题 6 的代码级缺口：`sess_activate()` 在切换模板时按片段 id 把旧值搬到新上下文，
**搬完不校验**。于是把某个选项改名/删掉之后，单选下拉里没有这一项、框里却还留着
旧文字；多选 Edit 里还带着已删除的选项；预览里也跟着出现 —— 「多个地方都不更新」。

口径 A 的语义是「模板是权威，旧值向新选项对齐」：
- 值里已不存在的选项丢掉、仍存在的保留；
- 自主补充文字保留（新模板还有补充项时）；
- **认不出的旧值原样留着** —— 宁可显示不一致，也不静默吞掉用户手填的内容。

本用例只做源码契约断言（不需要编译器）。真正的行为断言在 tests/test_choice.c。

Run with:  python3 tests/choice_realign.py
"""
import re
import sys
from pathlib import Path

failures = []


def fail(msg):
    failures.append(msg)
    print("FAIL: " + msg)


def strip_comments(text):
    """断言前先剥注释：修复说明里就写着这些关键字，不剥的话用例会假绿。"""
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def body(text, name):
    match = re.search(r"%s\([^;]+?\)\n\{" % name, text)
    if not match:
        return ""
    return text[match.start():text.index("\n}", match.end()) + 2]


# ---- 1. core 层的投影函数 ----
core = read_source("choices.c")
realign = body(core, "qp_choice_realign")
if not realign:
    fail("找不到生产函数 qp_choice_realign（口径 A 的旧值投影）")
else:
    code = strip_comments(realign)
    checks = [
        ("SEG_SELECT", "单选必须与多选分开处理：单选没有分隔符歧义，值对不上就该丢弃"),
        ("memset(checked", "必须先清空 checked，否则调用方缓冲里的残留会被当成命中"),
        ("segments", "多选要按「拆出了几段」判定分隔符是否可信，再决定清空还是保留"),
        ("custom_out", "自主补充文字要经 custom_out 原样返回，不能丢"),
    ]
    for need, why in checks:
        if need not in code:
            fail("qp_choice_realign 缺少 %s —— %s" % (need, why))
    # 拆不开时的兜底必须是「返回 -1 保持原值」。只看 segments 判定之后那一段 ——
    # 函数开头还有一句防御性的 return -1，全局搜 "return -1" 会假绿。
    tail = code.split("segments>=2", 1)[1] if "segments>=2" in code else ""
    if "return -1" not in tail:
        fail("多选拆不开的旧值必须返回 -1（保持原值）：改成 0 会把认不出的用户输入静默清空")

# ---- 2. UI 侧的接线 ----
ui = read_source("ui.c")
act = strip_comments(body(ui, "sess_activate"))
if not act:
    fail("找不到 sess_activate")
elif "sess_reconcile_choices(" not in act:
    fail("sess_activate 没有调用 sess_reconcile_choices：模板改了选项后旧值不会向新选项集对齐")
else:
    # 必须在求值之前对齐：qp_eval_run_checked 会把 value 定下来
    if act.find("sess_reconcile_choices(") > act.find("qp_eval_run_checked("):
        fail("sess_reconcile_choices 排在了 qp_eval_run_checked 之后：对齐要在求值之前")

recon = strip_comments(body(ui, "sess_reconcile_choices"))
if not recon:
    fail("找不到 sess_reconcile_choices（口径 A 在会话层的执行者）")
else:
    for need in ("qp_choice_realign(", "qp_choice_set(", "sess_clear_choice("):
        if need not in recon:
            fail("sess_reconcile_choices 缺少 " + need)
    if "user_touched" not in recon:
        fail("sess_reconcile_choices 没有按 user_touched 过滤：没填过的字段应保持模板默认值")
    if "SEG_MULTI_SELECT" not in recon or "SEG_SELECT" not in recon:
        fail("sess_reconcile_choices 没有限定只处理单选/多选字段")

# ---- 3. 行为断言必须落在 C 单测里 ----
test = (Path(ROOT) / "tests/test_choice.c").read_text()
if "qp_choice_realign(" not in test:
    fail("tests/test_choice.c 没有覆盖 qp_choice_realign 的行为")

if failures:
    print("\n%d 项约束不满足" % len(failures))
    sys.exit(1)
print("多选/单选：模板改选项后旧值向新选项集对齐（口径 A）符合约定")
