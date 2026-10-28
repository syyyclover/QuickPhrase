/*
 * test_lookup.c —— 「查表匹配」（vlookup 口径）端到端自测
 *
 * 这一组测的是「配件 + 引擎 + 存档 + 体检」四段接起来对不对：
 *   · 规则模型：查表规则的默认值、输出对列表的增删与克隆；
 *   · 求值：把「设备编号」的值当键去查名单，把联系人 / 电话填进目标变量；
 *     键也可以是另一个变量的值或固定值；没命中时目标变量保持原样；
 *   · 存档：规则字段与数据表清单的往返，以及旧档缺字段时的默认值；
 *   · 体检：没选表 / 表被删 / 列名没了 / 目标变量被删 / 键重复 都要报出来。
 *
 * 纯 C，本机 gcc 即可跑（`make test` 会带上）。
 */
#include "engine.h"
#include "storage.h"
#include "table.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#ifdef _WIN32
#  include <direct.h>
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#endif

static int g_pass = 0, g_fail = 0;

static void check(int cond, const char *what)
{
    if (cond) { ++g_pass; printf("  [ok]   %s\n", what); }
    else      { ++g_fail; printf("  [FAIL] %s\n", what); }
}

static void check_int(int got, int want, const char *what)
{
    if (got == want) { ++g_pass; printf("  [ok]   %s\n", what); }
    else {
        ++g_fail;
        printf("  [FAIL] %s\n         期望: %d\n         实际: %d\n", what, want, got);
    }
}

static void check_str(const wchar_t *got, const wchar_t *want, const char *what)
{
    if (got && want && wcscmp(got, want) == 0) {
        ++g_pass;
        printf("  [ok]   %s\n", what);
    } else {
        ++g_fail;
        printf("  [FAIL] %s\n         期望: 「%ls」\n         实际: 「%ls」\n",
               what, want ? want : L"(null)", got ? got : L"(null)");
    }
}

/* 子串断言：消息里带上人名/列名就够证明「报对了」，不必逐字比对整句 */
static void check_has(const wchar_t *hay, const wchar_t *needle, const char *what)
{
    if (hay && needle && qp_istr(hay, needle) != NULL) {
        ++g_pass;
        printf("  [ok]   %s\n", what);
    } else {
        ++g_fail;
        printf("  [FAIL] %s\n         应在「%ls」里出现「%ls」\n",
               what, hay ? hay : L"(null)", needle ? needle : L"(null)");
    }
}

/* 存档文本里有没有这个键（cJSON 带格式输出，冒号后面是制表符，别硬拼整段） */
static int json_has_key(const char *json, const char *key)
{
    char pat[128];
    if (!json) return 0;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    return strstr(json, pat) != NULL;
}

/* 键值对：带格式（冒号后制表符）与不带格式两种写法都认 */
static int json_has_kv(const char *json, const char *key, const char *val)
{
    char pat[192];
    if (!json) return 0;
    snprintf(pat, sizeof(pat), "\"%s\":\t\"%s\"", key, val);
    if (strstr(json, pat)) return 1;
    snprintf(pat, sizeof(pat), "\"%s\":\"%s\"", key, val);
    return strstr(json, pat) != NULL;
}

static int json_has_bool(const char *json, const char *key, int val)
{
    char pat[128];
    if (!json) return 0;
    snprintf(pat, sizeof(pat), "\"%s\":\t%s", key, val ? "true" : "false");
    if (strstr(json, pat)) return 1;
    snprintf(pat, sizeof(pat), "\"%s\":%s", key, val ? "true" : "false");
    return strstr(json, pat) != NULL;
}

/* ------------------------------------------------------------------ */
/* 夹具                                                                */
/* ------------------------------------------------------------------ */

static wchar_t g_dir[QP_PATH_MAX];
static wchar_t g_path[QP_PATH_MAX];
static DataTableRef *g_refs;

static const wchar_t *ROSTER =
    L"设备编号,联系人,电话,班组\n"
    L"NFV-D-001,张三,13800138000,甲班\n"
    L"NFV-D-002,李四,13900139000,乙班\n"
    L"NFV-D-003,王五,13700137000,甲班\n"
    L"NFV-D-001,赵六,13600136000,甲班\n";

static const wchar_t *MULTI =
    L"设备编号,值班人,电话\n"
    L"DEV-1,甲,111\n"
    L"DEV-1,乙,222\n"
    L"DEV-1,丙,333\n";

static void make_dir(const wchar_t *p)
{
#ifdef _WIN32
    _wmkdir(p);
#else
    char *u8 = qp_wide_to_utf8_alloc(p);
    if (u8) { mkdir(u8, 0700); free(u8); }
#endif
}

static int write_text(const wchar_t *text)
{
    char *u8 = qp_wide_to_utf8_alloc(text);
    FILE *f = NULL;
    int ok = 0;
    if (!u8) return 0;
#ifdef _WIN32
    f = _wfopen(g_path, L"wb");
#else
    {
        char *p8 = qp_wide_to_utf8_alloc(g_path);
        if (p8) { f = fopen(p8, "wb"); free(p8); }
    }
#endif
    if (f) {
        fwrite(u8, 1, strlen(u8), f);
        fclose(f);
        ok = 1;
    }
    free(u8);
    return ok;
}

/* 登记一张数据表（引用 + 磁盘内容都准备好） */
static DataTableRef *register_table(const wchar_t *name, const wchar_t *text)
{
    DataTableRef *r;
    if (!write_text(text)) { printf("  ... 写表文件失败\n"); return NULL; }
    r = dtref_create(name, g_path);
    if (!r) return NULL;
    r->encoding = QP_ENC_AUTO;
    r->delimiter = QP_DELIM_AUTO;
    r->has_header = 1;
    dtref_list_append(&g_refs, r);
    qp_table_set_refs(g_refs);
    qp_table_invalidate_all();
    return r;
}

static void init_paths(void)
{
#ifdef _WIN32
    const wchar_t *tmp = _wgetenv(L"TEMP");
    if (!tmp || !*tmp) tmp = L".";
    swprintf(g_dir, QP_PATH_MAX, L"%ls\\qp_lookup_test", tmp);
#else
    qp_strlcpy(g_dir, L"/tmp/qp_lookup_test", QP_PATH_MAX);
#endif
    make_dir(g_dir);
    swprintf(g_path, QP_PATH_MAX, L"%ls/roster.csv", g_dir);
    g_path[QP_PATH_MAX - 1] = L'\0';
}

static void teardown(void)
{
    char *u8;
    qp_table_set_refs(NULL);
    qp_table_invalidate_all();
    dtref_list_free(g_refs);
    g_refs = NULL;
    u8 = qp_wide_to_utf8_alloc(g_path);
    if (u8) { remove(u8); free(u8); }
    u8 = qp_wide_to_utf8_alloc(g_dir);
    if (u8) { remove(u8); free(u8); }
}

/* 模板：文本「设备编号」+ 文本「联系人」+ 文本「电话」+ 固定句 */
static Template *mk_tmpl(Segment **key_out, Segment **name_out,
                         Segment **phone_out, Segment **host_out)
{
    Template *t = tmpl_create(L"查表测试模板");
    Segment *k = seg_create(SEG_TEXT);
    Segment *n = seg_create(SEG_TEXT);
    Segment *p = seg_create(SEG_TEXT);
    Segment *h = seg_create(SEG_TEXT);
    Segment *s = seg_create(SEG_STATIC);

    qp_strlcpy(k->label, L"设备编号", QP_LABEL_MAX);
    qp_strlcpy(n->label, L"联系人", QP_LABEL_MAX);
    qp_strlcpy(p->label, L"电话", QP_LABEL_MAX);
    qp_strlcpy(h->label, L"查询用编号", QP_LABEL_MAX);
    qp_strlcpy(s->content, L"联系人：{联系人} 电话：{电话}", QP_CONTENT_MAX);

    seg_list_append(&t->segments, k);
    seg_list_append(&t->segments, n);
    seg_list_append(&t->segments, p);
    seg_list_append(&t->segments, h);
    seg_list_append(&t->segments, s);

    if (key_out) *key_out = k;
    if (name_out) *name_out = n;
    if (phone_out) *phone_out = p;
    if (host_out) *host_out = h;
    return t;
}

