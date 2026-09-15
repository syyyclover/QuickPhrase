"""Exercise Task 4's pure main-preview mode decision helper.

The application UI is Win32-only, so this local harness extracts the helper from
the production source and executes it with the host C compiler.  It also models
the source-declared Win32 radio-group boundary used by IsDialogMessageW.  Run
with `make test`.
"""
import pathlib
import re
import subprocess
import sys
import tempfile

repo = pathlib.Path(__file__).resolve().parents[1]
source = (repo / "src/ui.c").read_text()
match = re.search(
    r"static\s+OutputMode\s+ui_effective_output_mode\s*\([^;]+?\)\s*\n\{",
    source,
)
if not match:
    print("RED: missing production pure helper ui_effective_output_mode")
    sys.exit(1)

start = match.start()
depth = 0
end = None
for index in range(source.index("{", match.start()), len(source)):
    if source[index] == "{":
        depth += 1
    elif source[index] == "}":
        depth -= 1
        if depth == 0:
            end = index + 1
            break
if end is None:
    print("RED: malformed production helper ui_effective_output_mode")
    sys.exit(1)

helper = source[start:end]
driver = r'''
#include <stdio.h>
#include "engine.h"

static int failures;
static void require(int yes, const char *what) {
    if (!yes) { printf("FAIL: %s\n", what); ++failures; }
}
'''+ helper + r'''

int main(void) {
    /* A new template selection explicitly requests ordinary text. */
    require(ui_effective_output_mode(OUTPUT_TEXT, 1, L"{甲}\\t{乙}") == OUTPUT_TEXT,
            "a new template selection starts in text mode");
    require(ui_effective_output_mode(OUTPUT_TABLE, 0, L"{甲}\\t{乙}") == OUTPUT_TEXT,
            "disabled table output falls back to text");
    require(ui_effective_output_mode(OUTPUT_TABLE, 1, L"") == OUTPUT_TEXT,
            "empty table format falls back to text");
    require(ui_effective_output_mode(OUTPUT_TABLE, 1, L"{甲}\\t{乙}") == OUTPUT_TABLE,
            "enabled nonempty table format accepts table mode");
    if (!failures) puts("main preview mode decision checks passed");
    return failures ? 1 : 0;
}
'''

with tempfile.TemporaryDirectory(prefix="qp-task4-preview-") as tmp:
    exe = pathlib.Path(tmp) / "preview-mode"
    result = subprocess.run(
        ["cc", "-std=gnu11", "-O0", "-Wall", "-Wextra", "-I",
         str(repo / "src"), "-x", "c", "-", "-o", str(exe)],
        input=driver, text=True,
    )
    if result.returncode:
        sys.exit(result.returncode)
    result = subprocess.run([str(exe)])
    if result.returncode:
        sys.exit(result.returncode)

# GetNextDlgGroupItem stops at the next sibling carrying WS_GROUP.  Keep this
# source-backed interaction check narrow: the preview control must begin its own
# group, so right/left navigation can only traverse the text/table radios.
# 主界面预览现在由 preview.c 的自绘控件承担（文本 / 表格共用），所以这里检查
# 「主界面确实用 qp_preview_create 建了预览」+「那个工厂带 WS_GROUP、不进 Tab 顺序」。
preview = re.search(
    r"g_hPreview\s*=\s*qp_preview_create\s*\(\s*hwnd\s*,\s*IDC_PREVIEW\s*,",
    source,
)
if not preview:
    print("RED: missing main preview control creation")
    sys.exit(1)

preview_src = (repo / "src/preview.c").read_text()
factory = re.search(
    r"HWND\s+qp_preview_create\s*\([^)]*\)\s*\n\{.*?\n\}\n",
    preview_src, re.S,
)
if not factory:
    print("RED: missing qp_preview_create in src/preview.c")
    sys.exit(1)
factory_body = factory.group(0)
# 只看代码，注释里提到 WS_TABSTOP 不算
factory_code = re.sub(r"/\*.*?\*/", " ", factory_body, flags=re.S)
if "WS_GROUP" not in factory_code:
    print("RED: preview control must close the output-mode radio group")
    sys.exit(1)
if "WS_TABSTOP" in factory_code:
    print("RED: preview control group boundary must not change tab navigation")
    sys.exit(1)

radio_group = ["text", "table"]
def next_enabled(current, table_enabled, direction):
    enabled = ["text"] + (["table"] if table_enabled else [])
    index = enabled.index(current)
    return enabled[(index + direction) % len(enabled)]

if (next_enabled("table", True, 1) != "text" or
        next_enabled("text", True, -1) != "table" or
        next_enabled("text", False, 1) != "text" or
        next_enabled("text", False, -1) != "text"):
    print("FAIL: output-mode radio interaction does not wrap within its group")
    sys.exit(1)
print("main preview radio group interaction checks passed")
