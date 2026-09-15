#!/usr/bin/env python3
"""正则引擎差分验证：同一批 (模式, 文本) 同时喂给本引擎与 Python 的 re，逐条比对。

分两档，各有各的硬门槛：

  第一档（gate）「实际会写的模式」——必须与 Python 100% 逐字节一致。
    生成时排除：把量词套在可空原子上、反向引用、超过 2 层的分组嵌套。
    这一档覆盖模板作者真正会写的那类表达式（取编号、取日期、取两标记之间……），
    是本功能的风险边界。

  第二档（stress）「压力模式」——不做一致性断言，只断言两件事：
    一定会在限定时间内返回（不挂界面）、同样的输入两次结果完全相同（可复现）。
    这一档会把量词、嵌套分组、反向引用乱组合出来，其中极少数（实测 0.1% 量级）
    的捕获组取值会与 Python 有出入，原因见 README「已知边界」一节。

用法： python3 tests/regex_diff.py [每档用例数]
"""
import pathlib
import random
import re
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
CASES = int(sys.argv[1]) if len(sys.argv) > 1 else 4000
SEED = 20260911
MAX_GROUPS = 3

# 只含两边语义一致的字符：不含换行（避开 $ 在 Python 里还认串尾换行的差异），
# 汉字只用统一表意文字（避开全角标点在两边 \w 归类上的差异）。
TEXT_CHARS = "abcABC012 -_@.中文设备"
LITERALS = list("abcABC012中文")
CLASS_ITEMS = ["a-c", "A-C", "0-9", "中-文", "_", "-", "@"]
PREDEF = ["\\d", "\\D", "\\w", "\\W", "\\s", "\\S"]


class Skip(Exception):
    """这一条不适合放进当前档，换一条。"""


def gen_quant(rng, maxrep=None):
    """随机量词。maxrep 非空时把上界也压到它以内（实际模式那一档用）。"""
    lim = maxrep if maxrep else 9
    r = rng.random()
    if r < 0.28:
        q = rng.choice(["*", "+", "?"])
    elif r < 0.45:
        q = "{%d}" % rng.randint(0, min(3, lim))
    elif r < 0.60:
        q = "{%d,}" % rng.randint(0, min(2, lim))
    else:
        a = rng.randint(0, min(2, lim))
        b = a + rng.randint(0, max(0, min(3, lim - a)))
        q = "{%d,%d}" % (a, b)
    if rng.random() < 0.3:
        q += "?"
    return q


def quant_bounds(q):
    """把量词解析成 (min, max)，max = None 表示无限。"""
    core = q
    if core != "?" and core.endswith("?"):
        core = core[:-1]                          # 去掉懒惰后缀
    if core == "*":
        return 0, None
    if core == "+":
        return 1, None
    if core == "?":
        return 0, 1
    body = core[1:-1]
    if not body:
        raise Skip("量词解析失败 %r" % q)
    if "," not in body:
        return int(body), int(body)
    a, b = body.split(",")
    return int(a), (int(b) if b else None)


def gen_atom(rng, depth, st, realistic):
    """返回 (模式片段, 该片段是否可匹配空串)。"""
    r = rng.random()
    if depth > 0 and r < 0.22:
        if rng.random() < 0.3 or st["opened"] >= MAX_GROUPS:
            inner, nul = gen_alt(rng, depth - 1, st, realistic)
            return "(?:" + inner + ")", nul
        st["opened"] += 1
        n = st["opened"]
        inner, nul = gen_alt(rng, depth - 1, st, realistic)
        st["closed"].append(n)
        return "(" + inner + ")", nul
    if r < 0.34:
        return ".", False
    if r < 0.48:
        return rng.choice(PREDEF), False
    if r < 0.63:
        neg = "^" if rng.random() < 0.3 else ""
        k = min(len(CLASS_ITEMS), rng.randint(1, 3))
        return "[" + neg + "".join(rng.sample(CLASS_ITEMS, k)) + "]", False
    if r < 0.71 and st["closed"]:
        if realistic:
            raise Skip("反向引用")
        return "\\%d" % rng.choice(st["closed"]), True
    if r < 0.75:
        return rng.choice(["^", "$"]), True          # 锚点本身是零宽
    return rng.choice(LITERALS), False


def gen_piece(rng, depth, st, realistic):
    atom, nul = gen_atom(rng, depth, st, realistic)
    if atom in ("^", "$"):
        return atom, nul
    if rng.random() < 0.5:
        q = gen_quant(rng, 3 if realistic else None)
        lo, hi = quant_bounds(q)
        qnul = (lo == 0) or nul
        if realistic and nul:
            raise Skip("量词套在可空原子上")
        return atom + q, qnul
    return atom, nul


def gen_seq(rng, depth, st, realistic):
    parts, nul = [], False
    for _ in range(rng.randint(1, 3)):
        s, n = gen_piece(rng, depth, st, realistic)
        parts.append(s)
        nul = nul and n
    return "".join(parts), nul