/* 给 host 挂一条查表规则：键列 / 取值列都按列名 */
static ParseRule *add_lookup(Segment *host, const wchar_t *table_id,
                            const wchar_t *key_col, Segment *name_target,
                            const wchar_t *name_col, Segment *phone_target,
                            const wchar_t *phone_col)
{
    ParseRule *r = parse_rule_create();
    if (!r) return NULL;
    r->type = PARSE_LOOKUP;
    qp_strlcpy(r->lk_table_id, table_id, QP_ID_MAX);
    qp_strlcpy(r->lk_key_col, key_col, QP_LABEL_MAX);
    r->lk_key_col_index = -1;
    if (name_target && name_col) {
        LookupOut *o = lookup_out_create();
        if (o) {
            qp_strlcpy(o->col, name_col, QP_LABEL_MAX);
            o->col_index = -1;
            qp_strlcpy(o->target_segment_id, name_target->id, QP_ID_MAX);
            lookup_out_append(&r->lk_outs, o);
        }
    }
    if (phone_target && phone_col) {
        LookupOut *o = lookup_out_create();
        if (o) {
            qp_strlcpy(o->col, phone_col, QP_LABEL_MAX);
            o->col_index = -1;
            qp_strlcpy(o->target_segment_id, phone_target->id, QP_ID_MAX);
            lookup_out_append(&r->lk_outs, o);
        }
    }
    parse_rule_append(&host->parse_rules, r);
    return r;
}

/* ================================================================== */
/* ① 模型                                                              */
/* ================================================================== */

static void test_model(void)
{
    ParseRule *r = parse_rule_create();
    LookupOut *a, *b, *c;
    ParseRule *clone;

    printf("\n== 规则模型 ==\n");
    check(r != NULL, "查表规则能创建");
    if (!r) return;
    check_int(r->type, PARSE_SPLIT, "新建的规则默认是「按分隔符」");
    check_int(r->lk_key_mode, LOOKUP_KEY_HOST, "默认用本变量的值当键");
    check_int(r->lk_hit, LOOKUP_FIRST, "默认取第一条");
    check_str(r->lk_sep, L"、", "默认分隔符与多选一致（、）");
    check_int(r->lk_unique, 0, "默认不做去重");
    check(r->lk_outs == NULL, "默认未配置输出映射");
    check_int(lookup_out_count(r->lk_outs), 0, "输出列计数为 0");

    a = lookup_out_create();
    b = lookup_out_create();
    c = lookup_out_create();
    qp_strlcpy(a->col, L"联系人", QP_LABEL_MAX);
    qp_strlcpy(b->col, L"电话", QP_LABEL_MAX);
    qp_strlcpy(c->col, L"班组", QP_LABEL_MAX);
    lookup_out_append(&r->lk_outs, a);
    lookup_out_append(&r->lk_outs, b);
    lookup_out_append(&r->lk_outs, c);
    check_int(lookup_out_count(r->lk_outs), 3, "追加三个输出列");
    check(lookup_out_at(r->lk_outs, 0) == a, "第 1 个输出对还是第 1 个（顺序稳定）");
    check(lookup_out_at(r->lk_outs, 2) == c, "第 3 个输出对是最后追加的");
    check(lookup_out_at(r->lk_outs, 9) == NULL, "越界取输出对返回 NULL");
    check_int(lookup_out_remove_at(&r->lk_outs, 1), 1, "删掉中间那个输出对");
    check_int(lookup_out_count(r->lk_outs), 2, "删完剩 2 个");
    check(lookup_out_at(r->lk_outs, 1) == c, "后面的补上来且顺序不变");
    check_int(lookup_out_remove_at(&r->lk_outs, 5), 0, "删除越界下标返回 0");

    clone = parse_rule_clone(r);
    check(clone != NULL, "规则能克隆");
    if (clone) {
        check_int(clone->type, PARSE_SPLIT, "克隆保留类型");
        clone->type = PARSE_LOOKUP;
        check_int(lookup_out_count(clone->lk_outs), 2, "克隆带上了输出对");
        check(lookup_out_at(clone->lk_outs, 0) != lookup_out_at(r->lk_outs, 0),
              "克隆的输出对是深拷贝（不是同一个指针）");
        check_str(lookup_out_at(clone->lk_outs, 0)->col, L"联系人", "克隆的输出列名一致");
        qp_strlcpy(lookup_out_at(clone->lk_outs, 0)->col, L"改过的", QP_LABEL_MAX);
        check_str(lookup_out_at(r->lk_outs, 0)->col, L"联系人",
                  "改克隆不影响原规则");
        parse_rule_free(clone);
    }
    parse_rule_free(r);
    check(1, "释放规则不崩（输出对一并释放）");

    {
        ParseRule *fresh = parse_rule_create();
        check_int(fresh->lk_key_col_index, 0, "新建规则的键列下标是 0");
        parse_rule_free(fresh);
    }
}

/* ================================================================== */
/* ② 求值                                                              */
/* ================================================================== */

static void test_eval_basic(void)
{
    DataTableRef *ref = register_table(L"值班名单", ROSTER);
    Template *t;
    Segment *k, *n, *p, *h;
    EvalContext *ctx;
    int ik, in_, ip;

    printf("\n== 查表求值 ==\n");
    check(ref != NULL, "名单表登记成功");
    if (!ref) return;
    t = mk_tmpl(&k, &n, &p, &h);
    add_lookup(k, ref->id, L"设备编号", n, L"联系人", p, L"电话");

    ctx = eval_create(t);
    ik = eval_index_of_id(ctx, k->id);
    in_ = eval_index_of_id(ctx, n->id);
    ip = eval_index_of_id(ctx, p->id);

    eval_set_user_value(ctx, ik, L"NFV-D-002");
    eval_run(ctx);
    check_str(ctx->fields[in_].value, L"李四", "用「设备编号」的值查到联系人");
    check_str(ctx->fields[ip].value, L"13900139000", "同一条规则一次填了多个目标变量");
    check(ctx->fields[in_].rule_driven == 1, "联系人由规则推导（界面据此置灰）");

    /* 换了键，目标立刻跟着变 */
    eval_set_user_value(ctx, ik, L"NFV-D-003");
    eval_run(ctx);
    check_str(ctx->fields[in_].value, L"王五", "换编号后联系人跟着换");
    check_str(ctx->fields[ip].value, L"13700137000", "电话也跟着换");

    /* 键写错（表里没有）→ 不填 */
    eval_set_user_value(ctx, ik, L"NFV-D-999");
    eval_run(ctx);
    check_str(ctx->fields[in_].value, L"", "表里没有这个编号时不乱填");
    check_str(ctx->fields[ip].value, L"", "同一个规则的其它目标也不填");

    /* 未命中时，手动填过的值要留住（这是「填不了还能自己写」的兜底） */
    eval_set_user_value(ctx, in_, L"路人甲");
    eval_set_user_value(ctx, ik, L"NFV-D-999");
    eval_run(ctx);
    check_str(ctx->fields[in_].value, L"路人甲", "没查到时不覆盖手动填的值");
    eval_set_user_value(ctx, ik, L"NFV-D-001");
    eval_run(ctx);
    check_str(ctx->fields[in_].value, L"张三", "查到后又压过手动填的值");
    check(ctx->fields[in_].rule_driven == 1, "查到后目标算规则推导");

    /* 空键不查（否则会拿第一行乱填） */
    eval_set_user_value(ctx, ik, L"");
    eval_run(ctx);
    check_str(ctx->fields[in_].value, L"路人甲", "编号是空时不查表，手动值留着");
    eval_set_user_value(ctx, ik, L"   ");
    eval_run(ctx);
    check_str(ctx->fields[in_].value, L"路人甲", "编号只有空格时也不查表");

    /* 渲染出来是人话 */
    eval_set_user_value(ctx, ik, L"NFV-D-002");
    eval_run(ctx);
    {
        wchar_t out[QP_OUTPUT_MAX];
        eval_render(ctx, out, QP_OUTPUT_MAX);
        check(qp_istr(out, L"联系人：李四") != NULL, "渲染输出里带上了查到的联系人");
        check(qp_istr(out, L"13900139000") != NULL, "渲染输出里带上了查到的电话");
    }

    eval_destroy(ctx);
    tmpl_free(t);
}

static void test_eval_key_sources(void)
{
    DataTableRef *ref = register_table(L"值班名单", ROSTER);
    Template *t;
    Segment *k, *n, *p, *h;
    ParseRule *r;
    EvalContext *ctx;
    int ik, in_, ip, ih;

    printf("\n== 键的来源 ==\n");
    if (!ref) { check(0, "名单表没建起来"); return; }
    t = mk_tmpl(&k, &n, &p, &h);
    r = add_lookup(h, ref->id, L"设备编号", n, L"联系人", p, L"电话");
    /* 键改成「设备编号」这个变量的值：规则挂在「查询用编号」上，但键来自别处 */
    r->lk_key_mode = LOOKUP_KEY_VAR;
    qp_strlcpy(r->lk_key_var_id, k->id, QP_ID_MAX);

    ctx = eval_create(t);
    ik = eval_index_of_id(ctx, k->id);
    in_ = eval_index_of_id(ctx, n->id);
    ip = eval_index_of_id(ctx, p->id);
    ih = eval_index_of_id(ctx, h->id);

    eval_set_user_value(ctx, ik, L"NFV-D-003");
    eval_set_user_value(ctx, ih, L"这条值不该被当键");
    eval_run(ctx);
    check_str(ctx->fields[in_].value, L"王五", "键可以来自另一个变量的值");
    check_str(ctx->fields[ip].value, L"13700137000", "另一个变量的键一样能填满所有目标");

    /* 键来源变量被删 → 不命中，也不崩 */
    qp_strlcpy(r->lk_key_var_id, L"seg_不存在", QP_ID_MAX);
    eval_set_user_value(ctx, in_, L"自己填的");
    eval_run(ctx);
    check_str(ctx->fields[in_].value, L"自己填的", "键来源变量没了时不命中");

    /* 固定值当键 */
    r->lk_key_mode = LOOKUP_KEY_FIX;
    qp_strlcpy(r->lk_key_value, L"NFV-D-001", QP_MATCH_MAX);
    eval_set_user_value(ctx, ik, L"随便什么");
    eval_run(ctx);
    check_str(ctx->fields[in_].value, L"张三", "键可以是固定值");
    check_str(ctx->fields[ip].value, L"13800138000", "固定值键也能填满目标");

    qp_strlcpy(r->lk_key_value, L"", QP_MATCH_MAX);
    eval_set_user_value(ctx, in_, L"还是自己填的");
    eval_run(ctx);
    check_str(ctx->fields[in_].value, L"还是自己填的", "固定值为空时不命中");

    eval_destroy(ctx);
    tmpl_free(t);
}

