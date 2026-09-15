/*
 * test_engine.c —— 规则引擎自测（不依赖 Windows API，本机 gcc 即可运行）
 *   gcc -std=c11 -Wall -Wextra -o build/test_engine tests/test_engine.c src/engine.c && ./build/test_engine
 */
#include "../src/engine.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

static void check(int cond, const char *what)
{
    if (cond) { ++g_pass; printf("  [ok]   %s\n", what); }
    else      { ++g_fail; printf("  [FAIL] %s\n", what); }
}

static void check_str(const wchar_t *got, const wchar_t *want, const char *what)
{
    if (got && want && wcscmp(got, want) == 0) {
        ++g_pass;
        printf("  [ok]   %s\n", what);
    } else {
        ++g_fail;
        printf("  [FAIL] %s\n         期望: %ls\n         实际: %ls\n",
               what, want ? want : L"(null)", got ? got : L"(null)");
    }
}

/* ---------------------------------------------------------------- */
static void test_string_utils(void)
{
    wchar_t buf[128];
    printf("\n== 字符串工具 ==\n");

    check(qp_icmp(L"abc", L"ABC") == 0, "qp_icmp 大小写不敏感相等");
    check(qp_icmp(L"abc", L"abd") != 0, "qp_icmp 不相等");
    check(qp_icmp(L"存储服务器", L"存储服务器") == 0, "qp_icmp 中文相等");
    check(qp_istr(L"硬件、软件", L"软件") != NULL, "qp_istr 命中中文子串");
    check(qp_istr(L"硬件", L"软件") == NULL, "qp_istr 未命中");
    check(qp_istr(L"abc", L"") == NULL, "qp_istr 空针不命中");
    check(qp_istarts(L"NFV-D-X", L"nfv") == 1, "qp_istarts 前缀命中");
    check(qp_istarts(L"NFV", L"NFVX") == 0, "qp_istarts 前缀过长不命中");
    check(qp_iends(L"NFV-D-DBS", L"dbs") == 1, "qp_iends 后缀命中");
    check(qp_iends(L"报告。", L"。") == 1, "qp_iends 中文后缀命中");
    check(qp_iends(L"DBS", L"XDBS") == 0, "qp_iends 后缀过长不命中");
    check(qp_iends(L"abc", L"") == 0, "qp_iends 空后缀不命中");

    check(qp_split_take(L"A-B-C-D", L"-", 3, buf, 128) == 1, "split 取第3段成功");
    check_str(buf, L"C", "split 第3段 = C");
    check(qp_split_take(L"A-B", L"-", 5, buf, 128) == 0, "split 越界返回 0");
    check(qp_split_take(L"NFV-D-HNZJI-11A-1401-AD05-S-DBS-02", L"-", 8, buf, 128) == 1,
          "split 真实设备编号取第8段");
    check_str(buf, L"DBS", "真实设备编号第8段 = DBS");

    qp_strlcpy(buf, L"  空白  ", 128);
    qp_trim(buf);
    check_str(buf, L"空白", "qp_trim 去首尾空白");
    check(qp_is_blank(L"   ") == 1, "qp_is_blank 全空白");
    check(qp_is_blank(L" x ") == 0, "qp_is_blank 含内容");

    /* 溢出保护 */
    qp_strlcpy(buf, L"1234567890", 5);
    check_str(buf, L"1234", "qp_strlcpy 截断且以 NUL 结尾");
}

/* ---------------------------------------------------------------- */
/* 构造与需求文档一致的模板（但把分割段索引设为 8，见下方说明）        */
static Template *build_demo(Segment **out_type, Segment **out_code,
                            Segment **out_part, Segment **out_fault,
                            Segment **out_result)
{
    Template *t = tmpl_create(L"设备硬件告警核查口径");
    Segment *s;
    Segment *s_type, *s_code, *s_part, *s_fault, *s_result;
    ParseRule *pr;
    ConditionRule *cr;
    static const wchar_t *type_opts[] = { L"存储服务器", L"数据库服务器", L"XXX设备" };
    static const wchar_t *part_opts[] = { L"硬盘", L"内存", L"电源", L"网卡", L"主板" };
    static const wchar_t *fault_opts[] = { L"硬件", L"软件", L"配置", L"网络" };
    static const wchar_t *res_opts[] = { L"维护厂家跟进处理后告警消除", L"重启服务后告警消除" };

    s = seg_create(SEG_STATIC);
    qp_strlcpy(s->content, L"经核查由于", QP_CONTENT_MAX);
    seg_list_append(&t->segments, s);

    s_type = seg_create(SEG_SELECT);
    qp_strlcpy(s_type->label, L"设备类型", QP_LABEL_MAX);
    qp_strlcpy(s_type->default_value, L"存储服务器", QP_VALUE_MAX);
    seg_set_options(s_type, type_opts, 3);
    seg_list_append(&t->segments, s_type);

    s_code = seg_create(SEG_TEXT);
    qp_strlcpy(s_code->label, L"设备编号", QP_LABEL_MAX);
    seg_list_append(&t->segments, s_code);

    s = seg_create(SEG_STATIC);
    qp_strlcpy(s->content, L"一", QP_CONTENT_MAX);
    seg_list_append(&t->segments, s);

    s_part = seg_create(SEG_MULTI_SELECT);
    qp_strlcpy(s_part->label, L"故障部件", QP_LABEL_MAX);
    qp_strlcpy(s_part->default_value, L"硬盘", QP_VALUE_MAX);
    seg_set_options(s_part, part_opts, 5);
    seg_list_append(&t->segments, s_part);

    s_fault = seg_create(SEG_MULTI_SELECT);
    qp_strlcpy(s_fault->label, L"故障类型", QP_LABEL_MAX);
    qp_strlcpy(s_fault->default_value, L"硬件", QP_VALUE_MAX);
    seg_set_options(s_fault, fault_opts, 4);
    seg_list_append(&t->segments, s_fault);

    s = seg_create(SEG_STATIC);
    qp_strlcpy(s->content, L"异常引起告警，", QP_CONTENT_MAX);
    seg_list_append(&t->segments, s);

    s_result = seg_create(SEG_SELECT);
    qp_strlcpy(s_result->label, L"处理结果", QP_LABEL_MAX);
    qp_strlcpy(s_result->default_value, L"维护厂家跟进处理后告警消除", QP_VALUE_MAX);
    seg_set_options(s_result, res_opts, 2);
    seg_list_append(&t->segments, s_result);

    s = seg_create(SEG_STATIC);
    qp_strlcpy(s->content, L"，不影响业务。", QP_CONTENT_MAX);
    seg_list_append(&t->segments, s);

    /* 解析规则：设备编号按 "-" 分割，取 DBS 所在段 → 设备类型 */
    pr = parse_rule_create();
    pr->type = PARSE_SPLIT;
    qp_strlcpy(pr->delimiter, L"-", QP_SEP_MAX);
    pr->segment_index = 8;
    qp_strlcpy(pr->keyword, L"DBS", QP_KEYWORD_MAX);
    qp_strlcpy(pr->target_segment_id, s_type->id, QP_ID_MAX);
    qp_strlcpy(pr->fill_value, L"存储服务器", QP_VALUE_MAX);
    parse_rule_append(&s_code->parse_rules, pr);

    pr = parse_rule_create();
    pr->type = PARSE_SPLIT;
    qp_strlcpy(pr->delimiter, L"-", QP_SEP_MAX);
    pr->segment_index = 8;
    qp_strlcpy(pr->keyword, L"XXX", QP_KEYWORD_MAX);
    qp_strlcpy(pr->target_segment_id, s_type->id, QP_ID_MAX);
    qp_strlcpy(pr->fill_value, L"XXX设备", QP_VALUE_MAX);
    parse_rule_append(&s_code->parse_rules, pr);

    /* 条件规则：故障类型 → 处理结果 */
    cr = cond_rule_create();
    cond_rule_set_simple(cr, s_fault->id, OP_CONTAINS, 0, L"硬件");
    qp_strlcpy(cr->result_value, L"维护厂家跟进处理后告警消除", QP_RESULT_MAX);
    cond_rule_append(&s_result->condition_rules, cr);

    cr = cond_rule_create();
    cond_rule_set_simple(cr, s_fault->id, OP_CONTAINS, 0, L"软件");
    qp_strlcpy(cr->result_value, L"重启服务后告警消除", QP_RESULT_MAX);
    cond_rule_append(&s_result->condition_rules, cr);

    if (out_type)   *out_type = s_type;
    if (out_code)   *out_code = s_code;
    if (out_part)   *out_part = s_part;
    if (out_fault)  *out_fault = s_fault;
    if (out_result) *out_result = s_result;
    return t;
}

static void test_real_scenario(void)
{
    Template *t;
    Segment *s_type, *s_code, *s_part, *s_fault, *s_result;
    EvalContext *ctx;
    wchar_t out[QP_OUTPUT_MAX];
    int i_code, i_part, i_fault, i_type, i_result;

    printf("\n== 需求文档真实场景 ==\n");
    t = build_demo(&s_type, &s_code, &s_part, &s_fault, &s_result);
    ctx = eval_create(t);
    check(ctx != NULL && ctx->count == 9, "求值上下文含 9 个片段");

    i_code   = eval_index_of_id(ctx, s_code->id);
    i_part   = eval_index_of_id(ctx, s_part->id);
    i_fault  = eval_index_of_id(ctx, s_fault->id);
    i_type   = eval_index_of_id(ctx, s_type->id);
    i_result = eval_index_of_id(ctx, s_result->id);
    check(i_code == 2 && i_type == 1 && i_result == 7, "按 id 定位片段索引正确");
    check(eval_index_of_label(ctx, L"设备编号") == i_code, "按变量名定位片段索引正确");

    /* 填入真实设备编号 + 故障部件/类型 */
    eval_set_user_value(ctx, i_code, L"NFV-D-HNZJI-11A-1401-AD05-S-DBS-02");
    eval_set_user_value(ctx, i_part, L"硬盘");
    eval_set_user_value(ctx, i_fault, L"硬件");
    eval_run(ctx);

    check_str(ctx->fields[i_type].value, L"存储服务器", "解析规则自动填充设备类型");
    check_str(ctx->fields[i_result].value, L"维护厂家跟进处理后告警消除",
              "条件规则命中「硬件」");

    eval_render(ctx, out, QP_OUTPUT_MAX);
    check_str(out,
              L"经核查由于存储服务器NFV-D-HNZJI-11A-1401-AD05-S-DBS-02一硬盘硬件异常引起告警，"
              L"维护厂家跟进处理后告警消除，不影响业务。",
              "完整口径与需求文档一致");

    /* 切换故障类型为软件 → 处理结果随之变化 */
    eval_set_user_value(ctx, i_fault, L"软件");
    eval_run(ctx);
    check_str(ctx->fields[i_result].value, L"重启服务后告警消除", "条件规则改判为「软件」");

    /* 多选拼接值也能被 contains 命中（第一条命中即停止） */
    eval_set_user_value(ctx, i_fault, L"硬件、软件");
    eval_run(ctx);
    check_str(ctx->fields[i_result].value, L"维护厂家跟进处理后告警消除",
              "多选值按规则顺序取第一条命中");

    /* 未命中任何解析规则 → 设备类型回到默认值 */
    eval_set_user_value(ctx, i_code, L"NFV-D-HNZJI-11A-1401-AD05-S-ZZZ-02");
    eval_run(ctx);
    check_str(ctx->fields[i_type].value, L"存储服务器",
              "解析规则未命中时设备类型使用默认值");

    /* 命中另一条规则 */
    eval_set_user_value(ctx, i_code, L"NFV-D-HNZJI-11A-1401-AD05-S-XXX-02");
    eval_run(ctx);
    check_str(ctx->fields[i_type].value, L"XXX设备", "第二条解析规则命中");

    eval_destroy(ctx);
    tmpl_free(t);
}

/* ---------------------------------------------------------------- */
static void test_rule_beats_manual(void)
{
    Template *t;
    Segment *s_type, *s_code, *s_part, *s_fault, *s_result;
    EvalContext *ctx;
    int i_code, i_type;

    printf("\n== 变量匹配优先于手动选择 ==\n");
    t = build_demo(&s_type, &s_code, &s_part, &s_fault, &s_result);
    ctx = eval_create(t);
    i_code = eval_index_of_id(ctx, s_code->id);
    i_type = eval_index_of_id(ctx, s_type->id);

    eval_set_user_value(ctx, i_code, L"NFV-D-HNZJI-11A-1401-AD05-S-DBS-02");
    eval_run(ctx);
    check_str(ctx->fields[i_type].value, L"存储服务器", "规则先推导出存储服务器");
    check(ctx->fields[i_type].rule_driven == 1, "设备类型由规则驱动");

    /* 用户手动改成别的值 → 变量匹配的结果仍然胜出 */
    eval_set_user_value(ctx, i_type, L"数据库服务器");
    eval_run(ctx);
    check_str(ctx->fields[i_type].value, L"存储服务器", "手动选择让位给变量匹配");

    /* 触发源变化导致推导值改变 → 输出跟着规则走 */
    eval_set_user_value(ctx, i_code, L"NFV-D-HNZJI-11A-1401-AD05-S-XXX-02");
    eval_run(ctx);
    check_str(ctx->fields[i_type].value, L"XXX设备", "推导值随触发源变化");

    /* 规则不再命中 → 之前的手动选择重新生效 */
    eval_set_user_value(ctx, i_code, L"NFV-D-HNZJI");
    eval_run(ctx);
    check(ctx->fields[i_type].rule_driven == 0, "规则不再命中");
    check_str(ctx->fields[i_type].value, L"数据库服务器", "规则失配后回到手动选择");

    /* 重置回到默认 */
    eval_reset(ctx);
    check_str(ctx->fields[i_type].value, L"存储服务器", "重置回默认值");
    check(ctx->fields[i_code].value[0] == L'\0', "重置后文本变量为空");

    eval_destroy(ctx);
    tmpl_free(t);
}

/* ---------------------------------------------------------------- */
static void test_operators(void)
{
    Template *t = tmpl_create(L"运算符测试");
    Segment *trig = seg_create(SEG_TEXT);
    Segment *host = seg_create(SEG_STATIC);
    EvalContext *ctx;
    ConditionRule *cr;
    int i_trig, i_host;

    printf("\n== 条件运算符 ==\n");
    qp_strlcpy(trig->label, L"输入", QP_LABEL_MAX);
    qp_strlcpy(host->content, L"默认内容", QP_CONTENT_MAX);
    seg_list_append(&t->segments, trig);
    seg_list_append(&t->segments, host);

    cr = cond_rule_create();
    cond_rule_set_simple(cr, trig->id, OP_STARTS_WITH, 0, L"NFV");
    qp_strlcpy(cr->result_value, L"以NFV开头", QP_RESULT_MAX);
    cond_rule_append(&host->condition_rules, cr);

    cr = cond_rule_create();
    cond_rule_set_simple(cr, trig->id, OP_EQUALS, 0, L"精确值");
    qp_strlcpy(cr->result_value, L"完全相等", QP_RESULT_MAX);
    cond_rule_append(&host->condition_rules, cr);

    cr = cond_rule_create();
    cond_rule_set_simple(cr, trig->id, OP_IN, 0, L"甲, 乙 ,丙");
    qp_strlcpy(cr->result_value, L"属于列表", QP_RESULT_MAX);
    cond_rule_append(&host->condition_rules, cr);
    check(cond_term_at(cond_rule_at(host->condition_rules, 2)->terms, 0)->match_value_count == 3,
          "OP_IN 匹配值拆成 3 项");

    cr = cond_rule_create();
    cond_rule_set_simple(cr, trig->id, OP_ENDS_WITH, 0, L"-END");
    qp_strlcpy(cr->result_value, L"以END结尾", QP_RESULT_MAX);
    cond_rule_append(&host->condition_rules, cr);

    cr = cond_rule_create();
    cond_rule_set_simple(cr, trig->id, OP_CONTAINS, 0, L"告警");
    qp_strlcpy(cr->result_value, L"包含告警", QP_RESULT_MAX);
    cond_rule_append(&host->condition_rules, cr);

    ctx = eval_create(t);
    i_trig = eval_index_of_id(ctx, trig->id);
    i_host = eval_index_of_id(ctx, host->id);

    check_str(ctx->fields[i_host].value, L"默认内容", "无命中时用固定片段默认内容");

    eval_set_user_value(ctx, i_trig, L"NFV-D-01");
    eval_run(ctx);
    check_str(ctx->fields[i_host].value, L"以NFV开头", "startsWith 命中");

    eval_set_user_value(ctx, i_trig, L"精确值");
    eval_run(ctx);
    check_str(ctx->fields[i_host].value, L"完全相等", "equals 命中");

    eval_set_user_value(ctx, i_trig, L"乙");
    eval_run(ctx);
    check_str(ctx->fields[i_host].value, L"属于列表", "in 命中（含空格容错）");

    eval_set_user_value(ctx, i_trig, L"BATCH-END");
    eval_run(ctx);
    check_str(ctx->fields[i_host].value, L"以END结尾", "endsWith 命中");

    eval_set_user_value(ctx, i_trig, L"发生告警了");
    eval_run(ctx);
    check_str(ctx->fields[i_host].value, L"包含告警", "contains 命中");

    eval_set_user_value(ctx, i_trig, L"无关内容");
    eval_run(ctx);
    check_str(ctx->fields[i_host].value, L"默认内容", "全部未命中回落默认值");

    eval_destroy(ctx);
    tmpl_free(t);
}