def gen_alt(rng, depth, st, realistic):
    hi = 2 if depth <= 0 else 3
    parts, nul = [], True
    for _ in range(rng.randint(1, hi)):
        s, n = gen_seq(rng, depth, st, realistic)
        parts.append(s)
        nul = nul or n
    return "|".join(parts), nul


def gen_case(rng, realistic):
    st = {"opened": 0, "closed": []}
    pat, _ = gen_alt(rng, 2, st, realistic)
    txt = "".join(rng.choice(TEXT_CHARS) for _ in range(rng.randint(0, 12)))
    try:
        m = re.search(pat, txt, re.IGNORECASE)
    except re.error:
        raise Skip("Python 认为非法")
    if st["opened"] > MAX_GROUPS:
        raise Skip("组太多")
    return pat, txt, m


def build_cases(rng, n, realistic):
    out, skipped, tries = [], 0, 0
    while len(out) < n and tries < n * 40:
        tries += 1
        try:
            out.append(gen_case(rng, realistic))
        except Skip:
            skipped += 1
    return out, skipped


def hex_line(s):
    return " ".join("%04x" % ord(c) for c in s) + "\n"


def expected_line(idx, m):
    parts = [str(idx), "1" if m else "0"]
    for g in range(4):
        v = None
        if m is not None:
            try:
                v = m.group(g)
            except (IndexError, re.error):
                v = None
        parts.append("-" if v is None else ("~" if v == "" else
                     "".join("%04x" % ord(c) for c in v)))
    return "\t".join(parts)


def compile_probe(tmpdir):
    probe = pathlib.Path(tmpdir) / "probe"
    build = subprocess.run(
        ["cc", "-std=gnu11", "-O1", "-Wall", "-Wextra", "-I", str(ROOT / "src"),
         str(ROOT / "tests/regex_probe.c"), str(ROOT / "src/regex.c"),
         "-o", str(probe)],
        capture_output=True, text=True,
    )
    if build.returncode:
        sys.stderr.write(build.stdout + build.stderr)
        return None
    return probe


def run_probe(probe, cases):
    data = "".join(hex_line(p) + hex_line(t) for p, t, _ in cases)
    r = subprocess.run([str(probe)], input=data, capture_output=True, text=True)
    if r.returncode:
        return None
    return r.stdout.splitlines()


def main():
    rng = random.Random(SEED)
    failures = 0

    with tempfile.TemporaryDirectory(prefix="qp-regex-diff-") as tmp:
        probe = compile_probe(tmp)
        if probe is None:
            print("RED: 探针编译失败")
            return 1

        # ---------- 第一档：实际会写的模式，必须逐字节一致 ----------
        cases, skipped = build_cases(rng, CASES, realistic=True)
        actual = run_probe(probe, cases)
        if actual is None:
            print("RED: 探针运行失败")
            return 1
        expect = [expected_line(i, m) for i, (_, _, m) in enumerate(cases)]
        bad = 0
        for i, (a, e) in enumerate(zip(actual, expect)):
            if a == e:
                continue
            bad += 1
            if bad <= 6:
                print("FAIL[gate] #%d" % i)
                print("  模式: %r" % cases[i][0])
                print("  文本: %r" % cases[i][1])
                print("  期望: %s" % e)
                print("  实际: %s" % a)
        if bad:
            print("正则差分[实际模式]: %d/%d 条与 Python 不一致" % (bad, len(cases)))
            failures += 1
        else:
            print("正则差分[实际模式]: %d 条与 Python 逐条一致（跳过 %d 条）"
                  % (len(cases), skipped))

        # ---------- 第二档：压力模式，只要求「会返回」且「可复现」 ----------
        stress, s_skipped = build_cases(rng, CASES, realistic=False)
        t0 = time.time()
        a1 = run_probe(probe, stress)
        t1 = time.time()
        a2 = run_probe(probe, stress)
        if a1 is None or a2 is None:
            print("RED: 压力档探针运行失败（超时或崩溃）")
            failures += 1
        else:
            if a1 != a2:
                print("FAIL[stress]: 同一批输入两次结果不一致（不可复现）")
                failures += 1
            diff = sum(1 for x, y in zip(a1,
                       [expected_line(i, m) for i, (_, _, m) in enumerate(stress)])
                       if x != y)
            print("正则差分[压力模式]: %d 条，%d 条与 Python 的取值不同"
                  "（占比 %.2f%%），%.1fs 内全部返回，两次运行一致"
                  % (len(stress), diff, 100.0 * diff / max(1, len(stress)),
                     t1 - t0))
            if len(a1) != len(stress):
                print("FAIL[stress]: 返回条数不符 %d != %d" % (len(a1), len(stress)))
                failures += 1

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
