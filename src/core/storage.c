/*
 * storage.c —— cJSON 序列化 + 文件读写 + 导入导出 + 设置
 *
 * 除路径解析与文件打开外，其余逻辑为纯 C，可在非 Windows 平台编译以便自测
 * （见 tests/test_storage.c）。
 */
#include "storage.h"
#include "table.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <shlobj.h>
#  include <io.h>
#else
#  include <unistd.h>
#  include <fcntl.h>
#endif

/* ================================================================== */
/* UTF-8 <-> UTF-16 转换                                               */
/* ================================================================== */


wchar_t *qp_utf8_to_wide_alloc(const char *src)
{
    size_t need;
    wchar_t *buf;
    if (!src) src = "";
    need = qp_utf8_to_wide(src, NULL, 0);
    buf = (wchar_t *)malloc((need + 1) * sizeof(wchar_t));
    if (!buf) return NULL;
    qp_utf8_to_wide(src, buf, need + 1);
    return buf;
}

char *qp_wide_to_utf8_alloc(const wchar_t *src)
{
    size_t need;
    char *buf;
    if (!src) src = L"";
    need = qp_wide_to_utf8(src, NULL, 0);
    buf = (char *)malloc(need + 1);
    if (!buf) return NULL;
    qp_wide_to_utf8(src, buf, need + 1);
    return buf;
}

/* ================================================================== */
/* cJSON 便捷函数                                                      */
/* ================================================================== */

static int json_add_w(cJSON *obj, const char *key, const wchar_t *value)
{
    char *u8 = qp_wide_to_utf8_alloc(value);
    int ok;
    if (!u8) return 0;
    ok = cJSON_AddStringToObject(obj, key, u8) != NULL;
    free(u8);
    return ok;
}

/* Consumes child on success or failure: callers cannot leak an unattached node. */
static int json_array_take(cJSON *array, cJSON *child)
{
    if (array && child && cJSON_AddItemToArray(array, child)) return 1;
    cJSON_Delete(child);
    return 0;
}

static int json_array_add_w(cJSON *array, const wchar_t *value)
{
    char *u8 = qp_wide_to_utf8_alloc(value);
    cJSON *child;
    if (!u8) return 0;
    child = cJSON_CreateString(u8);
    free(u8);
    return json_array_take(array, child);
}

#define JSON_REQUIRE(expression) do { if (!(expression)) goto fail; } while (0)

/* 从 JSON 取字符串到定长 wchar_t 缓冲区 */
static void json_get_w(const cJSON *obj, const char *key, wchar_t *out, size_t cap)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (out && cap) out[0] = L'\0';
    if (!cJSON_IsString(it) || !it->valuestring) return;
    qp_utf8_to_wide(it->valuestring, out, cap);
}

static int json_get_int(const cJSON *obj, const char *key, int fallback)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(it)) return it->valueint;
    if (cJSON_IsBool(it)) return cJSON_IsTrue(it) ? 1 : 0;
    return fallback;
}

static const char *json_get_str(const cJSON *obj, const char *key, const char *fallback)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(it) && it->valuestring) return it->valuestring;
    return fallback;
}

static int collection_valid(const cJSON *root, const char *key);
static int string_collection_valid(const cJSON *root, const char *key);

/* ================================================================== */
/* 枚举 <-> 字符串                                                     */
/* ================================================================== */

static const char *seg_type_key(SegmentType t)
{
    switch (t) {
    case SEG_TEXT:         return "text";
    case SEG_SELECT:       return "select";
    case SEG_MULTI_SELECT: return "multi-select";
    case SEG_DATETIME:     return "datetime";
    case SEG_STATIC:
    default:               return "static";
    }
}

static SegmentType seg_type_from_key(const char *k)
{
    if (!k) return SEG_STATIC;
    if (strcmp(k, "text") == 0) return SEG_TEXT;
    if (strcmp(k, "select") == 0) return SEG_SELECT;
    if (strcmp(k, "multi-select") == 0 || strcmp(k, "multiSelect") == 0) return SEG_MULTI_SELECT;
    if (strcmp(k, "datetime") == 0) return SEG_DATETIME;
    return SEG_STATIC;
}

static const char *ref_mode_key(RefMode m)
{
    switch (m) {
    case REF_SPLIT:  return "split";
    case REF_SUBSTR: return "substr";
    case REF_WHOLE:
    default:         return "whole";
    }
}

static RefMode ref_mode_from_key(const char *k)
{
    if (!k) return REF_WHOLE;
    if (strcmp(k, "split") == 0) return REF_SPLIT;
    if (strcmp(k, "substr") == 0) return REF_SUBSTR;
    return REF_WHOLE;
}

static const char *parse_type_key(ParseRuleType t)
{
    switch (t) {
    case PARSE_CONTAINS: return "contains";
    case PARSE_BETWEEN:  return "between";
    case PARSE_REGEX:    return "regex";
    case PARSE_LOOKUP:   return "lookup";
    default:             return "split";
    }
}

static ParseRuleType parse_type_from_key(const char *k)
{
    if (k && strcmp(k, "contains") == 0) return PARSE_CONTAINS;
    if (k && strcmp(k, "between") == 0)  return PARSE_BETWEEN;
    if (k && strcmp(k, "regex") == 0)    return PARSE_REGEX;
    if (k && strcmp(k, "lookup") == 0)   return PARSE_LOOKUP;
    return PARSE_SPLIT;
}

/* 查表规则里的键来源：host（本变量）/ var（另一个变量）/ fix（固定值） */
static const char *lookup_keymode_key(int m)
{
    switch (m) {
    case LOOKUP_KEY_VAR: return "var";
    case LOOKUP_KEY_FIX: return "fix";
    default:             return "host";
    }
}

static int lookup_keymode_from_key(const char *k)
{
    if (!k) return LOOKUP_KEY_HOST;
    if (strcmp(k, "var") == 0) return LOOKUP_KEY_VAR;
    if (strcmp(k, "fix") == 0) return LOOKUP_KEY_FIX;
    return LOOKUP_KEY_HOST;
}

static const char *cond_op_key(CondOperator op)
{
    switch (op) {
    case OP_EQUALS:      return "equals";
    case OP_STARTS_WITH: return "startsWith";
    case OP_ENDS_WITH:   return "endsWith";
    case OP_IN:          return "in";
    case OP_EMPTY:       return "empty";
    case OP_CONTAINS:
    default:             return "contains";
    }
}

static CondOperator cond_op_from_key(const char *k)
{
    if (!k) return OP_CONTAINS;
    if (strcmp(k, "equals") == 0) return OP_EQUALS;
    if (strcmp(k, "startsWith") == 0) return OP_STARTS_WITH;
    if (strcmp(k, "endsWith") == 0) return OP_ENDS_WITH;
    if (strcmp(k, "in") == 0) return OP_IN;
    if (strcmp(k, "empty") == 0) return OP_EMPTY;
    return OP_CONTAINS;
}

static const char *theme_key(ThemeMode t)
{
    switch (t) {
    case THEME_DARK:   return "dark";
    case THEME_SYSTEM: return "system";
    case THEME_LIGHT:
    default:           return "light";
    }
}

static ThemeMode theme_from_key(const char *k)
{
    if (!k) return THEME_SYSTEM;
    if (strcmp(k, "dark") == 0) return THEME_DARK;
    if (strcmp(k, "light") == 0) return THEME_LIGHT;
    return THEME_SYSTEM;
}

/* ================================================================== */
/* AppData -> JSON                                                     */
/* ================================================================== */