/* ---------------------------------------------------------------- */
/* 空来源只允许 empty / not empty 判断；其它运算符即使取反也不命中。 */
static void test_empty_condition_semantics(void)
{
    Template *t = tmpl_create(L"空来源条件");
    Segment *source = seg_create(SEG_TEXT);
    Segment *not_contains = seg_create(SEG_STATIC);
    Segment *is_empty = seg_create(SEG_STATIC);
    Segment *not_empty = seg_create(SEG_STATIC);
    ConditionRule *cr;
    EvalContext *ctx;
    int i_source, i_not_contains, i_empty, i_not_empty;

    printf("\n== 空来源条件语义 ==\n");
    qp_strlcpy(source->label, L"来源", QP_LABEL_MAX);
    qp_strlcpy(not_contains->content, L"未判断", QP_CONTENT_MAX);
    qp_strlcpy(is_empty->content, L"未判断", QP_CONTENT_MAX);
    qp_strlcpy(not_empty->content, L"未判断", QP_CONTENT_MAX);

    cr = cond_rule_create();
    cond_rule_set_simple(cr, source->id, OP_CONTAINS, 1, L"告警");
    qp_strlcpy(cr->result_value, L"不包含告警", QP_RESULT_MAX);
    cond_rule_append(&not_contains->condition_rules, cr);

    cr = cond_rule_create();
    cond_rule_set_simple(cr, source->id, OP_EMPTY, 0, L"");
    qp_strlcpy(cr->result_value, L"来源为空", QP_RESULT_MAX);
    cond_rule_append(&is_empty->condition_rules, cr);

    cr = cond_rule_create();
    cond_rule_set_simple(cr, source->id, OP_EMPTY, 1, L"");
    qp_strlcpy(cr->result_value, L"来源不为空", QP_RESULT_MAX);
    cond_rule_append(&not_empty->condition_rules, cr);

    seg_list_append(&t->segments, source);
    seg_list_append(&t->segments, not_contains);
    seg_list_append(&t->segments, is_empty);
    seg_list_append(&t->segments, not_empty);

    ctx = eval_create(t);
    i_source = eval_index_of_id(ctx, source->id);
    i_not_contains = eval_index_of_id(ctx, not_contains->id);
    i_empty = eval_index_of_id(ctx, is_empty->id);
    i_not_empty = eval_index_of_id(ctx, not_empty->id);

    check_str(ctx->fields[i_not_contains].value, L"未判断",
              "空来源不满足取反 contains");
    check_str(ctx->fields[i_empty].value, L"来源为空",
              "空来源满足 empty");
    check_str(ctx->fields[i_not_empty].value, L"未判断",
              "空来源不满足取反 empty");

    eval_set_user_value(ctx, i_source, L"普通内容");
    eval_run(ctx);
    check_str(ctx->fields[i_not_contains].value, L"不包含告警",
              "非空来源可满足取反 contains");
    check_str(ctx->fields[i_empty].value, L"未判断",
              "非空来源不满足 empty");
    check_str(ctx->fields[i_not_empty].value, L"来源不为空",
              "非空来源满足取反 empty");

    eval_destroy(ctx);
    tmpl_free(t);
}

/* ---------------------------------------------------------------- */
static void test_parse_contains_and_chain(void)
{
    Template *t = tmpl_create(L"链式规则");
    Segment *s_in = seg_create(SEG_TEXT);
    Segment *s_mid = seg_create(SEG_SELECT);
    Segment *s_out = seg_create(SEG_STATIC);
    EvalContext *ctx;
    ParseRule *pr;
    ConditionRule *cr;
    int i_in, i_mid, i_out;

    printf("\n== contains 解析规则 + 链式触发 ==\n");
    qp_strlcpy(s_in->label, L"描述", QP_LABEL_MAX);
    qp_strlcpy(s_mid->label, L"等级", QP_LABEL_MAX);
    qp_strlcpy(s_mid->default_value, L"一般", QP_VALUE_MAX);
    qp_strlcpy(s_out->content, L"（常规处理）", QP_CONTENT_MAX);
    seg_list_append(&t->segments, s_in);
    seg_list_append(&t->segments, s_mid);
    seg_list_append(&t->segments, s_out);

    /* 描述包含「宕机」→ 等级 = 严重 */
    pr = parse_rule_create();
    pr->type = PARSE_CONTAINS;
    qp_strlcpy(pr->keyword, L"宕机", QP_KEYWORD_MAX);
    qp_strlcpy(pr->target_segment_id, s_mid->id, QP_ID_MAX);
    qp_strlcpy(pr->fill_value, L"严重", QP_VALUE_MAX);
    parse_rule_append(&s_in->parse_rules, pr);

    /* 等级 = 严重 → 固定片段内容变为「（需立即上报）」：链式，依赖上一步结果 */
    cr = cond_rule_create();
    cond_rule_set_simple(cr, s_mid->id, OP_EQUALS, 0, L"严重");
    qp_strlcpy(cr->result_value, L"（需立即上报）", QP_RESULT_MAX);
    cond_rule_append(&s_out->condition_rules, cr);

    ctx = eval_create(t);
    i_in = eval_index_of_id(ctx, s_in->id);
    i_mid = eval_index_of_id(ctx, s_mid->id);
    i_out = eval_index_of_id(ctx, s_out->id);

    eval_set_user_value(ctx, i_in, L"主机发生宕机");
    eval_run(ctx);
    check_str(ctx->fields[i_mid].value, L"严重", "contains 解析规则填充等级");
    check_str(ctx->fields[i_out].value, L"（需立即上报）", "链式条件规则在同一轮内生效");

    eval_set_user_value(ctx, i_in, L"轻微丢包");
    eval_run(ctx);
    check_str(ctx->fields[i_mid].value, L"一般", "解析未命中回落默认等级");
    check_str(ctx->fields[i_out].value, L"（常规处理）", "链式条件同步回落");

    /* 用变量名（而不是 id）引用目标，也应能工作（导入 JSON 容错） */
    qp_strlcpy(pr->target_segment_id, L"等级", QP_ID_MAX);
    eval_set_user_value(ctx, i_in, L"再次宕机");
    eval_run(ctx);
    check_str(ctx->fields[i_mid].value, L"严重", "目标变量按名称引用也能命中");

    eval_destroy(ctx);
    tmpl_free(t);
}

/* ---------------------------------------------------------------- */
static void test_list_ops(void)
{
    Template *t = tmpl_create(L"链表操作");
    Segment *a = seg_create(SEG_STATIC), *b = seg_create(SEG_STATIC), *c = seg_create(SEG_STATIC);
    Segment *cl;
    Template *tc;

    printf("\n== 链表 / 克隆 ==\n");
    qp_strlcpy(a->content, L"A", QP_CONTENT_MAX);
    qp_strlcpy(b->content, L"B", QP_CONTENT_MAX);
    qp_strlcpy(c->content, L"C", QP_CONTENT_MAX);
    seg_list_append(&t->segments, a);
    seg_list_append(&t->segments, b);
    seg_list_append(&t->segments, c);
    check(seg_list_count(t->segments) == 3, "追加 3 个片段");
    check(seg_list_index_of(t->segments, c) == 2, "索引定位正确");

    seg_list_move(&t->segments, 2, 0);
    check_str(seg_list_at(t->segments, 0)->content, L"C", "移动到首位");
    check_str(seg_list_at(t->segments, 2)->content, L"B", "其余顺序保持");

    seg_list_move(&t->segments, 0, 2);
    check_str(seg_list_at(t->segments, 2)->content, L"C", "移动回末位");

    seg_free(seg_list_remove_at(&t->segments, 1));
    check(seg_list_count(t->segments) == 2, "删除中间片段");

    /* 选项增删移动 */
    cl = seg_create(SEG_MULTI_SELECT);
    seg_add_option(cl, L"一");
    seg_add_option(cl, L"二");
    seg_add_option(cl, L"三");
    check(cl->option_count == 3, "添加 3 个选项");
    seg_move_option(cl, 0, 1);
    check_str(cl->options[0], L"二", "选项下移");
    seg_remove_option(cl, 2);
    check(cl->option_count == 2, "删除选项");
    seg_free(cl);

    /* 深拷贝独立性 */
    tc = tmpl_clone(t);
    check(tc != NULL && seg_list_count(tc->segments) == 2, "模板深拷贝片段数一致");
    qp_strlcpy(tc->segments->content, L"改过了", QP_CONTENT_MAX);
    check_str(t->segments->content, L"A", "修改副本不影响原件");
    tmpl_free(tc);
    tmpl_free(t);
}

/* ---------------------------------------------------------------- */
static void test_seed_and_render(void)
{
    AppData *d;
    wchar_t out[QP_OUTPUT_MAX];

    printf("\n== 内置示例数据 ==\n");
    d = appdata_create_seed();
    check(d != NULL, "示例数据创建成功");
    check(tmpl_list_count(d->templates) == 2, "含 2 个示例模板（基础 + 复杂）");
    check(group_list_count(d->groups) == 1, "含 1 个分组");
    check(group_list_find(d->groups, d->templates->group_id) != NULL, "模板归属分组有效");

    tmpl_render_defaults(d->templates, out, QP_OUTPUT_MAX);
    printf("  默认渲染: %ls\n", out);
    check(qp_istr(out, L"本次发布「订单系统」V2.3.1-20240816-rel（正式版）") != NULL,
          "示例默认渲染包含开头");
    check(qp_istr(out, L"影响范围：部分用户（分批灰度）") != NULL,
          "条件规则 startsWith 命中");
    check(qp_istr(out, L"回滚方案：升级前已备份，按需手工回滚") != NULL,
          "条件规则 contains 命中");

    /* 用户输入触发解析规则与更多条件规则 */
    {
        EvalContext *c = eval_create(d->templates);
        int iv = eval_index_of_label(c, L"版本号");
        int ic = eval_index_of_label(c, L"变更类型");
        check(iv >= 0 && ic >= 0, "按标签定位版本号/变更类型");
        eval_set_user_value(c, iv, L"V3.0.0-20240901-alpha");
        eval_set_user_value(c, ic, L"安全加固");
        eval_run(c);
        eval_render(c, out, QP_OUTPUT_MAX);
        printf("  规则渲染: %ls\n", out);
        check(qp_istr(out, L"（内测版）") != NULL, "解析规则 contains 命中 alpha → 内测版");
        check(qp_istr(out, L"影响范围：全部用户（安全加固强制升级）") != NULL,
              "条件规则 in 命中");
        check(qp_istr(out, L"回滚方案：已准备一键回滚") != NULL,
              "条件规则均未命中时回滚方案回落默认值");
        eval_destroy(c);
    }

    appdata_free(d);
}

/* ---------------------------------------------------------------- */
/* 复杂示例模板：它同时用到隐藏变量、多行文本、两类解析规则、多条件项
   规则，所以拿它当一条端到端回归用例最省事。                        */
static void test_demo_template(void)
{
    Template *t;
    EvalContext *c;
    wchar_t out[QP_OUTPUT_MAX];
    int i_ticket, i_level, i_kind, i_action, i_detail, i_reviewer, i_plat;

    printf("\n== 复杂示例模板 ==\n");
    t = tmpl_create_demo();
    check(t != NULL, "复杂示例模板创建成功");
    if (!t) return;

    c = eval_create(t);
    check(c != NULL, "复杂示例可求值");
    if (!c) { tmpl_free(t); return; }

    i_ticket   = eval_index_of_label(c, L"工单编号");
    i_level    = eval_index_of_label(c, L"违规等级");
    i_kind     = eval_index_of_label(c, L"违规类型");
    i_action   = eval_index_of_label(c, L"处置方式");
    i_detail   = eval_index_of_label(c, L"处置说明");
    i_reviewer = eval_index_of_label(c, L"复核人");
    i_plat     = eval_index_of_label(c, L"处置平台");
    check(i_ticket >= 0 && i_level >= 0 && i_kind >= 0 && i_action >= 0 &&
          i_detail >= 0 && i_reviewer >= 0 && i_plat >= 0, "各变量都能按名字定位");

    /* 隐藏 / 多行标记 */
    check(c->fields[eval_index_of_label(c, L"发生日期")].seg->hidden == 1,
          "「发生日期」是隐藏变量");
    check(c->fields[i_detail].seg->multiline == 1, "「处置说明」是多行文本框");
    check(c->fields[i_detail].seg->hidden == 0, "「处置说明」不隐藏");

    /* 默认值：工单编号一填，日期 / 等级 / 处置方式全被规则带出来 */
    eval_run(c);
    eval_render(c, out, QP_OUTPUT_MAX);
    printf("  默认渲染:\n%ls\n", out);
    check_str(c->fields[eval_index_of_label(c, L"发生日期")].value, L"20260817",
              "按分隔符取第 2 段 → 日期");
    check_str(c->fields[i_level].value, L"严重", "按分隔符取第 4 段 → 等级");
    check_str(c->fields[i_action].value, L"永久封禁", "按关键词命中 → 处置方式");
    check(qp_istr(out, L"2 小时内同步风控组") != NULL,
          "多条件项「全部满足」命中（严重 + 封禁）");
    check_str(c->fields[eval_index_of_label(c, L"复核状态")].value, L"（待复核）",
              "复核人为空时回落默认值");

    /* 变量匹配压过手动选择：手点「口头警告」也拗不过工单编号里的「封禁」 */
    eval_set_user_value(c, i_action, L"口头警告");
    eval_run(c);
    check_str(c->fields[i_action].value, L"永久封禁", "规则值压过手动选择");

    /* 换一张轻微工单：等级、处置方式跟着变，后续要求走到第三条规则 */
    eval_set_user_value(c, i_ticket, L"GK-20260101-B7-轻微-警告");
    eval_set_user_value(c, i_detail, L"14:20 群内刷屏，已口头提醒，当事人删除。");
    eval_run(c);
    check_str(c->fields[i_level].value, L"轻微", "换工单后等级随之改变");
    check_str(c->fields[i_action].value, L"口头警告", "换工单后处置方式随之改变");
    check_str(c->fields[eval_index_of_label(c, L"后续要求")].value,
              L"按常规流程归档即可",
              "等级为轻微时取反条件不成立，后续要求回落默认值");

    /* 任意满足：平台换成直播间就该走上报口径 */
    eval_set_user_value(c, i_plat, L"直播间");
    eval_run(c);
    check(qp_istr(c->fields[eval_index_of_label(c, L"后续要求")].value,
                  L"上报内容安全组") != NULL, "多条件项「任意满足」命中（直播间）");

    /* 多行文本也能驱动规则；复核人一填就翻成已复核 */
    eval_set_user_value(c, i_detail, L"14:20 刷屏\r\n14:25 截图取证\r\n14:30 已警告");
    eval_set_user_value(c, i_reviewer, L"张组长");
    eval_run(c);
    check_str(c->fields[eval_index_of_label(c, L"取证情况")].value, L"已附截图取证",
              "多行文本里的关键词也能命中解析规则");
    check_str(c->fields[eval_index_of_label(c, L"复核状态")].value, L"（已复核）",
              "「不为空」取反条件命中");

    /* 严重 + 违规内容：第一条规则不成立时才轮到第二条 */
    eval_set_user_value(c, i_ticket, L"GK-20260202-C1-严重-禁言");
    eval_set_user_value(c, i_kind, L"辱骂他人、违规内容");
    eval_set_user_value(c, i_plat, L"微信群");
    eval_run(c);
    check_str(c->fields[i_action].value, L"禁言 7 天", "关键词「禁言」→ 禁言 7 天");
    check(qp_istr(c->fields[eval_index_of_label(c, L"后续要求")].value,
                  L"上报内容安全组") != NULL,
          "第一条规则不成立时落到「任意满足」那条");

    eval_render(c, out, QP_OUTPUT_MAX);
    printf("  规则渲染:\n%ls\n", out);
    check(qp_istr(out, L"【处置通报】") != NULL, "输出保留固定片段与换行");

    eval_destroy(c);
    tmpl_free(t);
}

