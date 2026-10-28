/*
 * engine.c —— 数据模型生命周期管理 + 解析规则/条件规则引擎
 * 纯 C 实现，不含任何窗口 API，便于独立单元测试。
 */
#include "engine_internal.h"
#include "regex.h"
#include "table.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wctype.h>

/* ================================================================== */
/* 首次运行的示例数据（一条与业务无关的复杂测试口径，覆盖全部功能）    */
/* ================================================================== */

static Segment *seed_static(const wchar_t *text)
{
    Segment *s = seg_create(SEG_STATIC);
    if (s) qp_strlcpy(s->content, text, QP_CONTENT_MAX);
    return s;
}

static Segment *seed_var(SegmentType type, const wchar_t *label,
                         const wchar_t *placeholder, const wchar_t *defval)
{
    Segment *s = seg_create(type);
    if (!s) return NULL;
    qp_strlcpy(s->label, label, QP_LABEL_MAX);
    qp_strlcpy(s->placeholder, placeholder ? placeholder : L"", QP_PLACEHOLD_MAX);
    qp_strlcpy(s->default_value, defval ? defval : L"", QP_VALUE_MAX);
    return s;
}

static void seed_add_parse(Segment *host, const wchar_t *delim, int index,
                           const wchar_t *keyword, const Segment *target,
                           const wchar_t *fill)
{
    ParseRule *r = parse_rule_create();
    if (!r || !host) { parse_rule_free(r); return; }
    r->type = PARSE_SPLIT;
    qp_strlcpy(r->delimiter, delim, QP_SEP_MAX);
    r->segment_index = index;
    qp_strlcpy(r->keyword, keyword, QP_KEYWORD_MAX);
    if (target) qp_strlcpy(r->target_segment_id, target->id, QP_ID_MAX);
    qp_strlcpy(r->fill_value, fill, QP_VALUE_MAX);
    parse_rule_append(&host->parse_rules, r);
}

static void seed_add_parse_contains(Segment *host, const wchar_t *keyword,
                                    const Segment *target, const wchar_t *fill)
{
    ParseRule *r = parse_rule_create();
    if (!r || !host) { parse_rule_free(r); return; }
    r->type = PARSE_CONTAINS;
    qp_strlcpy(r->keyword, keyword, QP_KEYWORD_MAX);
    if (target) qp_strlcpy(r->target_segment_id, target->id, QP_ID_MAX);
    qp_strlcpy(r->fill_value, fill, QP_VALUE_MAX);
    parse_rule_append(&host->parse_rules, r);
}

static void seed_add_cond(Segment *host, const Segment *trigger, CondOperator op,
                          const wchar_t *match, const wchar_t *result)
{
    ConditionRule *r = cond_rule_create();
    if (!r || !host) { cond_rule_free(r); return; }
    cond_rule_set_simple(r, trigger ? trigger->id : L"", op, 0, match);
    qp_strlcpy(r->result_value, result, QP_RESULT_MAX);
    cond_rule_append(&host->condition_rules, r);
}

/* 多条件项的规则：先 begin 拿到规则，再逐个 seed_cond_term 加条件项 */
static ConditionRule *seed_cond_begin(Segment *host, CondJoin join,
                                      const wchar_t *result)
{
    ConditionRule *r = cond_rule_create();
    if (!r || !host) { cond_rule_free(r); return NULL; }
    cond_term_list_clear(&r->terms);      /* 丢掉自带的空白项，下面自己加 */
    r->join = join;
    qp_strlcpy(r->result_value, result, QP_RESULT_MAX);
    cond_rule_append(&host->condition_rules, r);
    return r;
}

static void seed_cond_term(ConditionRule *r, const Segment *trigger,
                           CondOperator op, int negate, const wchar_t *match)
{
    CondTerm *t;
    if (!r) return;
    t = cond_term_create();
    if (!t) return;
    qp_strlcpy(t->trigger_segment_id, trigger ? trigger->id : L"", QP_ID_MAX);
    t->op = op;
    t->negate = negate;
    cond_term_set_match(t, match ? match : L"");
    cond_term_append(&r->terms, t);
}

static Segment *seed_flag(Segment *s, int hidden, int multiline)
{
    if (s) { s->hidden = hidden; s->multiline = multiline; }
    return s;
}

