# Generic Rule Engine and Table Output Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build generic, template-configured extraction, multi-line processing, tabular output, predictable empty-value semantics, and robust Win32 preview/startup behavior without hard-coding the supplied templates.

**Architecture:** Keep `engine.c` as the platform-independent source of truth and express all scenario-specific behavior through JSON fields and rules. Storage only persists generic flags; Win32 code edits those flags and renders through the engine. Real templates are acceptance fixtures, not branching inputs in production code.

**Tech Stack:** C11, Win32 API, cJSON, MinGW-w64, native C unit tests, Make

**Spec:** `docs/superpowers/specs/2026-09-09-rule-engine-table-output-and-regression-design.md`

## Global Constraints

- Preserve the existing C11 + Win32 API + cJSON stack and static native executable.
- Do not hard-code template names, segment positions, device families, operators, or sample sentences in production code.
- New JSON fields are optional and old templates remain readable.
- Fixed content, explicit defaults, and datetime values may render with no user input; variables with no current source remain empty.
- Actual table column separators are U+0009 tabs; input text is never reinterpreted for escapes.
- Preserve save-time template linting and the nine-session product limit.
- Preserve all unrelated user changes in the dirty worktree.

## File Map

- `src/engine.h`: generic data model fields and public evaluation/rendering declarations.
- `src/engine.c`: empty-value semantics, per-line extraction, placeholder escapes, batch rendering, and linting.
- `src/storage.c`: backward-compatible JSON serialization for generic fields.
- `src/ui.c`: session state, live output, and batch rendering in the main window.
- `src/ui_dialogs.c`: template editor controls, live preview, bounded splitter layout, and save-time lint call.
- `src/main.c`: normal-launch versus `--tray` behavior.
- `src/launch.c`, `src/launch.h`: pure command-line launch policy shared by the Windows entry point and native test.
- `tests/test_engine.c`: platform-independent behavior and regression coverage.
- `tests/test_storage.c`: JSON compatibility and isolated file I/O tests.
- `data.json`: supplied real data used only as a migration/acceptance input.
- `QuickPhrase-模板.json`: deliverable import file for the generic巡检 table template.
- `示例完整.txt`: existing acceptance input fixture; tests read it without duplicating its contents.
- `README.md`, `QuickPhrase.md`: user and developer documentation.

---

### Task 1: Lock Down Correct Empty-Value Semantics

**Files:**
- Modify: `tests/test_engine.c`
- Modify: `src/engine.c`
- Modify: `src/engine.h`
- Modify: `src/ui.c`

**Interfaces:**
- Consumes: `EvalContext`, `eval_set_user_value()`, `eval_run()`, `eval_reset()`.
- Produces: deterministic `EvalField.value` values with no stale derived fallback; `cond_term_hit()` treats empty sources explicitly.

- [ ] **Step 1: Replace the sticky regression expectation with failing stale-value tests**

Add a test that derives a target, clears the source, runs evaluation, and expects the target to be empty:

```c
eval_set_user_value(ctx, source_idx, L"设备NFV-D-HNGZ-17A异常");
eval_run(ctx);
check_str(ctx->fields[target_idx].value, L"NFV-D-HNGZ-17A", "解析命中");
eval_set_user_value(ctx, source_idx, L"");
eval_run(ctx);
check_str(ctx->fields[target_idx].value, L"", "清空来源后派生值清空");
```

Add cases proving an empty source does not satisfy negated `contains`, while `OP_EMPTY` and negated `OP_EMPTY` retain their documented behavior. Add a select without `default_value` and expect an empty value instead of option zero.

- [ ] **Step 2: Run the focused engine tests and confirm failure**

Run: `make build/test_engine && ./build/test_engine`

Expected: the new clear-source and empty-select assertions fail against the current fallback behavior.

- [ ] **Step 3: Remove stale derived fallback from evaluation**

In `field_fallback_of()`, return `user_value` when `user_touched` is true, otherwise return `default_value`; if neither contains text, return `L""`. Do not return `f->sticky`. Stop assigning derived values into `EvalField.sticky` in `eval_run()`. Remove `sticky` and `has_sticky` from `EvalField`, and remove `sticks` plus all allocation, save, restore, and free paths from `Session`.

In `cond_term_hit()`, apply this guard before the non-empty operators and before negation:

```c
if (t->op != OP_EMPTY && qp_is_blank(trigger_value)) return 0;
```

In the initial select fallback path, use only `default_value`; do not select `options[0]` implicitly.

- [ ] **Step 4: Run the engine suite**

Run: `make build/test_engine && ./build/test_engine`

Expected: all assertions pass, including clear-after-input, condition-empty, explicit default, and empty select cases.