/* ---------------------------------------------------------------- */
/* 只靠变量匹配填充的变量（无选项、无默认值）：来源被清空或规则失配时，
   已经推导出的值必须立即清空，不能脱离来源继续存在。                   */
static void test_rule_only_value_clears(void)
{
    Template *t = tmpl_create(L"仅变量匹配");
    Segment *s_head = seg_create(SEG_STATIC);
    Segment *s_code = seg_create(SEG_TEXT);
    Segment *s_kind = seg_create(SEG_SELECT);   /* 无选项、无默认值 */
    Segment *s_area = seg_create(SEG_SELECT);   /* 有默认值，对比用 */
    Segment *s_tail = seg_create(SEG_STATIC);
    EvalContext *ctx;
    ParseRule *pr;
    ConditionRule *cr;
    wchar_t out[QP_OUTPUT_MAX];
    int i_code, i_kind, i_area;

    printf("\n== 仅变量匹配的变量随来源清空 ==\n");

    qp_strlcpy(s_head->content, L"经核查", QP_CONTENT_MAX);
    qp_strlcpy(s_code->label, L"设备编号", QP_LABEL_MAX);
    qp_strlcpy(s_kind->label, L"设备类型", QP_LABEL_MAX);
    qp_strlcpy(s_area->label, L"影响范围", QP_LABEL_MAX);
    qp_strlcpy(s_area->default_value, L"不影响业务", QP_VALUE_MAX);
    qp_strlcpy(s_tail->content, L"。", QP_CONTENT_MAX);

    /* 来源中出现设备编号 → 设备类型取解析结果 */
    pr = parse_rule_create();
    pr->type = PARSE_CONTAINS;
    qp_strlcpy(pr->keyword, L"NFV-D-HNGZ-17A", QP_KEYWORD_MAX);
    qp_strlcpy(pr->target_segment_id, s_kind->id, QP_ID_MAX);
    qp_strlcpy(pr->fill_value, L"NFV-D-HNGZ-17A", QP_VALUE_MAX);
    parse_rule_append(&s_code->parse_rules, pr);

    /* 影响范围：设备编号含 DBS 时改写，否则回落默认值 */
    cr = cond_rule_create();
    cond_rule_set_simple(cr, s_code->id, OP_CONTAINS, 0, L"DBS");
    qp_strlcpy(cr->result_value, L"影响数据库业务", QP_RESULT_MAX);
    cond_rule_append(&s_area->condition_rules, cr);

    seg_list_append(&t->segments, s_head);
    seg_list_append(&t->segments, s_code);
    seg_list_append(&t->segments, s_kind);
    seg_list_append(&t->segments, s_area);
    seg_list_append(&t->segments, s_tail);

    ctx = eval_create(t);
    i_code = eval_index_of_id(ctx, s_code->id);
    i_kind = eval_index_of_id(ctx, s_kind->id);
    i_area = eval_index_of_id(ctx, s_area->id);

    eval_set_user_value(ctx, i_code, L"设备NFV-D-HNGZ-17A异常");
    eval_run(ctx);
    check_str(ctx->fields[i_kind].value, L"NFV-D-HNGZ-17A", "解析命中");

    eval_set_user_value(ctx, i_code, L"");
    eval_run(ctx);
    check_str(ctx->fields[i_kind].value, L"", "清空来源后派生值清空");

    /* 输入被改成不再命中规则的内容 */
    eval_set_user_value(ctx, i_code, L"NFV-D-XXX-02");
    eval_run(ctx);
    check_str(ctx->fields[i_kind].value, L"",
              "规则不再命中时派生值清空");
    check_str(ctx->fields[i_area].value, L"不影响业务",
              "有默认值的变量照常回落默认值");
    eval_render(ctx, out, QP_OUTPUT_MAX);
    check(qp_istr(out, L"NFV-D-HNGZ-17A") == NULL, "输出不残留旧派生值");

    /* 没有规则命中时，用户手动值仍然生效 */
    eval_set_user_value(ctx, i_kind, L"存储服务器");
    eval_run(ctx);
    check_str(ctx->fields[i_kind].value, L"存储服务器", "用户手动输入作为兜底值");

    /* 重置：手动值清空 */
    eval_reset(ctx);
    check(ctx->fields[i_kind].value[0] == L'\0', "重置后无来源变量为空");
    check_str(ctx->fields[i_area].value, L"不影响业务", "重置后回到默认值");

    eval_destroy(ctx);
    tmpl_free(t);
}

/* ---------------------------------------------------------------- */
/* 「在填写界面隐藏」开关：默认关闭，克隆保留，且不影响渲染 */
static void test_hidden_flag(void)
{
    Template *t = tmpl_create(L"隐藏开关");
    Segment *s_txt = seg_create(SEG_TEXT);
    Segment *s_sel = seg_create(SEG_SELECT);
    Segment *clone;
    Template *tc;
    EvalContext *ctx;
    wchar_t out[QP_OUTPUT_MAX];

    printf("\n== 隐藏开关 ==\n");
    check(s_txt->hidden == 0 && s_sel->hidden == 0, "新建变量默认不隐藏");

    qp_strlcpy(s_txt->label, L"设备编号", QP_LABEL_MAX);
    qp_strlcpy(s_txt->default_value, L"编号A", QP_VALUE_MAX);
    qp_strlcpy(s_sel->label, L"设备类型", QP_LABEL_MAX);
    qp_strlcpy(s_sel->default_value, L"数据库服务器", QP_VALUE_MAX);
    s_sel->hidden = 1;
    seg_list_append(&t->segments, s_txt);
    seg_list_append(&t->segments, s_sel);

    clone = seg_clone(s_sel);
    check(clone && clone->hidden == 1, "seg_clone 保留隐藏标记");
    seg_free(clone);

    tc = tmpl_clone(t);
    check(tc && seg_list_at(tc->segments, 1)->hidden == 1, "tmpl_clone 保留隐藏标记");
    tmpl_free(tc);

    /* 隐藏只影响界面，输出照旧 */
    ctx = eval_create(t);
    eval_run(ctx);
    eval_render(ctx, out, QP_OUTPUT_MAX);
    check_str(out, L"编号A数据库服务器", "隐藏变量仍然参与输出");
    eval_destroy(ctx);
    tmpl_free(t);
}

/* ---------------------------------------------------------------- */
static void test_edge_cases(void)
{
    Template *t = tmpl_create(L"边界");
    Segment *s = seg_create(SEG_TEXT);
    Segment *sel = seg_create(SEG_SELECT);
    EvalContext *ctx;
    ParseRule *pr;
    ConditionRule *cr;
    wchar_t out[16];
    static const wchar_t *opts[] = { L"首个选项", L"次选项" };

    printf("\n== 边界与健壮性 ==\n");

    /* 空模板 */
    ctx = eval_create(t);
    check(ctx != NULL && ctx->count == 0, "空模板可求值");
    eval_render(ctx, out, 16);
    check_str(out, L"", "空模板渲染为空串");
    eval_destroy(ctx);

    /* 单选未设默认值 → 保持为空；选项本身不等于默认值 */
    seg_set_options(sel, opts, 2);
    qp_strlcpy(sel->label, L"单选", QP_LABEL_MAX);
    seg_list_append(&t->segments, sel);
    ctx = eval_create(t);
    check_str(ctx->fields[0].value, L"", "单选无默认值时为空");
    eval_destroy(ctx);

    /* 指向不存在目标 / 触发源的规则不应崩溃 */
    qp_strlcpy(s->label, L"文本", QP_LABEL_MAX);
    pr = parse_rule_create();
    pr->type = PARSE_CONTAINS;
    qp_strlcpy(pr->keyword, L"x", QP_KEYWORD_MAX);
    qp_strlcpy(pr->target_segment_id, L"不存在的片段", QP_ID_MAX);
    parse_rule_append(&s->parse_rules, pr);
    cr = cond_rule_create();
    cond_rule_set_simple(cr, L"也不存在", OP_CONTAINS, 0, L"y");
    qp_strlcpy(cr->result_value, L"不该出现", QP_RESULT_MAX);
    cond_rule_append(&s->condition_rules, cr);
    /* 取反的悬空条件也不能反过来「一律成立」 */
    cr = cond_rule_create();
    cond_rule_set_simple(cr, L"同样不存在", OP_CONTAINS, 1, L"y");
    qp_strlcpy(cr->result_value, L"更不该出现", QP_RESULT_MAX);
    cond_rule_append(&s->condition_rules, cr);
    seg_list_append(&t->segments, s);

    ctx = eval_create(t);
    eval_set_user_value(ctx, 1, L"含 x 的内容");
    eval_run(ctx);
    check(qp_istr(ctx->fields[1].value, L"不该出现") == NULL, "悬空规则被忽略且不崩溃");
    check(qp_istr(ctx->fields[1].value, L"更不该出现") == NULL, "悬空条件取反后仍不命中");
    eval_destroy(ctx);

    /* 渲染缓冲区溢出保护 */
    ctx = eval_create(t);
    eval_set_user_value(ctx, 1, L"这是一段很长很长很长的内容用于测试截断");
    eval_run(ctx);
    eval_render(ctx, out, 16);
    check(wcslen(out) < 16, "渲染结果不超出缓冲区");
    eval_destroy(ctx);

    tmpl_free(t);
}

/* ---------------------------------------------------------------- */
/* 多条件项：全部满足 / 任意满足 / 取反 / 为空                        */
static void test_multi_condition(void)
{
    Template *t = tmpl_create(L"多条件");
    Segment *sa = seg_create(SEG_TEXT);
    Segment *sb = seg_create(SEG_TEXT);
    Segment *host = seg_create(SEG_STATIC);
    ConditionRule *cr, *r_all, *r_any;
    CondTerm *term;
    EvalContext *ctx;
    wchar_t txt[512];
    int ia, ib, ih;

    printf("\n== 多条件项组合 ==\n");
    qp_strlcpy(sa->label, L"部件", QP_LABEL_MAX);
    qp_strlcpy(sb->label, L"状态", QP_LABEL_MAX);
    qp_strlcpy(host->content, L"默认", QP_CONTENT_MAX);
    seg_list_append(&t->segments, sa);
    seg_list_append(&t->segments, sb);
    seg_list_append(&t->segments, host);

    /* 规则 0：一个条件项都没有 —— 永远不该命中（放在最前面验证） */
    cr = cond_rule_create();
    cond_term_list_clear(&cr->terms);
    qp_strlcpy(cr->result_value, L"空规则不该命中", QP_RESULT_MAX);
    cond_rule_append(&host->condition_rules, cr);
    check(cond_term_count(cr->terms) == 0, "清空后条件项为 0");

    /* 规则 1（全部满足）：部件包含「硬盘」 且 状态不为空 */
    r_all = cond_rule_create();
    r_all->join = COND_JOIN_ALL;
    cond_rule_set_simple(r_all, sa->id, OP_CONTAINS, 0, L"硬盘");
    term = cond_term_create();
    qp_strlcpy(term->trigger_segment_id, sb->id, QP_ID_MAX);
    term->op = OP_EMPTY;
    term->negate = 1;
    cond_term_append(&r_all->terms, term);
    qp_strlcpy(r_all->result_value, L"两者都满足", QP_RESULT_MAX);
    cond_rule_append(&host->condition_rules, r_all);
    check(cond_term_count(r_all->terms) == 2, "cond_rule_set_simple 后追加条件项共 2 项");

    /* 规则 2（任意满足）：部件包含「内存」 或 状态等于「紧急」 */
    r_any = cond_rule_create();
    r_any->join = COND_JOIN_ANY;
    cond_rule_set_simple(r_any, sa->id, OP_CONTAINS, 0, L"内存");
    r_any->join = COND_JOIN_ANY;      /* set_simple 会重置为 ALL，这里再设回来 */
    term = cond_term_create();
    qp_strlcpy(term->trigger_segment_id, sb->id, QP_ID_MAX);
    term->op = OP_EQUALS;
    cond_term_set_match(term, L"紧急");
    cond_term_append(&r_any->terms, term);
    qp_strlcpy(r_any->result_value, L"任一满足", QP_RESULT_MAX);
    cond_rule_append(&host->condition_rules, r_any);

    ctx = eval_create(t);
    ia = eval_index_of_id(ctx, sa->id);
    ib = eval_index_of_id(ctx, sb->id);
    ih = eval_index_of_id(ctx, host->id);

    eval_set_user_value(ctx, ia, L"硬盘异常");
    eval_run(ctx);
    check_str(ctx->fields[ih].value, L"默认", "AND：只满足一半时不命中");

    eval_set_user_value(ctx, ib, L"已恢复");
    eval_run(ctx);
    check_str(ctx->fields[ih].value, L"两者都满足", "AND：两个条件都满足才命中");

    eval_set_user_value(ctx, ia, L"内存异常");
    eval_set_user_value(ctx, ib, L"");
    eval_run(ctx);
    check_str(ctx->fields[ih].value, L"任一满足", "OR：第一个条件满足即命中");

    eval_set_user_value(ctx, ia, L"风扇异常");
    eval_set_user_value(ctx, ib, L"紧急");
    eval_run(ctx);
    check_str(ctx->fields[ih].value, L"任一满足", "OR：第二个条件满足即命中");

    eval_set_user_value(ctx, ia, L"风扇异常");
    eval_set_user_value(ctx, ib, L"");
    eval_run(ctx);
    check_str(ctx->fields[ih].value, L"默认", "两条规则都不命中时回落默认内容");
    eval_destroy(ctx);

    /* 摘要文字 */
    cond_rule_summary(r_all, t, txt, 512);
    check(qp_istr(txt, L"部件 包含「硬盘」") != NULL, "摘要含第一个条件");
    check(qp_istr(txt, L" 且 ") != NULL, "摘要用「且」连接");
    check(qp_istr(txt, L"状态 不为空") != NULL, "摘要把取反的「为空」写成「不为空」");
    cond_rule_summary(r_any, t, txt, 512);
    check(qp_istr(txt, L" 或 ") != NULL, "任意满足的摘要用「或」连接");
    cond_rule_summary(cond_rule_at(host->condition_rules, 0), t, txt, 512);
    check(qp_istr(txt, L"还没有设置条件") != NULL, "空规则摘要有提示");
    cond_term_summary(r_all->terms, t, txt, 512);
    check_str(txt, L"部件 包含「硬盘」", "单个条件项摘要");

    check(qp_icmp(cond_op_phrase(OP_CONTAINS, 1), L"不包含") == 0, "取反说法：不包含");
    check(qp_icmp(cond_op_phrase(OP_IN, 1), L"不属于列表") == 0, "取反说法：不属于列表");
    check(cond_op_needs_value(OP_EMPTY) == 0, "「为空」不需要匹配值");
    check(cond_op_needs_value(OP_ENDS_WITH) == 1, "「结尾是」需要匹配值");

    /* 条件项增删 */
    check(cond_term_remove_at(&r_any->terms, 0) == 1, "删除第一个条件项");
    check(cond_term_count(r_any->terms) == 1, "删除后剩 1 项");
    check(cond_term_at(r_any->terms, 0)->op == OP_EQUALS, "剩下的是原来的第二项");
    check(cond_term_at(r_any->terms, 5) == NULL, "越界取条件项返回 NULL");

    /* cond_rule_assign：换内容，但保留 id 与链表位置 */
    {
        ConditionRule *src = cond_rule_create();
        ConditionRule *keep_next = r_all->next;
        wchar_t keep_id[QP_ID_MAX];
        qp_strlcpy(keep_id, r_all->id, QP_ID_MAX);
        cond_rule_set_simple(src, sa->id, OP_ENDS_WITH, 1, L"正常");
        qp_strlcpy(src->result_value, L"替换后的内容", QP_RESULT_MAX);
        cond_rule_assign(r_all, src);
        cond_rule_free(src);            /* assign 是深拷贝，释放源不影响目标 */
        check(wcscmp(r_all->id, keep_id) == 0 && r_all->next == keep_next,
              "cond_rule_assign 保留 id 与链表位置");
        check(cond_term_count(r_all->terms) == 1 &&
              r_all->terms->op == OP_ENDS_WITH && r_all->terms->negate == 1,
              "cond_rule_assign 换掉条件项");
        check_str(r_all->result_value, L"替换后的内容", "cond_rule_assign 换掉输出内容");
    }

    tmpl_free(t);
}