/* ------------------------------------------------------------------ */
/* 复杂示例：一条把所有功能都用上的通报口径，装完就能照着它试            */
/*                                                                    */
/* 覆盖：固定片段（含换行）、文本输入、下拉单选、下拉多选、隐藏变量、    */
/*       多行大文本框、按分隔符取段、按关键词命中、多条件项（全部满足 /  */
/*       任意满足 / 不满足取反）、以及「变量匹配的结果压过手动选择」。   */
/* ------------------------------------------------------------------ */
Template *tmpl_create_demo(void)
{
    Template *t = tmpl_create(L"复杂示例 · 违规处置通报");
    Segment *s_ticket, *s_date, *s_plat, *s_level, *s_kind, *s_action;
    Segment *s_detail, *s_evid, *s_follow, *s_reviewer, *s_review;
    ConditionRule *r;
    static const wchar_t *plat_opts[]   = { L"微信群", L"QQ 群", L"直播间", L"短视频评论区" };
    static const wchar_t *level_opts[]  = { L"严重", L"一般", L"轻微" };
    static const wchar_t *kind_opts[]   = { L"辱骂他人", L"刷屏广告", L"发布外链",
                                            L"诱导私加", L"违规内容" };
    static const wchar_t *action_opts[] = { L"口头警告", L"禁言 24 小时", L"禁言 7 天",
                                            L"永久封禁", L"移出并拉黑" };
    if (!t) return NULL;

    seg_list_append(&t->segments, seed_static(L"【处置通报】\r\n· 工单编号："));

    /* 一个变量喂饱下面一串：日期、等级、处置方式全从工单编号里拆出来 */
    s_ticket = seed_var(SEG_TEXT, L"工单编号", L"如 GK-20260817-A2-严重-封禁",
                        L"GK-20260817-A2-严重-封禁");
    seg_list_append(&t->segments, s_ticket);

    seg_list_append(&t->segments, seed_static(L"\r\n· 发生日期："));
    s_date = seed_flag(seed_var(SEG_TEXT, L"发生日期", L"由工单编号自动拆出", L""), 1, 0);
    seg_list_append(&t->segments, s_date);

    seg_list_append(&t->segments, seed_static(L"\r\n· 处置平台："));
    s_plat = seed_var(SEG_SELECT, L"处置平台", L"", L"微信群");
    seg_set_options(s_plat, plat_opts, (int)(sizeof(plat_opts) / sizeof(plat_opts[0])));
    seg_list_append(&t->segments, s_plat);

    seg_list_append(&t->segments, seed_static(L"　违规等级："));
    s_level = seed_var(SEG_SELECT, L"违规等级", L"", L"一般");
    seg_set_options(s_level, level_opts, (int)(sizeof(level_opts) / sizeof(level_opts[0])));
    seg_list_append(&t->segments, s_level);

    seg_list_append(&t->segments, seed_static(L"\r\n· 违规类型："));
    s_kind = seed_var(SEG_MULTI_SELECT, L"违规类型", L"可多选", L"刷屏广告");
    seg_set_options(s_kind, kind_opts, (int)(sizeof(kind_opts) / sizeof(kind_opts[0])));
    seg_list_append(&t->segments, s_kind);

    seg_list_append(&t->segments, seed_static(L"\r\n· 处置方式："));
    s_action = seed_var(SEG_SELECT, L"处置方式", L"", L"口头警告");
    seg_set_options(s_action, action_opts,
                    (int)(sizeof(action_opts) / sizeof(action_opts[0])));
    seg_list_append(&t->segments, s_action);

    seg_list_append(&t->segments, seed_static(L"\r\n· 处置说明：\r\n"));
    s_detail = seed_flag(seed_var(SEG_TEXT, L"处置说明",
                                  L"逐行写清取证时间、截图编号、当事人回应…", L""), 0, 1);
    seg_list_append(&t->segments, s_detail);

    seg_list_append(&t->segments, seed_static(L"\r\n· 取证情况："));
    s_evid = seed_flag(seed_var(SEG_TEXT, L"取证情况", L"", L"暂无截图，待补"), 1, 0);
    seg_list_append(&t->segments, s_evid);

    seg_list_append(&t->segments, seed_static(L"\r\n· 后续要求："));
    s_follow = seed_flag(seed_var(SEG_TEXT, L"后续要求", L"", L"按常规流程归档即可"), 1, 0);
    seg_list_append(&t->segments, s_follow);

    seg_list_append(&t->segments, seed_static(L"\r\n· 复核人："));
    s_reviewer = seed_var(SEG_TEXT, L"复核人", L"留空表示尚未复核", L"");
    seg_list_append(&t->segments, s_reviewer);

    s_review = seed_flag(seed_var(SEG_TEXT, L"复核状态", L"", L"（待复核）"), 1, 0);
    seg_list_append(&t->segments, s_review);

    /* --- 解析规则：挂在「工单编号」上，一次输入带出三个变量 --- */
    /* 按 "-" 取第 2 段 = 日期；不填充值，直接用取到的那一段 */
    seed_add_parse(s_ticket, L"-", 2, L"", s_date, L"");
    /* 按 "-" 取第 4 段，命中哪个词就填对应等级 */
    seed_add_parse(s_ticket, L"-", 4, L"严重", s_level, L"严重");
    seed_add_parse(s_ticket, L"-", 4, L"一般", s_level, L"一般");
    seed_add_parse(s_ticket, L"-", 4, L"轻微", s_level, L"轻微");
    /* 按关键词命中：整串里出现就算，顺序在前的先生效 */
    seed_add_parse_contains(s_ticket, L"封禁", s_action, L"永久封禁");
    seed_add_parse_contains(s_ticket, L"拉黑", s_action, L"移出并拉黑");
    seed_add_parse_contains(s_ticket, L"禁言", s_action, L"禁言 7 天");
    seed_add_parse_contains(s_ticket, L"警告", s_action, L"口头警告");
    /* 多行文本里写了「截图」就自动标注附件情况 */
    seed_add_parse_contains(s_detail, L"截图", s_evid, L"已附截图取证");

    /* --- 条件规则：挂在「后续要求」上，按顺序取第一条命中的 --- */
    /* 全部满足：等级严重 且 处置方式属于封禁/拉黑 */
    r = seed_cond_begin(s_follow, COND_JOIN_ALL,
                        L"2 小时内同步风控组，纳入平台黑名单，并在群内公示");
    seed_cond_term(r, s_level, OP_EQUALS, 0, L"严重");
    seed_cond_term(r, s_action, OP_IN, 0, L"永久封禁,移出并拉黑");
    /* 任意满足：平台在直播间/评论区，或违规类型里有「违规内容」 */
    r = seed_cond_begin(s_follow, COND_JOIN_ANY,
                        L"立即截图取证并上报内容安全组，24 小时内回执");
    seed_cond_term(r, s_plat, OP_IN, 0, L"直播间,短视频评论区");
    seed_cond_term(r, s_kind, OP_CONTAINS, 0, L"违规内容");
    /* 带取反：等级「不是」轻微 且 处置说明「不为空」 */
    r = seed_cond_begin(s_follow, COND_JOIN_ALL, L"抄送组长复核后归档");
    seed_cond_term(r, s_level, OP_EQUALS, 1, L"轻微");
    seed_cond_term(r, s_detail, OP_EMPTY, 1, L"");

    /* --- 条件规则：复核人填了就算已复核（同样是「不为空」的写法） --- */
    r = seed_cond_begin(s_review, COND_JOIN_ALL, L"（已复核）");
    seed_cond_term(r, s_reviewer, OP_EMPTY, 1, L"");

    return t;
}