static cJSON *parse_rule_to_json(const ParseRule *r)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    JSON_REQUIRE(json_add_w(o, "id", r->id));
    JSON_REQUIRE(cJSON_AddStringToObject(o, "type", parse_type_key(r->type)));
    JSON_REQUIRE(json_add_w(o, "delimiter", r->delimiter));
    JSON_REQUIRE(cJSON_AddNumberToObject(o, "segmentIndex", r->segment_index));
    JSON_REQUIRE(json_add_w(o, "keyword", r->keyword));
    JSON_REQUIRE(json_add_w(o, "markerAfter", r->marker_after));
    /* 正则模式按需写出：旧版本读到未知 type 会退回 split，不会报错 */
    if (r->pattern[0]) JSON_REQUIRE(json_add_w(o, "pattern", r->pattern));
    if (r->capture_group > 0) JSON_REQUIRE(cJSON_AddNumberToObject(o, "captureGroup", r->capture_group));
    if (r->per_paragraph) JSON_REQUIRE(cJSON_AddBoolToObject(o, "perParagraph", 1));
    if (r->paragraph_inline) {
        JSON_REQUIRE(cJSON_AddBoolToObject(o, "paragraphInline", 1));
    }
    if (r->paragraph_separator[0]) JSON_REQUIRE(json_add_w(o, "paragraphSeparator", r->paragraph_separator));
    JSON_REQUIRE(json_add_w(o, "targetSegmentId", r->target_segment_id));
    JSON_REQUIRE(json_add_w(o, "fillValue", r->fill_value));
    if (r->type == PARSE_LOOKUP) {
        const LookupOut *lo;
        cJSON *arr;
        JSON_REQUIRE(cJSON_AddStringToObject(o, "lkKeyMode", lookup_keymode_key(r->lk_key_mode)));
        if (r->lk_key_mode == LOOKUP_KEY_VAR) JSON_REQUIRE(json_add_w(o, "lkKeyVarId", r->lk_key_var_id));
        if (r->lk_key_mode == LOOKUP_KEY_FIX) JSON_REQUIRE(json_add_w(o, "lkKeyValue", r->lk_key_value));
        JSON_REQUIRE(json_add_w(o, "tableId", r->lk_table_id));
        JSON_REQUIRE(json_add_w(o, "keyCol", r->lk_key_col));
        JSON_REQUIRE(cJSON_AddNumberToObject(o, "keyColIndex", r->lk_key_col_index));
        JSON_REQUIRE(cJSON_AddStringToObject(o, "hit", qp_table_hit_key(r->lk_hit)));
        if (r->lk_sep[0] && wcscmp(r->lk_sep, L"、") != 0) JSON_REQUIRE(json_add_w(o, "sep", r->lk_sep));
        if (r->lk_unique) JSON_REQUIRE(cJSON_AddBoolToObject(o, "unique", 1));
        JSON_REQUIRE((arr = cJSON_AddArrayToObject(o, "outputs")));
        for (lo = r->lk_outs; arr && lo; lo = lo->next) {
            cJSON *e = cJSON_CreateObject();
            JSON_REQUIRE(json_array_take(arr, e));
            JSON_REQUIRE(json_add_w(e, "col", lo->col));
            JSON_REQUIRE(cJSON_AddNumberToObject(e, "colIndex", lo->col_index));
            JSON_REQUIRE(json_add_w(e, "targetSegmentId", lo->target_segment_id));
        }
    }
    return o;
fail:
    cJSON_Delete(o);
    return NULL;
}

static int cond_term_fill_json(cJSON *o, const CondTerm *t)
{
    JSON_REQUIRE(json_add_w(o, "triggerSegmentId", t->trigger_segment_id));
    JSON_REQUIRE(cJSON_AddStringToObject(o, "operator", cond_op_key(t->op)));
    JSON_REQUIRE(json_add_w(o, "matchValue", t->match_value));
    if (t->op == OP_IN && t->match_value_count > 0) {
        cJSON *arr;
        int i;
        JSON_REQUIRE((arr = cJSON_AddArrayToObject(o, "matchValues")));
        for (i = 0; arr && i < t->match_value_count; ++i) {
            JSON_REQUIRE(json_array_add_w(arr, t->match_values[i]));
        }
    }
    return 1;
fail:
    return 0;
}

static cJSON *cond_term_to_json(const CondTerm *t)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    JSON_REQUIRE(cond_term_fill_json(o, t));
    if (t->negate) JSON_REQUIRE(cJSON_AddBoolToObject(o, "negate", 1));
    return o;
fail:
    cJSON_Delete(o);
    return NULL;
}

static cJSON *cond_rule_to_json(const ConditionRule *r)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *arr;
    const CondTerm *t;
    if (!o) return NULL;
    JSON_REQUIRE(json_add_w(o, "id", r->id));
    JSON_REQUIRE(cJSON_AddStringToObject(o, "join", r->join == COND_JOIN_ANY ? "any" : "all"));
    JSON_REQUIRE((arr = cJSON_AddArrayToObject(o, "conditions")));
    for (t = r->terms; arr && t; t = t->next)
        JSON_REQUIRE(json_array_take(arr, cond_term_to_json(t)));
    /* 单条件且未取反时，同时写出老字段，旧版本仍能读懂这条规则 */
    if (r->terms && !r->terms->next && !r->terms->negate)
        JSON_REQUIRE(cond_term_fill_json(o, r->terms));
    JSON_REQUIRE(json_add_w(o, "resultValue", r->result_value));
    return o;
fail:
    cJSON_Delete(o);
    return NULL;
}

static cJSON *segment_to_json(const Segment *s)
{
    cJSON *o = cJSON_CreateObject();
    const ParseRule *pr;
    const ConditionRule *cr;
    if (!o) return NULL;
    JSON_REQUIRE(json_add_w(o, "id", s->id));
    JSON_REQUIRE(cJSON_AddStringToObject(o, "type", seg_type_key(s->type)));
    if (s->type == SEG_STATIC) {
        JSON_REQUIRE(json_add_w(o, "content", s->content));
    } else {
        JSON_REQUIRE(json_add_w(o, "label", s->label));
        JSON_REQUIRE(json_add_w(o, "placeholder", s->placeholder));
        JSON_REQUIRE(json_add_w(o, "defaultValue", s->default_value));
        if (s->type == SEG_SELECT || s->type == SEG_MULTI_SELECT) {
            cJSON *arr;
            int i;
            JSON_REQUIRE((arr = cJSON_AddArrayToObject(o, "options")));
            for (i = 0; arr && i < s->option_count; ++i) {
                JSON_REQUIRE(json_array_add_w(arr, s->options[i]));
            }
            if(qp_choice_custom_index(s)>=0)
                JSON_REQUIRE(cJSON_AddNumberToObject(o,"customOptionIndex",s->custom_option-1));
        }
        if (s->type == SEG_MULTI_SELECT) JSON_REQUIRE(json_add_w(o, "separator", s->separator));
        if (s->hidden) JSON_REQUIRE(cJSON_AddBoolToObject(o, "hidden", 1));
        if (s->no_output) JSON_REQUIRE(cJSON_AddBoolToObject(o, "noOutput", 1));
        if (s->type == SEG_TEXT && s->multiline) JSON_REQUIRE(cJSON_AddBoolToObject(o, "multiline", 1));
        if (s->type == SEG_DATETIME) JSON_REQUIRE(json_add_w(o, "timeFormat", s->time_format));
        /* 引用：不引用时整组字段都不写，存档保持干净 */
        if (s->ref_segment_id[0]) {
            JSON_REQUIRE(json_add_w(o, "refSegmentId", s->ref_segment_id));
            JSON_REQUIRE(cJSON_AddStringToObject(o, "refMode", ref_mode_key(s->ref_mode)));
            JSON_REQUIRE(json_add_w(o, "refDelimiter", s->ref_delimiter));
            JSON_REQUIRE(cJSON_AddNumberToObject(o, "refIndex", s->ref_index));
            JSON_REQUIRE(cJSON_AddNumberToObject(o, "refStart", s->ref_start));
            JSON_REQUIRE(cJSON_AddNumberToObject(o, "refLen", s->ref_len));
        }
    }
    if (s->parse_rules) {
        cJSON *arr;
        JSON_REQUIRE((arr = cJSON_AddArrayToObject(o, "parseRules")));
        for (pr = s->parse_rules; arr && pr; pr = pr->next)
            JSON_REQUIRE(json_array_take(arr, parse_rule_to_json(pr)));
    }
    if (s->condition_rules) {
        cJSON *arr;
        JSON_REQUIRE((arr = cJSON_AddArrayToObject(o, "conditionRules")));
        for (cr = s->condition_rules; arr && cr; cr = cr->next)
            JSON_REQUIRE(json_array_take(arr, cond_rule_to_json(cr)));
    }
    return o;
fail:
    cJSON_Delete(o);
    return NULL;
}