/* ---------------------------------------------------------------- */
/* 时间格式化：用固定的 struct tm，结果才是可断言的               */
static void test_time_format(void)
{
    struct tm tmv;
    wchar_t buf[128];

    printf("\n== 时间格式 ==\n");

    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = 2026 - 1900;
    tmv.tm_mon  = 8 - 1;
    tmv.tm_mday = 18;
    tmv.tm_hour = 9;
    tmv.tm_min  = 5;
    tmv.tm_sec  = 7;
    tmv.tm_wday = 2;                       /* 周二 */

    qp_format_time(&tmv, L"yyyy-MM-dd HH:mm:ss", buf, 128);
    check_str(buf, L"2026-08-18 09:05:07", "yyyy-MM-dd HH:mm:ss");
    qp_format_time(&tmv, L"yy/M/d H:m:s", buf, 128);
    check_str(buf, L"26/8/18 9:5:7", "两位年份与单字记号不补零");
    qp_format_time(&tmv, L"yyyy年M月d日 ddd", buf, 128);
    check_str(buf, L"2026年8月18日 周二", "中文格式与星期");
    qp_format_time(&tmv, L"yyyyMMddHHmmss", buf, 128);
    check_str(buf, L"20260818090507", "紧凑格式");
    qp_format_time(&tmv, L"截止 HH:mm 前完成", buf, 128);
    check_str(buf, L"截止 09:05 前完成", "非记号字符原样保留");
    qp_format_time(&tmv, L"", buf, 128);
    check_str(buf, L"2026-08-18 09:05:07", "空格式 = 默认格式");

    /* 缓冲不够时截断而不越界 */
    qp_format_time(&tmv, L"yyyy-MM-dd HH:mm:ss", buf, 6);
    check(wcslen(buf) < 6, "缓冲不足时安全截断");

    /* 预置格式表 */
    check(qp_time_format_count() >= 8, "预置格式不少于 8 个");
    check_str(qp_time_format_at(0), L"yyyy-MM-dd HH:mm:ss", "第一个预置格式");
    check_str(qp_time_format_at(-1), L"", "越界取预置格式得空串");
    check_str(qp_time_format_at(qp_time_format_count()), L"", "越界（上界）得空串");
}

/* ---------------------------------------------------------------- */
/* 时间变量：开这份填写时自动填当下，之后不再自己跳动              */
static void test_datetime_field(void)
{
    Template *t = tmpl_create(L"时间变量");
    Segment *s_head = seg_create(SEG_STATIC);
    Segment *s_when = seg_create(SEG_DATETIME);
    Segment *s_tail = seg_create(SEG_STATIC);
    EvalContext *ctx;
    wchar_t now[128];
    int i_when;

    printf("\n== 时间变量 ==\n");

    check_str(s_when->time_format, qp_time_format_at(0), "新建时间变量自带默认格式");

    qp_strlcpy(s_head->content, L"于 ", QP_CONTENT_MAX);
    qp_strlcpy(s_when->label, L"发生时间", QP_LABEL_MAX);
    qp_strlcpy(s_when->time_format, L"yyyy-MM-dd", QP_FORMAT_MAX);
    qp_strlcpy(s_tail->content, L" 发现告警。", QP_CONTENT_MAX);
    seg_list_append(&t->segments, s_head);
    seg_list_append(&t->segments, s_when);
    seg_list_append(&t->segments, s_tail);

    ctx = eval_create(t);
    i_when = eval_index_of_id(ctx, s_when->id);
    qp_format_time_now(L"yyyy-MM-dd", now, 128);
    check_str(ctx->fields[i_when].value, now, "开这份填写时自动填当下时间");
    check(ctx->fields[i_when].user_touched == 1, "时间快照落在「手动填写」那一层");
    check(ctx->fields[i_when].rule_driven == 0, "时间不是规则推导值，界面可改");

    /* 手改之后再跑规则，不会被自动填充顶回去 */
    eval_set_user_value(ctx, i_when, L"2020-01-01");
    eval_run(ctx);
    check_str(ctx->fields[i_when].value, L"2020-01-01", "手改的时间不会被自动刷新覆盖");

    /* 清空就是真的空：默认值那一层不会冒出来（时间类型也不该有默认值） */
    eval_set_user_value(ctx, i_when, L"");
    eval_run(ctx);
    check_str(ctx->fields[i_when].value, L"", "清空时间后不冒出别的值");

    /* 重置 = 重新拍一张快照 */
    eval_reset(ctx);
    qp_format_time_now(L"yyyy-MM-dd", now, 128);
    check_str(ctx->fields[i_when].value, now, "重置后重新填当下时间");

    eval_destroy(ctx);
    tmpl_free(t);
}

/* ---------------------------------------------------------------- */
/* 引用取法：三种模式与越界                                        */
static void test_ref_extract(void)
{
    Segment *s = seg_create(SEG_TEXT);
    wchar_t buf[64];

    printf("\n== 引用取法 ==\n");

    s->ref_mode = REF_WHOLE;
    seg_ref_extract(s, L"13800001234", buf, 64);
    check_str(buf, L"13800001234", "整体引用");

    s->ref_mode = REF_SPLIT;
    qp_strlcpy(s->ref_delimiter, L"-", QP_SEP_MAX);
    s->ref_index = 3;
    seg_ref_extract(s, L"NFV-D-HNZJI-11A", buf, 64);
    check_str(buf, L"HNZJI", "按分隔符取第 3 段");
    s->ref_index = 9;
    seg_ref_extract(s, L"NFV-D-HNZJI-11A", buf, 64);
    check_str(buf, L"", "段号越界得空串");

    s->ref_mode = REF_SUBSTR;
    s->ref_start = 8;
    s->ref_len = 4;
    seg_ref_extract(s, L"13800001234", buf, 64);
    check_str(buf, L"1234", "从第 8 个字起取 4 个（手机尾号）");
    s->ref_len = 0;
    seg_ref_extract(s, L"13800001234", buf, 64);
    check_str(buf, L"1234", "取几个留空 = 取到末尾");
    s->ref_len = 99;
    seg_ref_extract(s, L"13800001234", buf, 64);
    check_str(buf, L"1234", "取多了也只取到末尾");
    s->ref_start = 30;
    s->ref_len = 4;
    seg_ref_extract(s, L"13800001234", buf, 64);
    check_str(buf, L"", "起点越界得空串");
    s->ref_start = 0;                 /* 非法起点当作第 1 个字 */
    s->ref_len = 3;
    seg_ref_extract(s, L"13800001234", buf, 64);
    check_str(buf, L"138", "起点 0 当作第 1 个字");

    seg_free(s);
}

/* ---------------------------------------------------------------- */
/* 引用参与求值：优先级、链式、成环、悬空                          */
static void test_reference(void)
{
    Template *t = tmpl_create(L"引用");
    Segment *s_phone = seg_create(SEG_TEXT);      /* 来源：电话号码     */
    Segment *s_code  = seg_create(SEG_TEXT);      /* 来源：设备编号     */
    Segment *s_whole = seg_create(SEG_TEXT);      /* 整体引用电话       */
    Segment *s_tail4 = seg_create(SEG_TEXT);      /* 引用电话尾号       */
    Segment *s_echo  = seg_create(SEG_TEXT);      /* 引用尾号（链式）   */
    Segment *s_area  = seg_create(SEG_SELECT);    /* 引用设备编号第 3 段 */
    Segment *s_lose  = seg_create(SEG_TEXT);      /* 引用已删除的变量   */
    EvalContext *ctx;
    ParseRule *pr;
    ConditionRule *cr;
    int i_phone, i_code, i_whole, i_tail4, i_echo, i_area, i_lose;

    printf("\n== 引用 ==\n");

    qp_strlcpy(s_phone->label, L"联系电话", QP_LABEL_MAX);
    qp_strlcpy(s_code->label,  L"设备编号", QP_LABEL_MAX);
    qp_strlcpy(s_whole->label, L"回填电话", QP_LABEL_MAX);
    qp_strlcpy(s_tail4->label, L"手机尾号", QP_LABEL_MAX);
    qp_strlcpy(s_echo->label,  L"尾号复述", QP_LABEL_MAX);
    qp_strlcpy(s_area->label,  L"所属区段", QP_LABEL_MAX);
    qp_strlcpy(s_lose->label,  L"悬空引用", QP_LABEL_MAX);

    qp_strlcpy(s_whole->ref_segment_id, s_phone->id, QP_ID_MAX);
    s_whole->ref_mode = REF_WHOLE;

    qp_strlcpy(s_tail4->ref_segment_id, s_phone->id, QP_ID_MAX);
    s_tail4->ref_mode = REF_SUBSTR;
    s_tail4->ref_start = 8;
    s_tail4->ref_len = 4;

    qp_strlcpy(s_echo->ref_segment_id, s_tail4->id, QP_ID_MAX);
    s_echo->ref_mode = REF_WHOLE;

    qp_strlcpy(s_area->ref_segment_id, s_code->id, QP_ID_MAX);
    s_area->ref_mode = REF_SPLIT;
    qp_strlcpy(s_area->ref_delimiter, L"-", QP_SEP_MAX);
    s_area->ref_index = 3;

    qp_strlcpy(s_lose->ref_segment_id, L"seg-not-exist", QP_ID_MAX);
    qp_strlcpy(s_lose->default_value, L"兜底值", QP_VALUE_MAX);

    seg_list_append(&t->segments, s_phone);
    seg_list_append(&t->segments, s_code);
    seg_list_append(&t->segments, s_whole);
    seg_list_append(&t->segments, s_tail4);
    seg_list_append(&t->segments, s_echo);
    seg_list_append(&t->segments, s_area);
    seg_list_append(&t->segments, s_lose);

    ctx = eval_create(t);
    i_phone = eval_index_of_id(ctx, s_phone->id);
    i_code  = eval_index_of_id(ctx, s_code->id);
    i_whole = eval_index_of_id(ctx, s_whole->id);
    i_tail4 = eval_index_of_id(ctx, s_tail4->id);
    i_echo  = eval_index_of_id(ctx, s_echo->id);
    i_area  = eval_index_of_id(ctx, s_area->id);
    i_lose  = eval_index_of_id(ctx, s_lose->id);

    eval_set_user_value(ctx, i_phone, L"13800001234");
    eval_set_user_value(ctx, i_code, L"NFV-D-HNZJI-11A");
    eval_run(ctx);
    check_str(ctx->fields[i_whole].value, L"13800001234", "整体引用跟着来源");
    check_str(ctx->fields[i_tail4].value, L"1234", "部分引用跟着来源");
    check_str(ctx->fields[i_echo].value, L"1234", "链式引用一轮传到底");
    check_str(ctx->fields[i_area].value, L"HNZJI", "单选变量也能被引用填充");
    check(ctx->fields[i_whole].rule_driven == 1 && ctx->fields[i_area].rule_driven == 1,
          "引用命中即算规则接管（界面只读）");
    check_str(ctx->fields[i_lose].value, L"兜底值", "引用已删除的变量 = 没配引用");

    /* 来源变了，目标跟着变 */
    eval_set_user_value(ctx, i_phone, L"13912345678");
    eval_run(ctx);
    check_str(ctx->fields[i_tail4].value, L"5678", "来源改了目标立刻跟上");

    /* 手填压不过引用 */
    eval_set_user_value(ctx, i_whole, L"手填的号码");
    eval_run(ctx);
    check_str(ctx->fields[i_whole].value, L"13912345678", "引用压过手动填写");

    /* 来源为空 → 目标为空，不回落默认值（引用是明确的接管） */
    qp_strlcpy(s_whole->default_value, L"不该出现", QP_VALUE_MAX);
    eval_set_user_value(ctx, i_phone, L"");
    eval_run(ctx);
    check_str(ctx->fields[i_whole].value, L"", "来源为空时目标为空，不回落默认值");
    eval_set_user_value(ctx, i_phone, L"13912345678");

    /* 解析规则压过引用：设备编号里含 HNZJI → 回填电话 = 湖南 */
    pr = parse_rule_create();
    pr->type = PARSE_CONTAINS;
    qp_strlcpy(pr->keyword, L"HNZJI", QP_KEYWORD_MAX);
    qp_strlcpy(pr->target_segment_id, s_whole->id, QP_ID_MAX);
    qp_strlcpy(pr->fill_value, L"解析规则的值", QP_VALUE_MAX);
    parse_rule_append(&s_code->parse_rules, pr);
    eval_run(ctx);
    check_str(ctx->fields[i_whole].value, L"解析规则的值", "解析规则压过引用");

    /* 条件规则压过引用与解析规则 */
    cr = cond_rule_create();
    cond_rule_set_simple(cr, s_phone->id, OP_CONTAINS, 0, L"139");
    qp_strlcpy(cr->result_value, L"条件规则的值", QP_RESULT_MAX);
    cond_rule_append(&s_whole->condition_rules, cr);
    eval_run(ctx);
    check_str(ctx->fields[i_whole].value, L"条件规则的值", "条件规则压过解析规则与引用");
    check(ctx->unstable == 0, "多种规则叠加仍能收敛");

    /* 自引用：视为没配引用，退回手填 */
    qp_strlcpy(s_tail4->ref_segment_id, s_tail4->id, QP_ID_MAX);
    eval_set_user_value(ctx, i_tail4, L"自己填的");
    eval_run(ctx);
    check_str(ctx->fields[i_tail4].value, L"自己填的", "自引用不生效，退回手动填写");

    /* A→B→A 成环：整条链不生效，也不死循环 */
    qp_strlcpy(s_tail4->ref_segment_id, s_echo->id, QP_ID_MAX);   /* 尾号 ← 复述 */
    /* s_echo 仍然引用 s_tail4，于是两者成环 */
    eval_set_user_value(ctx, i_echo, L"环里的另一个");
    eval_run(ctx);
    check_str(ctx->fields[i_tail4].value, L"自己填的", "成环的引用不生效（一头）");
    check_str(ctx->fields[i_echo].value, L"环里的另一个", "成环的引用不生效（另一头）");
    check(ctx->fields[i_echo].rule_driven == 0, "成环的引用不算规则接管，界面仍可改");

    eval_destroy(ctx);
    tmpl_free(t);
}

/* ---------------------------------------------------------------- */
/* 引用的成环判定与文字说明（编辑器要用）                          */
static void test_ref_guard(void)
{
    Template *t = tmpl_create(L"成环判定");
    Segment *a = seg_create(SEG_TEXT);
    Segment *b = seg_create(SEG_TEXT);
    Segment *c = seg_create(SEG_TEXT);
    wchar_t buf[256];

    printf("\n== 引用成环判定 ==\n");

    qp_strlcpy(a->label, L"甲", QP_LABEL_MAX);
    qp_strlcpy(b->label, L"乙", QP_LABEL_MAX);
    qp_strlcpy(c->label, L"丙", QP_LABEL_MAX);
    seg_list_append(&t->segments, a);
    seg_list_append(&t->segments, b);
    seg_list_append(&t->segments, c);

    check(tmpl_ref_would_cycle(t, a, a->id) == 1, "自己引用自己算成环");
    check(tmpl_ref_would_cycle(t, b, a->id) == 0, "乙引用甲不成环");

    qp_strlcpy(b->ref_segment_id, a->id, QP_ID_MAX);        /* 乙 ← 甲 */
    check(tmpl_ref_would_cycle(t, a, b->id) == 1, "甲再引用乙就成环");
    check(tmpl_ref_would_cycle(t, c, b->id) == 0, "丙引用乙不成环");

    qp_strlcpy(c->ref_segment_id, b->id, QP_ID_MAX);        /* 丙 ← 乙 ← 甲 */
    check(tmpl_ref_would_cycle(t, a, c->id) == 1, "隔一级绕回来也算成环");
    check(tmpl_ref_would_cycle(t, a, L"") == 0, "不引用（空来源）不成环");
    check(tmpl_ref_would_cycle(t, a, L"seg-not-exist") == 0, "引用不存在的变量不成环");

    /* 文字说明 */
    seg_ref_summary(c, t, buf, 256);
    check_str(buf, L"引用[乙]", "整体引用的说明");
    c->ref_mode = REF_SPLIT;
    qp_strlcpy(c->ref_delimiter, L"-", QP_SEP_MAX);
    c->ref_index = 2;
    seg_ref_summary(c, t, buf, 256);
    check_str(buf, L"引用[乙] 按“-”取第 2 段", "分段引用的说明");
    c->ref_mode = REF_SUBSTR;
    c->ref_start = 3;
    c->ref_len = 4;
    seg_ref_summary(c, t, buf, 256);
    check_str(buf, L"引用[乙] 第 3 个字起取 4 个", "按字引用的说明");
    c->ref_len = 0;
    seg_ref_summary(c, t, buf, 256);
    check_str(buf, L"引用[乙] 第 3 个字起到末尾", "取几个留空的说明");
    qp_strlcpy(c->ref_segment_id, L"seg-not-exist", QP_ID_MAX);
    seg_ref_summary(c, t, buf, 256);
    check(wcsstr(buf, L"(已删除的变量)") != NULL, "悬空引用的说明写明变量已删除");
    c->ref_segment_id[0] = L'\0';
    seg_ref_summary(c, t, buf, 256);
    check_str(buf, L"", "没配引用则没有说明");

    tmpl_free(t);
}

