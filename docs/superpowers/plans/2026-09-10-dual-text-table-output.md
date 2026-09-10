# Dual Text and Table Output Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give every template an optional, explicit table output alongside its ordinary text output, with live mode switching and one-click copying of the selected format.

**Architecture:** Keep evaluation values shared, but make final rendering mode explicit: ordinary rendering consumes ordered segments, while table rendering expands a template-level format string. Persist the optional table definition in JSON, expose it in the existing Win32 editor and main preview, and migrate the supplied inspection template away from its hidden synthetic row segment.

**Tech Stack:** C11, Win32 API, cJSON, MinGW-w64, native C unit tests, Make

**Spec:** `docs/superpowers/specs/2026-09-10-dual-text-table-output-design.md`

## Global Constraints

- Preserve the existing C11, Win32 API and cJSON stack; add no Office or runtime dependency.
- Do not hard-code template names, labels, column counts or sample sentences in production code.
- Old JSON without table fields must load with ordinary-text-only behavior.
- Table separators copied to the clipboard must be actual U+0009 characters.
- `hidden` controls the filling UI and `noOutput` controls ordinary segment output; neither may prevent table placeholders from reading a value.
- Ordinary text is the default mode whenever a template is selected.
- Preserve live preview, save-time linting, batch processing and unrelated dirty-worktree changes.
- The repository intentionally ignores `/tests/` and `/QuickPhrase.md`; update them as local verification/developer artifacts, but do not force-add them or change `.gitignore` as part of this feature.

## File Map

- `src/engine.h`: table configuration fields, render-mode enum and public rendering declarations.
- `src/engine.c`: ordinary/table render dispatch, batch rendering for either mode, cloning and table lint.
- `src/storage.c`: optional `tableOutputEnabled` and `tableOutputFormat` JSON persistence.
- `tests/test_engine.c`: rendering, hidden/no-output, batch, lint and clone regressions.
- `tests/test_storage.c`: new/old JSON round-trip coverage.
- `src/resource.h`: IDs for main-window mode controls and editor table controls.
- `src/ui.c`: current preview mode, mode switch, live preview and copy labeling.
- `src/ui_dialogs.c`: table configuration editor, insert-variable action and editor preview mode.
- `QuickPhrase-模板.json`, `data.json`: migrated inspection template.
- `README.md`, `QuickPhrase.md`: authoring and user workflow documentation.

---

### Task 1: Add Explicit Engine Rendering Modes

**Files:**
- Modify: `tests/test_engine.c`
- Modify: `src/engine.h`
- Modify: `src/engine.c`

**Interfaces:**
- Consumes: `qp_expand_placeholders()`, `eval_render()`, current `eval_render_batch()` behavior.
- Produces: `OutputMode { OUTPUT_TEXT, OUTPUT_TABLE }`, `eval_render_mode(EvalContext *, OutputMode, wchar_t *, size_t)`, and `eval_render_batch_mode(EvalContext *, OutputMode, wchar_t *, size_t)`.

- [ ] **Step 1: Write failing engine tests**

Create a template containing a visible source, a hidden `no_output` derived field and ordinary fixed text. Set `table_output_enabled = 1` and `table_output_format = L"{来源}\\t{派生}"`. Assert ordinary mode renders ordered non-`no_output` segments, table mode renders `L"原文\t结果"`, and the Tab is one actual U+0009 character. Add batch assertions proving both modes evaluate each source line independently and restore the full input.

- [ ] **Step 2: Run the focused suite and observe RED**

Run: `make build/test_engine && ./build/test_engine`

Expected: compilation fails because the new template fields, enum and render functions do not exist.

- [ ] **Step 3: Implement the minimal model and render dispatch**

Add bounded storage to `Template`:

```c
int table_output_enabled;
wchar_t table_output_format[QP_OUTPUT_MAX];
```

Add:

```c
typedef enum { OUTPUT_TEXT = 0, OUTPUT_TABLE = 1 } OutputMode;
size_t eval_render_mode(EvalContext *ctx, OutputMode mode,
                        wchar_t *out, size_t cap);
size_t eval_render_batch_mode(EvalContext *ctx, OutputMode mode,
                              wchar_t *out, size_t cap);
```

For `OUTPUT_TABLE`, return empty unless enabled and format is nonempty; otherwise expand the format through the existing evaluated-field placeholder lookup. Keep `eval_render()` and `eval_render_batch()` as compatibility wrappers for `OUTPUT_TEXT`. Copy the table fields in `tmpl_clone()`.

- [ ] **Step 4: Verify GREEN and sanitizers**

Run: `make build/test_engine && ./build/test_engine`

