from source_helpers import read_source, INCLUDE_FLAGS, CORE_SOURCES
"""Check Task 4's main-preview header geometry at supported DPI scales.

This is a local layout harness, not a substitute for native Windows painting.
It exercises the production dimensions used by ui_layout against the window's
minimum size and a wider client area, including the largest permitted tree pane.
"""
import pathlib
import re
import sys

repo = pathlib.Path(__file__).resolve().parents[1]
source = read_source("ui.c")

required = [
    r"g_hPreviewTextMode",
    r"g_hPreviewTableMode",
    r"previewLabelW\s*=\s*DPIX\(96\)",
    r"modeW\s*=\s*DPIX\(62\)",
    r"rx\s*\+\s*previewLabelW\s*,\s*y\s*-\s*DPIX\(20\)",
    r"rx\s*\+\s*previewLabelW\s*\+\s*modeW\s*,\s*y\s*-\s*DPIX\(20\)",
]
missing = [pattern for pattern in required if not re.search(pattern, source)]
if missing:
    print("RED: main preview controls/layout are incomplete: " + ", ".join(missing))
    sys.exit(1)

failures = 0
cases = 0
for dpi in (96, 120, 144, 192):
    def px(logical):
        return (logical * dpi + 48) // 96

    for logical_width, logical_height in ((680, 460), (1280, 820)):
        width, height = px(logical_width), px(logical_height)
        # ui_tree_width_for() permits at most W - DPIX(320), and a user may
        # have dragged the tree there.  Test that worst-case right column.
        tree_width = width - px(320)
        rx = px(8) + tree_width + px(10)
        right_width = width - rx - px(8)
        preview_height = max(px(64), min(height // 4, px(200)))
        preview_y = height - px(8) - px(28) - px(6) - preview_height
        label = (rx, preview_y - px(18), px(96), px(16))
        text = (rx + px(96), preview_y - px(20), px(62), px(18))
        table = (rx + px(96) + px(62), preview_y - px(20), px(62), px(18))
        cases += 1
        for name, rect in (("label", label), ("text", text), ("table", table)):
            x, y, w, h = rect
            if x < rx or y < 0 or w < 0 or h < 0 or x + w > rx + right_width:
                print("FAIL dpi=%d size=%dx%d %s=%r right=(%d,%d)" %
                      (dpi, logical_width, logical_height, name, rect, rx, right_width))
                failures += 1

print("%d main-preview header geometry cases, %d failures" % (cases, failures))
sys.exit(1 if failures else 0)