- [ ] **Step 5: Commit the semantic correction**

```bash
git add src/engine.c src/engine.h src/ui.c tests/test_engine.c
git commit -m "fix: clear derived values when their source is empty"
```

### Task 2: Complete Generic Multi-Line Extraction and Table Rendering

**Files:**
- Modify: `tests/test_engine.c`
- Modify: `src/engine.h`
- Modify: `src/engine.c`

**Interfaces:**
- Consumes: `ParseRule.per_paragraph`, `Segment.no_output`, `Template.batch_mode`, `Template.batch_source_id`.
- Produces: `qp_between_take()`, `eval_render_batch(EvalContext *, wchar_t *, size_t)` and fixed-content escape expansion.

- [ ] **Step 1: Add failing general-purpose tests**

Cover all rule types with LF and CRLF, blank-line skipping, an unparseable line, single-line equivalence, and batch state restoration. Verify a fixed segment `L"{甲}\\t{乙}"` renders as `L"值甲\t值乙"`, while a user-entered `L"\\t"` remains two literal characters.

Add a content-conservation assertion for extracted fields: concatenate the extracted source-derived values in source order and verify each is a substring of the original row; constants are tested separately as explicit defaults.

- [ ] **Step 2: Run focused tests and confirm any incomplete cases fail**

Run: `make build/test_engine && ./build/test_engine`

Expected: newly uncovered CRLF, empty-line, or state-restoration failures are visible before production changes.

- [ ] **Step 3: Finish the generic implementation**

Ensure `parse_rule_apply()` splits on `\n`, removes one trailing `\r`, skips blank rows, and applies the unchanged underlying rule to each row. Ensure `eval_render_batch()` snapshots the source user state, evaluates every nonblank row independently, appends one `\n` between rendered rows, and restores the full source followed by `eval_run()`.

Keep escape handling inside static placeholder expansion:

```c
case L't': append_char(out, cap, &used, L'\t'); break;
case L'n': append_char(out, cap, &used, L'\n'); break;
case L'\\': append_char(out, cap, &used, L'\\'); break;
```

Skip `no_output` fields only in final rendering, never in evaluation or placeholder lookup.

- [ ] **Step 4: Run engine tests under sanitizers**

Run: `cc -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -Isrc -o /tmp/qp-test-engine tests/test_engine.c src/engine.c && /tmp/qp-test-engine`

Expected: every assertion passes and sanitizers report no error.

- [ ] **Step 5: Commit the generic pipeline**

```bash
git add src/engine.c src/engine.h tests/test_engine.c
git commit -m "feat: add generic multiline table rendering"
```

### Task 3: Persist Generic Fields and Isolate Storage Tests

**Files:**
- Modify: `tests/test_storage.c`
- Modify: `src/storage.c`
- Modify: `src/storage.h`

**Interfaces:**
- Consumes: engine model fields from Task 2.
- Produces: optional JSON keys `markerAfter`, `perParagraph`, `noOutput`, `batchMode`, and `batchSourceId`; a test-only writable data directory setup.

- [ ] **Step 1: Add JSON compatibility and test-directory assertions**

Test each new field through serialize/parse/serialize. Parse an old JSON object lacking every new field and assert `0`/empty defaults. Before file I/O assertions, call:

```c
storage_set_data_dir(L"/tmp/quickphrase-storage-test");
```

Use an explicit unique filename or directory for the test run and restore `storage_set_data_dir(NULL)` before exit.

- [ ] **Step 2: Run storage tests and verify the pre-fix result**

Run: `make build/test_storage && ./build/test_storage`

Expected: compatibility assertions expose missing persistence if any; the previous `/root/.quickphrase` permission failure is eliminated by test isolation.

- [ ] **Step 3: Complete JSON read/write and validation**

Serialize truthy optional booleans and batch source only when batch mode is enabled. Parse missing keys to zero/empty. Reject invalid parse types by falling back to `split`, clamp indexes to at least 1, and clear batch source IDs that no longer identify text segments when the editor saves.

- [ ] **Step 4: Run storage and engine suites**

Run: `make test`

Expected: zero failures in both suites.

- [ ] **Step 5: Commit storage compatibility**

```bash
git add src/storage.c src/storage.h tests/test_storage.c
git commit -m "feat: persist generic table template options"
```

### Task 4: Make Every Template Edit Refresh Preview and Preserve Linting

**Files:**
- Modify: `src/ui_dialogs.c`
- Modify: `src/resource.h`

**Interfaces:**
- Consumes: `tmpl_render_defaults()`, `tmpl_lint()` and generic model fields.
- Produces: a single `ed_update_preview(EditorCtx *)` refresh path after every committed edit; unchanged save-time lint gate.