static void test_eval_hit_modes(void)
{
    DataTableRef *ref = register_table(L"多行名单", MULTI);
    Template *t;
    Segment *k, *n, *p, *h;
    ParseRule *r;
    EvalContext *ctx;
    int ik, in_, ip;

    printf("\n== 键不唯一时的命中策略 ==\n");
    if (!ref) { check(0, "多行名单没建起来"); return; }
    t = mk_tmpl(&k, &n, &p, &h);
    r = add_lookup(k, ref->id, L"设备编号", n, L"值班人", p, L"电话");
    r->lk_key_col_index = -1;
    qp_strlcpy(lookup_out_at(r->lk_outs, 1)->col, L"电话", QP_LABEL_MAX);

    ctx = eval_create(t);
    ik = eval_index_of_id(ctx, k->id);
    in_ = eval_index_of_id(ctx, n->id);
    ip = eval_index_of_id(ctx, p->id);
    eval_set_user_value(ctx, ik, L"DEV-1");

    eval_run(ctx);
    check_str(ctx->fields[in_].value, L"甲", "取第一条 = 名单里第一个值班人");
    check_str(ctx->fields[ip].value, L"111", "电话也取第一条");

    r->lk_hit = LOOKUP_LAST;
    eval_run(ctx);
    check_str(ctx->fields[in_].value, L"丙", "取最后一条 = 名单里最后一个值班人");
    check_str(ctx->fields[ip].value, L"333", "电话也取最后一条");

    r->lk_hit = LOOKUP_ALL;
    eval_run(ctx);
    check_str(ctx->fields[in_].value, L"甲、乙、丙", "全部取出按「、」拼接");
    check_str(ctx->fields[ip].value, L"111、222、333", "电话列同样全部取出");

    qp_strlcpy(r->lk_sep, L" / ", QP_SEP_MAX);
    eval_run(ctx);
    check_str(ctx->fields[in_].value, L"甲 / 乙 / 丙", "分隔符可自定义");

    qp_strlcpy(r->lk_sep, L"、", QP_SEP_MAX);
    r->lk_unique = 1;
    {
        /* 让三行的电话都一样，验证「重复值只留一个」 */
        DataTableRef *r2 = register_table(L"重复值名单",
            L"设备编号,值班人,电话\nDEV-9,甲,555\nDEV-9,乙,555\nDEV-9,丙,666\n");
        qp_strlcpy(r->lk_table_id, r2->id, QP_ID_MAX);
        qp_table_invalidate_all();
        eval_set_user_value(ctx, ik, L"DEV-9");
        eval_run(ctx);
        check_str(ctx->fields[in_].value, L"甲、乙、丙", "去重只对「值」生效，人名不重复");
        check_str(ctx->fields[ip].value, L"555、666", "电话列里重复的 555 只留一份");
    }
    r->lk_unique = 0;
    eval_run(ctx);
    check_str(ctx->fields[ip].value, L"555、555、666", "关掉去重后重复值原样列出来");

    /* 命中全部但值的总长超过变量上限：截断而不是溢出，且必须停在完整的一项上。
       三行各 800 字，拼接需要 800+1+800+1+800 = 2402 字，超过 QP_VALUE_MAX。
       正确做法是只收前两项（800+1+800 = 1601），第三项整项不写 ——
       绝不能写成一个被拦腰截断的半截值。 */
    {
        wchar_t *big = (wchar_t *)malloc(8192 * sizeof(wchar_t));
        DataTableRef *r3;
        size_t used = 0;
        int i, k;
        if (big) {
            used += (size_t)swprintf(big + used, 8192 - used, L"设备编号,备注\n");
            for (i = 0; i < 3; ++i) {
                used += (size_t)swprintf(big + used, 8192 - used, L"LONG,");
                for (k = 0; k < 800; ++k)
                    used += (size_t)swprintf(big + used, 8192 - used, L"字");
                used += (size_t)swprintf(big + used, 8192 - used, L"\n");
            }
        }
        r3 = big ? register_table(L"超长名单", big) : NULL;
        free(big);
        check(r3 != NULL, "超长名单表建起来了");
        if (r3) {
            qp_strlcpy(r->lk_table_id, r3->id, QP_ID_MAX);
            qp_strlcpy(lookup_out_at(r->lk_outs, 0)->col, L"备注", QP_LABEL_MAX);
            lookup_out_at(r->lk_outs, 0)->col_index = -1;
            qp_table_invalidate_all();
            eval_set_user_value(ctx, ik, L"LONG");
            eval_run(ctx);
            check_int((int)wcslen(ctx->fields[in_].value), 800 + 1 + 800,
                      "超长拼接停在完整项上（两项共 1601 字，不写半个值）");
            check(ctx->fields[in_].value[0] != L'\0', "截断后仍然有内容");
            check(ctx->fields[in_].value[1600] == L'字' &&
                  ctx->fields[in_].value[1601] == L'\0',
                  "末尾收在一个完整取值的最后一字上，不是半截也不是悬空分隔符");
        }
    }

    eval_destroy(ctx);
    tmpl_free(t);
}

static void test_eval_priority(void)
{
    DataTableRef *ref = register_table(L"值班名单", ROSTER);
    Template *t;
    Segment *k, *n, *p, *h;
    EvalContext *ctx;
    int ik, in_;

    printf("\n== 与其它规则的优先级 ==\n");
    if (!ref) { check(0, "名单表没建起来"); return; }
    t = mk_tmpl(&k, &n, &p, &h);
    add_lookup(k, ref->id, L"设备编号", n, L"联系人", p, L"电话");

    /* 联系人同时被「引用」指向设备编号：查表规则应当压过引用 */
    qp_strlcpy(n->ref_segment_id, k->id, QP_ID_MAX);
    n->ref_mode = REF_WHOLE;

    ctx = eval_create(t);
    ik = eval_index_of_id(ctx, k->id);
    in_ = eval_index_of_id(ctx, n->id);
    eval_set_user_value(ctx, ik, L"NFV-D-002");
    eval_run(ctx);
    check_str(ctx->fields[in_].value, L"李四", "解析规则（查表）压过引用");
    check(ctx->fields[in_].rule_driven == 1, "查表结果算规则推导");

    /* 再加一条条件规则命中联系人：条件规则优先级最高（条件规则挂在目标片段上） */
    {
        ConditionRule *cr = cond_rule_create();
        CondTerm *term = cond_term_create();
        qp_strlcpy(term->trigger_segment_id, k->id, QP_ID_MAX);
        term->op = OP_EQUALS;
        cond_term_set_match(term, L"NFV-D-002");
        qp_strlcpy(cr->result_value, L"条件规则说了算", QP_RESULT_MAX);
        cond_term_list_clear(&cr->terms);
        cond_term_append(&cr->terms, term);
        cond_rule_append(&n->condition_rules, cr);
    }
    eval_run(ctx);
    check_str(ctx->fields[in_].value, L"条件规则说了算", "条件规则压过查表规则");

    eval_destroy(ctx);
    tmpl_free(t);
}

