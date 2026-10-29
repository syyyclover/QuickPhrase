# QuickPhrase 绘制与全量文案 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Follow the master plan's selected superpowers:executing-plans or superpowers:subagent-driven-development workflow. Native UI conclusions need native evidence.

**Goal:** 建立不会主动掩盖残影的验收，整理布局/绘制边界和全部产品引导。

**Architecture:** 先自然绘制诊断，再集中布局提交损坏区域；编辑器布局独立、窄窗口页签化；引导清单与实际行为和字体度量一起验证。

**Tech Stack:** C11、Win32 HWND/GDI、纯 C 几何、Python 契约检查。

**Spec:** `docs/superpowers/specs/2026-10-07-quickphrase-350-hardening-design.md`。

## Global Constraints

逐字沿用[总计划 Global Constraints](2026-10-07-quickphrase-350-hardening.md#global-constraints)。A8 的 QpRenderResult 是 UI 完整/失败状态的唯一来源，不另造显示串作为复制数据。

## Review Focus

自然重绘与参考不同；非客户区边框；绘制缓冲失败；窄/矮工作区与焦点滚动；引导所说的取消、空值、更新和热键必须真实。B1–B4 各有相应断言。

## 文件结构

- `src/ui/layout_queue.c`：从 main_host 提取私有布局队列，负责同父原生批次、失败重放及损坏区域。
- `src/ui/editor_internal.h`：EditorCtx 与编辑器模块的私有声明；`src/ui/editor_layout.c`：几何、显隐和滚动收敛。
- `src/core/editor_layout.c/.h`：不含 windows.h 的响应式编辑器外框计算，单位是输入 DPI 下的物理像素。
- `src/ui/text_paint.c`、`preview.c`：局部绘制和失败直接绘制降级；模型构建不混进绘制函数。
- `docs/ui-copy-review.json`：用户可见字符串审查清单；`tools/audit_ui_copy.py` 与回归负责覆盖，具体文案就近放在所属模块。
- `tests/source_helpers.py` 及提取测试使用新位置，不继续依赖旧 ui.c 拼接的偶然顺序。

### Task 1: B1 原生自然绘制对照，先暴露测试盲区

**Files:** Modify `tests/test_win32.c`, `tests/run_win32.py`; Create `docs/windows-display-check.md`。

**Interfaces:** 测试内部 `check_natural_paint(HWND,const char *case_name)`：泵送已有消息，截取目标窗口客户区实际屏幕像素（不使用会主动调用绘制的 PrintWindow），再参考全重绘。只捕获测试窗口区域，图片存唯一临时目录；遮挡或桌面不可用报告跳过，不伪报通过。

- [ ] 添加“测试探针”子窗口，故意在尺寸变化后保留错误旧像素；验证自然/参考比较确实报失败。移除每次 SetWindowPos 后的先验整窗失效；对照阶段才允许 ALLCHILDREN/FRAME 重绘。
- [ ] 原生案例：单改宽、单改高、缩小放大 30 轮、预览条上下拖、属性顶底滚动，96/120/144/192 DPI 和深浅主题；使用实际缩放字体，隐藏/屏蔽光标及动画差异。
- [ ] `make build/test_win32.exe` 检查编译，`make win32-test` 尝试运行。支持 `--artifacts <dir>` 保留 BMP 和差异统计，run_win32 使用唯一隔离 profile、不删除用户数据。
- [ ] 若 Windows/Wine 不能运行，记录此任务“诊断程序已构建，视觉断言未执行”。提供新测试账号原生运行步骤，不用 mock 结果完成视觉验收。

### Task 2: B2 集中布局事务与正确绘制降级

**Files:** Create `src/ui/layout_queue.c`, `tests/layout_damage.py`, `tests/paint_failures.py`; Modify `src/ui/main_host.c`, `src/ui/ui.h`, `src/ui/dialog_editor.c`, `src/ui/main_layout.c`, `src/ui/text_paint.c`, `src/ui/preview.c`, `tests/layout_fallback.py`, `tests/source_helpers.py`。

**Interfaces:** 继续 `HDWP qp_layout_begin(int)`、`HDWP qp_defer(HDWP,HWND,int,int,int,int)`、`void qp_layout_end(HDWP)`，保持私有队列配对；队列记录父 HWND 和旧/新损坏区域。跨父输入先提交当前原生批次，再建立下一批，回退不丢任何位置。纯移动保持既定 NOCOPYBITS 规则。

- [ ] `layout_damage.py` 验证旧露出区、新变尺寸区、必要边框均排队失效；顺序移动/原生 Begin-Defer-End/队列 realloc 失败时位置完整，分配数回基线。`paint_failures.py` 注入 DC/位图/文字分配失败，断言有正确直接绘制或明确保留更新请求，不得 EndPaint 后无任何内容。
- [ ] 先执行两测试及 `python3 tests/layout_fallback.py` 确认 RED。
- [ ] 提取布局队列，合并异步失效，不加每控件 UPDATENOW；复用共同绘制路径以实现 buffer/direct 一致。缓存只在有测量收益时启用，失败不中断正确绘制。
- [ ] 对残影机制，先利用 B1 证据分别试父窗有效像素保留、提交后后代/FRAME 失效、兄弟裁剪；选择单独能消除差异的最小机制，保护标准边框计算。没有原生证据时只交付可诊断候选，不宣布根因已验证，不把三种策略同时默认打开。
- [ ] 重跑两测试、原布局回退、`make test`、B1 的原生对照。记录绘制次数、GDI/USER 对象和自然像素差异；模拟检查不代替这一步。

### Task 3: B3 工作区内可操作的编辑器

**Files:** Create `src/core/editor_layout.c`, `src/core/editor_layout.h`, `src/ui/editor_internal.h`, `src/ui/editor_layout.c`, `tests/test_editor_layout.c`, `tests/window_workarea.py`; Modify `src/ui/dialog_editor.c`, `src/ui/dialog_common.c`, `src/ui/main_common.c`, `src/ui/resource.h`, `src/ui/ui.h`, `tests/editor_geometry.py`, `tests/editor_interaction.py`, `tests/source_helpers.py`, `Makefile`。

**Interfaces:** 核心 `QpEditorLayoutInput { int dpi,width,height,preview_height,active_page; }`、`QpEditorLayout` 包含 name/group/batch/segment/properties/page_tabs/preview/cancel/ok 的 QpRect 与 narrow 标志；`void qp_editor_layout(const QpEditorLayoutInput *,QpEditorLayout *)`。QpRect 复用 rule_layout.h。私有 `void ed_layout(EditorCtx *)`、`Segment *ed_cur_seg(EditorCtx *)`；`int qp_window_work_area(HWND owner,RECT *work)` 查询 owner monitor，失败回退主屏工作区。

- [ ] 1366×768 工作区与任务栏扣减高度、96/120/144/192 DPI、跨显示器恢复、首次无设置、最大化恢复、宽度在 800 逻辑像素两侧、预览极高：保存/取消可见，子窗口可访问，不出现负尺寸。
- [ ] 窄布局上半区「片段」「属性」页签可切换，状态/当前片段/滚动位置不丢；属性控件 Tab/SetFocus 可滚入。滚动条变化最多 3 次收敛计算，仍不稳定时采用保守带滚动条视口，不递归无上限。
- [ ] `make build/test_editor_layout && build/test_editor_layout` 和 `python3 tests/window_workarea.py` 确认旧固定最小尺寸 RED。
- [ ] 提取 EditorCtx/布局，宽窗口保留现有双栏；窄阈值 `<DPIX(800)`，头部按钮折行，属性页完整宽度。初次/记忆尺寸/最小跟踪均限 owner 工作区；按实际可用尺寸夹取预览而非修改用户记忆的默认值。
- [ ] 更新适配器读取真实新文件；运行新测试、两编辑器测试、`make test`、`make build/test_win32.exe`；原生工作区/键盘导航独立验收。

### Task 4: B4 全量引导清单、规范文案和布局

**Files:** Create `docs/ui-copy-review.json`, `tools/audit_ui_copy.py`, `tests/ui_copy_audit.py`; Modify `src/ui/dialog_common.c`, `src/ui/dialog_editor.c`, `src/ui/dialog_rules.c`, `src/ui/dialog_lookup.c`, `src/ui/dialog_settings.c`, `src/ui/dialog_tables.c`, `src/ui/main_actions.c`, `src/ui/main_common.c`, `src/ui/main_fields.c`, `src/ui/main_sessions.c`, `src/ui/main_tree.c`, `src/ui/main_window.c`, `src/ui/preview.c`, `src/platform/main.c`, `src/core/summary.c`, `src/core/lint.c`, `src/core/table.c`, `installer/installer.c`, `installer/uninstall.c`, `installer/setup_res.h`, `docs/DEVELOPMENT.md`, `tests/test_win32.c`, `Makefile`。

**Interfaces:** 清单每项有 `file, anchor, kind, source_hash, text, previous_text, decision, reason, behavior_test, layout_check`；text 是当前文案，previous_text 对 replace 保存旧文案，其他为空；decision 仅 replace/retain/product_name/business_content。C 字符串扫描忽略注释，拼接相邻字面量，关联实际 UI/error 输出调用；宏和动态格式也审查，避免把 JSON 字段或类名当文案。`python3 tools/audit_ui_copy.py --check` 检查缺项/过期项。

- [ ] 先生成当前清单，逐项给出修改或保留理由；`ui_copy_audit.py` 覆盖新引导漏审、源码字符串变化、引用不存在按钮、固定可配置热键、旧“按名称引用”以及业务正文误替换，确认 RED。
- [ ] 按设计第 6 节全部规范修改。重点：失败真实保留/回滚结果、合法空值、取消边界、容量限制、动态热键；产品名不改。错误不推断剪贴板占用或安全软件，能取得 API 原因则显示实际原因。
- [ ] 操作说明采用可测量折行或可选择多行摘要；安装/卸载长引导也调整高度，清单记录每个保留短文案的布局结论；数据表刷新引导只引用实际入口。帮助/摘要在静态路径下可按纯 C 测试，不引入国际化运行库。
- [ ] 运行 `python3 tests/ui_copy_audit.py`、`python3 tools/audit_ui_copy.py --check`、`make test`、`make setup`。原生缩放字体测量和截图里长业务路径另行列验收；不是搜索几个词无结果就宣布全量完成。