Run: `cc -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -Isrc -o /tmp/qp-dual-engine tests/test_engine.c src/engine.c && ASAN_OPTIONS=detect_leaks=0 /tmp/qp-dual-engine`

Expected: all assertions pass with no sanitizer error.

- [ ] **Step 5: Commit the engine unit**

```bash
git add src/engine.h src/engine.c
git commit -m "feat: add explicit text and table rendering"
```

### Task 2: Persist and Validate Table Definitions

**Files:**
- Modify: `tests/test_storage.c`
- Modify: `tests/test_engine.c`
- Modify: `src/storage.c`
- Modify: `src/engine.c`

**Interfaces:**
- Consumes: `Template.table_output_enabled`, `Template.table_output_format`, `qp_placeholder_at()`, `tmpl_lint()`.
- Produces: optional JSON keys `tableOutputEnabled`, `tableOutputFormat`; lint issues for invalid table definitions.

- [ ] **Step 1: Write failing storage and lint tests**

Round-trip an enabled format `L"{甲}\\t{乙}"`; assert both fields survive. Load legacy JSON without the fields and assert disabled/empty defaults. Add lint tests for enabled-empty format, unknown placeholder, duplicate-label placeholder and enabled format containing no `\\t`; verify a valid format has none of these issues.

- [ ] **Step 2: Run tests and observe RED**

Run: `make build/test_engine build/test_storage && ./build/test_engine && ./build/test_storage`

Expected: round-trip and new lint expectations fail.

- [ ] **Step 3: Add backward-compatible persistence and lint**

In `template_to_json()`, write `tableOutputEnabled` when true and `tableOutputFormat` when nonempty. In `template_from_json()`, default missing fields to zero/empty. Extend `tmpl_lint()` without removing existing checks: count matching labels for every table placeholder, report zero or multiple matches, report enabled-empty, and report a nonempty enabled format with no literal `\\t` escape and no actual Tab.

- [ ] **Step 4: Run the full native tests**

Run: `make test`

Expected: engine, storage and launch tests all pass.

- [ ] **Step 5: Commit persistence and validation**

```bash
git add src/storage.c src/engine.c
git commit -m "feat: persist and validate table output formats"
```

### Task 3: Expose Table Authoring in the Template Editor

**Files:**
- Modify: `src/resource.h`
- Modify: `src/ui_dialogs.c`

**Interfaces:**
- Consumes: engine render modes and table template fields from Tasks 1–2.
- Produces: table enable checkbox, format editor, variable insertion control and editor preview mode switch.

- [ ] **Step 1: Add a geometry/state harness before UI production changes**

Extend the existing editor layout harness or create a pure helper test covering minimum/maximized window sizes, table controls hidden/shown and preview dragged to both bounds. Assert the table editor remains clipped above the splitter and all visible controls have nonnegative dimensions.

- [ ] **Step 2: Run the harness and observe RED**

Run the repository's editor geometry harness command documented in the previous task report, or compile the extracted helper with `HOSTCC` when the harness is local.

Expected: the new table-control cases cannot compile or fail because their layout/state does not exist.

- [ ] **Step 3: Implement editor controls and live preview**

Add control IDs and `EditorCtx` members for enable checkbox, multiline format edit, insert-variable combo/button, and text/table preview radio buttons. On every change, first write to the cloned `Template`, then call `ed_update_preview()`. Insert exactly `L"{" + label + L"}"` at the format edit selection. Disable table preview and format controls when table output is off; automatically return editor preview to text mode when disabling it. Route editor rendering through `eval_render_mode()` or `eval_render_batch_mode()`.

- [ ] **Step 4: Preserve layout and save checks**

Place the new authoring controls inside the existing scrollable property host or another bounded host above the preview splitter. Keep `ed_check_lint()` in the save path after current control values are committed. Run: `make app`

Expected: `build/QuickPhrase.exe` compiles without warnings or errors.

- [ ] **Step 5: Commit the editor unit**

```bash
git add src/resource.h src/ui_dialogs.c
git commit -m "feat: add table format controls to template editor"
```

### Task 4: Add Main Preview Mode Switching and Mode-Aware Copy

**Files:**
- Modify: `src/resource.h`
- Modify: `src/ui.c`

**Interfaces:**
- Consumes: `eval_render_mode()` and `eval_render_batch_mode()`.
- Produces: an ordinary/table selection in the preview header and one copy action tied to the selected mode.

- [ ] **Step 1: Define testable mode-selection rules**

Extract a pure decision helper whose inputs are requested mode, template enabled flag and nonempty format, and whose output is the effective mode. Add assertions: new template selection yields text; disabled/empty table always yields text; enabled nonempty table accepts table.

- [ ] **Step 2: Run the helper test and observe RED**

Run: `make test`

