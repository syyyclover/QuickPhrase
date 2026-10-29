# QuickPhrase 性能、减重与交付 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use the master plan's selected superpowers:executing-plans or superpowers:subagent-driven-development workflow. Require fresh verification before completion.

**Goal:** 避免无变化重算与重复资源，测量选择构建组合并交付有证据的候选版本。

**Architecture:** 复用局部求值工作区与查表匹配结果；预览按内容/几何/字体/主题变化分流；分阶段对象构建和固定基准控制体积/热点退化。

**Tech Stack:** C11、Win32、MinGW GCC/BFD、host C benchmark、Python 资源/预算检查。

**Spec:** `docs/superpowers/specs/2026-10-07-quickphrase-350-hardening-design.md`。

## Global Constraints

逐字沿用[总计划 Global Constraints](2026-10-07-quickphrase-350-hardening.md#global-constraints)。测试候选输出到唯一 build/dist 子目录；不运行 make clean、不覆盖保留的 3.4 包。

## Review Focus

反复相同结果导致阅读位置丢失；匹配复用混入其他规则/模板；基准波动与构建可重复性；图标去重遗漏尺寸；平台未验收却声称发布完成。C1–C3 分别覆盖。

## 文件结构

- `src/core/eval.c`：上下文拥有的可复用工作区；`table.c/.h`：拥有匹配结果、借用只读表的查询对象；销毁边界明确。
- `src/ui/preview.c`：缓存已提交内容/marks/format，分离排版与绘制；`dialog_editor.c`：生命周期内复用可更新的预览上下文。
- `tests/bench_core.c`、`tools/bench_compare.py`：固定夹具、正确性校验与五次中位数；原生输出另建记录，不混称。
- Makefile：对象级编译优化配置，manifest/icon 检查不减；`tests/size_budget.py`：发行字节预算与 PE 段统计。

### Task 1: C1：基准及无变化更新

**Files:** Create `tests/bench_core.c`, `tools/bench_compare.py`, `tests/preview_refresh.py`, `tests/test_table_match.c`; Modify `src/core/eval.c`, `src/core/engine.h`, `src/core/table.c`, `src/core/table.h`, `src/ui/preview.c`, `src/ui/dialog_editor.c`, `src/ui/main_fields.c`, `tests/preview_allocations.py`, `tests/preview_mode.py`, `tests/source_helpers.py`, `Makefile`。

**Interfaces:** 不改变 A8 QpRenderResult。新增不透明 `QpTableMatch`，`QpTableMatch *qp_table_match(QpTable *,int key_col,const wchar_t *key)` 返回拥有结果（无命中仍有合法空结果，NULL 仅表示失败），`void qp_table_match_free(QpTableMatch *)`；`QpTableLookupResult qp_table_match_value(const QpTableMatch *,int val_col,int mode,const wchar_t *sep,int unique,wchar_t *,size_t)` 消费 A8 类型，匹配对象仅在 A6 表读取范围有效，必须在 end 前释放。FIRST/LAST 的索引保存首/末行，ALL 只构造一次顺序结果。

- [ ] 基准固定普通/100/500 片段模板、4000 正则模式、10万行 CSV（含重复键和多输出）。正确性校验摘要后才计时，五次取中位数；自建表位于唯一临时目录。记录 O2 基线、工作区分配次数、查询/模型刷新次数。
- [ ] `preview_refresh.py` 断言同内容/marks/format 刷新保持 scrollX/Y 和选区、测量次数不增；字体/DPI 变化重测，宽度只排版、主题只绘制，标记变化必须更新。复制当轮最新输入，非 OK 不复制，失败保留已提交原文/映射。
- [ ] `test_table_match` 验证 FIRST/LAST/ALL 和各输出与旧正确结果一致，合法空值与数值精确优先保留；一个键多个输出只匹配一次，分配失败状态不变成未命中。运行 `python3 tests/preview_refresh.py`、`make build/test_table_match && build/test_table_match` 确认 RED。
- [ ] 复用求值工作区（由 EvalContext 销毁），编辑器只在模板结构变化时重建预览上下文；相同预览不发布新模型。暂不加输入延迟定时器，保持同步复制语义；如需合并通知，必须先有可靠测量并更新计划。
- [ ] 重跑以上和 `make test`；`make bench` 生成五次结果，`tools/bench_compare.py` 明确校验热点≤10%退化。host 指标不作为 Windows 首次启动、拖动和峰值资源验收。

### Task 2: C2：资源去重与构建组合选择

**Files:** Modify `resource.rc`, `src/ui/resource.h`, `Makefile`, `installer/setup.rc`, `tests/icon_resources.py`, `tests/manifest_validation.py`; Create `tests/size_budget.py`, `tests/build_configuration.py`。

**Interfaces:** Makefile 配置 `CORE_OPT`、`UI_OPT`、`LTO` 和可覆盖 BUILD/DIST；默认候选 core O2/UI Os、LTO=0，另比较全 O2、全 Os、Os+LTO。RC 中内嵌 app/uninstall 路径由构建宏传入，不继续固定 build 路径。`make check-size` 校验主程序 458752 B/安装包 589824 B 上限；报告 .text/.rsrc/嵌入负载。

`tests/icon_resources.py` 增加可选 `--embedded-app PATH --embedded-uninstall PATH`，显式指定候选内嵌比较源，默认仍为现有 build 文件。不同优化选项使用不同对象目录或参数指纹依赖，防止切换配置时复用旧对象。

- [ ] `icon_resources.py` 新回归：只有一组 app icon，但全部九尺寸与原始 ICO 逐字节相同；托盘、小图标加载仍 IDI_APPICON。当前重复资源须被检查发现。
- [ ] `build_configuration.py` 检查候选 BUILD/DIST 不串用旧二进制，资源/版本头变更会重构所需对象，优化选项改变不会复用不兼容对象；超预算测试必须失败。执行三个新增/修改检查确认 RED。
- [ ] 删除未使用 IDI_APPICON_SMALL 的重复资源；构建设置 CJSON_HIDE_SYMBOLS，保留第三方上游文件。编译成对象以区分 core/UI 优化，保留有效 gc-sections、static、x64 展开信息、九图标帧及当前系统目标。
- [ ] 每配置以唯一目录构建、运行完整 test 和基准，比较实际字节与热点；仅选满足门槛的配置。LTO 不是必选，不关闭 ANSI stdio 功能或降低 PE 对齐代替正常优化。
- [ ] 运行 `make setup`、`make check-size`、三资源检查；确认内嵌文件来自该构建且绿色包与内嵌 app 相同，非“编译成功即可”。

### Task 3: C3：最终回归、审查与候选交付

**Files:** Update `docs/DEVELOPMENT.md`, `docs/3.5.0-release.md`, `docs/3.5.0-display-fixes.md`, `docs/3.5.0-deep-audit.md`, `docs/ui-copy-review.json`, `dist/QuickPhrase-3.5.0.sha256`; new report `docs/3.5.0-hardening-results.md`。实际 dist 只在确认构建/资源检查通过后更新。

**Interfaces:** 产物仍为 QuickPhrase.exe、QuickPhrase-3.5.0-setup.exe；图标诊断和 Win32 验收程序独立，不增加安装负载。报告每缺陷包含 RED 证据、GREEN 命令、测试层级及未执行项。

- [ ] 串行执行 `make test`、默认 `make sanitize`、`make setup`、`make icon-check`、`make build/test_win32.exe`；保存最新独立日志。不能与固定临时夹具套件并发。
- [ ] LSan 因 ptrace 不支持失败时保留原失败，另跑 ASan/UBSan 并单独标明；业务夹具缺失和原生 Windows 未执行不计作通过。`make win32-test` 的环境失败记录退出码和实际错误。
- [ ] 执行 manifest/icon/内嵌文件/预算/文案清单/Python 编译/shell语法/`git diff --check`，同一目录校验 SHA-256；记录编译工具链、最终字节和基准。
- [ ] 依 requesting-code-review 做一次独立整体验证，先处理 Critical/Important 并补回归，再重跑受影响及完整套件；不自动提交/推送。
- [ ] 按 `docs/windows-display-check.md` 在新 Windows 测试账号验证自然绘制、四档字体、低工作区、取消/失败、复制、业务模板、图标、安装升级和卸载保留数据。环境不可用时只给“候选交付”，对应缺陷留待验收，不能将图像差异测试交叉编译称为残影已解决。
- [ ] 更新全部报告和校验和，交付绿色包/安装包/诊断链接、已验证结果与剩余限制；不宣称所有潜在问题已发现。
