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
    qp_strlcpy(cr->trigger_segment_id, s_fault->id, QP_ID_MAX);
    cr->op = OP_CONTAINS;
    cond_rule_set_match(cr, L"硬件");
    qp_strlcpy(cr->result_value, L"维护厂家跟进处理后告警消除", QP_RESULT_MAX);
    cond_rule_append(&s_result->condition_rules, cr);

    cr = cond_rule_create();
    qp_strlcpy(cr->trigger_segment_id, s_fault->id, QP_ID_MAX);
    cr->op = OP_CONTAINS;
    cond_rule_set_match(cr, L"软件");
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
static void test_user_override(void)
{
    Template *t;
    Segment *s_type, *s_code, *s_part, *s_fault, *s_result;
    EvalContext *ctx;
    int i_code, i_type;

    printf("\n== 用户手动覆盖规则推导值 ==\n");
    t = build_demo(&s_type, &s_code, &s_part, &s_fault, &s_result);
    ctx = eval_create(t);
    i_code = eval_index_of_id(ctx, s_code->id);
    i_type = eval_index_of_id(ctx, s_type->id);

    eval_set_user_value(ctx, i_code, L"NFV-D-HNZJI-11A-1401-AD05-S-DBS-02");
    eval_run(ctx);
    check_str(ctx->fields[i_type].value, L"存储服务器", "规则先推导出存储服务器");

    /* 用户手动改成别的值 → 应当保留用户选择 */
    eval_set_user_value(ctx, i_type, L"数据库服务器");
    eval_run(ctx);
    check_str(ctx->fields[i_type].value, L"数据库服务器", "用户手动选择被保留");
    check(ctx->fields[i_type].user_override == 1, "标记为用户覆盖");

    /* 触发源变化导致推导值改变 → 规则重新接管 */
    eval_set_user_value(ctx, i_code, L"NFV-D-HNZJI-11A-1401-AD05-S-XXX-02");
    eval_run(ctx);
    check_str(ctx->fields[i_type].value, L"XXX设备", "推导值变化后规则重新接管");

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
    qp_strlcpy(cr->trigger_segment_id, trig->id, QP_ID_MAX);
    cr->op = OP_STARTS_WITH;
    cond_rule_set_match(cr, L"NFV");
    qp_strlcpy(cr->result_value, L"以NFV开头", QP_RESULT_MAX);
    cond_rule_append(&host->condition_rules, cr);

    cr = cond_rule_create();
    qp_strlcpy(cr->trigger_segment_id, trig->id, QP_ID_MAX);
    cr->op = OP_EQUALS;
    cond_rule_set_match(cr, L"精确值");
    qp_strlcpy(cr->result_value, L"完全相等", QP_RESULT_MAX);
    cond_rule_append(&host->condition_rules, cr);

    cr = cond_rule_create();
    qp_strlcpy(cr->trigger_segment_id, trig->id, QP_ID_MAX);
    cr->op = OP_IN;
    cond_rule_set_match(cr, L"甲, 乙 ,丙");
    qp_strlcpy(cr->result_value, L"属于列表", QP_RESULT_MAX);
    cond_rule_append(&host->condition_rules, cr);
    check(cond_rule_at(host->condition_rules, 2)->match_value_count == 3,
          "OP_IN 匹配值拆成 3 项");

    cr = cond_rule_create();
    qp_strlcpy(cr->trigger_segment_id, trig->id, QP_ID_MAX);
    cr->op = OP_CONTAINS;
    cond_rule_set_match(cr, L"告警");
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
    qp_strlcpy(cr->trigger_segment_id, s_mid->id, QP_ID_MAX);
    cr->op = OP_EQUALS;
    cond_rule_set_match(cr, L"严重");
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
    check(tmpl_list_count(d->templates) == 1, "含 1 个示例模板");
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

    /* 单选未设默认值 → 取第一个选项 */
    seg_set_options(sel, opts, 2);
    qp_strlcpy(sel->label, L"单选", QP_LABEL_MAX);
    seg_list_append(&t->segments, sel);
    ctx = eval_create(t);
    check_str(ctx->fields[0].value, L"首个选项", "单选无默认值时取第一个选项");
    eval_destroy(ctx);

    /* 指向不存在目标 / 触发源的规则不应崩溃 */
    qp_strlcpy(s->label, L"文本", QP_LABEL_MAX);
    pr = parse_rule_create();
    pr->type = PARSE_CONTAINS;
    qp_strlcpy(pr->keyword, L"x", QP_KEYWORD_MAX);
    qp_strlcpy(pr->target_segment_id, L"不存在的片段", QP_ID_MAX);
    parse_rule_append(&s->parse_rules, pr);
    cr = cond_rule_create();
    qp_strlcpy(cr->trigger_segment_id, L"也不存在", QP_ID_MAX);
    cr->op = OP_CONTAINS;
    cond_rule_set_match(cr, L"y");
    qp_strlcpy(cr->result_value, L"不该出现", QP_RESULT_MAX);
    cond_rule_append(&s->condition_rules, cr);
    seg_list_append(&t->segments, s);

    ctx = eval_create(t);
    eval_set_user_value(ctx, 1, L"含 x 的内容");
    eval_run(ctx);
    check(qp_istr(ctx->fields[1].value, L"不该出现") == NULL, "悬空规则被忽略且不崩溃");
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

int main(void)
{
    setlocale(LC_ALL, "");
    printf("QuickPhrase 规则引擎自测\n=========================\n");

    test_string_utils();
    test_real_scenario();
    test_user_override();
    test_operators();
    test_parse_contains_and_chain();
    test_list_ops();
    test_seed_and_render();
    test_edge_cases();

    printf("\n=========================\n通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