/* ---------------------------------------------------------------- */
static const wchar_t *ph_lookup(void *user, const wchar_t *name)
{
    (void)user;
    if (wcscmp(name, L"甲") == 0) return L"张三";
    if (wcscmp(name, L"空的") == 0) return L"";     /* 认得，但此刻是空 */
    if (wcscmp(name, L"套") == 0) return L"{甲}";   /* 值里还带占位符   */
    if (wcscmp(name, L"含转义") == 0) return L"X\\tY"; /* 值里带字面 \t  */
    return NULL;
}

static void test_placeholder(void)
{
    wchar_t buf[128], name[QP_LABEL_MAX];
    size_t n;

    printf("\n== 占位符 ==\n");

    qp_expand_placeholders(L"你好{甲}，请查收", ph_lookup, NULL, buf, 128);
    check_str(buf, L"你好张三，请查收", "占位符换成变量的值");

    qp_expand_placeholders(L"{甲}和{甲}", ph_lookup, NULL, buf, 128);
    check_str(buf, L"张三和张三", "同一个占位符出现两次都换");

    qp_expand_placeholders(L"余额 {{ 元", ph_lookup, NULL, buf, 128);
    check_str(buf, L"余额 { 元", "{{ 输出一个字面的 {");

    qp_expand_placeholders(L"{{甲}", ph_lookup, NULL, buf, 128);
    check_str(buf, L"{甲}", "{{ 转义后整段不再当占位符");

    qp_expand_placeholders(L"{没有这个人}在", ph_lookup, NULL, buf, 128);
    check_str(buf, L"{没有这个人}在", "认不出的名字原样保留（写错了要看得见）");

    qp_expand_placeholders(L"{空的}收到", ph_lookup, NULL, buf, 128);
    check_str(buf, L"收到", "认得但值为空 → 换成空");

    qp_expand_placeholders(L"{ 甲 }", ph_lookup, NULL, buf, 128);
    check_str(buf, L"张三", "名字两侧的空白忽略");

    qp_expand_placeholders(L"缺个右括号 {甲 就这样", ph_lookup, NULL, buf, 128);
    check_str(buf, L"缺个右括号 {甲 就这样", "少了 } 不吞后面的正文");

    qp_expand_placeholders(L"{}空名", ph_lookup, NULL, buf, 128);
    check_str(buf, L"{}空名", "空名字不算占位符");

    qp_expand_placeholders(L"[{套}]", ph_lookup, NULL, buf, 128);
    check_str(buf, L"[{甲}]", "换进来的内容里的 {…} 不再展开（只替换一层）");

    n = qp_expand_placeholders(L"{甲}也是{甲}", ph_lookup, NULL, buf, 4);
    check(n == 3 && wcslen(buf) == 3, "缓冲装不下时按 cap 截断");

    qp_expand_placeholders(L"没有占位符", ph_lookup, NULL, buf, 128);
    check_str(buf, L"没有占位符", "没有占位符时原样输出");

    check(qp_placeholder_at(L"{甲}和{乙}", 0, name, QP_LABEL_MAX) == 1, "取第 0 个占位符");
    check_str(name, L"甲", "第 0 个占位符的名字");
    check(qp_placeholder_at(L"{甲}和{乙}", 1, name, QP_LABEL_MAX) == 1, "取第 1 个占位符");
    check_str(name, L"乙", "第 1 个占位符的名字");
    check(qp_placeholder_at(L"{甲}和{乙}", 2, name, QP_LABEL_MAX) == 0, "没有第 2 个");
    check(qp_placeholder_at(L"没有占位符", 0, name, QP_LABEL_MAX) == 0, "没占位符时返回 0");
    check(qp_placeholder_at(L"{{甲}", 0, name, QP_LABEL_MAX) == 0, "转义的不算占位符");
}

/* ---------------------------------------------------------------- */
static void test_placeholder_in_eval(void)
{
    Template *t = tmpl_create(L"占位符参与求值");
    Segment *phone = seg_create(SEG_TEXT);      /* 手填：电话           */
    Segment *kind  = seg_create(SEG_TEXT);      /* 隐藏：靠条件规则拿值 */
    Segment *line  = seg_create(SEG_STATIC);
    ConditionRule *r;
    EvalContext *ctx;
    wchar_t out[QP_OUTPUT_MAX];

    printf("\n== 占位符参与求值 ==\n");

    qp_strlcpy(phone->label, L"电话", QP_LABEL_MAX);
    qp_strlcpy(kind->label, L"类型", QP_LABEL_MAX);
    kind->hidden = 1;
    r = cond_rule_create();
    cond_rule_set_simple(r, phone->id, OP_STARTS_WITH, 0, L"139");
    qp_strlcpy(r->result_value, L"移动号码", QP_RESULT_MAX);
    cond_rule_append(&kind->condition_rules, r);
    /* 一句话里同时夹两个变量 —— 从前得拆成「文本-变量-文本-变量-文本」五段 */
    qp_strlcpy(line->content, L"｜请回电 {电话}（{类型}），{{ 谢谢", QP_CONTENT_MAX);

    seg_list_append(&t->segments, phone);
    seg_list_append(&t->segments, kind);
    seg_list_append(&t->segments, line);

    ctx = eval_create(t);
    eval_set_user_value(ctx, 0, L"13900001111");
    eval_run(ctx);
    eval_render(ctx, out, QP_OUTPUT_MAX);
    check(wcsstr(out, L"｜请回电 13900001111（移动号码），{ 谢谢") != NULL,
          "固定内容里的占位符取到了手填值与规则推导值");

    /* 规则本轮才推出的值，占位符也能取到（同一个不动点里收敛）。
       换成不命中的号码后，类型随来源立即清空。 */
    eval_set_user_value(ctx, 0, L"13800002222");
    eval_run(ctx);
    eval_render(ctx, out, QP_OUTPUT_MAX);
    check(wcsstr(out, L"｜请回电 13800002222（），{ 谢谢") != NULL,
          "规则失配后占位符中的派生值清空");

    /* 用户自己敲进文本框的 { 永远是字面量 */
    eval_set_user_value(ctx, 0, L"{类型}");
    eval_run(ctx);
    eval_render(ctx, out, QP_OUTPUT_MAX);
    check(wcsstr(out, L"｜请回电 {类型}（），{ 谢谢") != NULL,
          "手打的 { 是字面量，换进来后也不再展开");
    eval_destroy(ctx);

    /* 换一份干净的填写：规则不命中 → 占位符是空 */
    ctx = eval_create(t);
    eval_set_user_value(ctx, 0, L"13800002222");
    eval_run(ctx);
    eval_render(ctx, out, QP_OUTPUT_MAX);
    check(wcsstr(out, L"｜请回电 13800002222（），{ 谢谢") != NULL,
          "规则没命中过时占位符是空");
    eval_destroy(ctx);

    /* 编辑器预览走的是同一条路（默认值渲染） */
    qp_strlcpy(phone->default_value, L"13911112222", QP_VALUE_MAX);
    tmpl_render_defaults(t, out, QP_OUTPUT_MAX);
    check(wcsstr(out, L"｜请回电 13911112222（移动号码），{ 谢谢") != NULL,
          "默认值预览里占位符也已展开");

    /* 指向自己不展开（固定片段没有变量名，只可能写 id，属于手误） */
    {
        wchar_t want[QP_CONTENT_MAX];
        swprintf(line->content, QP_CONTENT_MAX, L"自己{%ls}", line->id);
        swprintf(want, QP_CONTENT_MAX, L"自己{%ls}", line->id);
        tmpl_render_defaults(t, out, QP_OUTPUT_MAX);
        check(wcsstr(out, want) != NULL, "占位符指向自己时原样保留，不自我展开");
    }

    tmpl_free(t);
}

/* ---------------------------------------------------------------- */
static void test_placeholder_in_condition_results(void)
{
    Template *t = tmpl_create(L"条件结果占位符");
    Segment *mode = seg_create(SEG_TEXT);
    Segment *left = seg_create(SEG_TEXT);
    Segment *right = seg_create(SEG_SELECT);
    Segment *helper = seg_create(SEG_TEXT);
    Segment *manual = seg_create(SEG_TEXT);
    Segment *def = seg_create(SEG_TEXT);
    Segment *source = seg_create(SEG_TEXT);
    Segment *parsed = seg_create(SEG_TEXT);
    Segment *referenced = seg_create(SEG_TEXT);
    ConditionRule *r;
    ParseRule *pr;
    EvalContext *ctx;
    wchar_t out[QP_OUTPUT_MAX];
    int i_mode, i_left, i_right, i_helper, i_manual, i_source, i_parsed, i_ref;

    printf("\n== 条件结果里的占位符 ==\n");

    qp_strlcpy(mode->label, L"模式", QP_LABEL_MAX);
    qp_strlcpy(left->label, L"甲", QP_LABEL_MAX);
    qp_strlcpy(right->label, L"乙", QP_LABEL_MAX);
    qp_strlcpy(helper->label, L"助手", QP_LABEL_MAX);
    helper->hidden = 1;
    helper->no_output = 1;
    qp_strlcpy(manual->label, L"手填", QP_LABEL_MAX);
    qp_strlcpy(def->label, L"默认", QP_LABEL_MAX);
    qp_strlcpy(def->default_value, L"{甲}", QP_VALUE_MAX);
    qp_strlcpy(source->label, L"原文", QP_LABEL_MAX);
    qp_strlcpy(parsed->label, L"解析", QP_LABEL_MAX);
    qp_strlcpy(referenced->label, L"引用", QP_LABEL_MAX);
    qp_strlcpy(referenced->ref_segment_id, source->id, QP_ID_MAX);

    r = cond_rule_create();
    cond_rule_set_simple(r, mode->id, OP_EQUALS, 0, L"组合");
    qp_strlcpy(r->result_value, L"{甲}-{乙}", QP_RESULT_MAX);
    cond_rule_append(&helper->condition_rules, r);
    r = cond_rule_create();
    cond_rule_set_simple(r, mode->id, OP_EQUALS, 0, L"缺失");
    qp_strlcpy(r->result_value, L"{不存在}", QP_RESULT_MAX);
    cond_rule_append(&helper->condition_rules, r);
    r = cond_rule_create();
    cond_rule_set_simple(r, mode->id, OP_EQUALS, 0, L"自己");
    qp_strlcpy(r->result_value, L"{助手}", QP_RESULT_MAX);
    cond_rule_append(&helper->condition_rules, r);

    pr = parse_rule_create();
    pr->type = PARSE_BETWEEN;
    qp_strlcpy(pr->keyword, L"起", QP_KEYWORD_MAX);
    qp_strlcpy(pr->marker_after, L"止", QP_KEYWORD_MAX);
    qp_strlcpy(pr->target_segment_id, parsed->id, QP_ID_MAX);
    parse_rule_append(&source->parse_rules, pr);

    seg_list_append(&t->segments, mode);
    seg_list_append(&t->segments, left);
    seg_list_append(&t->segments, right);
    seg_list_append(&t->segments, helper);
    seg_list_append(&t->segments, manual);
    seg_list_append(&t->segments, def);
    seg_list_append(&t->segments, source);
    seg_list_append(&t->segments, parsed);
    seg_list_append(&t->segments, referenced);
    t->table_output_enabled = 1;
    qp_strlcpy(t->table_output_format,
               L"{助手}\\t{手填}\\t{默认}\\t{解析}", QP_OUTPUT_MAX);

    ctx = eval_create(t);
    i_mode = eval_index_of_label(ctx, L"模式");
    i_left = eval_index_of_label(ctx, L"甲");
    i_right = eval_index_of_label(ctx, L"乙");
    i_helper = eval_index_of_label(ctx, L"助手");
    i_manual = eval_index_of_label(ctx, L"手填");
    i_source = eval_index_of_label(ctx, L"原文");
    i_parsed = eval_index_of_label(ctx, L"解析");
    i_ref = eval_index_of_label(ctx, L"引用");

    eval_set_user_value(ctx, i_left, L"左");
    eval_set_user_value(ctx, i_right, L"右");
    eval_set_user_value(ctx, i_manual, L"{甲}");
    eval_set_user_value(ctx, i_source, L"起{甲}止");
    eval_set_user_value(ctx, i_mode, L"组合");
    eval_run(ctx);
    check_str(ctx->fields[i_helper].value, L"左-右",
              "隐藏且不输出的文本变量可由条件结果组合两个字段");
    check_str(ctx->fields[i_manual].value, L"{甲}",
              "手填文本里的花括号保持字面量");
    check_str(ctx->fields[eval_index_of_label(ctx, L"默认")].value, L"{甲}",
              "默认文本里的花括号保持字面量");
    check_str(ctx->fields[i_parsed].value, L"{甲}",
              "解析提取文本里的花括号保持字面量");
    check_str(ctx->fields[i_ref].value, L"起{甲}止",
              "引用取得的来源文本里的花括号保持字面量");
    eval_render_mode(ctx, OUTPUT_TABLE, out, QP_OUTPUT_MAX);
    check_str(out, L"左-右\t{甲}\t{甲}\t{甲}",
              "表格可引用条件组合助手且不递归展开换入的字面量");

    eval_set_user_value(ctx, i_mode, L"缺失");
    eval_run(ctx);
    check_str(ctx->fields[i_helper].value, L"{不存在}",
              "条件结果中的未知占位符原样保留");
    eval_set_user_value(ctx, i_mode, L"自己");
    eval_run(ctx);
    check_str(ctx->fields[i_helper].value, L"{助手}",
              "条件结果中的自占位符原样保留");
    eval_set_user_value(ctx, i_mode, L"不命中");
    eval_run(ctx);
    check_str(ctx->fields[i_helper].value, L"",
              "条件失配后清空先前展开的结果而不残留旧值");

    eval_destroy(ctx);
    tmpl_free(t);
}

/* ---------------------------------------------------------------- */
/* 取两标记之间（强裁切：夹在两个不同词之间的元素一步取出）           */
static void test_between_take(void)
{
    wchar_t buf[128];
    printf("\n== 取两标记之间 ==\n");

    /* 真实场景：设备名夹在「网络设备」与「经核查」之间（源里 经核查 出现两次，
       取的是 start 之后的第一次） */
    check(qp_between_take(L"经核查网络设备NFV-D-HNGZ-17A经核查为工程割接",
                          L"网络设备", L"经核查", buf, 128) == 1, "前后标记都命中");
    check_str(buf, L"NFV-D-HNGZ-17A", "取出两标记之间的内容");

    check(qp_between_take(L"ABCDEF", L"", L"DEF", buf, 128) == 1, "前标记空 = 从头");
    check_str(buf, L"ABC", "从头取到后标记之前");

    check(qp_between_take(L"ABCDEF", L"ABC", L"", buf, 128) == 1, "后标记空 = 到末尾");
    check_str(buf, L"DEF", "从前标记取到末尾");

    check(qp_between_take(L"ABCDEF", L"", L"", buf, 128) == 1, "前后标记都空 = 整串");
    check_str(buf, L"ABCDEF", "整串取出");

    check(qp_between_take(L"ABCDEF", L"XYZ", L"", buf, 128) == 0, "前标记找不到 → 未命中");
    check_str(buf, L"", "未命中时输出清空");

    check(qp_between_take(L"ABCDEF", L"AB", L"XYZ", buf, 128) == 0, "后标记找不到 → 未命中");

    check(qp_between_take(L"aXXbYYc", L"xx", L"yy", buf, 128) == 1, "标记大小写不敏感");
    check_str(buf, L"b", "大小写不敏感取值");

    check(qp_between_take(L"AB", L"A", L"B", buf, 128) == 1, "两标记相邻 → 命中");
    check_str(buf, L"", "相邻标记之间是空串");

    check(qp_between_take(L"ABCDEFGHIJ", L"", L"", buf, 4) == 1, "越界仍返回命中");
    check(wcslen(buf) == 3, "越界时按 cap 截断");
}