static cJSON *template_to_json(const Template *t)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *arr;
    const Segment *s;
    if (!o) return NULL;
    JSON_REQUIRE(json_add_w(o, "id", t->id));
    JSON_REQUIRE(json_add_w(o, "name", t->name));
    JSON_REQUIRE(json_add_w(o, "groupId", t->group_id));
    if (t->batch_mode) {
        JSON_REQUIRE(cJSON_AddBoolToObject(o, "batchMode", 1));
        JSON_REQUIRE(json_add_w(o, "batchSourceId", t->batch_source_id));
    }
    if (t->table_output_enabled)
        JSON_REQUIRE(cJSON_AddBoolToObject(o, "tableOutputEnabled", 1));
    if (t->table_output_format[0])
        JSON_REQUIRE(json_add_w(o, "tableOutputFormat", t->table_output_format));
    JSON_REQUIRE(json_add_w(o, "createdAt", t->created_at));
    JSON_REQUIRE(json_add_w(o, "updatedAt", t->updated_at));
    JSON_REQUIRE((arr = cJSON_AddArrayToObject(o, "segments")));
    for (s = t->segments; arr && s; s = s->next)
        JSON_REQUIRE(json_array_take(arr, segment_to_json(s)));
    return o;
fail:
    cJSON_Delete(o);
    return NULL;
}

static cJSON *appdata_to_cjson(const AppData *d)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr;
    const Group *g;
    const Template *t;
    const DataTableRef *ref;
    if (!root) return NULL;
    JSON_REQUIRE(cJSON_AddNumberToObject(root, "version", 1));
    JSON_REQUIRE(cJSON_AddStringToObject(root, "app", "QuickPhrase"));
    JSON_REQUIRE((arr = cJSON_AddArrayToObject(root, "groups")));
    for (g = d ? d->groups : NULL; arr && g; g = g->next) {
        cJSON *o = cJSON_CreateObject();
        JSON_REQUIRE(json_array_take(arr, o));
        JSON_REQUIRE(json_add_w(o, "id", g->id));
        JSON_REQUIRE(json_add_w(o, "name", g->name));
        JSON_REQUIRE(cJSON_AddNumberToObject(o, "order", g->order));
    }
    JSON_REQUIRE((arr = cJSON_AddArrayToObject(root, "templates")));
    for (t = d ? d->templates : NULL; arr && t; t = t->next)
        JSON_REQUIRE(json_array_take(arr, template_to_json(t)));
    /* 数据表：只存「引用」，不存内容；每周覆盖磁盘上同一个文件即可 */
    JSON_REQUIRE((arr = cJSON_AddArrayToObject(root, "dataTables")));
    for (ref = d ? d->tables : NULL; arr && ref; ref = ref->next) {
        cJSON *o = cJSON_CreateObject();
        JSON_REQUIRE(json_array_take(arr, o));
        JSON_REQUIRE(json_add_w(o, "id", ref->id));
        JSON_REQUIRE(json_add_w(o, "name", ref->name));
        JSON_REQUIRE(json_add_w(o, "path", ref->path));
        JSON_REQUIRE(cJSON_AddStringToObject(o, "encoding", qp_table_encoding_key(ref->encoding)));
        JSON_REQUIRE(cJSON_AddStringToObject(o, "delimiter", qp_table_delim_key(ref->delimiter)));
        JSON_REQUIRE(cJSON_AddBoolToObject(o, "hasHeader", ref->has_header ? 1 : 0));
    }
    return root;
fail:
    cJSON_Delete(root);
    return NULL;
}

char *appdata_to_json(const AppData *d)
{
    cJSON *root = appdata_to_cjson(d);
    char *text;
    if (!root) return NULL;
    text = cJSON_Print(root);
    cJSON_Delete(root);
    return text;
}

/* ================================================================== */
/* JSON -> AppData                                                     */
/* ================================================================== */

static ParseRule *parse_rule_from_json(const cJSON *o)
{
    ParseRule *r = parse_rule_create();
    if (!r) return NULL;
    if (cJSON_GetObjectItemCaseSensitive(o, "id"))
        json_get_w(o, "id", r->id, QP_ID_MAX);
    if (!r->id[0]) qp_new_id(r->id, QP_ID_MAX, L"pr");
    r->type = parse_type_from_key(json_get_str(o, "type", "split"));
    json_get_w(o, "delimiter", r->delimiter, QP_SEP_MAX);
    r->segment_index = json_get_int(o, "segmentIndex", 1);
    if (r->segment_index < 1) r->segment_index = 1;
    json_get_w(o, "keyword", r->keyword, QP_KEYWORD_MAX);
    json_get_w(o, "markerAfter", r->marker_after, QP_KEYWORD_MAX);
    json_get_w(o, "pattern", r->pattern, QP_PATTERN_MAX);
    r->capture_group = json_get_int(o, "captureGroup", 0);
    if (r->capture_group < 0) r->capture_group = 0;
    r->per_paragraph = json_get_int(o, "perParagraph", 0) ? 1 : 0;
    r->paragraph_inline = json_get_int(o, "paragraphInline", 0) ? 1 : 0;
    json_get_w(o, "paragraphSeparator", r->paragraph_separator, QP_SEP_MAX);
    json_get_w(o, "targetSegmentId", r->target_segment_id, QP_ID_MAX);
    json_get_w(o, "fillValue", r->fill_value, QP_VALUE_MAX);
    if (r->type == PARSE_LOOKUP) {
        const cJSON *arr, *it;
        if (!collection_valid(o, "outputs")) { parse_rule_free(r); return NULL; }
        r->lk_key_mode = lookup_keymode_from_key(json_get_str(o, "lkKeyMode", "host"));
        json_get_w(o, "lkKeyVarId", r->lk_key_var_id, QP_ID_MAX);
        json_get_w(o, "lkKeyValue", r->lk_key_value, QP_MATCH_MAX);
        json_get_w(o, "tableId", r->lk_table_id, QP_ID_MAX);
        json_get_w(o, "keyCol", r->lk_key_col, QP_LABEL_MAX);
        r->lk_key_col_index = json_get_int(o, "keyColIndex", -1);
        if (r->lk_key_col_index < -1) r->lk_key_col_index = -1;
        r->lk_hit = qp_table_hit_from_key(json_get_str(o, "hit", "first"));
        json_get_w(o, "sep", r->lk_sep, QP_SEP_MAX);
        if (!r->lk_sep[0]) qp_strlcpy(r->lk_sep, L"、", QP_SEP_MAX);
        r->lk_unique = json_get_int(o, "unique", 0) ? 1 : 0;
        arr = cJSON_GetObjectItemCaseSensitive(o, "outputs");
        if (cJSON_IsArray(arr)) {
            cJSON_ArrayForEach(it, arr) {
                LookupOut *lo;
                if (!cJSON_IsObject(it)) continue;
                lo = lookup_out_create();
                if (!lo) { parse_rule_free(r); return NULL; }
                json_get_w(it, "col", lo->col, QP_LABEL_MAX);
                lo->col_index = json_get_int(it, "colIndex", -1);
                if (lo->col_index < -1) lo->col_index = -1;
                json_get_w(it, "targetSegmentId", lo->target_segment_id, QP_ID_MAX);
                lookup_out_append(&r->lk_outs, lo);
            }
        }
    }
    return r;
}