static void test_eval_broken_setup(void)
{
    Template *t;
    Segment *k, *n, *p, *h;
    ParseRule *r;
    EvalContext *ctx;
    int ik, in_;

    printf("\n== 配置有问题时不崩、不乱填 ==\n");
    t = mk_tmpl(&k, &n, &p, &h);
    ctx = eval_create(t);
    ik = eval_index_of_id(ctx, k->id);
    in_ = eval_index_of_id(ctx, n->id);
    eval_set_user_value(ctx, ik, L"NFV-D-001");
    eval_set_user_value(ctx, in_, L"手填的");

    /* ① 压根没选表 */
    r = add_lookup(k, L"", L"设备编号", n, L"联系人", NULL, NULL);
    eval_run(ctx);
    check_str(ctx->fields[in_].value, L"手填的", "没选表时不命中");
    qp_strlcpy(r->lk_table_id, L"tbl_不存在", QP_ID_MAX);
    eval_run(ctx);
    check_str(ctx->fields[in_].value, L"手填的", "表 id 是悬空的时不命中");

    /* ② 表在，但键列名改了 */
    {
        DataTableRef *ref = register_table(L"名单", ROSTER);
        qp_strlcpy(r->lk_table_id, ref->id, QP_ID_MAX);
        qp_strlcpy(r->lk_key_col, L"不存在的列", QP_LABEL_MAX);
        r->lk_key_col_index = -1;
        qp_table_invalidate_all();
        eval_run(ctx);
        check_str(ctx->fields[in_].value, L"手填的",
                  "键列名在表里找不到时不猜列号（宁可不填）");

        /* ③ 键列对了，取值列名改了：这一项不填，但另一项照填 */
        qp_strlcpy(r->lk_key_col, L"设备编号", QP_LABEL_MAX);
        qp_strlcpy(lookup_out_at(r->lk_outs, 0)->col, L"不存在的列", QP_LABEL_MAX);
        lookup_out_at(r->lk_outs, 0)->col_index = -1;
        eval_run(ctx);
        check_str(ctx->fields[in_].value, L"手填的", "取值列找不到时这一项不填");

        /* ④ 输出目标变量被删：跳过这一项，别崩 */
        qp_strlcpy(lookup_out_at(r->lk_outs, 0)->col, L"联系人", QP_LABEL_MAX);
        qp_strlcpy(lookup_out_at(r->lk_outs, 0)->target_segment_id,
                   L"seg_不存在", QP_ID_MAX);
        eval_run(ctx);
        check_str(ctx->fields[in_].value, L"手填的", "目标变量被删时不填它");
        check_int(ctx->fields[in_].rule_driven, 0, "目标变量被删时不算规则接管");

        /* ⑤ 一条输出都没配：不崩、不填 */
        qp_strlcpy(lookup_out_at(r->lk_outs, 0)->target_segment_id, n->id, QP_ID_MAX);
        lookup_out_list_free(r->lk_outs);
        r->lk_outs = NULL;
        eval_run(ctx);
        check_str(ctx->fields[in_].value, L"手填的", "没有输出列时不填任何东西");
    }

    /* ⑥ 表文件被删：不崩，没查到 */
    {
        DataTableRef *ref = g_refs;
        char *u8 = qp_wide_to_utf8_alloc(g_path);
        if (u8) { remove(u8); free(u8); }
        {
            ParseRule *r2 = parse_rule_create();
            LookupOut *o = lookup_out_create();
            r2->type = PARSE_LOOKUP;
            qp_strlcpy(r2->lk_table_id, ref->id, QP_ID_MAX);
            qp_strlcpy(r2->lk_key_col, L"设备编号", QP_LABEL_MAX);
            r2->lk_key_col_index = -1;
            qp_strlcpy(o->col, L"联系人", QP_LABEL_MAX);
            o->col_index = -1;
            qp_strlcpy(o->target_segment_id, n->id, QP_ID_MAX);
            lookup_out_append(&r2->lk_outs, o);
            parse_rule_append(&k->parse_rules, r2);
            qp_table_invalidate_all();
            eval_run(ctx);
            check_str(ctx->fields[in_].value, L"手填的", "表文件被删时查不到也不崩");
        }
        register_table(L"名单", ROSTER);        /* 把文件写回来，后面的用例还要用 */
    }

    eval_destroy(ctx);
    tmpl_free(t);
}

/* ================================================================== */
/* ③ 存档                                                              */
/* ================================================================== */

static void test_storage(void)
{
    Template *t;
    Segment *k, *n, *p, *h;
    ParseRule *r;
    AppData *d;
    DataTableRef *ref;
    char *json;

    printf("\n== 存档往返 ==\n");
    t = mk_tmpl(&k, &n, &p, &h);
    r = add_lookup(k, L"tbl_test01", L"设备编号", n, L"联系人", p, L"电话");
    r->lk_key_mode = LOOKUP_KEY_FIX;
    qp_strlcpy(r->lk_key_value, L"NFV-D-001", QP_MATCH_MAX);
    r->lk_hit = LOOKUP_ALL;
    qp_strlcpy(r->lk_sep, L" | ", QP_SEP_MAX);
    r->lk_unique = 1;
    r->lk_key_col_index = -1;
    qp_strlcpy(lookup_out_at(r->lk_outs, 1)->col, L"电话", QP_LABEL_MAX);

    d = appdata_create();
    tmpl_list_append(&d->templates, t);
    ref = dtref_create(L"值班名单", L"D:\\名单\\2026年第33周.csv");
    ref->encoding = QP_ENC_GBK;
    ref->delimiter = QP_DELIM_SEMI;
    ref->has_header = 1;
    dtref_list_append(&d->tables, ref);

    json = appdata_to_json(d);
    check(json != NULL, "存档能写出来");
    if (json) {
        check(json_has_key(json, "dataTables"), "存档里有数据表清单");
        check(json_has_kv(json, "type", "lookup"), "查表规则写成 type=lookup");
        check(json_has_kv(json, "lkKeyMode", "fix"), "键来源写进了存档");
        check(json_has_kv(json, "hit", "all"), "命中策略写进了存档");
        check(json_has_bool(json, "unique", 1), "去重开关写进了存档");
        check(json_has_key(json, "outputs"), "输出列写成了数组");
        check(json_has_kv(json, "encoding", "gbk"), "表的编码写进了存档");
        check(json_has_kv(json, "delimiter", "semi"), "表的分隔符写进了存档");
        check(strstr(json, "2026") != NULL, "表的路径写进了存档");

        {
            AppData *back = appdata_from_json(json);
            check(back != NULL, "存档能读回来");
            if (back) {
                Template *t2 = back->templates;
                Segment *s2 = t2 ? t2->segments : NULL;
                ParseRule *r2 = s2 ? s2->parse_rules : NULL;
                check_int(dtref_list_count(back->tables), 1, "数据表清单读回来 1 条");
                if (back->tables) {
                    check_str(back->tables->name, L"值班名单", "表名往返一致");
                    check_str(back->tables->path, L"D:\\名单\\2026年第33周.csv",
                              "表路径往返一致");
                    check_int(back->tables->encoding, QP_ENC_GBK, "表编码往返一致");
                    check_int(back->tables->delimiter, QP_DELIM_SEMI, "表分隔符往返一致");
                    check_int(back->tables->has_header, 1, "表头标记往返一致");
                }
                check(r2 != NULL, "查表规则读回来了");
                if (r2) {
                    check_int(r2->type, PARSE_LOOKUP, "规则type往返一致");
                    check_str(r2->lk_table_id, L"tbl_test01", "表 id 往返一致");
                    check_str(r2->lk_key_col, L"设备编号", "键列名往返一致");
                    check_int(r2->lk_key_col_index, -1, "键列下标往返一致");
                    check_int(r2->lk_key_mode, LOOKUP_KEY_FIX, "键来源往返一致");
                    check_str(r2->lk_key_value, L"NFV-D-001", "固定键往返一致");
                    check_int(r2->lk_hit, LOOKUP_ALL, "命中策略往返一致");
                    check_str(r2->lk_sep, L" | ", "分隔符往返一致");
                    check_int(r2->lk_unique, 1, "去重开关往返一致");
                    check_int(lookup_out_count(r2->lk_outs), 2, "输出列数量往返一致");
                    if (r2->lk_outs) {
                        check_str(r2->lk_outs->col, L"联系人", "第 1 个输出列名往返一致");
                        check_str(r2->lk_outs->target_segment_id,
                                  lookup_out_at(r->lk_outs, 0)->target_segment_id,
                                  "第 1 个输出目标往返一致");
                    }
                    if (lookup_out_at(r2->lk_outs, 1)) {
                        check_str(lookup_out_at(r2->lk_outs, 1)->col, L"电话",
                                  "第 2 个输出列名往返一致");
                    }
                }
                appdata_free(back);
            }
        }
        free(json);
    }
    appdata_free(d);
}