AppData *appdata_create_seed(void)
{
    AppData *d = appdata_create();
    Group *g;
    Template *t;
    Segment *s_prod, *s_ver, *s_vertype, *s_impact, *s_rollback, *s_change;
    static const wchar_t *prod_opts[] = { L"订单系统", L"库存系统", L"财务系统", L"报表系统" };
    static const wchar_t *vertype_opts[] = { L"正式版", L"测试版", L"内测版" };
    static const wchar_t *impact_opts[] = { L"全部用户", L"部分用户", L"内部用户" };
    static const wchar_t *rollback_opts[] = { L"已准备一键回滚", L"灰度观察暂不回滚", L"按需手工回滚" };
    static const wchar_t *change_opts[] = { L"功能新增", L"缺陷修复", L"性能优化", L"安全加固" };
    if (!d) return NULL;

    g = group_create(L"示例");
    if (g) { g->order = 0; group_list_append(&d->groups, g); }

    t = tmpl_create(L"软件版本发布说明口径");
    if (!t) return d;
    if (g) qp_strlcpy(t->group_id, g->id, QP_ID_MAX);

    seg_list_append(&t->segments, seed_static(L"本次发布「"));

    s_prod = seed_var(SEG_SELECT, L"产品名称", L"选择产品", L"订单系统");
    seg_set_options(s_prod, prod_opts, (int)(sizeof(prod_opts) / sizeof(prod_opts[0])));
    seg_list_append(&t->segments, s_prod);

    seg_list_append(&t->segments, seed_static(L"」"));

    s_ver = seed_var(SEG_TEXT, L"版本号", L"如 V2.3.1-20240816-rel", L"V2.3.1-20240816-rel");
    seg_list_append(&t->segments, s_ver);

    seg_list_append(&t->segments, seed_static(L"（"));

    s_vertype = seed_var(SEG_SELECT, L"版本性质", L"", L"正式版");
    seg_set_options(s_vertype, vertype_opts, (int)(sizeof(vertype_opts) / sizeof(vertype_opts[0])));
    seg_list_append(&t->segments, s_vertype);

    seg_list_append(&t->segments, seed_static(L"），主要变更："));

    s_change = seed_var(SEG_MULTI_SELECT, L"变更类型", L"可多选", L"功能新增、缺陷修复");
    seg_set_options(s_change, change_opts, (int)(sizeof(change_opts) / sizeof(change_opts[0])));
    seg_list_append(&t->segments, s_change);

    seg_list_append(&t->segments, seed_static(L"；影响范围："));

    s_impact = seed_var(SEG_SELECT, L"影响范围", L"", L"全部用户");
    seg_set_options(s_impact, impact_opts, (int)(sizeof(impact_opts) / sizeof(impact_opts[0])));
    seg_list_append(&t->segments, s_impact);

    seg_list_append(&t->segments, seed_static(L"；回滚方案："));

    s_rollback = seed_var(SEG_SELECT, L"回滚方案", L"", L"已准备一键回滚");
    seg_set_options(s_rollback, rollback_opts, (int)(sizeof(rollback_opts) / sizeof(rollback_opts[0])));
    seg_list_append(&t->segments, s_rollback);

    seg_list_append(&t->segments, seed_static(L"。"));

    /* 解析规则（挂在版本号上）：按 "-" 取第 3 段判断发布性质，或按关键词 */
    seed_add_parse(s_ver, L"-", 3, L"rel",  s_vertype, L"正式版");
    seed_add_parse(s_ver, L"-", 3, L"beta", s_vertype, L"测试版");
    seed_add_parse_contains(s_ver, L"alpha", s_vertype, L"内测版");

    /* 条件规则（挂在影响范围上）：
       变更类型属于「安全加固」→ 全部用户强制升级；版本号以 V2 开头 → 分批灰度 */
    seed_add_cond(s_impact, s_change, OP_IN, L"安全加固", L"全部用户（安全加固强制升级）");
    seed_add_cond(s_impact, s_ver, OP_STARTS_WITH, L"V2", L"部分用户（分批灰度）");

    /* 条件规则（挂在回滚方案上）：
       版本性质等于「测试版」→ 灰度观察；变更类型包含「缺陷修复」→ 升级前已备份 */
    seed_add_cond(s_rollback, s_vertype, OP_EQUALS, L"测试版", L"灰度观察暂不回滚");
    seed_add_cond(s_rollback, s_change, OP_CONTAINS, L"缺陷修复", L"升级前已备份，按需手工回滚");

    tmpl_list_append(&d->templates, t);

    /* 再放一份把全部功能都用上的复杂示例，方便照着它试规则 */
    t = tmpl_create_demo();
    if (t) {
        if (g) qp_strlcpy(t->group_id, g->id, QP_ID_MAX);
        tmpl_list_append(&d->templates, t);
    }

    return d;
}