/* 读一个条件项的匹配值：优先 matchValues 数组，其次 matchValue 文本 */
static int cond_term_read_match(CondTerm *t, const cJSON *o)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(o, "matchValues");
    wchar_t raw[QP_MATCH_MAX];
    raw[0] = L'\0';
    if (cJSON_IsArray(arr) && cJSON_GetArraySize(arr) > 0) {
        /* Explicit values are a list, not a comma-encoded string. Preserve
           commas and long members; the bounded raw field is presentation. */
        const cJSON *it;
        int index = 0;
        t->match_value_count = cJSON_GetArraySize(arr);
        t->match_values = (wchar_t **)calloc((size_t)t->match_value_count, sizeof(wchar_t *));
        if (!t->match_values) { t->match_value_count = 0; return 0; }
        cJSON_ArrayForEach(it, arr) {
            wchar_t *w;
            w = qp_utf8_to_wide_alloc(it->valuestring);
            if (!w) return 0;
            qp_trim(w);
            t->match_values[index++] = w;
            if (raw[0]) wcsncat(raw, L",", QP_MATCH_MAX - wcslen(raw) - 1);
            wcsncat(raw, w, QP_MATCH_MAX - wcslen(raw) - 1);
        }
        if (cJSON_IsString(cJSON_GetObjectItemCaseSensitive(o, "matchValue")))
            json_get_w(o, "matchValue", raw, QP_MATCH_MAX);
        qp_strlcpy(t->match_value, raw, QP_MATCH_MAX);
        return 1;
    } else {
        json_get_w(o, "matchValue", raw, QP_MATCH_MAX);
    }
    return qp_cond_term_set_match(t, raw);
}

static CondTerm *cond_term_from_json(const cJSON *o)
{
    CondTerm *t = cond_term_create();
    if (!t) return NULL;
    if (!string_collection_valid(o, "matchValues")) { cond_term_free(t); return NULL; }
    json_get_w(o, "triggerSegmentId", t->trigger_segment_id, QP_ID_MAX);
    t->op = cond_op_from_key(json_get_str(o, "operator", "contains"));
    t->negate = json_get_int(o, "negate", 0) ? 1 : 0;
    if (!cond_term_read_match(t, o)) { cond_term_free(t); return NULL; }
    return t;
}

static ConditionRule *cond_rule_from_json(const cJSON *o)
{
    ConditionRule *r = cond_rule_create();
    const cJSON *arr;
    if (!r) return NULL;
    if (!collection_valid(o, "conditions")) { cond_rule_free(r); return NULL; }
    json_get_w(o, "id", r->id, QP_ID_MAX);
    if (!r->id[0]) qp_new_id(r->id, QP_ID_MAX, L"cr");
    r->join = (strcmp(json_get_str(o, "join", "all"), "any") == 0)
                ? COND_JOIN_ANY : COND_JOIN_ALL;

    /* cond_rule_create 预置了一个空白条件项，这里按文件内容重建 */
    cond_term_list_clear(&r->terms);
    arr = cJSON_GetObjectItemCaseSensitive(o, "conditions");
    if (cJSON_IsArray(arr) && cJSON_GetArraySize(arr) > 0) {
        const cJSON *it;
        cJSON_ArrayForEach(it, arr) {
            if (!cJSON_IsObject(it)) continue;
            CondTerm *term = cond_term_from_json(it);
            if (!term) { cond_rule_free(r); return NULL; }
            cond_term_append(&r->terms, term);
        }
    } else {
        /* 老格式：整条规则就是一个条件项 */
        CondTerm *term = cond_term_from_json(o);
        if (!term) { cond_rule_free(r); return NULL; }
        cond_term_append(&r->terms, term);
    }
    json_get_w(o, "resultValue", r->result_value, QP_RESULT_MAX);
    return r;
}

static Segment *segment_from_json(const cJSON *o)
{
    Segment *s;
    const cJSON *arr, *it;
    SegmentType type = seg_type_from_key(json_get_str(o, "type", "static"));
    s = seg_create(type);
    if (!s) return NULL;
    if (!string_collection_valid(o, "options") || !collection_valid(o, "parseRules") ||
        !collection_valid(o, "conditionRules")) { seg_free(s); return NULL; }
    json_get_w(o, "id", s->id, QP_ID_MAX);
    if (!s->id[0]) qp_new_id(s->id, QP_ID_MAX, L"seg");
    json_get_w(o, "label", s->label, QP_LABEL_MAX);
    json_get_w(o, "content", s->content, QP_CONTENT_MAX);
    json_get_w(o, "placeholder", s->placeholder, QP_PLACEHOLD_MAX);
    json_get_w(o, "defaultValue", s->default_value, QP_VALUE_MAX);
    s->hidden = (type != SEG_STATIC && json_get_int(o, "hidden", 0)) ? 1 : 0;
    s->no_output = (type != SEG_STATIC && json_get_int(o, "noOutput", 0)) ? 1 : 0;
    s->multiline = (type == SEG_TEXT && json_get_int(o, "multiline", 0)) ? 1 : 0;
    if (cJSON_GetObjectItemCaseSensitive(o, "separator")) {
        json_get_w(o, "separator", s->separator, QP_SEP_MAX);
        if (!s->separator[0]) qp_strlcpy(s->separator, L"、", QP_SEP_MAX);
    }
    if (type == SEG_DATETIME) {
        json_get_w(o, "timeFormat", s->time_format, QP_FORMAT_MAX);
        if (!s->time_format[0])
            qp_strlcpy(s->time_format, qp_time_format_at(0), QP_FORMAT_MAX);
    }
    /* 引用：固定片段没有「值」可言，读进来也一律丢掉 */
    if (type != SEG_STATIC) {
        json_get_w(o, "refSegmentId", s->ref_segment_id, QP_ID_MAX);
        s->ref_mode = ref_mode_from_key(json_get_str(o, "refMode", "whole"));
        json_get_w(o, "refDelimiter", s->ref_delimiter, QP_SEP_MAX);
        s->ref_index = json_get_int(o, "refIndex", 1);
        s->ref_start = json_get_int(o, "refStart", 1);
        s->ref_len = json_get_int(o, "refLen", 0);
        if (s->ref_index < 1) s->ref_index = 1;
        if (s->ref_start < 1) s->ref_start = 1;
        if (s->ref_len < 0) s->ref_len = 0;
    }

    arr = cJSON_GetObjectItemCaseSensitive(o, "options");
    if (cJSON_IsArray(arr)) {
        cJSON_ArrayForEach(it, arr) {
            if (cJSON_IsString(it) && it->valuestring) {
                wchar_t *w = qp_utf8_to_wide_alloc(it->valuestring);
                if (!w) { seg_free(s); return NULL; }
                int ok = qp_seg_add_option(s, w);
                free(w);
                if (!ok) { seg_free(s); return NULL; }
            }
        }
    }
    if(type==SEG_SELECT || type==SEG_MULTI_SELECT) {
        const cJSON *custom=cJSON_GetObjectItemCaseSensitive(o,"customOptionIndex");
        if(cJSON_IsNumber(custom) && custom->valuedouble==(double)custom->valueint &&
           custom->valueint>=0 && custom->valueint<s->option_count)s->custom_option=custom->valueint+1;
    }
    arr = cJSON_GetObjectItemCaseSensitive(o, "parseRules");
    if (cJSON_IsArray(arr)) {
        cJSON_ArrayForEach(it, arr) {
            if (cJSON_IsObject(it)) {
                ParseRule *rule = parse_rule_from_json(it);
                if (!rule) { seg_free(s); return NULL; }
                parse_rule_append(&s->parse_rules, rule);
            }
        }
    }
    arr = cJSON_GetObjectItemCaseSensitive(o, "conditionRules");
    if (cJSON_IsArray(arr)) {
        cJSON_ArrayForEach(it, arr) {
            if (cJSON_IsObject(it)) {
                ConditionRule *rule = cond_rule_from_json(it);
                if (!rule) { seg_free(s); return NULL; }
                cond_rule_append(&s->condition_rules, rule);
            }
        }
    }
    return s;
}