/* ---------------------------------------------------------------- */
/* 固定内容转义符（列输出的落点：\t 分列、\n 分行）                    */
static void test_escape_expand(void)
{
    wchar_t buf[128];
    printf("\n== 固定内容转义符 ==\n");

    qp_expand_placeholders(L"甲\\t乙", ph_lookup, NULL, buf, 128);
    check_str(buf, L"甲\t乙", "\\t 展开成制表符");

    qp_expand_placeholders(L"甲\\n乙", ph_lookup, NULL, buf, 128);
    check_str(buf, L"甲\n乙", "\\n 展开成换行");

    qp_expand_placeholders(L"甲\\\\乙", ph_lookup, NULL, buf, 128);
    check_str(buf, L"甲\\乙", "\\\\ 展开成一个反斜杠");

    qp_expand_placeholders(L"甲\\d乙", ph_lookup, NULL, buf, 128);
    check_str(buf, L"甲\\d乙", "认不出的 \\x 原样保留");

    qp_expand_placeholders(L"结尾\\", ph_lookup, NULL, buf, 128);
    check_str(buf, L"结尾\\", "结尾单个反斜杠原样保留");

    /* 整行汇总：占位符 + 制表符 + {{ 混排 —— 一句话拼出一整行表格 */
    qp_expand_placeholders(L"{甲}\\t尾{{", ph_lookup, NULL, buf, 128);
    check_str(buf, L"张三\t尾{", "占位符 + 制表符 + {{ 混排");

    /* 换进来的变量值原样保留：值里的 \t 不当转义（只有固定片段正文做转义） */
    qp_expand_placeholders(L"{含转义}", ph_lookup, NULL, buf, 128);
    check_str(buf, L"X\\tY", "占位符换进来的值原样保留，不再解转义");

    /* 集成路径：固定片段正文解释转义，用户值经占位符注入后保持字面量。 */
    {
        Template *t = tmpl_create(L"转义范围");
        Segment *a = seg_create(SEG_TEXT);
        Segment *b = seg_create(SEG_TEXT);
        Segment *line = seg_create(SEG_STATIC);
        EvalContext *ctx;
        wchar_t rendered[128];

        qp_strlcpy(a->label, L"甲", QP_LABEL_MAX);
        qp_strlcpy(b->label, L"乙", QP_LABEL_MAX);
        a->no_output = 1;
        b->no_output = 1;
        qp_strlcpy(line->content, L"{甲}\\t{乙}", QP_CONTENT_MAX);
        seg_list_append(&t->segments, a);
        seg_list_append(&t->segments, b);
        seg_list_append(&t->segments, line);

        ctx = eval_create(t);
        eval_set_user_value(ctx, eval_index_of_label(ctx, L"甲"), L"值甲");
        eval_set_user_value(ctx, eval_index_of_label(ctx, L"乙"), L"值乙");
        eval_run(ctx);
        eval_render(ctx, rendered, 128);
        check_str(rendered, L"值甲\t值乙", "固定片段 {甲}\\t{乙} 渲染为真实制表符");

        eval_set_user_value(ctx, eval_index_of_label(ctx, L"甲"), L"\\t");
        eval_run(ctx);
        eval_render(ctx, rendered, 128);
        check_str(rendered, L"\\t\t值乙", "用户输入的 \\t 保持两个字面字符");

        eval_destroy(ctx);
        tmpl_free(t);
    }
}

/* ---------------------------------------------------------------- */
/* 逐段抽取（需求 2：第 19 元素装多段话时，每段各抽特征填第 5 元素）   */
static void test_per_paragraph(void)
{
    Template *t = tmpl_create(L"逐段");
    Segment *src = seg_create(SEG_TEXT);
    Segment *dst = seg_create(SEG_TEXT);
    ParseRule *pr;
    EvalContext *ctx;
    int is, id;

    printf("\n== 逐段抽取 ==\n");
    qp_strlcpy(src->label, L"源", QP_LABEL_MAX);
    qp_strlcpy(dst->label, L"设备名", QP_LABEL_MAX);
    dst->hidden = 1;

    pr = parse_rule_create();
    pr->type = PARSE_BETWEEN;
    qp_strlcpy(pr->keyword, L"设备", QP_KEYWORD_MAX);
    qp_strlcpy(pr->marker_after, L"异常", QP_KEYWORD_MAX);
    pr->per_paragraph = 1;
    qp_strlcpy(pr->target_segment_id, dst->id, QP_ID_MAX);
    parse_rule_append(&src->parse_rules, pr);

    seg_list_append(&t->segments, src);
    seg_list_append(&t->segments, dst);

    ctx = eval_create(t);
    is = eval_index_of_label(ctx, L"源");
    id = eval_index_of_label(ctx, L"设备名");
    check(is >= 0 && id >= 0, "找得到源与目标变量");

    eval_set_user_value(ctx, is, L"设备AAA异常\n设备BBB异常\n设备CCC异常");
    eval_run(ctx);
    check_str(ctx->fields[id].value, L"AAA\nBBB\nCCC", "逐段抽取按行合并");

    /* 中间一段没有标记 → 跳过，其余照常合并 */
    eval_set_user_value(ctx, is, L"设备AAA异常\n这行没有标记\n设备CCC异常");
    eval_run(ctx);
    check_str(ctx->fields[id].value, L"AAA\nCCC", "未命中的段跳过，命中的合并");

    /* 关闭逐段：整块只当一段处理，取第一处 */
    pr->per_paragraph = 0;
    eval_set_user_value(ctx, is, L"设备AAA异常\n设备BBB异常");
    eval_run(ctx);
    check_str(ctx->fields[id].value, L"AAA", "关闭逐段：整块只取第一处");

    eval_destroy(ctx);
    tmpl_free(t);
}

/* ---------------------------------------------------------------- */
/* 三类解析规则共用同一套逐行语义：LF/CRLF、空行、坏行及单行兼容。   */
static void test_per_paragraph_all_types(void)
{
    Template *t = tmpl_create(L"逐行通用规则");
    Segment *split_src = seg_create(SEG_TEXT);
    Segment *split_dst = seg_create(SEG_TEXT);
    Segment *contains_src = seg_create(SEG_TEXT);
    Segment *contains_dst = seg_create(SEG_TEXT);
    Segment *between_src = seg_create(SEG_TEXT);
    Segment *between_dst = seg_create(SEG_TEXT);
    ParseRule *split_rule = parse_rule_create();
    ParseRule *contains_rule = parse_rule_create();
    ParseRule *between_rule = parse_rule_create();
    EvalContext *ctx;
    int split_i, contains_i, between_i;
    wchar_t single_off[QP_VALUE_MAX];
    const wchar_t *actual[3];
    static const wchar_t *source_rows[3][2] = {
        { L"记录-甲", L"记录-乙" },
        { L"包含标签的甲行", L"包含标签的乙行" },
        { L"左甲右", L"左乙右" }
    };
    int i;

    printf("\n== 三类规则逐行一致性 ==\n");
    qp_strlcpy(split_src->label, L"分割源", QP_LABEL_MAX);
    qp_strlcpy(split_dst->label, L"分割值", QP_LABEL_MAX);
    qp_strlcpy(contains_src->label, L"包含源", QP_LABEL_MAX);
    qp_strlcpy(contains_dst->label, L"包含值", QP_LABEL_MAX);
    qp_strlcpy(between_src->label, L"夹取源", QP_LABEL_MAX);
    qp_strlcpy(between_dst->label, L"夹取值", QP_LABEL_MAX);

    split_rule->type = PARSE_SPLIT;
    qp_strlcpy(split_rule->delimiter, L"-", QP_SEP_MAX);
    split_rule->segment_index = 2;
    split_rule->per_paragraph = 1;
    qp_strlcpy(split_rule->target_segment_id, split_dst->id, QP_ID_MAX);
    parse_rule_append(&split_src->parse_rules, split_rule);

    contains_rule->type = PARSE_CONTAINS;
    qp_strlcpy(contains_rule->keyword, L"标签", QP_KEYWORD_MAX);
    contains_rule->per_paragraph = 1;
    qp_strlcpy(contains_rule->target_segment_id, contains_dst->id, QP_ID_MAX);
    parse_rule_append(&contains_src->parse_rules, contains_rule);

    between_rule->type = PARSE_BETWEEN;
    qp_strlcpy(between_rule->keyword, L"左", QP_KEYWORD_MAX);
    qp_strlcpy(between_rule->marker_after, L"右", QP_KEYWORD_MAX);
    between_rule->per_paragraph = 1;
    qp_strlcpy(between_rule->target_segment_id, between_dst->id, QP_ID_MAX);
    parse_rule_append(&between_src->parse_rules, between_rule);

    seg_list_append(&t->segments, split_src);
    seg_list_append(&t->segments, split_dst);
    seg_list_append(&t->segments, contains_src);
    seg_list_append(&t->segments, contains_dst);
    seg_list_append(&t->segments, between_src);
    seg_list_append(&t->segments, between_dst);

    ctx = eval_create(t);
    split_i = eval_index_of_label(ctx, L"分割值");
    contains_i = eval_index_of_label(ctx, L"包含值");
    between_i = eval_index_of_label(ctx, L"夹取值");

    eval_set_user_value(ctx, eval_index_of_label(ctx, L"分割源"),
                        L"记录-甲\n\n坏行\r\n  \r\n记录-乙");
    eval_set_user_value(ctx, eval_index_of_label(ctx, L"包含源"),
                        L"包含标签的甲行\r\n\r\n不匹配\n \n包含标签的乙行");
    eval_set_user_value(ctx, eval_index_of_label(ctx, L"夹取源"),
                        L"左甲右\n坏行\r\n\t\r\n左乙右");
    eval_run(ctx);
    check_str(ctx->fields[split_i].value, L"甲\n乙",
              "split 对 LF/CRLF 跳过空行与不可解析行");
    check_str(ctx->fields[contains_i].value, L"标签\n标签",
              "contains 对 LF/CRLF 跳过空行与不命中行");
    check_str(ctx->fields[between_i].value, L"甲\n乙",
              "between 对 LF/CRLF 跳过空行与不可解析行");

    /* 直接消费上面三个实际提取结果：按换行拆出每个派生值，再逐项核对原始行。
       常量/default 列不走这条守恒检查，在批量测试里另作显式断言。 */
    actual[0] = ctx->fields[split_i].value;
    actual[1] = ctx->fields[contains_i].value;
    actual[2] = ctx->fields[between_i].value;
    for (i = 0; i < 3; ++i) {
        wchar_t extracted[QP_VALUE_MAX];
        wchar_t *state = NULL;
        wchar_t *token;
        int row;

        qp_strlcpy(extracted, actual[i], QP_VALUE_MAX);
        token = wcstok(extracted, L"\n", &state);
        for (row = 0; row < 2; ++row) {
            check(token != NULL && wcsstr(source_rows[i][row], token) != NULL,
                  "实际源派生值逐项保留于对应原始行中");
            if (token) token = wcstok(NULL, L"\n", &state);
        }
        check(token == NULL, "实际源派生值数量与命中的原始行一致");
    }

    eval_set_user_value(ctx, eval_index_of_label(ctx, L"分割源"), L"记录-单");
    split_rule->per_paragraph = 0;
    eval_run(ctx);
    qp_strlcpy(single_off, ctx->fields[split_i].value, QP_VALUE_MAX);
    split_rule->per_paragraph = 1;
    eval_run(ctx);
    check_str(ctx->fields[split_i].value, single_off, "split 单行开关前后等价");

    eval_set_user_value(ctx, eval_index_of_label(ctx, L"包含源"), L"单行标签");
    contains_rule->per_paragraph = 0;
    eval_run(ctx);
    qp_strlcpy(single_off, ctx->fields[contains_i].value, QP_VALUE_MAX);
    contains_rule->per_paragraph = 1;
    eval_run(ctx);
    check_str(ctx->fields[contains_i].value, single_off, "contains 单行开关前后等价");

    eval_set_user_value(ctx, eval_index_of_label(ctx, L"夹取源"), L"左单右");
    between_rule->per_paragraph = 0;
    eval_run(ctx);
    qp_strlcpy(single_off, ctx->fields[between_i].value, QP_VALUE_MAX);
    between_rule->per_paragraph = 1;
    eval_run(ctx);
    check_str(ctx->fields[between_i].value, single_off, "between 单行开关前后等价");

    eval_destroy(ctx);
    tmpl_free(t);
}

/* ---------------------------------------------------------------- */
/* 不计入输出开关：参与匹配/引用/占位符，但不进入最终输出              */
static void test_no_output(void)
{
    Template *t = tmpl_create(L"不计入输出");
    Segment *v = seg_create(SEG_TEXT);
    Segment *line = seg_create(SEG_STATIC);
    Segment *clone;
    Template *tc;
    EvalContext *ctx;
    wchar_t out[QP_OUTPUT_MAX];

    printf("\n== 不计入输出开关 ==\n");
    check(v->no_output == 0, "新建变量默认计入输出");

    qp_strlcpy(v->label, L"设备名", QP_LABEL_MAX);
    qp_strlcpy(v->default_value, L"AAA", QP_VALUE_MAX);
    v->no_output = 1;
    qp_strlcpy(line->content, L"设备是{设备名}", QP_CONTENT_MAX);
    seg_list_append(&t->segments, v);
    seg_list_append(&t->segments, line);

    clone = seg_clone(v);
    check(clone && clone->no_output == 1, "seg_clone 保留不计入输出标记");
    seg_free(clone);

    tc = tmpl_clone(t);
    check(tc && seg_list_at(tc->segments, 0)->no_output == 1, "tmpl_clone 保留不计入输出标记");
    tmpl_free(tc);

    ctx = eval_create(t);
    eval_run(ctx);
    eval_render(ctx, out, QP_OUTPUT_MAX);
    check_str(out, L"设备是AAA",
              "变量本身不进入输出，但占位符仍取到它的值");
    eval_destroy(ctx);
    tmpl_free(t);
}

/* ---------------------------------------------------------------- */
/* 普通文本与表格模式各自渲染；批量时两种模式都按行重新求值。          */
static void test_output_modes(void)
{
    Template *t = tmpl_create(L"双输出");
    Segment *src = seg_create(SEG_TEXT);
    Segment *derived = seg_create(SEG_TEXT);
    Segment *ordinary = seg_create(SEG_STATIC);
    ParseRule *pr;
    EvalContext *ctx;
    Template *clone;
    wchar_t out[QP_OUTPUT_MAX];
    int isrc;

    printf("\n== 普通文本与表格输出 ==\n");
    qp_strlcpy(src->label, L"来源", QP_LABEL_MAX);
    qp_strlcpy(derived->label, L"派生", QP_LABEL_MAX);
    derived->hidden = 1;
    derived->no_output = 1;
    qp_strlcpy(ordinary->content, L"｜普通文本", QP_CONTENT_MAX);

    pr = parse_rule_create();
    pr->type = PARSE_CONTAINS;
    qp_strlcpy(pr->keyword, L"原文", QP_KEYWORD_MAX);
    qp_strlcpy(pr->target_segment_id, derived->id, QP_ID_MAX);
    qp_strlcpy(pr->fill_value, L"结果", QP_VALUE_MAX);
    parse_rule_append(&src->parse_rules, pr);
    pr = parse_rule_create();
    pr->type = PARSE_CONTAINS;
    qp_strlcpy(pr->keyword, L"甲", QP_KEYWORD_MAX);
    qp_strlcpy(pr->target_segment_id, derived->id, QP_ID_MAX);
    qp_strlcpy(pr->fill_value, L"结果甲", QP_VALUE_MAX);
    parse_rule_append(&src->parse_rules, pr);
    pr = parse_rule_create();
    pr->type = PARSE_CONTAINS;
    qp_strlcpy(pr->keyword, L"乙", QP_KEYWORD_MAX);
    qp_strlcpy(pr->target_segment_id, derived->id, QP_ID_MAX);
    qp_strlcpy(pr->fill_value, L"结果乙", QP_VALUE_MAX);
    parse_rule_append(&src->parse_rules, pr);

    seg_list_append(&t->segments, src);
    seg_list_append(&t->segments, derived);
    seg_list_append(&t->segments, ordinary);
    t->table_output_enabled = 1;
    qp_strlcpy(t->table_output_format, L"{来源}\\t{派生}", QP_OUTPUT_MAX);

    clone = tmpl_clone(t);
    check(clone && clone->table_output_enabled == 1 &&
          wcscmp(clone->table_output_format, L"{来源}\\t{派生}") == 0,
          "模板克隆保留表格输出配置");
    tmpl_free(clone);

    ctx = eval_create(t);
    isrc = eval_index_of_label(ctx, L"来源");
    eval_set_user_value(ctx, isrc, L"原文");
    eval_run(ctx);
    eval_render_mode(ctx, OUTPUT_TEXT, out, QP_OUTPUT_MAX);
    check_str(out, L"原文｜普通文本", "普通模式按顺序输出非 no_output 片段");
    eval_render_mode(ctx, OUTPUT_TABLE, out, QP_OUTPUT_MAX);
    check_str(out, L"原文\t结果", "表格模式展开来源与隐藏派生值");
    check(out[2] == L'\t' && out[3] == L'结', "表格分列符是一个实际 Tab");

    t->table_output_enabled = 0;
    eval_render_mode(ctx, OUTPUT_TABLE, out, QP_OUTPUT_MAX);
    check_str(out, L"", "未启用表格输出时表格模式为空");
    t->table_output_enabled = 1;

    t->batch_mode = 1;
    qp_strlcpy(t->batch_source_id, src->id, QP_ID_MAX);
    eval_set_user_value(ctx, isrc, L"甲行\n乙行");
    eval_run(ctx);
    eval_render_batch_mode(ctx, OUTPUT_TEXT, out, QP_OUTPUT_MAX);
    check_str(out, L"甲行｜普通文本\n乙行｜普通文本",
              "批量普通模式逐行求值并按行合并");
    eval_render_batch_mode(ctx, OUTPUT_TABLE, out, QP_OUTPUT_MAX);
    check_str(out, L"甲行\t结果甲\n乙行\t结果乙",
              "批量表格模式逐行求值并按行合并");

    t->table_output_enabled = 0;
    eval_render_batch_mode(ctx, OUTPUT_TABLE, out, QP_OUTPUT_MAX);
    check_str(out, L"", "批量表格模式在未启用时不留下分隔换行");
    t->table_output_enabled = 1;
    t->table_output_format[0] = L'\0';
    eval_render_batch_mode(ctx, OUTPUT_TABLE, out, QP_OUTPUT_MAX);
    check_str(out, L"", "批量表格模式在格式为空时不留下分隔换行");
    qp_strlcpy(t->table_output_format, L"{来源}\\t{派生}", QP_OUTPUT_MAX);

    check_str(ctx->fields[isrc].user_value, L"甲行\n乙行",
              "两种批量渲染后都恢复完整来源输入");
    check_str(ctx->fields[isrc].value, L"甲行\n乙行",
              "批量渲染后来源生效值恢复完整输入");

    eval_destroy(ctx);
    tmpl_free(t);
}

