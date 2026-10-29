# QuickPhrase 数据与查表安全 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Follow the execution method selected for the master plan: superpowers:executing-plans or superpowers:subagent-driven-development. Check off each test/fix cycle.

**Goal:** 消除残缺保存、克隆丢内容、事务失效和查表误命中，明确文本截断状态。

**Architecture:** 完整候选后提交，所有权集中转移；精确键与一致表快照；带状态渲染保留旧 API 封装。

**Tech Stack:** C11、cJSON、Win32 事务适配器、Python、ASan/UBSan。

**Spec:** `docs/superpowers/specs/2026-10-07-quickphrase-350-hardening-design.md`。

## Global Constraints

逐字沿用[总计划 Global Constraints](2026-10-07-quickphrase-350-hardening.md#global-constraints)。故障注入只影响自建测试对象/唯一临时夹具；不读取或写入真实用户档案。

## Review Focus

模型和序列化的中间分配失败；取消所有路径；外部设置补偿失败；求值借用表刷新；UTF-16 和末尾合法空值。具体断言分别在 A1–A8，不用一次套件成功替代分支覆盖。

## 文件结构

- `src/core/model.c`：完整克隆及拥有负载的转移，原公共生命周期入口保留。
- `src/core/storage.c`：JSON 字段/子对象完整构建、读取与原子保存返回状态。
- `src/platform/settings_apply.c`：设置候选应用、持久化与补偿；UI 负责显示结果。
- `src/ui/main_sessions.c`：快照和会话切换；`main_fields.c`、`main_tree.c`、`dialog_settings.c` 传播失败。
- `src/core/table_key.c/.h`：纯 C 完整文本键和精确十进制键；`table.c`：CSV、索引、缓存与匹配结果。
- `src/core/eval.c`：规则优先级和逐段命中；`render.c`、`strings.c`、`unicode.c`：完整字符及输出状态。
- 测试新增文件随各任务加入 Makefile；提取式 UI 适配器通过 `tests/source_helpers.py` 正确读取新文件。

### Task 1: A1 深克隆全有或全无

**Files:** Modify `src/core/model.c`, `src/core/engine.h`, `Makefile`; Create `tests/test_model_failures.c`。

**Interfaces:** 保留 `Segment *seg_clone(const Segment *)`、`ParseRule *parse_rule_clone(const ParseRule *)`、`ConditionRule *cond_rule_clone(const ConditionRule *)`、`Template *tmpl_clone(const Template *)` 和对应 free；非空输入的任一子克隆失败必须整体 NULL。列表空输入的 NULL 仍表示合法空链表。

为 JSON 构建提供 checked 变更入口：`int qp_seg_set_options(Segment *,const wchar_t *const *,int count)`、`int qp_seg_add_option(Segment *,const wchar_t *)`、`int qp_cond_term_set_match(CondTerm *,const wchar_t *)`；返回 1 表示完整成功，0 不改变旧负载。旧 void API 保留兼容封装，新的 UI/读取路径检查返回值。

- [ ] 增加 `clone_every_allocation_failure_is_atomic`：完整模板含选项、查表输出、条件值、静态/动态片段；逐点失败 malloc/calloc/realloc；最小复现 `assert(!copy || seg_list_count(copy->segments)==2)`，并逐字段验证“NULL 或完整语义相等”，原模型不变，活动分配数回到基线。
- [ ] 运行 `make build/test_model_failures && build/test_model_failures`，确认当前两片段模板变成一片段的复现失败。
- [ ] 逐层检查并清理候选，禁止 seg_list_append(NULL 克隆) 后继续成功。包含条件匹配值和所有拥有字符串；新增测试加入 TEST_NAMES。
- [ ] 重跑该命令和 `make test`；记录差异，不提交。

### Task 2: A2 JSON 读写失败不发布残缺数据

**Files:** Modify `src/core/storage.c`, `tests/storage_atomic.py`, `Makefile`; Create `tests/test_json_failures.c`。

**Interfaces:** 保留 `char *appdata_to_json(const AppData *)`、`AppData *appdata_from_json(const char *)`、`int storage_save(const AppData *)`。消费 A1 的 checked 选项/条件值入口，不能用 void mutator 构建候选后忽略失败。内部 `json_add_w` 改返回 int，成功添加才转移所有权；所有 to/from_json 子构建失败沿调用链传播。

- [ ] `serialize_failure_preserves_file` 扫描每个 cJSON/UTF-8 分配；最小复现 `assert(!json || strstr(json,"KEEP_CONTENT"))`，失败只能 NULL，成功 JSON 往返后的 groups/templates/tables/segments/rules/options 全相等；自建旧文件字节不变。
- [ ] `deserialize_failure_never_returns_partial_model` 扫描 cJSON 解析后的模型分配；返回 NULL 或完整模型，非法类型/尾随数据旧回归保持。运行 `make build/test_json_failures && build/test_json_failures` 确认 RED。
- [ ] 检查每个创建/添加/转换，失败统一清理；不改 JSON 字段，不用容错跳过必需对象。存储保存只接受完整序列化结果。
- [ ] 执行该测试、`python3 tests/storage_atomic.py`、`make test`；验证 close/flush/replace 失败仍保留旧文件。

### Task 3: A3 规则所有权转移与嵌套取消

**Files:** Modify `src/core/model.c`, `src/core/engine.h`, `src/ui/dialog_editor.c`, `src/ui/dialog_lookup.c`, `tests/lookup_interaction.py`, `tests/test_win32.c`; Create `tests/test_rule_transfer.c`。

**Interfaces:** 新增 `void qp_parse_rule_take(ParseRule *dst, ParseRule *owned)` 和 `void qp_cond_rule_take(ConditionRule *dst, ConditionRule *owned)`；消耗独立候选节点，保留 dst id/next，释放旧拥有负载，不允许 dst==owned。保留旧 `cond_rule_assign` 为完整克隆后提交的兼容入口。

- [ ] 测试连续 100 次确认替换、取消、空输出链表和候选克隆失败；断言链表位置/id 不变、旧负载释放、活动分配数不累增。取消最小断言 `assert(result==0)`、`assert(wcscmp(rule->lk_table_id,original_id)==0)`，覆盖下拉切换/删除输出后 Cancel、Esc、关闭及调用者整体内容不变。
- [ ] 运行 `make build/test_rule_transfer && build/test_rule_transfer` 和 `python3 tests/lookup_interaction.py`，确认泄漏/取消路径 RED。
- [ ] 实现 take；查表窗口内部持有独立候选，result 只在 IDOK 成功设 1。替换结构体裸赋值；编辑选项/条件值也消费 A1 checked mutator，失败不显示成功。管理数据表仍独立保存，不回滚该全局操作。
- [ ] 重跑上述命令及原生测试交叉编译。真实 HWND 取消路径列入 Windows 清单，不能把适配器称为原生通过。

### Task 4: A4 会话快照失败不丢最新输入

**Files:** Modify `src/ui/main_sessions.c`, `src/ui/main_fields.c`, `src/ui/main_tree.c`, `src/ui/dialog_settings.c`, `src/ui/ui.h`, `src/ui/ui_internal.h`, `tests/ui_data_transactions.py`; Create `tests/session_transactions.py`。

**Interfaces:** `sess_activate`、`sess_deactivate`、`ui_select_template`、`ui_reset_selection` 返回 int（1 完成，0 保留原活动状态）；更新全部声明/调用者。先成功构造新 EvalContext/id/值快照，再提交；失败不能销毁 g_ctx 或释放其模板。非破坏性窗口通知可忽略成功值，数据释放/替换路径必须检查。

- [ ] `snapshot_failure_keeps_live_input` 扫描快照分配，断言 `assert(!ui_select_template(other))`、`assert(g_ctx==original_ctx)`，旧快照、当前完整新输入均保留；重复/关闭/删除/覆盖导入/目录切换不得因此释放活动模型。
- [ ] 运行 `python3 tests/session_transactions.py` 确认 RED；同时覆盖 user_touched 的空字符串与未触摸 NULL 区别。
- [ ] 所有快照整体提交，成功后才切换/销毁；重绑失败恢复原控件/选择，并正确报告无法切换。会话复制分配失败不得显示复制成功。
- [ ] 运行新测试、`python3 tests/ui_data_transactions.py`、`make app`；检查所有调用点与失败回滚。

### Task 5: A5 设置应用与补偿状态真实

**Files:** Create `src/platform/settings_apply.c`, `tests/settings_transactions.py`; Modify `src/platform/app.h`, `src/ui/dialog_settings.c`, `src/platform/main.c`, `Makefile`。

**Interfaces:** 在 app.h 定义 `QpSettingsResult { unsigned failures, rollback_failures; }`，位标志 `QP_SETTINGS_HOTKEY=1u, QP_SETTINGS_THEME=2u, QP_SETTINGS_AUTOSTART=4u, QP_SETTINGS_SAVE=8u`；新增 `int qp_settings_commit(const Settings *candidate, QpSettingsResult *result)`。保存无 UI 的候选，结果包含真实运行态/补偿失败；对话框显示消息。

- [ ] 适配真实 helper，注入热键占用、注册失败、注册旧键失败、自动启动写失败、保存失败和补偿失败。断言 `assert(!qp_settings_commit(&candidate,&result))`，旧设置在可恢复情形全恢复；模拟旧热键重注册失败时 `assert(result.rollback_failures & QP_SETTINGS_HOTKEY)`，标志对应真实系统状态。
- [ ] `python3 tests/settings_transactions.py` 确认 RED；明确单独目录切换不被取消回滚。
- [ ] 实现 candidate/应用/保存/补偿顺序；取消不留普通设置候选副作用。所有运行态结果都按 API 成功值更新，不先假设成功。
- [ ] 重跑测试及 `make app`；Windows 清单保留实际热键和注册表补偿验证。

### Task 6: A6 完整精确键与一致缓存

**Files:** Create `src/core/table_key.c`, `src/core/table_key.h`, `tests/test_table_keys.c`, `tests/table_cache_race.py`; Modify `src/core/table.c`, `src/core/table.h`, `src/core/eval.c`, `tests/source_helpers.py`, `tests/lookup_interaction.py`, `Makefile`。

**Interfaces:** `wchar_t *qp_table_text_key(const wchar_t *src)`、`wchar_t *qp_table_numeric_key(const wchar_t *src)` 返回拥有字符串；numeric 非十进制输入返回 NULL，不展开巨大指数。表读取范围新增 `void qp_table_read_begin(void)`、`void qp_table_read_end(void)`，可嵌套；该范围内表首次读取冻结，失效/淘汰延迟到最后 end，eval_run 配对使用。

- [ ] 数值/长键断言：`assert(!qp_table_lookup(tbl,0,L"9007199254740993",1,LOOKUP_FIRST,NULL,0,out,QP_VALUE_MAX))`，表仅有 9007199254740992；两种小数不碰撞；1e30 无浮点转换；007/7/7.0/7e0 兼容但精确文本优先；300×a+X 不命中 Y。追加零、负零、非法数字、NaN/Infinity、指数超长和规范化 OOM。
- [ ] 读取后元数据前覆写 CSV 的定向钩子；断言下一次查询不是“旧内容＋新时间戳”。范围内触发失效/表清单切换，旧借用指针有效，范围外读到新代。运行 `make build/test_table_keys && build/test_table_keys`、`python3 tests/table_cache_race.py` 确认 RED。
- [ ] 用完整文本/有效数字＋指数建索引；哈希后完整比较。读取前后检查，初读＋最多 2 次重试，仍变化返回错误；不替换正在借用的表。
- [ ] 重跑上述与 `make test`，包括 FIRST/LAST/ALL、去重、表头和 UTF-8/GBK 原回归。

### Task 7: A7 逐段成功空值保持行位置

**Files:** Modify `src/core/eval.c`, `tests/test_lookup.c`。

**Interfaces:** 不改外部 API；apply_lookup_rule 每目标独立保存成功次数，命中空值仍 derived_set/parse_set。

- [ ] A→空、B→bee、目标默认 fallback：单次/逐段 A 都为空且 rule_driven=1；A\nB 断言 `assert(!wcscmp(ctx->fields[1].value,L"\nbee"))`；B\nA 为 bee\n；未命中段按旧行为跳过。两个输出各自空值、重复目标与更早规则优先一起断言。
- [ ] `make build/test_lookup && build/test_lookup` 确认 RED。
- [ ] 成功次数与累计长度分开，按成功段数插入换行；保持逐段优先级、空白键跳过和缓冲限制。
- [ ] 重跑此测试和 `make test`。

### Task 8: A8 完整字符与带状态输出

**Files:** Modify `src/core/unicode.c`, `src/core/strings.c`, `src/core/render.c`, `src/core/eval.c`, `src/core/table.c`, `src/core/table.h`, `src/core/engine.h`, `src/core/engine_internal.h`, `src/ui/main_fields.c`, `src/ui/main_actions.c`, `src/ui/dialog_editor.c`; Create `tests/test_render_status.c`; Update `tests/preview_allocations.py`, `Makefile`。

**Interfaces:** `size_t qp_utf16_prefix(const wchar_t *,size_t length,size_t max_units)`；内部 `size_t qp_append_checked(wchar_t *,size_t cap,size_t used,const wchar_t *,size_t n,unsigned *flags)`。定义 `QpRenderStatus {QP_RENDER_OK,QP_RENDER_TRUNCATED,QP_RENDER_NOMEM,QP_RENDER_INVALID}`、`QpRenderResult { QpRenderStatus status; size_t written; int mark_count; }`；新增 `QpRenderResult qp_eval_render_checked(EvalContext *,OutputMode,int batch,wchar_t *,size_t,int *,int)`。EvalContext 添加 output_flags 传播中间展开/查表/逐段失败与截断；旧 render API 保持封装。

表查找新增 `QpTableLookupResult { int found, truncated, failed; size_t written; }`，`QpTableLookupResult qp_table_lookup_checked(QpTable *,int key_col,const wchar_t *key,int val_col,int mode,const wchar_t *sep,int unique,wchar_t *out,size_t cap)`；旧 bool API 包装 checked。内部标志在 engine_internal.h 定义 `QP_TEXT_TRUNCATED=1u`、`QP_TEXT_NOMEM=2u`，不把未命中当分配失败。

- [ ] 边界字符 A+代理对、CRLF、Tab、占位符、多段查表、批量末行和标记容量；`assert(qp_utf16_prefix(L"A\xD83D\xDE00",3,2)==1)`；不足容量 `assert(result.status==QP_RENDER_TRUNCATED)`，分配失败 NOMEM，失败不伪装成单次批量回退；原文和 marks 对应。复制适配器断言非 OK 不写剪贴板。
- [ ] `make build/test_render_status && build/test_render_status` 确认 RED；可用容量恰好够的结果必须 OK，旧 API 仍兼容。
- [ ] 实现逐层状态传播和 UTF-16 安全边界，UI 显示完整/截断/失败状态且阻止正常复制截断结果；不增加旧 JSON 字段。
- [ ] 运行此测试、`python3 tests/preview_allocations.py`、`make test`、`make app`；报告业务夹具缺失及原生剪贴板未执行。