static Template *template_from_json(const cJSON *o)
{
    Template *t = tmpl_create(L"");
    const cJSON *arr, *it;
    if (!t) return NULL;
    if (!collection_valid(o, "segments")) { tmpl_free(t); return NULL; }
    json_get_w(o, "id", t->id, QP_ID_MAX);
    if (!t->id[0]) qp_new_id(t->id, QP_ID_MAX, L"tpl");
    json_get_w(o, "name", t->name, QP_NAME_MAX);
    if (!t->name[0]) qp_strlcpy(t->name, L"未命名模板", QP_NAME_MAX);
    json_get_w(o, "groupId", t->group_id, QP_ID_MAX);
    t->batch_mode = json_get_int(o, "batchMode", 0) ? 1 : 0;
    json_get_w(o, "batchSourceId", t->batch_source_id, QP_ID_MAX);
    t->table_output_enabled = json_get_int(o, "tableOutputEnabled", 0) ? 1 : 0;
    json_get_w(o, "tableOutputFormat", t->table_output_format, QP_OUTPUT_MAX);
    json_get_w(o, "createdAt", t->created_at, QP_TIME_MAX);
    json_get_w(o, "updatedAt", t->updated_at, QP_TIME_MAX);
    if (!t->created_at[0]) qp_now_iso(t->created_at, QP_TIME_MAX);
    if (!t->updated_at[0]) qp_strlcpy(t->updated_at, t->created_at, QP_TIME_MAX);
    arr = cJSON_GetObjectItemCaseSensitive(o, "segments");
    if (cJSON_IsArray(arr)) {
        cJSON_ArrayForEach(it, arr) {
            if (cJSON_IsObject(it)) {
                Segment *seg = segment_from_json(it);
                if (!seg) { tmpl_free(t); return NULL; }
                seg_list_append(&t->segments, seg);
            }
        }
    }
    return t;
}

static int array_valid(const cJSON *root, const char *key, int strings)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, key), *it;
    if (!arr) return 1; /* 旧版缺少可选集合仍兼容。 */
    if (!cJSON_IsArray(arr)) return 0;
    cJSON_ArrayForEach(it, arr)
        if (strings ? !cJSON_IsString(it) || !it->valuestring : !cJSON_IsObject(it)) return 0;
    return 1;
}

static int collection_valid(const cJSON *root, const char *key)
{ return array_valid(root, key, 0); }

static int string_collection_valid(const cJSON *root, const char *key)
{ return array_valid(root, key, 1); }

AppData *appdata_from_json(const char *utf8)
{
    cJSON *root;
    const cJSON *arr, *it;
    AppData *d;

    if (!utf8) return NULL;
    root = cJSON_ParseWithOpts(utf8, NULL, 1);
    if (!root || !cJSON_IsObject(root) || !collection_valid(root, "templates") ||
        !collection_valid(root, "groups") || !collection_valid(root, "dataTables")) {
        cJSON_Delete(root);
        return NULL;
    }
    d = appdata_create();
    if (!d) { cJSON_Delete(root); return NULL; }

    arr = cJSON_GetObjectItemCaseSensitive(root, "groups");
    if (cJSON_IsArray(arr)) {
        cJSON_ArrayForEach(it, arr) {
            Group *g;
            if (!cJSON_IsObject(it)) continue;
            g = group_create(L"");
            if (!g) goto fail;
            json_get_w(it, "id", g->id, QP_ID_MAX);
            if (!g->id[0]) qp_new_id(g->id, QP_ID_MAX, L"grp");
            json_get_w(it, "name", g->name, QP_NAME_MAX);
            if (!g->name[0]) qp_strlcpy(g->name, L"未命名分组", QP_NAME_MAX);
            g->order = json_get_int(it, "order", group_list_count(d->groups));
            group_list_append(&d->groups, g);
        }
    }
    arr = cJSON_GetObjectItemCaseSensitive(root, "templates");
    if (cJSON_IsArray(arr)) {
        cJSON_ArrayForEach(it, arr) {
            if (cJSON_IsObject(it)) {
                Template *tmpl = template_from_json(it);
                if (!tmpl) goto fail;
                tmpl_list_append(&d->templates, tmpl);
            }
        }
    }
    arr = cJSON_GetObjectItemCaseSensitive(root, "dataTables");
    if (cJSON_IsArray(arr)) {
        cJSON_ArrayForEach(it, arr) {
            DataTableRef *ref;
            if (!cJSON_IsObject(it)) continue;
            ref = dtref_create(L"", L"");
            if (!ref) goto fail;
            json_get_w(it, "id", ref->id, QP_ID_MAX);
            json_get_w(it, "name", ref->name, QP_NAME_MAX);
            json_get_w(it, "path", ref->path, QP_PATH_MAX);
            ref->encoding = qp_table_encoding_from_key(json_get_str(it, "encoding", "auto"));
            ref->delimiter = qp_table_delim_from_key(json_get_str(it, "delimiter", "auto"));
            ref->has_header = json_get_int(it, "hasHeader", 1) ? 1 : 0;
            /* 没有路径的引用是脏数据：留着只会让规则报「读不出来」 */
            if (!ref->path[0]) { dtref_free(ref); continue; }
            if (!ref->id[0]) qp_new_id(ref->id, QP_ID_MAX, L"tbl");
            if (!ref->name[0]) qp_strlcpy(ref->name, L"未命名数据表", QP_NAME_MAX);
            dtref_list_append(&d->tables, ref);
        }
    }
    cJSON_Delete(root);
    return d;
fail:
    cJSON_Delete(root);
    appdata_free(d);
    return NULL;
}

/* ================================================================== */
/* 设置序列化                                                          */
/* ================================================================== */

void settings_defaults(Settings *s)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->hotkey_mods = 0x0001;      /* MOD_ALT */
    s->hotkey_vk = 'Q';
    s->autostart = 0;
    s->theme = THEME_SYSTEM;
    qp_strlcpy(s->default_separator, L"、", QP_SEP_MAX);
    s->win_x = s->win_y = -1;     /* -1 = 居中 */
    s->win_w = 900;
    s->win_h = 620;
    s->win_maximized = 0;
    s->tree_width = 230;
    s->preview_height = 0;        /* 0 = 自动（按窗口高度取 1/4） */
    s->editor_w = s->editor_h = 0;         /* 0 = 用编辑器的设计尺寸 */
    s->editor_preview_height = 0;          /* 0 = 用编辑器的设计高度 */
    s->minimize_to_tray_hint = 0;
}

char *settings_to_json(const Settings *s)
{
    cJSON *root, *win;
    char *text;
    if (!s) return NULL;
    root = cJSON_CreateObject();
    if (!root) return NULL;
    JSON_REQUIRE(cJSON_AddNumberToObject(root, "version", 1));
    JSON_REQUIRE(cJSON_AddNumberToObject(root, "hotkeyMods", (double)s->hotkey_mods));
    JSON_REQUIRE(cJSON_AddNumberToObject(root, "hotkeyVk", (double)s->hotkey_vk));
    JSON_REQUIRE(cJSON_AddBoolToObject(root, "autostart", s->autostart ? 1 : 0));
    JSON_REQUIRE(cJSON_AddStringToObject(root, "theme", theme_key(s->theme)));
    JSON_REQUIRE(json_add_w(root, "defaultSeparator", s->default_separator));
    JSON_REQUIRE(cJSON_AddNumberToObject(root, "treeWidth", s->tree_width));
    JSON_REQUIRE(cJSON_AddNumberToObject(root, "previewHeight", s->preview_height));
    JSON_REQUIRE(cJSON_AddNumberToObject(root, "editorW", s->editor_w));
    JSON_REQUIRE(cJSON_AddNumberToObject(root, "editorH", s->editor_h));
    JSON_REQUIRE(cJSON_AddNumberToObject(root, "editorPreviewHeight", s->editor_preview_height));
    JSON_REQUIRE(cJSON_AddBoolToObject(root, "trayHintShown", s->minimize_to_tray_hint ? 1 : 0));
    JSON_REQUIRE(json_add_w(root, "dataDir", s->data_dir));
    JSON_REQUIRE((win = cJSON_AddObjectToObject(root, "window")));
    if (win) {
        JSON_REQUIRE(cJSON_AddNumberToObject(win, "x", s->win_x));
        JSON_REQUIRE(cJSON_AddNumberToObject(win, "y", s->win_y));
        JSON_REQUIRE(cJSON_AddNumberToObject(win, "w", s->win_w));
        JSON_REQUIRE(cJSON_AddNumberToObject(win, "h", s->win_h));
        JSON_REQUIRE(cJSON_AddBoolToObject(win, "maximized", s->win_maximized ? 1 : 0));
    }
    text = cJSON_Print(root);
    cJSON_Delete(root);
    return text;
fail:
    cJSON_Delete(root);
    return NULL;
}