static void test_storage_defaults(void)
{
    /* 旧档：没有 dataTables，查表规则也缺字段 —— 必须给出能用的默认值 */
    static const char *old_json =
        "{\"version\":1,\"app\":\"QuickPhrase\",\"templates\":[{\"id\":\"tpl_1\","
        "\"name\":\"老模板\",\"segments\":[{\"id\":\"seg_1\",\"type\":\"text\","
        "\"label\":\"设备编号\",\"parseRules\":[{\"id\":\"pr_1\",\"type\":\"lookup\","
        "\"tableId\":\"tbl_old\"}]}]}]}";
    AppData *d = appdata_from_json(old_json);
    ParseRule *r;

    printf("\n== 旧档缺字段的默认值 ==\n");
    check(d != NULL, "缺 dataTables 的旧档能读");
    if (!d) return;
    check(d->tables == NULL, "没有数据表时清单为空（不是坏指针）");
    r = d->templates->segments->parse_rules;
    check(r != NULL, "老档里的 lookup 规则读出来了");
    if (r) {
        check_int(r->type, PARSE_LOOKUP, "类型还是 lookup");
        check_int(r->lk_key_mode, LOOKUP_KEY_HOST, "没写键来源时默认用本变量");
        check_int(r->lk_hit, LOOKUP_FIRST, "没写命中策略时默认取第一条");
        check_str(r->lk_sep, L"、", "没写分隔符时用默认的「、」");
        check_int(r->lk_unique, 0, "没写去重时默认不去重");
        check(r->lk_outs == NULL, "没写输出列时就是没有");
        check_str(r->lk_table_id, L"tbl_old", "已有的表 id 不丢");
    }

    appdata_free(d);
    /* 认不出的枚举键名要安全回落，而不是变成随机值 */
    {
        static const char *weird =
            "{\"templates\":[{\"id\":\"tpl_2\",\"name\":\"怪模板\",\"segments\":"
            "[{\"id\":\"seg_2\",\"type\":\"text\",\"label\":\"编号\",\"parseRules\":"
            "[{\"id\":\"pr_2\",\"type\":\"lookup\",\"lkKeyMode\":\"乱写\","
            "\"hit\":\"乱写\",\"keyColIndex\":-5}]}]}],"
            "\"dataTables\":[{\"id\":\"tbl_2\",\"name\":\"表\",\"path\":\"C:\\\\a.csv\","
            "\"encoding\":\"乱写\",\"delimiter\":\"乱写\"},"
            "{\"id\":\"tbl_3\",\"name\":\"没有路径的表\"}]}";
        AppData *w = appdata_from_json(weird);
        check(w != NULL, "字段值乱写的档也能读，不报错");
        if (w) {
            ParseRule *wr = w->templates->segments->parse_rules;
            check_int(wr->lk_key_mode, LOOKUP_KEY_HOST, "认不出的键来源回落成默认");
            check_int(wr->lk_hit, LOOKUP_FIRST, "认不出的命中策略回落成默认");
            check_int(wr->lk_key_col_index, -1, "非法列下标被夹回 -1（= 按列名找）");
            check_int(dtref_list_count(w->tables), 1, "没有路径的表引用被丢掉");
            if (w->tables) {
                check_int(w->tables->encoding, QP_ENC_AUTO, "认不出的编码回落成自动识别");
                check_int(w->tables->delimiter, QP_DELIM_AUTO, "认不出的分隔符回落成自动识别");
            }
            appdata_free(w);
        }
    }

    /* 非查表规则不该带上查表字段 */
    {
        Template *t = tmpl_create(L"普通模板");
        Segment *s = seg_create(SEG_TEXT);
        ParseRule *pr = parse_rule_create();
        AppData *d2 = appdata_create();
        char *json;
        qp_strlcpy(s->label, L"编号", QP_LABEL_MAX);
        pr->type = PARSE_SPLIT;
        parse_rule_append(&s->parse_rules, pr);
        seg_list_append(&t->segments, s);
        tmpl_list_append(&d2->templates, t);
        json = appdata_to_json(d2);
        check(json && !json_has_key(json, "lkKeyMode"), "普通规则不写查表字段");
        check(json && !json_has_key(json, "outputs"), "普通规则不写 outputs");
        check(json && json_has_key(json, "dataTables"),
              "没有数据表时也写出空清单（存档结构稳定）");
        if (json) {
            AppData *back = appdata_from_json(json);
            check(back && back->tables == NULL, "空清单读回来就是「没有表」");
            if (back) appdata_free(back);
        }
        free(json);
        appdata_free(d2);
    }
}

/* ================================================================== */
/* ④ 体检与摘要                                                        */
/* ================================================================== */

static int lint_has(const LintIssue *is, int n, LintKind kind, const wchar_t *needle)
{
    int i;
    for (i = 0; i < n; ++i) {
        if (is[i].kind != kind) continue;
        if (!needle || qp_istr(is[i].text, needle) != NULL) return 1;
    }
    return 0;
}

static void test_lint(void)
{
    LintIssue *is = (LintIssue *)calloc(QP_LINT_MAX, sizeof(LintIssue));
    Template *t;
    Segment *k, *n, *p, *h;
    ParseRule *r;
    int count;

    printf("\n== 模板体检 ==\n");
    t = mk_tmpl(&k, &n, &p, &h);
    if (!is) { check(0, "体检结果缓冲没申请到"); tmpl_free(t); return; }

    /* ① 一条都没配 */
    r = add_lookup(k, L"", L"", NULL, NULL, NULL, NULL);
    count = tmpl_lint(t, is, QP_LINT_MAX);
    check(lint_has(is, count, LINT_LOOKUP_SETUP, L"尚未选择数据表"),
          "没选表：体检报「尚未选择数据表」");
    check(lint_has(is, count, LINT_LOOKUP_SETUP, L"没有配置任何输出列"),
          "没有输出列：体检报「没有配置任何输出列」");

    /* ② 表被删 */
    qp_strlcpy(r->lk_table_id, L"tbl_不存在", QP_ID_MAX);
    qp_strlcpy(r->lk_key_col, L"设备编号", QP_LABEL_MAX);
    r->lk_key_col_index = -1;
    {
        LookupOut *o = lookup_out_create();
        qp_strlcpy(o->col, L"联系人", QP_LABEL_MAX);
        o->col_index = -1;
        qp_strlcpy(o->target_segment_id, n->id, QP_ID_MAX);
        lookup_out_append(&r->lk_outs, o);
    }
    count = tmpl_lint(t, is, QP_LINT_MAX);
    check(lint_has(is, count, LINT_LOOKUP_TABLE, L"已被删除"),
          "表被删：体检报「引用的数据表已被删除」");

    /* ③ 表在，列名改了 */
    {
        DataTableRef *ref = register_table(L"名单", ROSTER);
        qp_strlcpy(r->lk_table_id, ref->id, QP_ID_MAX);
        qp_table_invalidate_all();

        qp_strlcpy(r->lk_key_col, L"不存在的列", QP_LABEL_MAX);
        count = tmpl_lint(t, is, QP_LINT_MAX);
        check(lint_has(is, count, LINT_LOOKUP_COL, L"键列"),
              "键列名找不到：体检指名道姓报出来");

        qp_strlcpy(r->lk_key_col, L"设备编号", QP_LABEL_MAX);
        qp_strlcpy(r->lk_outs->col, L"不存在的列", QP_LABEL_MAX);
        count = tmpl_lint(t, is, QP_LINT_MAX);
        check(lint_has(is, count, LINT_LOOKUP_COL, L"取值列"),
              "取值列名找不到：体检也报");

        /* ④ 键重复：提醒当前命中策略的后果 */
        qp_strlcpy(r->lk_outs->col, L"联系人", QP_LABEL_MAX);
        count = tmpl_lint(t, is, QP_LINT_MAX);
        check(lint_has(is, count, LINT_LOOKUP_DUPKEY, L"不是唯一键"),
              "键列有重复：体检提醒「不是唯一键」");
        check(lint_has(is, count, LINT_LOOKUP_DUPKEY, L"取第一条"),
              "提醒里说清了当前命中策略");

        /* ⑤ 目标变量被删 */
        qp_strlcpy(r->lk_outs->target_segment_id, L"seg_不存在", QP_ID_MAX);
        count = tmpl_lint(t, is, QP_LINT_MAX);
        check(lint_has(is, count, LINT_LOOKUP_TARGET, L"目标变量已被删除"),
              "目标变量被删：体检报出来");
    }

    /* ⑥ 配得整整齐齐的一张表：不该有任何查表相关的提醒 */
    {
        Template *t2 = mk_tmpl(&k, &n, &p, &h);
        DataTableRef *ref = register_table(L"干净名单",
            L"设备编号,联系人,电话\nDEV-1,甲,111\nDEV-2,乙,222\n");
        ParseRule *r2;
        int i, bad = 0;
        r2 = add_lookup(k, ref->id, L"设备编号", n, L"联系人", p, L"电话");
        r2->lk_key_col_index = -1;
        qp_table_invalidate_all();
        count = tmpl_lint(t2, is, QP_LINT_MAX);
        for (i = 0; i < count; ++i) {
            if (is[i].kind == LINT_LOOKUP_SETUP || is[i].kind == LINT_LOOKUP_TABLE ||
                is[i].kind == LINT_LOOKUP_COL || is[i].kind == LINT_LOOKUP_TARGET ||
                is[i].kind == LINT_LOOKUP_DUPKEY) ++bad;
        }
        check_int(bad, 0, "配得齐全的查表规则不产生任何提醒");
        tmpl_free(t2);
    }

    /* ⑦ 键来源变量被删 */
    {
        Template *t3 = mk_tmpl(&k, &n, &p, &h);
        DataTableRef *ref = register_table(L"名单3", ROSTER);
        ParseRule *r3 = add_lookup(h, ref->id, L"设备编号", n, L"联系人", NULL, NULL);
        r3->lk_key_col_index = -1;
        r3->lk_key_mode = LOOKUP_KEY_VAR;
        qp_strlcpy(r3->lk_key_var_id, L"seg_不存在", QP_ID_MAX);
        qp_table_invalidate_all();
        count = tmpl_lint(t3, is, QP_LINT_MAX);
        check(lint_has(is, count, LINT_LOOKUP_SETUP, L"作为键来源的变量已被删除"),
              "当键的变量被删：体检报出来");
        tmpl_free(t3);
    }

    free(is);
    tmpl_free(t);
}