- [ ] **Step 1: Audit every editor mutation path**

Build a checklist for segment add/delete/move/type/property, option add/edit/delete/move, reference source/mode/parameters, parse rule add/edit/delete/move, condition rule add/edit/delete/move, batch toggle/source, and drag reorder. Each successful mutation must update the model first and then call `ed_update_preview(c)`.

- [ ] **Step 2: Compile the audited mutation paths**

Keep model mutation in the existing focused helpers and make `ed_update_preview(c)` the final action of every successful mutation path identified in Step 1. Run `make app` with the repository's warning flags to catch unused control IDs and invalid branches.

- [ ] **Step 3: Preserve the save-time lint call**

Keep this order in `IDOK`: commit current properties, validate names/references, call `ed_check_lint(hwnd, c)`, and only close when it returns true. Confirm `ed_check_lint()` calls `tmpl_lint()` and selects the reported segment when the user declines to continue.

- [ ] **Step 4: Cross-compile the application**

Run: `make app`

Expected: `build/QuickPhrase.exe` is created with no compiler error.

- [ ] **Step 5: Commit preview and lint behavior**

```bash
git add src/ui_dialogs.c src/resource.h
git commit -m "fix: refresh template preview after every rule edit"
```

### Task 5: Bound the Editor Layout During Preview Dragging

**Files:**
- Modify: `src/ui_dialogs.c`

**Interfaces:**
- Consumes: `ed_preview_bounds()`, `ed_splitter_y()`, `ed_metrics()`, `ed_layout()`.
- Produces: bounded upper editor geometry and a scrollable or clipped properties area that cannot overlap the preview.

- [ ] **Step 1: Record layout invariants in code assertions for debug builds**

At the end of geometry calculation, verify:

```c
assert(bodybot >= DPIX(130));
assert(prevY >= bodybot + ED_SPLIT_HIT);
assert(prevY + prevH <= btnY - DPIX(8));
```

Use the exact relation dictated by the final geometry; assertions must describe real non-overlap rather than alter release behavior.

- [ ] **Step 2: Reproduce through all geometry entry points**

Exercise minimum editor size, maximized size, saved preview heights outside current DPI bounds, drag to both limits, double-click reset, and window resize after dragging. Record which control rectangle crosses `bodybot` before changing layout.

- [ ] **Step 3: Implement one authoritative boundary**

Compute `bodybot` once per layout and pass it into the property layout. Clamp list heights to zero or greater. Place property controls in a child host clipped to `[top, bodybot)` and add vertical scrolling when their total required height exceeds the host client height. Invalidate both the previous and current splitter bands during drag; avoid calling independent layout formulas with different constants.

- [ ] **Step 4: Build and perform native UI checks**

Run: `make app`

On Windows, repeat Step 2 at 100%, 125%, and 150% display scaling. Expected: no overlap, residual line, clipped button, or negative control size.

- [ ] **Step 5: Commit the layout correction**

```bash
git add src/ui_dialogs.c
git commit -m "fix: keep editor controls above preview splitter"
```

### Task 6: Correct Normal Startup and Existing-Instance Activation

**Files:**
- Create: `src/launch.c`
- Create: `src/launch.h`
- Create: `tests/test_launch.c`
- Modify: `src/main.c`
- Modify: `Makefile`
- Modify: `README.md`

**Interfaces:**
- Consumes: `cmdline_has()`, `app_show_main()`, registered show message.
- Produces: `int qp_should_show_main(const wchar_t *cmdline)`; visible normal launch, silent explicit tray launch, foreground activation for an existing instance.

- [ ] **Step 1: Extract command-line launch policy**

Define a pure helper returning whether the window should show:

```c
int qp_should_show_main(const wchar_t *cmdline)
{
    return !qp_cmdline_has(cmdline, L"--tray") &&
           !qp_cmdline_has(cmdline, L"/tray");
}
```

Declare the helper in `src/launch.h`. Add assertions to `tests/test_launch.c`: empty command line and `--show` return true, while `--tray` and `/tray` return false. Add `build/test_launch` to `make test`, linking only `tests/test_launch.c` and `src/launch.c` so the test creates no window.

- [ ] **Step 2: Apply the launch policy**

Include `launch.h` from `main.c` and replace the first-run-only visibility branch with `qp_should_show_main(lpCmdLine)`. Retain the first-run educational notification separately. Do not change the autostart registry command, which must remain `"QuickPhrase.exe" --tray`.

- [ ] **Step 3: Verify second-instance and tray activation paths**

Confirm the registered broadcast message and tray click both call `app_show_main()`. For a minimized window, restore before `BringWindowToTop()` and `SetForegroundWindow()`.