int settings_from_json(const char *utf8, Settings *out)
{
    cJSON *root;
    const cJSON *win;
    if (!utf8 || !out) return 0;
    root = cJSON_ParseWithOpts(utf8, NULL, 1);
    if (!root || !cJSON_IsObject(root)) { cJSON_Delete(root); return 0; }
    settings_defaults(out);
    out->hotkey_mods = (unsigned)json_get_int(root, "hotkeyMods", (int)out->hotkey_mods);
    out->hotkey_vk = (unsigned)json_get_int(root, "hotkeyVk", (int)out->hotkey_vk);
    out->autostart = json_get_int(root, "autostart", 0) ? 1 : 0;
    out->theme = theme_from_key(json_get_str(root, "theme", "system"));
    json_get_w(root, "defaultSeparator", out->default_separator, QP_SEP_MAX);
    if (!out->default_separator[0]) qp_strlcpy(out->default_separator, L"、", QP_SEP_MAX);
    out->tree_width = json_get_int(root, "treeWidth", out->tree_width);
    if (out->tree_width < 120) out->tree_width = 120;
    /* 0 = 自动；其余按像素存，下限交给界面层夹取（见 ui_preview_height_for） */
    out->preview_height = json_get_int(root, "previewHeight", out->preview_height);
    if (out->preview_height < 0) out->preview_height = 0;
    /* 模板编辑器的窗口尺寸与预览高度：同样是 0 = 用设计值，其余交界面层夹取 */
    out->editor_w = json_get_int(root, "editorW", out->editor_w);
    out->editor_h = json_get_int(root, "editorH", out->editor_h);
    if (out->editor_w < 0) out->editor_w = 0;
    if (out->editor_h < 0) out->editor_h = 0;
    out->editor_preview_height =
        json_get_int(root, "editorPreviewHeight", out->editor_preview_height);
    if (out->editor_preview_height < 0) out->editor_preview_height = 0;
    out->minimize_to_tray_hint = json_get_int(root, "trayHintShown", 0) ? 1 : 0;
    json_get_w(root, "dataDir", out->data_dir, QP_PATH_MAX);
    win = cJSON_GetObjectItemCaseSensitive(root, "window");
    if (cJSON_IsObject(win)) {
        out->win_x = json_get_int(win, "x", out->win_x);
        out->win_y = json_get_int(win, "y", out->win_y);
        out->win_w = json_get_int(win, "w", out->win_w);
        out->win_h = json_get_int(win, "h", out->win_h);
        out->win_maximized = json_get_int(win, "maximized", 0) ? 1 : 0;
        if (out->win_w < 640) out->win_w = 640;
        if (out->win_h < 480) out->win_h = 480;
    }
    cJSON_Delete(root);
    return 1;
}

/* ================================================================== */
/* 路径与文件                                                          */
/* ================================================================== */

/* 自定义数据目录（空 = 使用默认 %APPDATA%\QuickPhrase）。
   settings.json 始终位于默认目录：程序启动时靠它找到 data_dir，
   因此它不能跟随自定义目录移动。 */
static wchar_t s_custom_data_dir[QP_PATH_MAX];

void storage_set_data_dir(const wchar_t *dir)
{
    if (dir && dir[0]) {
        qp_strlcpy(s_custom_data_dir, dir, QP_PATH_MAX);
        /* 去掉结尾的反斜杠，保证拼接一致 */
        {
            size_t n = wcslen(s_custom_data_dir);
            while (n > 0 && (s_custom_data_dir[n - 1] == L'\\' ||
                             s_custom_data_dir[n - 1] == L'/'))
                s_custom_data_dir[--n] = L'\0';
        }
    } else {
        s_custom_data_dir[0] = L'\0';
    }
}

int storage_get_data_dir(wchar_t *out, size_t cap)
{
    if (s_custom_data_dir[0]) {
        qp_strlcpy(out, s_custom_data_dir, cap);
        return 1;
    }
#ifdef _WIN32
    wchar_t base[MAX_PATH];
    base[0] = L'\0';
    if (GetEnvironmentVariableW(L"APPDATA", base, MAX_PATH) == 0) {
        if (!SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, base)))
            return 0;
    }
    if (!base[0]) return 0;
    swprintf(out, cap, L"%ls\\QuickPhrase", base);
    return 1;
#else
    const char *home = getenv("HOME");
    wchar_t *w;
    if (!home) home = "/tmp";
    w = qp_utf8_to_wide_alloc(home);
    if (!w) return 0;
    swprintf(out, cap, L"%ls/.quickphrase", w);
    free(w);
    return 1;
#endif
}

/* settings.json 始终在默认目录：自定义 data_dir 后仍需能读回该设置 */
static int storage_get_default_dir(wchar_t *out, size_t cap)
{
#ifdef _WIN32
    wchar_t base[MAX_PATH];
    base[0] = L'\0';
    if (GetEnvironmentVariableW(L"APPDATA", base, MAX_PATH) == 0) {
        if (!SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, base)))
            return 0;
    }
    if (!base[0]) return 0;
    swprintf(out, cap, L"%ls\\QuickPhrase", base);
    return 1;
#else
    const char *home = getenv("HOME");
    wchar_t *w;
    if (!home) home = "/tmp";
    w = qp_utf8_to_wide_alloc(home);
    if (!w) return 0;
    swprintf(out, cap, L"%ls/.quickphrase", w);
    free(w);
    return 1;
#endif
}

int storage_get_data_path(wchar_t *out, size_t cap)
{
    wchar_t dir[QP_PATH_MAX];
    if (!storage_get_data_dir(dir, QP_PATH_MAX)) return 0;
#ifdef _WIN32
    swprintf(out, cap, L"%ls\\data.json", dir);
#else
    swprintf(out, cap, L"%ls/data.json", dir);
#endif
    return 1;
}

int storage_get_settings_path(wchar_t *out, size_t cap)
{
    wchar_t dir[QP_PATH_MAX];
    if (!storage_get_default_dir(dir, QP_PATH_MAX)) return 0;
#ifdef _WIN32
    swprintf(out, cap, L"%ls\\settings.json", dir);
#else
    swprintf(out, cap, L"%ls/settings.json", dir);
#endif
    return 1;
}

int storage_ensure_dir(void)
{
    wchar_t dir[QP_PATH_MAX];
    if (!storage_get_data_dir(dir, QP_PATH_MAX)) return 0;
#ifdef _WIN32
    if (CreateDirectoryW(dir, NULL)) return 1;
    return GetLastError() == ERROR_ALREADY_EXISTS;
#else
    {
        char *u8 = qp_wide_to_utf8_alloc(dir);
        char *p;
        int ok = 1;
        if (!u8 || !*u8) { free(u8); return 0; }
        for (p = u8 + 1; ; ++p) {
            char saved;
            struct stat st;
            if (*p && *p != '/') continue;
            saved = *p; *p = 0;
            if (mkdir(u8, 0700) != 0 &&
                (errno != EEXIST || stat(u8, &st) != 0 || !S_ISDIR(st.st_mode))) ok = 0;
            *p = saved;
            if (!ok || !saved) break;
        }
        free(u8);
        return ok;
    }
#endif
}