Expected: compilation fails until the decision helper exists.

- [ ] **Step 3: Implement main-window controls and shared renderer**

Add two radio-style buttons next to the preview label. Reset to `OUTPUT_TEXT` in `ui_select_template()`. Hide or disable table selection when unavailable. Make `ui_refresh_current()` and `ui_copy_current()` call one shared mode-aware render helper so preview and clipboard cannot diverge. Change copy button and hints to “一键复制文本/表格” and state that table output can be pasted into Excel. Empty output must not call the clipboard function.

- [ ] **Step 4: Verify resize, keyboard and build behavior**

Ensure `Ctrl+Enter` still invokes `ui_copy_current()`, radio buttons survive light/dark theming, and preview controls remain within the right column at minimum width and common DPI scaling. Run: `make app && make test`

Expected: Windows build succeeds and all native tests pass.

- [ ] **Step 5: Commit the main UI unit**

```bash
git add src/resource.h src/ui.c
git commit -m "feat: switch and copy text or table previews"
```

### Task 5: Migrate the Delivered Inspection Template

**Files:**
- Modify: `QuickPhrase-模板.json`
- Modify: `data.json`
- Modify: `tests/test_storage.c`

**Interfaces:**
- Consumes: new JSON keys and both rendering modes.
- Produces: an importable inspection template with visible raw input and explicit table format.

- [ ] **Step 1: Write a failing acceptance test against both JSON files**

Load each file, locate the inspection template by structural properties rather than production branching, and assert: source field is visible and participates in text; table output is enabled; format has real table semantics; the synthetic row segment is absent. Feed every nonempty row from `示例完整.txt`; ordinary mode must contain the untouched input, table mode must produce the same number of rows and the documented column count.

- [ ] **Step 2: Run the acceptance test and observe RED**

Run: `make build/test_storage && ./build/test_storage`

Expected: the current synthetic-row configuration violates the new assertions.

- [ ] **Step 3: Migrate only template configuration**

Set `tableOutputEnabled: true`, move the existing row expression into `tableOutputFormat`, clear `noOutput` on “巡检数据”, and remove `seg_inspect_row`. Keep rule-derived intermediate fields hidden and `noOutput`. Apply the same migration to the matching template in `data.json`; do not alter unrelated templates or sample wording.

- [ ] **Step 4: Validate JSON and acceptance output**

Run: `jq -e . data.json >/dev/null && jq -e . 'QuickPhrase-模板.json' >/dev/null && make test`

Expected: both JSON files parse and all acceptance assertions pass. Inspect the saved TSV fixture with `od` or `xxd` to confirm separators are byte `09` after UTF-8 conversion.

- [ ] **Step 5: Commit the deliverable migration**

```bash
git add data.json QuickPhrase-模板.json
git commit -m "feat: deliver explicit inspection table output"
```

### Task 6: Document, Audit and Verify the Complete Workflow

**Files:**
- Modify: `README.md`
- Modify: `QuickPhrase.md`

**Interfaces:**
- Consumes: completed UI and template behavior.
- Produces: user-facing instructions and final evidence.

- [ ] **Step 1: Update documentation**

Document the two preview modes, ordinary-text default, authoring `tableOutputFormat`, variable insertion, literal `\\t` versus clipboard Tab, batch behavior, and one-click/`Ctrl+Enter` copying. Remove guidance that recommends a synthetic fixed row as the primary table mechanism; retain it only as backward-compatible legacy behavior if mentioned.

- [ ] **Step 2: Run comprehensive verification**

Run: `make clean && make test && make`

Run: `jq -e . data.json >/dev/null && jq -e . 'QuickPhrase-模板.json' >/dev/null && git diff --check`

Run the Clang ASan/UBSan engine and storage binaries with leak detection disabled only if ptrace restrictions require it.

Expected: every test passes, both JSON files validate, the full Windows app/installer build succeeds, sanitizers report no memory/undefined-behavior issue and diff check is clean.

- [ ] **Step 3: Perform or record native Windows UI acceptance**

Verify import, visible raw input, text/table switching, live editor preview, one-click copy into Excel, `Ctrl+Enter`, splitter drag at 100/125/150/200% DPI and save-time lint. If the current environment cannot execute native Windows UI, record each item explicitly as pending rather than claiming it passed.

- [ ] **Step 4: Request final code review and fix required findings**

Review the complete diff against `docs/superpowers/specs/2026-09-10-dual-text-table-output-design.md`, focusing on backward compatibility, preview/copy parity, buffer bounds, stale state and UI geometry. Re-run Step 2 after every required fix.

- [ ] **Step 5: Commit documentation and verified fixes**

```bash
git add README.md
git commit -m "docs: explain text and table output modes"
```