static void test_summary(void)
{
    Template *t;
    Segment *k, *n, *p, *h;
    ParseRule *r;
    wchar_t out[QP_RESULT_MAX];
    DataTableRef *ref;

    printf("\n== 规则摘要 ==\n");
    t = mk_tmpl(&k, &n, &p, &h);
    ref = register_table(L"值班名单", ROSTER);
    r = add_lookup(k, ref->id, L"设备编号", n, L"联系人", p, L"电话");

    lookup_rule_summary(r, t, out, QP_RESULT_MAX);
    check_has(out, L"值班名单", "摘要里带上了表名");
    check_has(out, L"本变量", "摘要说明了键取的是本变量的值");
    check_has(out, L"联系人", "摘要里列出了第 1 个输出列");

    r->lk_key_mode = LOOKUP_KEY_VAR;
    qp_strlcpy(r->lk_key_var_id, h->id, QP_ID_MAX);
    lookup_rule_summary(r, t, out, QP_RESULT_MAX);
    check_has(out, L"查询用编号", "键来自别的变量时摘要写出变量名");

    r->lk_key_mode = LOOKUP_KEY_FIX;
    qp_strlcpy(r->lk_key_value, L"NFV-D-001", QP_MATCH_MAX);
    r->lk_hit = LOOKUP_LAST;
    lookup_rule_summary(r, t, out, QP_RESULT_MAX);
    check_has(out, L"NFV-D-001", "固定值键写进摘要");
    check_has(out, L"取最后一条", "命中策略写进摘要");

    r->lk_hit = LOOKUP_ALL;
    lookup_rule_summary(r, t, out, QP_RESULT_MAX);
    check_has(out, L"全部拼接", "「全部」策略写进摘要");

    qp_strlcpy(r->lk_table_id, L"tbl_没有", QP_ID_MAX);
    lookup_rule_summary(r, t, out, QP_RESULT_MAX);
    check_has(out, L"已删除", "表被删时摘要明说");

    qp_strlcpy(r->lk_table_id, L"", QP_ID_MAX);
    lookup_rule_summary(r, t, out, QP_RESULT_MAX);
    check_has(out, L"未选择表", "没选表时摘要明说");

    qp_strlcpy(r->lk_table_id, ref->id, QP_ID_MAX);
    lookup_out_list_free(r->lk_outs);
    r->lk_outs = NULL;
    lookup_rule_summary(r, t, out, QP_RESULT_MAX);
    check_has(out, L"未配置输出映射", "没有输出列时摘要明说");

    /* 缓冲很小时也不能溢出 */
    {
        wchar_t tiny[16];
        memset(tiny, 0x41, sizeof(tiny));
        qp_strlcpy(r->lk_table_id, ref->id, QP_ID_MAX);
        {
            LookupOut *o = lookup_out_create();
            qp_strlcpy(o->col, L"联系人", QP_LABEL_MAX);
            lookup_out_append(&r->lk_outs, o);
        }
        lookup_rule_summary(r, t, tiny, 8);
        check(wcslen(tiny) < 8, "小结缓冲很小时安全截断并结尾");
    }

    check(wcscmp(parse_type_name(PARSE_LOOKUP), L"查表匹配") == 0,
          "规则类型的中文名是「查表匹配」");
    tmpl_free(t);
}

/* ================================================================== */
/* ⑦ 批量：一次粘进多行，逐行查表                                     */
/*                                                                    */
/* 真实用法就是这样：粘进一段多行编号，每行去名单里查一个人，结果按行   */
/* 拼出来。这一节把「逐行查表」这条链路整条钉住 —— 行序、未命中、命中   */
/* 策略、键取自别的变量，四件事都要在批量下各自正确。                 */
/* ================================================================== */

static void test_batch(void)
{
    DataTableRef *tr = register_table(L"值班名单", ROSTER);
    Segment *key, *nm, *ph, *host;
    Template *t = mk_tmpl(&key, &nm, &ph, &host);
    ParseRule *rule;
    EvalContext *ctx;
    wchar_t out[4096];
    int marks[64], nmarks = 0, hi, ki;

    printf("\n== 批量：粘多行，逐行查表 ==\n");
    check(tr != NULL, "值班名单表登记好了");
    if (!tr) { tmpl_free(t); return; }

    rule = add_lookup(host, tr->id, L"设备编号", nm, L"联系人", ph, L"电话");
    check(rule != NULL, "查表规则挂上了");
    if (!rule) { tmpl_free(t); return; }

    /* 填写区那几个变量只为拼行服务：勾上「不计入输出」，
       输出里就只剩那一个固定句（README 推荐的拆行做法）。 */
    key->no_output = 1;
    nm->no_output = 1;
    ph->no_output = 1;
    host->no_output = 1;

    /* 开批量：源变量就是「查询用编号」，一行一个编号 */
    t->batch_mode = 1;
    qp_strlcpy(t->batch_source_id, host->id, QP_ID_MAX);

    ctx = eval_create(t);
    check(ctx != NULL, "求值上下文建起来了");
    if (!ctx) { tmpl_free(t); return; }
    hi = eval_index_of_id(ctx, host->id);
    ki = eval_index_of_id(ctx, key->id);
    check(hi >= 0 && ki >= 0, "找得到批量源变量与键变量");

    eval_set_user_value(ctx, hi, L"NFV-D-001\nNFV-D-003\nNFV-D-002");
    eval_run(ctx);
    nmarks = 0;
    check(eval_render_batch_text_marked(ctx, out, 4096, marks, 64, &nmarks) > 0,
          "批量渲染有输出");
    check_str(out,
              L"联系人：张三 电话：13800138000\n"
              L"联系人：王五 电话：13700137000\n"
              L"联系人：李四 电话：13900139000",
              "三行各自查到对应的人，行序与输入一致");
    check_str(ctx->fields[hi].value, L"NFV-D-001\nNFV-D-003\nNFV-D-002",
              "批量渲染后源变量仍保留整段输入");

    /* 某一行查不到：这一行照样占一行，只是那两处空着并被标出来 */
    eval_set_user_value(ctx, hi, L"NFV-D-002\nNFV-D-999");
    eval_run(ctx);
    nmarks = 0;
    eval_render_batch_text_marked(ctx, out, 4096, marks, 64, &nmarks);
    check_str(out,
              L"联系人：李四 电话：13900139000\n"
              L"联系人： 电话：",
              "查不到的那一行照样成行，只是值空着");
    check(nmarks >= 1, "查不到的位置被记下来（预览据此标红）");

    /* 命中策略在批量里同样逐行生效 */
    rule->lk_hit = LOOKUP_LAST;
    eval_set_user_value(ctx, hi, L"NFV-D-001\nNFV-D-002");
    eval_run(ctx);
    eval_render_batch_text_marked(ctx, out, 4096, marks, 64, &nmarks);
    check_str(out,
              L"联系人：赵六 电话：13600136000\n"
              L"联系人：李四 电话：13900139000",
              "批量里「取最后一条」逐行生效");

    /* 键取自另一个变量时，批量每行拿的是同一个键（常量列逐行不变） */
    rule->lk_hit = LOOKUP_FIRST;
    rule->lk_key_mode = LOOKUP_KEY_VAR;
    qp_strlcpy(rule->lk_key_var_id, key->id, QP_ID_MAX);
    eval_set_user_value(ctx, ki, L"NFV-D-003");
    eval_set_user_value(ctx, hi, L"占位行一\n占位行二");
    eval_run(ctx);
    eval_render_batch_text_marked(ctx, out, 4096, marks, 64, &nmarks);
    check_str(out,
              L"联系人：王五 电话：13700137000\n"
              L"联系人：王五 电话：13700137000",
              "键取自另一个变量时，批量每行都用那个键");

    eval_destroy(ctx);
    tmpl_free(t);
}

/* ================================================================== */
/* ⑧ 导入导出：表清单必须跟着走                                       */
/*                                                                    */
/* 导出的 JSON 里带了数据表清单（dataTables），导入时也得并进来 ——      */
/* 否则把模板导到另一台机器，所有查表规则都指向不存在的表，静默失效。   */
/* 而本地已登记的表一律不许被导入清掉：它指的是本机文件。               */
/* ================================================================== */