static FILE *open_w(const wchar_t *path, const wchar_t *mode)
{
#ifdef _WIN32
    return _wfopen(path, mode);
#else
    FILE *f = NULL;
    char *p = qp_wide_to_utf8_alloc(path);
    char *m = qp_wide_to_utf8_alloc(mode);
    if (p && m) f = fopen(p, m);
    free(p);
    free(m);
    return f;
#endif
}

char *storage_read_bytes(const wchar_t *path, size_t *size_out)
{
    FILE *f;
    long size;
    char *buf;
    size_t got;

    if (size_out) *size_out = 0;
    if (!path) return NULL;
    f = open_w(path, L"rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    size = ftell(f);
    /* 数据表可能有好几万行，上限放宽到 256 MB；JSON 那边仍然按各自的限额来 */
    if (size < 0 || size > 256L * 1024 * 1024) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    got = fread(buf, 1, (size_t)size, f);
    {
        int failed = got != (size_t)size || ferror(f);
        int saved_errno = failed ? (errno ? errno : EIO) : 0;
        if (fclose(f) != 0) {
            failed = 1;
            if (!saved_errno) saved_errno = errno ? errno : EIO;
        }
        if (failed) { free(buf); errno = saved_errno; return NULL; }
    }
    buf[got] = '\0';
    if (size_out) *size_out = got;
    return buf;
}

char *storage_read_text(const wchar_t *path)
{
    size_t got = 0;
    char *buf = storage_read_bytes(path, &got);
    if (!buf) return NULL;
    if (memchr(buf, '\0', got)) { free(buf); return NULL; }
    /* 跳过 UTF-8 BOM */
    if (got >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB &&
        (unsigned char)buf[2] == 0xBF) {
        memmove(buf, buf + 3, got - 3 + 1);
    }
    return buf;
}

int storage_write_text(const wchar_t *path, const char *utf8)
{
    wchar_t tmp[600];
    FILE *f = NULL;
    int ok = 0, opened = 0;
    size_t len;
#ifndef _WIN32
    char *temp_path = NULL, *dest_path = NULL;
#endif

    if (!path || !utf8) return 0;
    len = strlen(utf8);

    /* 先写临时文件再替换，避免写入中断损坏数据 */
    if (swprintf(tmp, 600, L"%ls.tmp", path) < 0) return 0;
#ifdef _WIN32
    f = open_w(tmp, L"wb");
#else
    /* Prepare cleanup paths before opening: cleanup must not allocate on OOM. */
    temp_path = qp_wide_to_utf8_alloc(tmp);
    dest_path = qp_wide_to_utf8_alloc(path);
    if (!temp_path || !dest_path) goto done;
    f = fopen(temp_path, "wb");
#endif
    if (!f) goto done;
    opened = 1;
    if (len && fwrite(utf8, 1, len, f) != len) goto done;
    if (fflush(f) != 0) goto done;
#ifdef _WIN32
    if (_commit(_fileno(f)) != 0) goto done;
#else
    if (fsync(fileno(f)) != 0) goto done;
#endif
    {
        int closed = fclose(f);
        f = NULL;
        if (closed != 0) goto done;
    }

#ifdef _WIN32
    ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    ok = rename(temp_path, dest_path) == 0;
#endif
done:
    if (f) fclose(f);
    if (!ok && opened) {
#ifdef _WIN32
        DeleteFileW(tmp);
#else
        remove(temp_path);
#endif
    }
#ifndef _WIN32
    free(temp_path); free(dest_path);
#endif
    return ok;
}

/* ================================================================== */
/* 载入 / 保存                                                         */
/* ================================================================== */

AppData *storage_load(int *created)
{
    wchar_t path[600];
    char *text;
    AppData *d;

    if (created) *created = 0;
    if (!storage_get_data_path(path, 600)) return appdata_create_seed();

    text = storage_read_text(path);
    if (!text) {
#ifdef _WIN32
        DWORD attrs = GetFileAttributesW(path);
        DWORD error = GetLastError();
        if (attrs != INVALID_FILE_ATTRIBUTES ||
            (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)) return NULL;
#else
        char *u8path = qp_wide_to_utf8_alloc(path);
        struct stat st;
        int exists = u8path && stat(u8path, &st) == 0;
        free(u8path);
        if (exists) return NULL;
#endif
        /* 首次运行：写入示例数据 */
        d = appdata_create_seed();
        if (created) *created = 1;
        if (d) storage_save(d);
        return d;
    }
    d = appdata_from_json(text);
    free(text);
    /* 损坏或无法读取的已有文件不能退回示例数据，否则后续保存会覆盖原数据。 */
    return d;
}

int storage_save(const AppData *d)
{
    wchar_t path[600];
    char *text;
    int ok;

    if (!d) return 0;
    if (!storage_ensure_dir()) return 0;
    if (!storage_get_data_path(path, 600)) return 0;
    text = appdata_to_json(d);
    if (!text) return 0;
    ok = storage_write_text(path, text);
    free(text);
    return ok;
}

int storage_export(const AppData *d, const wchar_t *path)
{
    char *text = NULL;
    cJSON *root, *arr, *item;
    const DataTableRef *ref;
    int ok = 0;
    if (!d || !path) return 0;
    root = appdata_to_cjson(d);
    if (!root) return 0;
    arr = cJSON_GetObjectItemCaseSensitive(root, "dataTables");
    ref = d->tables;
    cJSON_ArrayForEach(item, arr) {
        size_t size = 0;
        char *raw = storage_read_bytes(ref->path, &size);
        char *hex;
        const wchar_t *base = ref->path;
        if (!raw) goto done;
        hex = malloc(size * 2 + 1); /* storage_read_bytes caps input at 256 MiB. */
        if (!hex) { free(raw); goto done; }
        for (size_t i = 0; i < size; ++i) {
            static const char digits[] = "0123456789abcdef";
            hex[2*i] = digits[(unsigned char)raw[i] >> 4];
            hex[2*i+1] = digits[(unsigned char)raw[i] & 15];
        }
        hex[size*2] = 0;
        free(raw);
        cJSON *payload = cJSON_AddStringToObject(item, "contentHex", hex);
        free(hex);
        if (!payload) goto done;
        for (const wchar_t *p = ref->path; *p; ++p)
            if (*p == L'/' || *p == L'\\') base = p + 1;
        cJSON_DeleteItemFromObjectCaseSensitive(item, "path");
        if (!json_add_w(item, "path", *base ? base : L"table.csv")) goto done;
        ref = ref->next;
    }
    text = cJSON_Print(root);
    if (!text) goto done;
    /* Exports must remain readable by the same 256 MiB bounded reader. */
    if (strlen(text) > 256u * 1024u * 1024u) goto done;
    ok = storage_write_text(path, text);
done:
    free(text);
    cJSON_Delete(root);
    return ok;
}

static void remove_imported_file(const wchar_t *path)
{
#ifdef _WIN32
    DeleteFileW(path);
#else
    char p[QP_PATH_MAX * 4];
    if (qp_wide_to_utf8(path, p, sizeof(p)) > 0) remove(p);
#endif
}

/* Exclusive creation: neither an imported path nor a generated-name collision
   can overwrite an existing file. Partial writes belong to this transaction. */
static int write_imported_file(const wchar_t *path, const unsigned char *data, size_t size)
{
    int ok;
#ifdef _WIN32
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    if (file == INVALID_HANDLE_VALUE) return 0;
    ok = WriteFile(file, data, (DWORD)size, &written, NULL) && written == size;
    if (ok) ok = FlushFileBuffers(file) != 0;
    if (!CloseHandle(file)) ok = 0;
#else
    char *p = qp_wide_to_utf8_alloc(path);
    if (!p) return 0;
    int fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0600);
    free(p);
    if (fd < 0) return 0;
    size_t offset = 0;
    while (offset < size) {
        ssize_t n = write(fd, data + offset, size - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        offset += (size_t)n;
    }
    ok = offset == size && fsync(fd) == 0;
    if (close(fd) != 0) ok = 0;
#endif
    if (!ok) remove_imported_file(path);
    return ok;
}

static int hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Optional payloads extend old JSON without changing normal data.json saves.
   Prepare every file before publishing the candidate model; roll back on error. */
static AppData *import_candidate(const char *text)
{
    AppData *incoming = appdata_from_json(text);
    cJSON *root = NULL;
    const cJSON *arr, *item;
    DataTableRef *ref;
    wchar_t (*created)[QP_PATH_MAX] = NULL;
    wchar_t dir[QP_PATH_MAX];
    int count = 0, ok = 0;
    if (!incoming) return NULL;
    root = cJSON_ParseWithOpts(text, NULL, 1);
    if (!root) goto done;
    arr = cJSON_GetObjectItemCaseSensitive(root, "dataTables");
    int total = cJSON_GetArraySize(arr);
    if (total) {
        created = calloc((size_t)total, sizeof(*created));
        if (!created) goto done;
    }
    ref = incoming->tables;
    cJSON_ArrayForEach(item, arr) {
        const cJSON *payload = cJSON_GetObjectItemCaseSensitive(item, "contentHex");
        wchar_t original[QP_PATH_MAX];
        json_get_w(item, "path", original, QP_PATH_MAX);
        if (!original[0]) { if (payload) goto done; continue; }
        if (!ref) goto done;
        if (payload) {
            if (!cJSON_IsString(payload) || !payload->valuestring) goto done;
            size_t length = strlen(payload->valuestring);
            if (length % 2 || length / 2 > 256u * 1024u * 1024u) goto done;
            unsigned char *bytes = malloc(length / 2 + 1);
            if (!bytes) goto done;
            for (size_t i = 0; i < length; i += 2) {
                int a = hex_digit((unsigned char)payload->valuestring[i]);
                int b = hex_digit((unsigned char)payload->valuestring[i+1]);
                if (a < 0 || b < 0) { free(bytes); goto done; }
                bytes[i/2] = (unsigned char)((a << 4) | b);
            }
            if (!storage_get_data_dir(dir, QP_PATH_MAX) || !storage_ensure_dir()) {
                free(bytes); goto done;
            }
            wchar_t id[QP_ID_MAX];
            qp_new_id(id, QP_ID_MAX, L"imported");
            int n = swprintf(created[count], QP_PATH_MAX, L"%ls/%ls.csv", dir, id);
            if (n < 0 || n >= QP_PATH_MAX ||
                !write_imported_file(created[count], bytes, length / 2)) {
                free(bytes); goto done;
            }
            free(bytes);
            qp_strlcpy(ref->path, created[count++], QP_PATH_MAX);
        }
        ref = ref->next;
    }
    ok = 1;
done:
    if (!ok) {
        for (int i = 0; i < count; ++i) remove_imported_file(created[i]);
        appdata_free(incoming); incoming = NULL;
    }
    free(created);
    cJSON_Delete(root);
    return incoming;
}

/* 把一条导入进来的表引用并进本地清单：同 id 覆盖（名字/路径/编码/分隔符/表头），
   新 id 追加。
   为什么两种导入模式都只「并」不「换」：表引用指向的是**本机磁盘上的文件**，
   导入的 JSON 里那串路径在别的机器上多半不存在。把本地清单换成导入的那一份，
   等于把用户自己登记的表全清掉 —— 那些没参与本次导入的模板，查表规则会一起
   静默失效。所以导入只增不减，多出来的旧登记用户可以在「数据表」窗口里删。 */
static void merge_table_ref(DataTableRef **head, DataTableRef *r)
{
    DataTableRef *old;
    if (!head || !r) return;
    old = dtref_list_find(*head, r->id);
    if (old) {
        qp_strlcpy(old->name, r->name, QP_NAME_MAX);
        qp_strlcpy(old->path, r->path, QP_PATH_MAX);
        old->encoding = r->encoding;
        old->delimiter = r->delimiter;
        old->has_header = r->has_header;
        dtref_free(r);
    } else {
        dtref_list_append(head, r);
    }
}

/* 把 incoming 的表清单并进 d，然后把 incoming 的那一份摘干净 */
static void merge_tables(DataTableRef **dst, DataTableRef **incoming)
{
    DataTableRef *r = *incoming;
    while (r) {
        DataTableRef *next = r->next;
        merge_table_ref(dst, r);
        r = next;
    }
    *incoming = NULL;                 /* 已经接管，别让 appdata_free 再释放一次 */
}

int storage_import(AppData *d, const wchar_t *path, int mode, int *added, int *updated)
{
    char *text;
    AppData *incoming;
    Template *t;
    Group *g;
    int n_add = 0, n_upd = 0;

    if (added) *added = 0;
    if (updated) *updated = 0;
    if (!d || !path) return 0;

    text = storage_read_text(path);
    if (!text) return 0;
    incoming = import_candidate(text);
    free(text);
    if (!incoming) return 0;

    if (mode == QP_IMPORT_REPLACE) {
        /* 整体替换：模板与分组直接接管 incoming 的链表；
           表清单是并进来而不是替换掉（理由见 merge_table_ref 上面那段）。 */
        Group *og = d->groups;
        Template *ot = d->templates;
        while (og) { Group *n = og->next; group_free(og); og = n; }
        while (ot) { Template *n = ot->next; tmpl_free(ot); ot = n; }
        d->groups = incoming->groups;
        d->templates = incoming->templates;
        merge_tables(&d->tables, &incoming->tables);
        n_add = tmpl_list_count(d->templates);
        incoming->groups = NULL;
        incoming->templates = NULL;
        appdata_free(incoming);
        if (added) *added = n_add;
        return 1;
    }

    /* 合并：分组按 id 去重，模板同 id 覆盖、新 id 追加 */
    g = incoming->groups;
    while (g) {
        Group *next = g->next;
        if (!group_list_find(d->groups, g->id)) {
            g->next = NULL;
            g->order = group_list_count(d->groups);
            group_list_append(&d->groups, g);
        } else {
            group_free(g);
        }
        g = next;
    }
    incoming->groups = NULL;

    t = incoming->templates;
    while (t) {
        Template *next = t->next;
        t->next = NULL;
        /* 分组不存在时归到未分组，避免出现悬空 groupId */
        if (t->group_id[0] && !group_list_find(d->groups, t->group_id))
            t->group_id[0] = L'\0';
        if (tmpl_list_find(d->templates, t->id)) {
            tmpl_list_replace(&d->templates, t);
            ++n_upd;
        } else {
            tmpl_list_append(&d->templates, t);
            ++n_add;
        }
        t = next;
    }
    incoming->templates = NULL;
    merge_tables(&d->tables, &incoming->tables);
    appdata_free(incoming);

    if (added) *added = n_add;
    if (updated) *updated = n_upd;
    return 1;
}

/* ================================================================== */
/* 设置读写                                                            */
/* ================================================================== */

int settings_load(Settings *s)
{
    wchar_t path[600];
    char *text;
    int ok;

    if (!s) return 0;
    settings_defaults(s);
    if (!storage_get_settings_path(path, 600)) return 0;
    text = storage_read_text(path);
    if (!text) return 0;
    ok = settings_from_json(text, s);
    free(text);
    return ok;
}

int settings_save(const Settings *s)
{
    wchar_t path[600];
    char *text;
    int ok;

    if (!s) return 0;
    storage_ensure_dir();
    if (!storage_get_settings_path(path, 600)) return 0;
    text = settings_to_json(s);
    if (!text) return 0;
    ok = storage_write_text(path, text);
    free(text);
    return ok;
}