- [ ] **Step 4: Build and document behavior**

Run: `make app`

Update the README command-line table so no-argument launch says it displays the main window; `--tray` remains the explicit background mode.

- [ ] **Step 5: Commit startup behavior**

```bash
git add src/launch.c src/launch.h src/main.c tests/test_launch.c Makefile README.md
git commit -m "fix: show the main window on normal launch"
```

### Task 7: Build Real, Importable Acceptance Templates Without Hard-Coding

**Files:**
- Modify: `data.json`
- Modify: `QuickPhrase-模板.json`
- Modify: `tests/test_storage.c`
- Modify: `tests/test_engine.c`

**Interfaces:**
- Consumes: only public generic JSON fields and engine APIs from Tasks 1–3.
- Produces: migrated “日间管控” and importable generic “巡检” table template.

- [ ] **Step 1: Add fixture-driven template evaluation tests**

Load `data.json`, locate “日间管控” by name only in test code, locate its 19th and 5th segments, feed one device row and multiple device rows, and assert strict fourth-dash-segment extraction. Load `QuickPhrase-模板.json`, locate “巡检”, feed all six nonempty lines from `示例完整.txt`, and assert output row count and tab column count.

- [ ] **Step 2: Run acceptance tests and confirm current JSON fails**

Run: `make test`

Expected: 日间 multi-line and 巡检 batch/table assertions fail before template migration.

- [ ] **Step 3: Migrate 日间管控 declaratively**

Set `perParagraph: true` on the existing parse rule owned by segment 19 and targeting segment 5. Do not add a production-code condition for template name or segment number.

- [ ] **Step 4: Rebuild the 巡检 template declaratively**

Set `batchMode: true`, point `batchSourceId` at the multiline source, mark source and intermediate variables `noOutput: true`, configure generic `between`/`split`/condition rules, and add one final static row containing placeholders separated by `\\t`. Constants such as status, handler state, and empty spreadsheet columns must be explicit template defaults/static content.

- [ ] **Step 5: Verify exact acceptance output**

Run the test renderer on `示例完整.txt`. Assert six output rows, identical tab counts for every row, no literal `\\t`, and expected sample cells including complete device IDs. Save the rendered text under `/tmp` for manual spreadsheet paste verification, not as a repository artifact.

- [ ] **Step 6: Commit templates and acceptance tests**

```bash
git add data.json QuickPhrase-模板.json tests/test_engine.c tests/test_storage.c
git commit -m "feat: add importable multiline inspection template"
```

### Task 8: Documentation, Full Regression, and Release Evidence

**Files:**
- Modify: `README.md`
- Modify: `QuickPhrase.md`
- Modify: `Makefile` only if a deterministic verification target is needed.

**Interfaces:**
- Consumes: all completed tasks.
- Produces: documented generic behavior and repeatable verification results.

- [ ] **Step 1: Update documentation**

Document empty-source semantics, explicit select defaults, `between`, `perParagraph`, `noOutput`, batch source behavior, static `\\t`/`\\n`, normal versus tray startup, and preserved save-time template linting. Examples must be illustrative and explicitly state that rules work for arbitrary templates.

- [ ] **Step 2: Run formatting and diff checks**

Run: `git diff --check`

Expected: no whitespace errors.

- [ ] **Step 3: Run all native tests with sanitizers**

Run: `make test`

Run: `cc -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -Isrc -o /tmp/qp-test-engine tests/test_engine.c src/engine.c && /tmp/qp-test-engine`

Run: `cc -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -Isrc -o /tmp/qp-test-storage tests/test_storage.c src/engine.c src/storage.c src/cJSON.c && /tmp/qp-test-storage`

Expected: zero failed assertions, no sanitizer findings.

- [ ] **Step 4: Build all Windows artifacts**

Run: `make clean && make`

Expected: application, uninstaller, and installer are produced successfully.

- [ ] **Step 5: Perform the Windows UI matrix**

Verify normal/`--show`/`--tray` startup, second-instance activation, template import, input then clear, reset, session switching, all editor mutation preview paths, preview dragging at three DPI scales, save-time lint warning and navigation, copy to Excel, and uninstall-safe startup behavior. Record any platform limitation rather than claiming an unperformed check.

- [ ] **Step 6: Request code review and address findings**

Use `superpowers:requesting-code-review`, review the complete diff against the spec, and correct only verified issues. Re-run Steps 2–5 after any correction.

- [ ] **Step 7: Commit final documentation and verification support**

```bash
git add README.md QuickPhrase.md Makefile
git commit -m "docs: document generic parsing and table workflows"
```