static void test_import(void)
{
    wchar_t path[QP_PATH_MAX];
    wchar_t snapshots[3][QP_PATH_MAX] = {{0}};
    AppData *src = appdata_create();
    AppData *dst = appdata_create();
    DataTableRef *ref;
    Template *t;
    Segment *k, *n, *p, *h;
    int added = 0, updated = 0;

    printf("\n== 导入导出：表清单跟着走 ==\n");
    swprintf(path, QP_PATH_MAX, L"%ls/exchange.json", g_dir);
    storage_set_data_dir(g_dir);

    t = mk_tmpl(&k, &n, &p, &h);
    add_lookup(k, L"tbl_imp01", L"设备编号", n, L"联系人", p, L"电话");
    tmpl_list_append(&src->templates, t);
    ref = dtref_create(L"值班名单", g_path);
    ref->encoding = QP_ENC_UTF8;
    ref->delimiter = QP_DELIM_COMMA;
    ref->has_header = 1;
    dtref_list_append(&src->tables, ref);

    check_int(storage_export(src, path), 1, "导出到文件成功");

    /* 目标端是空库：导入后表清单要跟着来 */
    check_int(storage_import(dst, path, QP_IMPORT_MERGE, &added, &updated), 1,
              "合并导入成功");
    check_int(added, 1, "导入 1 个模板");
    check_int(dtref_list_count(dst->tables), 1,
              "合并导入把数据表清单也带过来了");
    if (dst->tables) {
        check_str(dst->tables->name, L"值班名单", "表名跟着走");
        check(wcscmp(dst->tables->path, g_path) != 0, "共享表快照使用接收端路径");
        size_t original_size = 0, imported_size = 0;
        char *original = storage_read_bytes(g_path, &original_size);
        char *imported = storage_read_bytes(dst->tables->path, &imported_size);
        check(original && imported && original_size == imported_size &&
              !memcmp(original, imported, original_size), "共享表快照保留原始内容");
        free(original); free(imported);
        qp_strlcpy(snapshots[0], dst->tables->path, QP_PATH_MAX);
        check_int(dst->tables->encoding, QP_ENC_UTF8, "表编码跟着走");
    }

    /* 同 id 再导一次：更新而不是堆出第二份 */
    added = updated = 0;
    storage_import(dst, path, QP_IMPORT_MERGE, &added, &updated);
    if (dst->tables) qp_strlcpy(snapshots[1], dst->tables->path, QP_PATH_MAX);
    check_int(dtref_list_count(dst->tables), 1, "重复导入不会堆出第二份表");
    check_int(tmpl_list_count(dst->templates), 1, "重复导入也不会堆出第二个模板");

    /* 本地多一张别的表：导入不许把它清掉 */
    {
        DataTableRef *local = dtref_create(L"本机名单", g_path);
        dtref_list_append(&dst->tables, local);
        check_int(dtref_list_count(dst->tables), 2, "本地先有两张表");

        added = updated = 0;
        check_int(storage_import(dst, path, QP_IMPORT_REPLACE, &added, &updated), 1,
                  "整体替换导入成功");
        check_int(tmpl_list_count(dst->templates), 1, "替换后模板数正确");
        check_int(dtref_list_count(dst->tables), 2,
                  "整体替换后本地表一张都没丢（导入只增不减）");
        if (dst->tables) qp_strlcpy(snapshots[2], dst->tables->path, QP_PATH_MAX);
    }

    appdata_free(src);
    appdata_free(dst);
    for (int i = 0; i < 3; ++i) {
        char *u8 = qp_wide_to_utf8_alloc(snapshots[i]);
        if (u8) { remove(u8); free(u8); }
    }
    storage_set_data_dir(NULL);
    {
        char *u8 = qp_wide_to_utf8_alloc(path);
        if (u8) { remove(u8); free(u8); }
    }
}

/* ================================================================== */
/* ⑧ 逐段查表（per_paragraph）                                         */
/* ================================================================== */

/* 其它三种解析规则早就有「逐段处理」，查表匹配当初漏了这一条：
   一个变量里粘进多段编号时只能整体当键，表里当然查不到。这里把补上的
   行为钉住：每段各查一次、命中的按换行合并、空白段与未命中段跳过、
   单段时与不勾逐段完全一致。 */
static void test_per_paragraph(void)
{
    DataTableRef *ref = register_table(L"值班名单", ROSTER);
    Segment *key, *nm, *ph, *host;
    Template *t;
    ParseRule *r;
    EvalContext *ctx;
    int hh, ni, pi;

    printf("\n== 逐段查表：一个变量里粘多段编号，每段各查一次 ==\n");
    check(ref != NULL, "名单表登记成功");
    if (!ref) return;
    t = mk_tmpl(&key, &nm, &ph, &host);
    r = add_lookup(host, ref->id, L"设备编号", nm, L"联系人", ph, L"电话");
    check(r != NULL, "查表规则挂上了");
    if (!r) { tmpl_free(t); return; }

    ctx = eval_create(t);
    check(ctx != NULL, "求值上下文建起来了");
    if (!ctx) { tmpl_free(t); return; }
    hh = eval_index_of_id(ctx, host->id);
    ni = eval_index_of_id(ctx, nm->id);
    pi = eval_index_of_id(ctx, ph->id);

    /* 不勾逐段：整段多行文本被当成一个键，表里没有这个键 → 不命中 */
    eval_set_user_value(ctx, hh, L"NFV-D-001\nNFV-D-002");
    eval_run(ctx);
    check_str(ctx->fields[ni].value, L"", "不勾逐段时多行键整体查不到（不命中）");

    /* 勾上逐段：每段各查一次，命中的结果按换行合并 */
    r->per_paragraph = 1;
    eval_run(ctx);
    check_str(ctx->fields[ni].value, L"张三\n李四", "逐段各查一次，命中结果按行合并");
    check_str(ctx->fields[pi].value, L"13800138000\n13900139000",
              "同一条规则的其它目标同样逐段合并");
    check(ctx->fields[ni].rule_driven == 1, "逐段命中的目标算规则推导");

    /* 空白段跳过、查不到的段跳过、CRLF 也认 */
    eval_set_user_value(ctx, hh, L"NFV-D-003\r\n\r\n  \r\nNFV-D-999\nNFV-D-002");
    eval_run(ctx);
    check_str(ctx->fields[ni].value, L"王五\n李四",
              "空白段与未命中的段跳过，CRLF 正常拆段");

    /* 一段都没命中 → 整条规则不命中，手动填的值留着 */
    eval_set_user_value(ctx, ni, L"手动联系人");
    eval_set_user_value(ctx, hh, L"X-1\nX-2");
    eval_run(ctx);
    check_str(ctx->fields[ni].value, L"手动联系人",
              "所有段都查不到时不命中，手动填的值保留");

    /* 只有一段时，与不勾逐段的结果完全一致 */
    eval_set_user_value(ctx, hh, L"NFV-D-001");
    eval_run(ctx);
    check_str(ctx->fields[ni].value, L"张三", "只有一段时与不勾逐段结果一致");

    /* 键的某个段落本身带前后空白：按表的比对口径照样命中 */
    eval_set_user_value(ctx, hh, L"  NFV-D-002  \nNFV-D-001");
    eval_run(ctx);
    check_str(ctx->fields[ni].value, L"李四\n张三", "段内首尾空白不影响命中");

    r->paragraph_inline=1;
    eval_run(ctx);
    check_str(ctx->fields[ni].value,L"李四张三","逐段查表连续连接");
    check_str(ctx->fields[pi].value,L"1390013900013800138000","其它查表目标同样连续连接");
    qp_strlcpy(r->paragraph_separator,L" / ",QP_SEP_MAX);
    eval_set_user_value(ctx,hh,L"NFV-D-001\r\nmissing\r\nNFV-D-002");
    eval_run(ctx);
    check_str(ctx->fields[ni].value,L"张三 / 李四","连续查表跳过未命中且保留分隔符空格");
    check_str(ctx->fields[pi].value,L"13800138000 / 13900139000","自定义分隔符用于所有查表目标");

    eval_destroy(ctx);
    tmpl_free(t);
}

/* ================================================================== */
/* ⑨ 体检：查表填出来的变量不算「可穷举」；查表死规则要报出来          */
/* ================================================================== */

/* 体检的两处毛病：
   · 误判：只由查表填值的变量被当成「只有默认值和空串」，引用它的条件被
     冤枉成「永远不成立」；
   · 漏判：配了输出列却一个目标变量都没选（或固定值为空的键），这类规则
     永远不会填写任何东西，体检却一声不吭。 */