/* ---------------------------------------------------------------- */
/* 批量模式（需求 1 第 3 点：多行源 → 多行表格；常量列不变、派生列逐行变）*/
static void test_batch_render(void)
{
    Template *t = tmpl_create(L"批量");
    Segment *src = seg_create(SEG_TEXT);     /* 源：多行告警，不计入输出 */
    Segment *dev = seg_create(SEG_TEXT);     /* 设备名：解析规则逐行派生 */
    Segment *op  = seg_create(SEG_TEXT);     /* 操作员：手填一次，常量列 */
    Segment *row = seg_create(SEG_STATIC);   /* 汇总成行：{设备}\t{操作员} */
    ParseRule *pr;
    EvalContext *ctx;
    Template *clone;
    wchar_t out[QP_OUTPUT_MAX];
    int isrc;

    printf("\n== 批量模式 ==\n");
    qp_strlcpy(src->label, L"告警", QP_LABEL_MAX);
    src->no_output = 1;
    qp_strlcpy(dev->label, L"设备", QP_LABEL_MAX);
    dev->hidden = 1; dev->no_output = 1;
    qp_strlcpy(op->label, L"操作员", QP_LABEL_MAX);
    qp_strlcpy(op->default_value, L"张三", QP_VALUE_MAX);
    op->no_output = 1;
    qp_strlcpy(row->content, L"{设备}\\t{操作员}", QP_CONTENT_MAX);

    pr = parse_rule_create();
    pr->type = PARSE_BETWEEN;
    qp_strlcpy(pr->keyword, L"设备", QP_KEYWORD_MAX);
    qp_strlcpy(pr->marker_after, L"异常", QP_KEYWORD_MAX);
    qp_strlcpy(pr->target_segment_id, dev->id, QP_ID_MAX);
    parse_rule_append(&src->parse_rules, pr);

    seg_list_append(&t->segments, src);
    seg_list_append(&t->segments, dev);
    seg_list_append(&t->segments, op);
    seg_list_append(&t->segments, row);

    t->batch_mode = 1;
    qp_strlcpy(t->batch_source_id, src->id, QP_ID_MAX);

    clone = tmpl_clone(t);
    check(clone && clone->batch_mode == 1 &&
          wcscmp(clone->batch_source_id, src->id) == 0,
          "模板克隆保留批量开关与源变量");
    tmpl_free(clone);

    ctx = eval_create(t);
    isrc = eval_index_of_label(ctx, L"告警");

    /* 多行源 → 多行表格：设备名逐行变、操作员每行不变 */
    eval_set_user_value(ctx, isrc,
        L"设备AAA异常\n设备BBB异常\n设备CCC异常");
    eval_run(ctx);
    eval_render_batch(ctx, out, QP_OUTPUT_MAX);
    check_str(out, L"AAA\t张三\nBBB\t张三\nCCC\t张三",
              "多行源逐行渲染：派生列逐行变、常量列不变");

    /* 批量渲染后源字段还原为「整段已粘贴」状态 */
    check(ctx->fields[isrc].user_touched == 1, "批量后源字段仍是手填态");
    check_str(ctx->fields[isrc].value,
              L"设备AAA异常\n设备BBB异常\n设备CCC异常", "批量后源还原为整段");

    /* CRLF 与空白行不产生记录；非空但不可解析的行仍独立渲染为空派生列。 */
    eval_set_user_value(ctx, isrc,
        L"设备甲异常\r\n\r\n  \r\n无法解析\n设备乙异常");
    eval_run(ctx);
    eval_render_batch(ctx, out, QP_OUTPUT_MAX);
    check_str(out, L"甲\t张三\n\t张三\n乙\t张三",
              "批量按非空行独立求值并正确处理 CRLF");
    check(ctx->fields[isrc].user_touched == 1,
          "CRLF 批量后源字段恢复手填状态");
    check_str(ctx->fields[isrc].user_value,
              L"设备甲异常\r\n\r\n  \r\n无法解析\n设备乙异常",
              "批量后完整恢复源字段用户原文");
    check_str(ctx->fields[isrc].value,
              L"设备甲异常\r\n\r\n  \r\n无法解析\n设备乙异常",
              "批量后重新求值恢复源字段生效全文");
    check_str(ctx->fields[eval_index_of_label(ctx, L"设备")].value, L"甲",
              "批量恢复后派生状态与完整源重新求值一致");
    check_str(ctx->fields[eval_index_of_label(ctx, L"操作员")].value, L"张三",
              "显式默认列在每行保持常量并在批量后恢复");

    /* 单行源 → 退化为单次渲染 */
    eval_set_user_value(ctx, isrc, L"设备ZZZ异常");
    eval_run(ctx);
    eval_render_batch(ctx, out, QP_OUTPUT_MAX);
    check_str(out, L"ZZZ\t张三", "单行源退化为单次渲染");

    /* 关闭批量 → 等同普通 eval_render */
    t->batch_mode = 0;
    eval_set_user_value(ctx, isrc, L"设备AAA异常\n设备BBB异常");
    eval_run(ctx);
    {
        wchar_t direct[QP_OUTPUT_MAX];
        eval_render(ctx, direct, QP_OUTPUT_MAX);
        eval_render_batch(ctx, out, QP_OUTPUT_MAX);
        check(wcscmp(out, direct) == 0, "关闭批量时 batch 渲染等同普通渲染");
    }

    eval_destroy(ctx);
    tmpl_free(t);
}

/* ---------------------------------------------------------------- */
static int lint_count(const LintIssue *is, int n, LintKind k)
{
    int i, c = 0;
    for (i = 0; i < n; ++i)
        if (is[i].kind == k) ++c;
    return c;
}

static void test_lint(void)
{
    Template *t = tmpl_create(L"体检");
    Segment *kind   = seg_create(SEG_SELECT);   /* 硬件 / 软件            */
    Segment *shadow = seg_create(SEG_TEXT);     /* 隐藏，只靠自己的规则拿值 */
    Segment *typed  = seg_create(SEG_TEXT);     /* 能手打 → 取值穷举不了   */
    Segment *dead   = seg_create(SEG_TEXT);     /* 规则永远不命中          */
    Segment *blank  = seg_create(SEG_TEXT);     /* 规则命中后变成空        */
    Segment *noterm = seg_create(SEG_TEXT);     /* 规则一个条件都没有      */
    Segment *gone   = seg_create(SEG_TEXT);     /* 条件指向已删除的变量    */
    Segment *fine   = seg_create(SEG_TEXT);     /* 一切正常，不该被报      */
    Segment *line   = seg_create(SEG_STATIC);
    LintIssue *is = (LintIssue *)calloc(QP_LINT_MAX, sizeof(LintIssue));
    ConditionRule *r;
    CondTerm *term;
    ParseRule *pr;
    int n;

    printf("\n== 模板体检 ==\n");
    if (!is) { check(0, "体检结果数组分配成功"); tmpl_free(t); return; }

    qp_strlcpy(kind->label, L"类别", QP_LABEL_MAX);
    seg_add_option(kind, L"硬件");
    seg_add_option(kind, L"软件");

    /* 影子变量：自己带两条规则，所以取值只可能是 空 / 计算服务器 / 存储服务器 */
    qp_strlcpy(shadow->label, L"影子", QP_LABEL_MAX);
    shadow->hidden = 1;
    r = cond_rule_create();
    cond_rule_set_simple(r, kind->id, OP_CONTAINS, 0, L"硬件");
    qp_strlcpy(r->result_value, L"计算服务器", QP_RESULT_MAX);
    cond_rule_append(&shadow->condition_rules, r);
    r = cond_rule_create();
    cond_rule_set_simple(r, kind->id, OP_CONTAINS, 0, L"软件");
    qp_strlcpy(r->result_value, L"存储服务器", QP_RESULT_MAX);
    cond_rule_append(&shadow->condition_rules, r);

    /* 手打的变量：什么值都可能，任何条件都不该被判死 */
    qp_strlcpy(typed->label, L"备注", QP_LABEL_MAX);

    /* 拿影子变量去匹配「硬件」——影子里永远只会是那三种值，这条规则是死的 */
    qp_strlcpy(dead->label, L"死规则", QP_LABEL_MAX);
    dead->hidden = 1;
    r = cond_rule_create();
    cond_rule_set_simple(r, shadow->id, OP_CONTAINS, 0, L"硬件");
    qp_strlcpy(r->result_value, L"旧口径", QP_RESULT_MAX);
    cond_rule_append(&dead->condition_rules, r);

    qp_strlcpy(blank->label, L"空结果", QP_LABEL_MAX);
    r = cond_rule_create();
    cond_rule_set_simple(r, kind->id, OP_CONTAINS, 0, L"硬件");
    r->result_value[0] = L'\0';
    cond_rule_append(&blank->condition_rules, r);

    qp_strlcpy(noterm->label, L"没有条件", QP_LABEL_MAX);
    r = cond_rule_create();
    cond_term_list_clear(&r->terms);
    qp_strlcpy(r->result_value, L"永远轮不到", QP_RESULT_MAX);
    cond_rule_append(&noterm->condition_rules, r);

    qp_strlcpy(gone->label, L"悬空条件", QP_LABEL_MAX);
    r = cond_rule_create();
    cond_rule_set_simple(r, L"seg-已经删了", OP_EQUALS, 0, L"随便");
    qp_strlcpy(r->result_value, L"轮不到", QP_RESULT_MAX);
    cond_rule_append(&gone->condition_rules, r);

    /* 正常的规则：一个条件穷举不了（手打），一个条件确实可能成立 */
    qp_strlcpy(fine->label, L"正常", QP_LABEL_MAX);
    r = cond_rule_create();
    cond_rule_set_simple(r, typed->id, OP_CONTAINS, 0, L"随便什么");
    term = cond_term_create();
    qp_strlcpy(term->trigger_segment_id, kind->id, QP_ID_MAX);
    term->op = OP_EQUALS;
    cond_term_set_match(term, L"硬件");
    cond_term_append(&r->terms, term);
    qp_strlcpy(r->result_value, L"没问题", QP_RESULT_MAX);
    cond_rule_append(&fine->condition_rules, r);

    /* 解析规则的目标已经删了 */
    pr = parse_rule_create();
    pr->type = PARSE_CONTAINS;
    qp_strlcpy(pr->keyword, L"停机", QP_KEYWORD_MAX);
    qp_strlcpy(pr->target_segment_id, L"seg-已经删了", QP_ID_MAX);
    parse_rule_append(&typed->parse_rules, pr);

    /* 一个认得的占位符 + 一个写错的 */
    qp_strlcpy(line->content, L"处理{影子}完毕，{手滑写错的}", QP_CONTENT_MAX);

    seg_list_append(&t->segments, kind);
    seg_list_append(&t->segments, shadow);
    seg_list_append(&t->segments, typed);
    seg_list_append(&t->segments, dead);
    seg_list_append(&t->segments, blank);
    seg_list_append(&t->segments, noterm);
    seg_list_append(&t->segments, gone);
    seg_list_append(&t->segments, fine);
    seg_list_append(&t->segments, line);

    n = tmpl_lint(t, is, QP_LINT_MAX);
    check(n == 6, "六类问题各报一条，正常的规则不报");
    if (n != 6) {
        int i;
        for (i = 0; i < n; ++i) printf("         · %ls\n", is[i].text);
    }
    check(lint_count(is, n, LINT_COND_NEVER) == 1, "取值可穷举时判出死规则");
    check(lint_count(is, n, LINT_COND_NOTERM) == 1, "报出没有条件的规则");
    check(lint_count(is, n, LINT_COND_DANGLING) == 1, "报出条件指向已删除的变量");
    check(lint_count(is, n, LINT_COND_EMPTY) == 1, "报出命中后把片段变成空");
    check(lint_count(is, n, LINT_PARSE_DANGLING) == 1, "报出解析规则目标已删除");
    check(lint_count(is, n, LINT_PLACEHOLDER) == 1, "报出认不出的占位符（认得的不报）");

    /* 描述里得说清是哪一段、哪一条、为什么 */
    {
        int i, at = -1;
        for (i = 0; i < n; ++i) if (is[i].kind == LINT_COND_NEVER) at = i;
        check(at >= 0 && is[at].seg_index == 3 && is[at].rule_index == 0,
              "死规则指向第 4 段的第 1 条规则");
        check(at >= 0 && wcsstr(is[at].text, L"计算服务器") != NULL &&
              wcsstr(is[at].text, L"存储服务器") != NULL,
              "死规则的说明里列出了触发变量的全部可能取值");
    }

    /* 上限：多了也只写 cap 条 */
    check(tmpl_lint(t, is, 2) == 2, "超过上限只写 cap 条");
    check(tmpl_lint(NULL, is, QP_LINT_MAX) == 0, "空模板不报");

    /* 保守：影子变量一旦可能被别人的解析规则填，取值就穷举不了，死规则不再判死 */
    pr = parse_rule_create();
    pr->type = PARSE_CONTAINS;
    qp_strlcpy(pr->keyword, L"硬件", QP_KEYWORD_MAX);
    qp_strlcpy(pr->target_segment_id, shadow->id, QP_ID_MAX);
    parse_rule_append(&typed->parse_rules, pr);
    n = tmpl_lint(t, is, QP_LINT_MAX);
    check(lint_count(is, n, LINT_COND_NEVER) == 0,
          "触发变量可能被解析规则填 → 不再判死（宁可漏报）");

    /* 同样地：引用来的值也穷举不了 */
    parse_rule_remove_at(&typed->parse_rules, 1);
    qp_strlcpy(shadow->ref_segment_id, typed->id, QP_ID_MAX);
    n = tmpl_lint(t, is, QP_LINT_MAX);
    check(lint_count(is, n, LINT_COND_NEVER) == 0, "触发变量是引用来的 → 不再判死");
    shadow->ref_segment_id[0] = L'\0';

    /* 取反的条件不判死：穷举一反过来最容易冤枉人 */
    r = cond_rule_at(dead->condition_rules, 0);
    r->terms->negate = 1;
    n = tmpl_lint(t, is, QP_LINT_MAX);
    check(lint_count(is, n, LINT_COND_NEVER) == 0, "取反的条件不做穷举判断");
    r->terms->negate = 0;

    /*
     * 「任意满足」的规则：一项死不等于规则死 —— 别的条件还能把它救活，
     * 所以说明里只能说这一项白写；全都死了才敢说规则不会生效。
     */
    {
        Segment *some = seg_create(SEG_TEXT);   /* 一死一活 */
        Segment *all  = seg_create(SEG_TEXT);   /* 两条都死 */
        ConditionRule *ra;
        int i, at_some = -1, at_all = -1;

        qp_strlcpy(some->label, L"或有一半", QP_LABEL_MAX);
        some->hidden = 1;
        ra = cond_rule_create();
        cond_rule_set_simple(ra, shadow->id, OP_CONTAINS, 0, L"硬件");  /* 死 */
        ra->join = COND_JOIN_ANY;       /* set_simple 会把组合方式打回「全部」 */
        term = cond_term_create();
        qp_strlcpy(term->trigger_segment_id, kind->id, QP_ID_MAX);
        term->op = OP_EQUALS;
        cond_term_set_match(term, L"硬件");                             /* 活 */
        cond_term_append(&ra->terms, term);
        qp_strlcpy(ra->result_value, L"还能命中", QP_RESULT_MAX);
        cond_rule_append(&some->condition_rules, ra);

        qp_strlcpy(all->label, L"或全都死", QP_LABEL_MAX);
        all->hidden = 1;
        ra = cond_rule_create();
        cond_rule_set_simple(ra, shadow->id, OP_CONTAINS, 0, L"硬件");  /* 死 */
        ra->join = COND_JOIN_ANY;
        term = cond_term_create();
        qp_strlcpy(term->trigger_segment_id, shadow->id, QP_ID_MAX);
        term->op = OP_EQUALS;
        cond_term_set_match(term, L"软件");                             /* 也死 */
        cond_term_append(&ra->terms, term);
        qp_strlcpy(ra->result_value, L"轮不到", QP_RESULT_MAX);
        cond_rule_append(&all->condition_rules, ra);

        seg_list_append(&t->segments, some);
        seg_list_append(&t->segments, all);

        n = tmpl_lint(t, is, QP_LINT_MAX);
        check(lint_count(is, n, LINT_COND_NEVER) == 3,
              "三条死规则各报一条（一条规则里几个死条件也只报一句）");
        for (i = 0; i < n; ++i) {
            if (is[i].kind != LINT_COND_NEVER) continue;
            if (is[i].seg_index == 9)  at_some = i;
            if (is[i].seg_index == 10) at_all  = i;
        }
        check(at_some >= 0 && wcsstr(is[at_some].text, L"白写") != NULL &&
              wcsstr(is[at_some].text, L"不会生效") == NULL,
              "任意满足且还有活条件时，只说这一项白写");
        check(at_all >= 0 && wcsstr(is[at_all].text, L"每个条件都永远") != NULL,
              "任意满足且条件全死时，才说这条规则不会生效");
    }

    free(is);
    tmpl_free(t);
}

/* ---------------------------------------------------------------- */
static void test_table_lint(void)
{
    Template *t = tmpl_create(L"表格体检");
    Segment *a = seg_create(SEG_TEXT);
    Segment *b = seg_create(SEG_TEXT);
    Segment *duplicate = seg_create(SEG_TEXT);
    LintIssue *is = (LintIssue *)calloc(QP_LINT_MAX, sizeof(LintIssue));
    int n;

    printf("\n== 表格格式体检 ==\n");
    if (!t || !a || !b || !duplicate || !is) {
        check(0, "表格体检创建测试模板");
        free(is); tmpl_free(t); return;
    }
    qp_strlcpy(a->label, L"甲", QP_LABEL_MAX);
    qp_strlcpy(b->label, L"乙", QP_LABEL_MAX);
    qp_strlcpy(duplicate->label, L"甲", QP_LABEL_MAX);
    seg_list_append(&t->segments, a);
    seg_list_append(&t->segments, b);
    seg_list_append(&t->segments, duplicate);
    t->table_output_enabled = 1;

    n = tmpl_lint(t, is, QP_LINT_MAX);
    check(n == 1 && wcsstr(is[0].text, L"格式为空") != NULL,
          "启用表格输出但格式为空会被体检报告");

    qp_strlcpy(t->table_output_format, L"{丙}\\t{乙}", QP_OUTPUT_MAX);
    n = tmpl_lint(t, is, QP_LINT_MAX);
    check(n == 1 && wcsstr(is[0].text, L"丙") != NULL &&
          wcsstr(is[0].text, L"找不到") != NULL,
          "表格格式引用未知变量会被体检报告");

    qp_strlcpy(t->table_output_format, L"{甲}\\t{乙}", QP_OUTPUT_MAX);
    n = tmpl_lint(t, is, QP_LINT_MAX);
    check(n == 1 && wcsstr(is[0].text, L"甲") != NULL &&
          wcsstr(is[0].text, L"不唯一") != NULL,
          "表格格式引用重名变量会被体检报告");

    qp_strlcpy(t->table_output_format, L"{乙}", QP_OUTPUT_MAX);
    n = tmpl_lint(t, is, QP_LINT_MAX);
    check(n == 1 && wcsstr(is[0].text, L"分列") != NULL,
          "启用表格输出但没有分列符会被体检报告");

    qp_strlcpy(t->table_output_format, L"{乙}\\t固定", QP_OUTPUT_MAX);
    check(tmpl_lint(t, is, QP_LINT_MAX) == 0,
          "含字面量反斜杠 t 的有效表格格式不报问题");
    qp_strlcpy(t->table_output_format, L"{乙}\t固定", QP_OUTPUT_MAX);
    check(tmpl_lint(t, is, QP_LINT_MAX) == 0,
          "含实际 Tab 的有效表格格式不报问题");

    free(is);
    tmpl_free(t);
}

/* ---------------------------------------------------------------- */
/* 解析规则第四种：正则匹配                                          */
/* ---------------------------------------------------------------- */
static ParseRule *add_regex_rule(Segment *host, const wchar_t *pattern,
                                 int group, const Segment *target,
                                 const wchar_t *fill)
{
    ParseRule *r = parse_rule_create();
    if (!r) return NULL;
    r->type = PARSE_REGEX;
    qp_strlcpy(r->pattern, pattern, QP_PATTERN_MAX);
    r->capture_group = group;
    if (target) qp_strlcpy(r->target_segment_id, target->id, QP_ID_MAX);
    if (fill) qp_strlcpy(r->fill_value, fill, QP_VALUE_MAX);
    parse_rule_append(&host->parse_rules, r);
    return r;
}

static void test_parse_regex(void)
{
    Template *t;
    Segment *s_src, *s_dev, *s_date, *s_flag;
    EvalContext *ctx;
    int si, di, ti, fi;

    printf("\n== 解析规则：正则 ==\n");
    t = tmpl_create(L"正则测试");
    s_src = seg_create(SEG_TEXT);
    qp_strlcpy(s_src->label, L"原文", QP_LABEL_MAX);
    seg_list_append(&t->segments, s_src);
    s_dev = seg_create(SEG_TEXT);
    qp_strlcpy(s_dev->label, L"设备", QP_LABEL_MAX);
    seg_list_append(&t->segments, s_dev);
    s_date = seg_create(SEG_TEXT);
    qp_strlcpy(s_date->label, L"日期", QP_LABEL_MAX);
    seg_list_append(&t->segments, s_date);
    s_flag = seg_create(SEG_TEXT);
    qp_strlcpy(s_flag->label, L"标记", QP_LABEL_MAX);
    seg_list_append(&t->segments, s_flag);

    /* 捕获组 1：设备编号；整体匹配：日期；命中即用固定值：标记 */
    add_regex_rule(s_src, L"设备编号[:：]\\s*([A-Za-z0-9\\-]+)", 1, s_dev, L"");
    add_regex_rule(s_src, L"\\d{4}-\\d{2}-\\d{2}", 0, s_date, L"");
    add_regex_rule(s_src, L"硬件|软件", 0, s_flag, L"有故障描述");

    ctx = eval_create(t);
    check(ctx != NULL, "正则模板求值上下文创建");
    if (ctx) {
        si = eval_index_of_id(ctx, s_src->id);
        di = eval_index_of_id(ctx, s_dev->id);
        ti = eval_index_of_id(ctx, s_date->id);
        fi = eval_index_of_id(ctx, s_flag->id);
        check(si >= 0 && di >= 0 && ti >= 0 && fi >= 0, "四个字段都能按 id 定位");

        eval_set_user_value(ctx, si,
            L"工单 设备编号：NFV-D-HNZJI-11A 于 2026-08-18 上报硬件异常");
        eval_run(ctx);
        check_str(ctx->fields[di].value, L"NFV-D-HNZJI-11A", "正则捕获组填目标变量");
        check_str(ctx->fields[ti].value, L"2026-08-18", "正则整体匹配填目标变量");
        check_str(ctx->fields[fi].value, L"有故障描述", "正则命中后用固定填充值");

        /* 大小写不敏感：模式里写小写范围，喂大写输入也应命中 */
        {
            ParseRule *r0 = parse_rule_at(s_src->parse_rules, 0);
            qp_strlcpy(r0->pattern, L"设备编号[:：]\\s*([a-z0-9\\-]+)", QP_PATTERN_MAX);
            eval_set_user_value(ctx, si, L"设备编号：NFV-D-ABC-01 异常");
            eval_run(ctx);
            check_str(ctx->fields[di].value, L"NFV-D-ABC-01", "正则大小写不敏感");
            qp_strlcpy(r0->pattern, L"设备编号[:：]\\s*([A-Za-z0-9\\-]+)", QP_PATTERN_MAX);
        }

        /* 失配：派生值立即清空，不保留上一次结果 */
        eval_set_user_value(ctx, si, L"这句话里既没有编号也没有日期");
        eval_run(ctx);
        check_str(ctx->fields[di].value, L"", "正则失配后目标清空");
        check_str(ctx->fields[ti].value, L"", "正则失配后日期清空");
        check_str(ctx->fields[fi].value, L"", "正则失配后固定值也清空");

        /* 语法错误的模式一律不命中，不崩 */
        add_regex_rule(s_src, L"(未闭合", 0, s_dev, L"不应出现");
        eval_set_user_value(ctx, si, L"任意文本");
        eval_run(ctx);
        check_str(ctx->fields[di].value, L"", "非法正则不命中");
        parse_rule_remove_at(&s_src->parse_rules, 3);

        /* 逐段处理：每行各抽一次，结果按行合并 */
        parse_rule_at(s_src->parse_rules, 1)->per_paragraph = 1;
        eval_set_user_value(ctx, si,
            L"第一行 2026-01-02 结束\n第二行没有日期\n第三行 2027-03-04 结束");
        eval_run(ctx);
        check_str(ctx->fields[ti].value, L"2026-01-02\n2027-03-04",
                  "正则逐段处理按行合并结果");
        parse_rule_at(s_src->parse_rules, 1)->per_paragraph = 0;
        eval_destroy(ctx);
    }

    /* 体检：正则没填 / 语法有误 → 报告「永远不会命中」 */
    {
        LintIssue *is = (LintIssue *)malloc(sizeof(LintIssue) * QP_LINT_MAX);
        int n, i, found = 0;
        add_regex_rule(s_src, L"(未闭合", 0, s_dev, L"");
        add_regex_rule(s_src, L"", 0, s_flag, L"");
        n = is ? tmpl_lint(t, is, QP_LINT_MAX) : 0;
        for (i = 0; i < n; ++i)
            if (is[i].kind == LINT_PARSE_REGEX) ++found;
        check(found >= 2, "正则模式非法 / 为空都会被体检报告");
        free(is);
    }
    tmpl_free(t);
}

/* ---------------------------------------------------------------- */
/* 文本预览的失败标记（eval_render_text_marked）                      */
/* ---------------------------------------------------------------- */
static void test_render_marks(void)
{
    Template *t;
    Segment *s_static, *s_type, *s_part, *s_code, *s_model;
    ParseRule *pr;
    EvalContext *ctx;
    wchar_t a[QP_OUTPUT_MAX], b[QP_OUTPUT_MAX];
    int marks[64], nm = 0;
    int code_idx;

    printf("\n== 文本预览的失败标记 ==\n");
    t = tmpl_create(L"标记测试");
    s_static = seg_create(SEG_STATIC);
    qp_strlcpy(s_static->content, L"经核查由于{设备类型}异常引起告警，",
               QP_CONTENT_MAX);
    seg_list_append(&t->segments, s_static);

    s_type = seg_create(SEG_SELECT);          /* 没有默认值 → 取值为空 */
    qp_strlcpy(s_type->label, L"设备类型", QP_LABEL_MAX);
    seg_list_append(&t->segments, s_type);

    s_part = seg_create(SEG_TEXT);
    qp_strlcpy(s_part->label, L"故障部件", QP_LABEL_MAX);
    qp_strlcpy(s_part->default_value, L"硬盘", QP_VALUE_MAX);
    seg_list_append(&t->segments, s_part);

    s_code = seg_create(SEG_TEXT);
    qp_strlcpy(s_code->label, L"设备编号", QP_LABEL_MAX);
    seg_list_append(&t->segments, s_code);

    s_model = seg_create(SEG_TEXT);
    qp_strlcpy(s_model->label, L"机型", QP_LABEL_MAX);
    seg_list_append(&t->segments, s_model);

    /* 解析规则挂在「设备编号」上、目标是「机型」；源为空时规则不命中 */
    pr = parse_rule_create();
    qp_strlcpy(pr->delimiter, L"-", QP_SEP_MAX);
    pr->segment_index = 8;
    qp_strlcpy(pr->target_segment_id, s_model->id, QP_ID_MAX);
    parse_rule_append(&s_code->parse_rules, pr);

    ctx = eval_create(t);
    check(ctx != NULL, "标记测试上下文创建");
    if (ctx) {
        code_idx = eval_index_of_id(ctx, s_code->id);
        eval_run(ctx);
        eval_render_text_marked(ctx, a, QP_OUTPUT_MAX, marks, 64, &nm);
        check_str(a, L"经核查由于异常引起告警，硬盘", "带标记渲染的文本内容");
        check(nm == 2, "标记两处：空占位符 1 处 + 未命中的解析目标 1 处");
        check(nm == 2 && marks[0] == 5, "空占位符标在「经核查由于」之后");
        check(nm == 2 && marks[1] == 14, "未命中的解析目标标在文本末尾");

        /* 预览用的带标记渲染必须与复制用的普通渲染逐字一致 */
        eval_render_mode(ctx, OUTPUT_TEXT, b, QP_OUTPUT_MAX);
        check(wcscmp(a, b) == 0, "带标记渲染与普通渲染文本一致");

        /* 命中之后标记应当消失 */
        eval_set_user_value(ctx, code_idx, L"A-1-2-3-4-5-6-DBS-9");
        eval_run(ctx);
        eval_render_text_marked(ctx, a, QP_OUTPUT_MAX, marks, 64, &nm);
        check(nm == 1 && marks[0] == 5, "解析命中后只留下空占位符那一处标记");
        check_str(a, L"经核查由于异常引起告警，硬盘A-1-2-3-4-5-6-DBS-9DBS",
                  "命中后的文本（源变量自己也照常进普通文本）");

        /* 批量：逐行各渲染一遍，行内标记要折算成整段输出里的位置 */
        eval_set_user_value(ctx, code_idx,
                            L"A-1-2-3-4-5-6-DBS-9\nA-1-2-3-4-5-6-XYZ-9");
        s_code->no_output = 1;                 /* 批量源本身不进普通文本 */
        t->batch_mode = 1;
        qp_strlcpy(t->batch_source_id, s_code->id, QP_ID_MAX);
        eval_run(ctx);
        eval_render_batch_text_marked(ctx, a, QP_OUTPUT_MAX, marks, 64, &nm);
        check_str(a, L"经核查由于异常引起告警，硬盘DBS\n经核查由于异常引起告警，硬盘XYZ",
                  "批量逐行渲染的文本");
        check(nm == 2 && marks[0] == 5 && marks[1] == 23,
              "批量下每行的标记位置都折算正确");
        eval_destroy(ctx);
    }
    tmpl_free(t);
}

int main(void)
{
    setlocale(LC_ALL, "");
    printf("QuickPhrase 规则引擎自测\n=========================\n");

    test_string_utils();
    test_real_scenario();
    test_rule_beats_manual();
    test_operators();
    test_empty_condition_semantics();
    test_multi_condition();
    test_parse_contains_and_chain();
    test_list_ops();
    test_seed_and_render();
    test_demo_template();
    test_rule_only_value_clears();
    test_hidden_flag();
    test_time_format();
    test_datetime_field();
    test_ref_extract();
    test_reference();
    test_ref_guard();
    test_placeholder();
    test_placeholder_in_eval();
    test_placeholder_in_condition_results();
    test_between_take();
    test_escape_expand();
    test_per_paragraph();
    test_per_paragraph_all_types();
    test_no_output();
    test_output_modes();
    test_batch_render();
    test_lint();
    test_table_lint();
    test_parse_regex();
    test_render_marks();
    test_edge_cases();

    printf("\n=========================\n通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