static void test_lint_lookup_fill(void)
{
    DataTableRef *ref = register_table(L"值班名单", ROSTER);
    Template *t;
    Segment *key, *nm, *ph, *host, *lvl, *say;
    ParseRule *r;
    ConditionRule *cr;
    LintIssue *is;
    int n;

    printf("\n== 体检：查表填充的变量 / 查表死规则 ==\n");
    check(ref != NULL, "名单表登记成功");
    if (!ref) return;
    is = (LintIssue *)calloc(QP_LINT_MAX, sizeof(LintIssue));
    if (!is) { check(0, "体检结果缓冲没申请到"); return; }

    t = mk_tmpl(&key, &nm, &ph, &host);
    r = add_lookup(host, ref->id, L"设备编号", nm, L"联系人", ph, L"电话");
    check(r != NULL, "查表规则挂上了");
    if (!r) { free(is); tmpl_free(t); return; }

    /* 一个隐藏变量，只由查表填值；再拿它写一条条件规则 */
    lvl = seg_create(SEG_TEXT);
    qp_strlcpy(lvl->label, L"联系人副本", QP_LABEL_MAX);
    lvl->hidden = 1;
    seg_list_append(&t->segments, lvl);
    say = seg_create(SEG_STATIC);
    qp_strlcpy(say->content, L"联系人：{联系人副本}", QP_CONTENT_MAX);
    seg_list_append(&t->segments, say);
    {
        LookupOut *o = lookup_out_create();
        qp_strlcpy(o->col, L"联系人", QP_LABEL_MAX);
        o->col_index = -1;
        qp_strlcpy(o->target_segment_id, lvl->id, QP_ID_MAX);
        lookup_out_append(&r->lk_outs, o);
    }
    cr = cond_rule_create();
    cond_rule_set_simple(cr, lvl->id, OP_EQUALS, 0, L"张三");
    qp_strlcpy(cr->result_value, L"命中", QP_RESULT_MAX);
    cond_rule_append(&say->condition_rules, cr);

    n = tmpl_lint(t, is, QP_LINT_MAX);
    check(!lint_has(is, n, LINT_COND_NEVER, L"永远不成立"),
          "只由查表填值的变量不被当成可穷举，条件不被冤枉");

    /* 输出列一个都没选目标变量 → 查到了也填不出去，要报 */
    {
        LookupOut *o;
        for (o = r->lk_outs; o; o = o->next) o->target_segment_id[0] = L'\0';
        n = tmpl_lint(t, is, QP_LINT_MAX);
        check(lint_has(is, n, LINT_LOOKUP_SETUP, L"都没有选择目标变量"),
              "输出列全都没选目标变量：体检报「查到了也填不出去」");
        for (o = r->lk_outs; o; o = o->next)
            qp_strlcpy(o->target_segment_id, lvl->id, QP_ID_MAX);
    }

    /* 取值列没选 → 这一项永远不会被填写，要报 */
    lookup_out_at(r->lk_outs, 2)->col[0] = L'\0';
    lookup_out_at(r->lk_outs, 2)->col_index = -1;
    n = tmpl_lint(t, is, QP_LINT_MAX);
    check(lint_has(is, n, LINT_LOOKUP_COL, L"尚未选择取值列"),
          "取值列没选：体检报「这一项不会被填写」");
    qp_strlcpy(lookup_out_at(r->lk_outs, 2)->col, L"联系人", QP_LABEL_MAX);
    lookup_out_at(r->lk_outs, 2)->col_index = -1;

    /* 键来源选「固定值」却是空的：空键一律不命中，要报 */
    r->lk_key_mode = LOOKUP_KEY_FIX;
    r->lk_key_value[0] = L'\0';
    n = tmpl_lint(t, is, QP_LINT_MAX);
    check(lint_has(is, n, LINT_LOOKUP_SETUP, L"固定值是空的"),
          "固定值为空：体检报这条规则不会生效");
    r->lk_key_mode = LOOKUP_KEY_HOST;

    /* 键来源选「另一个变量」却没选变量：措辞要说「尚未选择」而不是「已删除」 */
    r->lk_key_mode = LOOKUP_KEY_VAR;
    r->lk_key_var_id[0] = L'\0';
    n = tmpl_lint(t, is, QP_LINT_MAX);
    check(lint_has(is, n, LINT_LOOKUP_SETUP, L"尚未选择"),
          "键变量没选：体检说「尚未选择」");
    r->lk_key_mode = LOOKUP_KEY_HOST;

    /* 恢复成一条健康规则后，不该再有任何「不会生效」类的体检项
       （键列重复只是提醒，不属于这一类） */
    n = tmpl_lint(t, is, QP_LINT_MAX);
    check(!lint_has(is, n, LINT_LOOKUP_SETUP, NULL) &&
          !lint_has(is, n, LINT_LOOKUP_COL, NULL) &&
          !lint_has(is, n, LINT_LOOKUP_TARGET, NULL) &&
          !lint_has(is, n, LINT_COND_NEVER, NULL),
          "配置齐全的查表模板不再有「不会生效」类体检项");

    free(is);
    tmpl_free(t);
}

/* 查表没命中时，目标那一处要在文本预览里标出「该有值却空着」——
   与其它解析规则同一表现（引擎里 field_is_parse_target 得把 lk_outs 也算上）。 */
static void test_miss_mark(void)
{
    DataTableRef *ref = register_table(L"值班名单", ROSTER);
    Segment *key, *nm, *ph, *host;
    Template *t;
    EvalContext *ctx;
    wchar_t out[QP_OUTPUT_MAX];
    int marks[64], nmark = 0, hh;

    printf("\n== 查表没命中：文本预览标出「该有值却空着」 ==\n");
    check(ref != NULL, "名单表登记成功");
    if (!ref) return;
    t = mk_tmpl(&key, &nm, &ph, &host);
    add_lookup(host, ref->id, L"设备编号", nm, L"联系人", ph, L"电话");
    ctx = eval_create(t);
    check(ctx != NULL, "求值上下文建起来了");
    if (!ctx) { tmpl_free(t); return; }
    hh = eval_index_of_id(ctx, host->id);
    eval_set_user_value(ctx, hh, L"NFV-D-999");       /* 表里没有这个键 */
    eval_run(ctx);
    eval_render_text_marked(ctx, out, QP_OUTPUT_MAX, marks, 64, &nmark);
    check(nmark >= 2, "查表没命中：两个目标位置都被标了出来");
    eval_destroy(ctx);
    tmpl_free(t);
}

static void paragraph_blank_preserves_position(void)
{
    const wchar_t *file=L"/tmp/qp_lookup_blank_position.csv";
    assert(storage_write_text(file,"key,value\nA,\nB,bee\n"));
    DataTableRef *ref=dtref_create(L"Blank",file);
    ref->encoding=QP_ENC_UTF8;ref->delimiter=QP_DELIM_COMMA;ref->has_header=1;
    qp_table_set_refs(ref);
    Template *t=tmpl_create(L"Blank");Segment *source=seg_create(SEG_TEXT),*target=seg_create(SEG_TEXT);
    seg_list_append(&t->segments,source);seg_list_append(&t->segments,target);
    wcscpy(target->default_value,L"fallback");
    ParseRule *r=parse_rule_create();r->type=PARSE_LOOKUP;r->per_paragraph=1;
    wcscpy(r->lk_table_id,ref->id);r->lk_key_col_index=0;
    r->lk_outs=lookup_out_create();r->lk_outs->col_index=1;wcscpy(r->lk_outs->target_segment_id,target->id);
    source->parse_rules=r;
    EvalContext *ctx=eval_create(t);assert(ctx);
    eval_set_user_value(ctx,0,L"A");eval_run(ctx);
    assert(ctx->fields[1].rule_driven && !ctx->fields[1].value[0]);
    eval_set_user_value(ctx,0,L"A\nB");eval_run(ctx);assert(!wcscmp(ctx->fields[1].value,L"\nbee"));
    eval_set_user_value(ctx,0,L"B\nA");eval_run(ctx);assert(!wcscmp(ctx->fields[1].value,L"bee\n"));
    eval_set_user_value(ctx,0,L"MISSING\nA\nB");eval_run(ctx);assert(!wcscmp(ctx->fields[1].value,L"\nbee"));
    eval_destroy(ctx);tmpl_free(t);qp_table_invalidate_all();qp_table_set_refs(NULL);dtref_free(ref);
    remove("/tmp/qp_lookup_blank_position.csv");
}

int main(void)
{
    setlocale(LC_ALL, "");
    printf("== 查表匹配（vlookup 口径）自测 ==\n");
    init_paths();

    test_model();
    test_eval_basic();
    test_eval_key_sources();
    test_eval_hit_modes();
    test_eval_priority();
    test_batch();
    test_per_paragraph();
    test_miss_mark();
    test_eval_broken_setup();
    test_storage();
    test_import();
    test_storage_defaults();
    test_lint();
    test_lint_lookup_fill();
    test_summary();
    paragraph_blank_preserves_position();

    teardown();
    printf("\n=========================\n通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
