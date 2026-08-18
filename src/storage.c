/*
 * storage.c —— cJSON 序列化 + 文件读写 + 导入导出 + 设置
 *
 * 除路径解析与文件打开外，其余逻辑为纯 C，可在非 Windows 平台编译以便自测
 * （见 tests/test_storage.c）。
 */
#include "storage.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <shlobj.h>
#endif

/* ================================================================== */
/* UTF-8 <-> UTF-16 转换                                               */
/* ================================================================== */

#ifdef _WIN32

size_t qp_utf8_to_wide(const char *src, wchar_t *dst, size_t cap)
{
    int need;
    if (!src) { if (dst && cap) dst[0] = L'\0'; return 0; }
    need = MultiByteToWideChar(CP_UTF8, 0, src, -1, NULL, 0);
    if (need <= 0) { if (dst && cap) dst[0] = L'\0'; return 0; }
    if (!dst || cap == 0) return (size_t)(need - 1);
    if ((size_t)need > cap) {
        /* 截断：按 cap 转换，保证结尾 NUL */
        MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, (int)cap);
        dst[cap - 1] = L'\0';
        return wcslen(dst);
    }
    MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, (int)cap);
    return (size_t)(need - 1);
}

size_t qp_wide_to_utf8(const wchar_t *src, char *dst, size_t cap)
{
    int need;
    if (!src) { if (dst && cap) dst[0] = '\0'; return 0; }
    need = WideCharToMultiByte(CP_UTF8, 0, src, -1, NULL, 0, NULL, NULL);
    if (need <= 0) { if (dst && cap) dst[0] = '\0'; return 0; }
    if (!dst || cap == 0) return (size_t)(need - 1);
    if ((size_t)need > cap) {
        WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, (int)cap, NULL, NULL);
        dst[cap - 1] = '\0';
        return strlen(dst);
    }
    WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, (int)cap, NULL, NULL);
    return (size_t)(need - 1);
}

#else /* 非 Windows：便携实现，供单元测试使用 */

static const char *utf8_decode(const char *s, unsigned *cp)
{
    unsigned char c = (unsigned char)*s;
    if (c < 0x80) { *cp = c; return s + 1; }
    if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        *cp = ((unsigned)(c & 0x1F) << 6) | (unsigned)(s[1] & 0x3F);
        return s + 2;
    }
    if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *cp = ((unsigned)(c & 0x0F) << 12) | ((unsigned)(s[1] & 0x3F) << 6) |
              (unsigned)(s[2] & 0x3F);
        return s + 3;
    }
    if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
        (s[3] & 0xC0) == 0x80) {
        *cp = ((unsigned)(c & 0x07) << 18) | ((unsigned)(s[1] & 0x3F) << 12) |
              ((unsigned)(s[2] & 0x3F) << 6) | (unsigned)(s[3] & 0x3F);
        return s + 4;
    }
    *cp = 0xFFFD;
    return s + 1;
}

size_t qp_utf8_to_wide(const char *src, wchar_t *dst, size_t cap)
{
    size_t n = 0;
    const char *p = src;
    if (!src) { if (dst && cap) dst[0] = L'\0'; return 0; }
    while (*p) {
        unsigned cp;
        p = utf8_decode(p, &cp);
        if (sizeof(wchar_t) == 2 && cp > 0xFFFF) {
            unsigned v = cp - 0x10000;
            if (dst) {
                if (n + 2 >= cap) break;
                dst[n] = (wchar_t)(0xD800 + (v >> 10));
                dst[n + 1] = (wchar_t)(0xDC00 + (v & 0x3FF));
            }
            n += 2;
        } else {
            if (dst) {
                if (n + 1 >= cap) break;
                dst[n] = (wchar_t)cp;
            }
            n += 1;
        }
    }
    if (dst && cap) dst[n < cap ? n : cap - 1] = L'\0';
    return n;
}

size_t qp_wide_to_utf8(const wchar_t *src, char *dst, size_t cap)
{
    size_t n = 0;
    const wchar_t *p = src;
    if (!src) { if (dst && cap) dst[0] = '\0'; return 0; }
    while (*p) {
        unsigned cp = (unsigned)*p++;
        char buf[4];
        size_t len;
        if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp <= 0xDBFF && *p >= 0xDC00 &&
            *p <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + ((unsigned)*p - 0xDC00);
            ++p;
        }
        if (cp < 0x80) { buf[0] = (char)cp; len = 1; }
        else if (cp < 0x800) {
            buf[0] = (char)(0xC0 | (cp >> 6));
            buf[1] = (char)(0x80 | (cp & 0x3F));
            len = 2;
        } else if (cp < 0x10000) {
            buf[0] = (char)(0xE0 | (cp >> 12));
            buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[2] = (char)(0x80 | (cp & 0x3F));
            len = 3;
        } else {
            buf[0] = (char)(0xF0 | (cp >> 18));
            buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
            buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[3] = (char)(0x80 | (cp & 0x3F));
            len = 4;
        }
        if (dst) {
            if (n + len + 1 > cap) break;
            memcpy(dst + n, buf, len);
        }
        n += len;
    }
    if (dst && cap) dst[n < cap ? n : cap - 1] = '\0';
    return n;
}

#endif /* _WIN32 */

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

static void json_add_w(cJSON *obj, const char *key, const wchar_t *value)
{
    char *u8 = qp_wide_to_utf8_alloc(value);
    if (u8) {
        cJSON_AddStringToObject(obj, key, u8);
        free(u8);
    } else {
        cJSON_AddStringToObject(obj, key, "");
    }
}

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
    return t == PARSE_CONTAINS ? "contains" : "split";
}

static ParseRuleType parse_type_from_key(const char *k)
{
    return (k && strcmp(k, "contains") == 0) ? PARSE_CONTAINS : PARSE_SPLIT;
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
    json_add_w(o, "id", r->id);
    cJSON_AddStringToObject(o, "type", parse_type_key(r->type));
    json_add_w(o, "delimiter", r->delimiter);
    cJSON_AddNumberToObject(o, "segmentIndex", r->segment_index);
    json_add_w(o, "keyword", r->keyword);
    json_add_w(o, "targetSegmentId", r->target_segment_id);
    json_add_w(o, "fillValue", r->fill_value);
    return o;
}

static void cond_term_fill_json(cJSON *o, const CondTerm *t)
{
    json_add_w(o, "triggerSegmentId", t->trigger_segment_id);
    cJSON_AddStringToObject(o, "operator", cond_op_key(t->op));
    json_add_w(o, "matchValue", t->match_value);
    if (t->op == OP_IN && t->match_value_count > 0) {
        cJSON *arr = cJSON_AddArrayToObject(o, "matchValues");
        int i;
        for (i = 0; arr && i < t->match_value_count; ++i) {
            char *u8 = qp_wide_to_utf8_alloc(t->match_values[i]);
            if (u8) {
                cJSON_AddItemToArray(arr, cJSON_CreateString(u8));
                free(u8);
            }
        }
    }
}

static cJSON *cond_term_to_json(const CondTerm *t)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cond_term_fill_json(o, t);
    if (t->negate) cJSON_AddBoolToObject(o, "negate", 1);
    return o;
}

static cJSON *cond_rule_to_json(const ConditionRule *r)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *arr;
    const CondTerm *t;
    if (!o) return NULL;
    json_add_w(o, "id", r->id);
    cJSON_AddStringToObject(o, "join", r->join == COND_JOIN_ANY ? "any" : "all");
    arr = cJSON_AddArrayToObject(o, "conditions");
    for (t = r->terms; arr && t; t = t->next)
        cJSON_AddItemToArray(arr, cond_term_to_json(t));
    /* 单条件且未取反时，同时写出老字段，旧版本仍能读懂这条规则 */
    if (r->terms && !r->terms->next && !r->terms->negate)
        cond_term_fill_json(o, r->terms);
    json_add_w(o, "resultValue", r->result_value);
    return o;
}

static cJSON *segment_to_json(const Segment *s)
{
    cJSON *o = cJSON_CreateObject();
    const ParseRule *pr;
    const ConditionRule *cr;
    if (!o) return NULL;
    json_add_w(o, "id", s->id);
    cJSON_AddStringToObject(o, "type", seg_type_key(s->type));
    if (s->type == SEG_STATIC) {
        json_add_w(o, "content", s->content);
    } else {
        json_add_w(o, "label", s->label);
        json_add_w(o, "placeholder", s->placeholder);
        json_add_w(o, "defaultValue", s->default_value);
        if (s->type == SEG_SELECT || s->type == SEG_MULTI_SELECT) {
            cJSON *arr = cJSON_AddArrayToObject(o, "options");
            int i;
            for (i = 0; arr && i < s->option_count; ++i) {
                char *u8 = qp_wide_to_utf8_alloc(s->options[i]);
                if (u8) {
                    cJSON_AddItemToArray(arr, cJSON_CreateString(u8));
                    free(u8);
                }
            }
        }
        if (s->type == SEG_MULTI_SELECT) json_add_w(o, "separator", s->separator);
        if (s->hidden) cJSON_AddBoolToObject(o, "hidden", 1);
        if (s->type == SEG_TEXT && s->multiline) cJSON_AddBoolToObject(o, "multiline", 1);
        if (s->type == SEG_DATETIME) json_add_w(o, "timeFormat", s->time_format);
        /* 引用：不引用时整组字段都不写，存档保持干净 */
        if (s->ref_segment_id[0]) {
            json_add_w(o, "refSegmentId", s->ref_segment_id);
            cJSON_AddStringToObject(o, "refMode", ref_mode_key(s->ref_mode));
            json_add_w(o, "refDelimiter", s->ref_delimiter);
            cJSON_AddNumberToObject(o, "refIndex", s->ref_index);
            cJSON_AddNumberToObject(o, "refStart", s->ref_start);
            cJSON_AddNumberToObject(o, "refLen", s->ref_len);
        }
    }
    if (s->parse_rules) {
        cJSON *arr = cJSON_AddArrayToObject(o, "parseRules");
        for (pr = s->parse_rules; arr && pr; pr = pr->next)
            cJSON_AddItemToArray(arr, parse_rule_to_json(pr));
    }
    if (s->condition_rules) {
        cJSON *arr = cJSON_AddArrayToObject(o, "conditionRules");
        for (cr = s->condition_rules; arr && cr; cr = cr->next)
            cJSON_AddItemToArray(arr, cond_rule_to_json(cr));
    }
    return o;
}

static cJSON *template_to_json(const Template *t)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *arr;
    const Segment *s;
    if (!o) return NULL;
    json_add_w(o, "id", t->id);
    json_add_w(o, "name", t->name);
    json_add_w(o, "groupId", t->group_id);
    json_add_w(o, "createdAt", t->created_at);
    json_add_w(o, "updatedAt", t->updated_at);
    arr = cJSON_AddArrayToObject(o, "segments");
    for (s = t->segments; arr && s; s = s->next)
        cJSON_AddItemToArray(arr, segment_to_json(s));
    return o;
}

static cJSON *appdata_to_cjson(const AppData *d)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr;
    const Group *g;
    const Template *t;
    if (!root) return NULL;
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddStringToObject(root, "app", "QuickPhrase");
    arr = cJSON_AddArrayToObject(root, "groups");
    for (g = d ? d->groups : NULL; arr && g; g = g->next) {
        cJSON *o = cJSON_CreateObject();
        if (!o) continue;
        json_add_w(o, "id", g->id);
        json_add_w(o, "name", g->name);
        cJSON_AddNumberToObject(o, "order", g->order);
        cJSON_AddItemToArray(arr, o);
    }
    arr = cJSON_AddArrayToObject(root, "templates");
    for (t = d ? d->templates : NULL; arr && t; t = t->next)
        cJSON_AddItemToArray(arr, template_to_json(t));
    return root;
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
    json_get_w(o, "targetSegmentId", r->target_segment_id, QP_ID_MAX);
    json_get_w(o, "fillValue", r->fill_value, QP_VALUE_MAX);
    return r;
}

/* 读一个条件项的匹配值：优先 matchValues 数组，其次 matchValue 文本 */
static void cond_term_read_match(CondTerm *t, const cJSON *o)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(o, "matchValues");
    wchar_t raw[QP_MATCH_MAX];
    raw[0] = L'\0';
    if (cJSON_IsArray(arr) && cJSON_GetArraySize(arr) > 0) {
        /* 由列表重建原始文本，再统一走 set_match 拆分 */
        const cJSON *it;
        cJSON_ArrayForEach(it, arr) {
            wchar_t *w;
            if (!cJSON_IsString(it) || !it->valuestring) continue;
            w = qp_utf8_to_wide_alloc(it->valuestring);
            if (!w) continue;
            if (raw[0]) wcsncat(raw, L",", QP_MATCH_MAX - wcslen(raw) - 1);
            wcsncat(raw, w, QP_MATCH_MAX - wcslen(raw) - 1);
            free(w);
        }
    } else {
        json_get_w(o, "matchValue", raw, QP_MATCH_MAX);
    }
    cond_term_set_match(t, raw);
}

static CondTerm *cond_term_from_json(const cJSON *o)
{
    CondTerm *t = cond_term_create();
    if (!t) return NULL;
    json_get_w(o, "triggerSegmentId", t->trigger_segment_id, QP_ID_MAX);
    t->op = cond_op_from_key(json_get_str(o, "operator", "contains"));
    t->negate = json_get_int(o, "negate", 0) ? 1 : 0;
    cond_term_read_match(t, o);
    return t;
}

static ConditionRule *cond_rule_from_json(const cJSON *o)
{
    ConditionRule *r = cond_rule_create();
    const cJSON *arr;
    if (!r) return NULL;
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
            cond_term_append(&r->terms, cond_term_from_json(it));
        }
    } else {
        /* 老格式：整条规则就是一个条件项 */
        cond_term_append(&r->terms, cond_term_from_json(o));
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
    json_get_w(o, "id", s->id, QP_ID_MAX);
    if (!s->id[0]) qp_new_id(s->id, QP_ID_MAX, L"seg");
    json_get_w(o, "label", s->label, QP_LABEL_MAX);
    json_get_w(o, "content", s->content, QP_CONTENT_MAX);
    json_get_w(o, "placeholder", s->placeholder, QP_PLACEHOLD_MAX);
    json_get_w(o, "defaultValue", s->default_value, QP_VALUE_MAX);
    s->hidden = (type != SEG_STATIC && json_get_int(o, "hidden", 0)) ? 1 : 0;
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
                if (w) { seg_add_option(s, w); free(w); }
            }
        }
    }
    arr = cJSON_GetObjectItemCaseSensitive(o, "parseRules");
    if (cJSON_IsArray(arr)) {
        cJSON_ArrayForEach(it, arr) {
            if (cJSON_IsObject(it))
                parse_rule_append(&s->parse_rules, parse_rule_from_json(it));
        }
    }
    arr = cJSON_GetObjectItemCaseSensitive(o, "conditionRules");
    if (cJSON_IsArray(arr)) {
        cJSON_ArrayForEach(it, arr) {
            if (cJSON_IsObject(it))
                cond_rule_append(&s->condition_rules, cond_rule_from_json(it));
        }
    }
    return s;
}

static Template *template_from_json(const cJSON *o)
{
    Template *t = tmpl_create(L"");
    const cJSON *arr, *it;
    if (!t) return NULL;
    json_get_w(o, "id", t->id, QP_ID_MAX);
    if (!t->id[0]) qp_new_id(t->id, QP_ID_MAX, L"tpl");
    json_get_w(o, "name", t->name, QP_NAME_MAX);
    if (!t->name[0]) qp_strlcpy(t->name, L"未命名模板", QP_NAME_MAX);
    json_get_w(o, "groupId", t->group_id, QP_ID_MAX);
    json_get_w(o, "createdAt", t->created_at, QP_TIME_MAX);
    json_get_w(o, "updatedAt", t->updated_at, QP_TIME_MAX);
    if (!t->created_at[0]) qp_now_iso(t->created_at, QP_TIME_MAX);
    if (!t->updated_at[0]) qp_strlcpy(t->updated_at, t->created_at, QP_TIME_MAX);
    arr = cJSON_GetObjectItemCaseSensitive(o, "segments");
    if (cJSON_IsArray(arr)) {
        cJSON_ArrayForEach(it, arr) {
            if (cJSON_IsObject(it))
                seg_list_append(&t->segments, segment_from_json(it));
        }
    }
    return t;
}

AppData *appdata_from_json(const char *utf8)
{
    cJSON *root;
    const cJSON *arr, *it;
    AppData *d;

    if (!utf8) return NULL;
    root = cJSON_Parse(utf8);
    if (!root) return NULL;
    d = appdata_create();
    if (!d) { cJSON_Delete(root); return NULL; }

    arr = cJSON_GetObjectItemCaseSensitive(root, "groups");
    if (cJSON_IsArray(arr)) {
        cJSON_ArrayForEach(it, arr) {
            Group *g;
            if (!cJSON_IsObject(it)) continue;
            g = group_create(L"");
            if (!g) continue;
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
            if (cJSON_IsObject(it))
                tmpl_list_append(&d->templates, template_from_json(it));
        }
    }
    cJSON_Delete(root);
    return d;
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
    s->minimize_to_tray_hint = 0;
}

char *settings_to_json(const Settings *s)
{
    cJSON *root, *win;
    char *text;
    if (!s) return NULL;
    root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddNumberToObject(root, "hotkeyMods", (double)s->hotkey_mods);
    cJSON_AddNumberToObject(root, "hotkeyVk", (double)s->hotkey_vk);
    cJSON_AddBoolToObject(root, "autostart", s->autostart ? 1 : 0);
    cJSON_AddStringToObject(root, "theme", theme_key(s->theme));
    json_add_w(root, "defaultSeparator", s->default_separator);
    cJSON_AddNumberToObject(root, "treeWidth", s->tree_width);
    cJSON_AddNumberToObject(root, "previewHeight", s->preview_height);
    cJSON_AddBoolToObject(root, "trayHintShown", s->minimize_to_tray_hint ? 1 : 0);
    json_add_w(root, "dataDir", s->data_dir);
    win = cJSON_AddObjectToObject(root, "window");
    if (win) {
        cJSON_AddNumberToObject(win, "x", s->win_x);
        cJSON_AddNumberToObject(win, "y", s->win_y);
        cJSON_AddNumberToObject(win, "w", s->win_w);
        cJSON_AddNumberToObject(win, "h", s->win_h);
        cJSON_AddBoolToObject(win, "maximized", s->win_maximized ? 1 : 0);
    }
    text = cJSON_Print(root);
    cJSON_Delete(root);
    return text;
}

int settings_from_json(const char *utf8, Settings *out)
{
    cJSON *root;
    const cJSON *win;
    if (!utf8 || !out) return 0;
    root = cJSON_Parse(utf8);
    if (!root) return 0;
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
        int ok = 0;
        if (u8) {
            char cmd[1024];
            snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", u8);
            ok = system(cmd) == 0;
            free(u8);
        }
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

char *storage_read_text(const wchar_t *path)
{
    FILE *f;
    long size;
    char *buf;
    size_t got;

    if (!path) return NULL;
    f = open_w(path, L"rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    size = ftell(f);
    if (size < 0 || size > 64L * 1024 * 1024) { fclose(f); return NULL; }
    rewind(f);
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
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
    FILE *f;
    size_t len;

    if (!path || !utf8) return 0;
    len = strlen(utf8);

    /* 先写临时文件再替换，避免写入中断损坏数据 */
    swprintf(tmp, 600, L"%ls.tmp", path);
    f = open_w(tmp, L"wb");
    if (!f) return 0;
    if (len && fwrite(utf8, 1, len, f) != len) { fclose(f); return 0; }
    if (fflush(f) != 0) { fclose(f); return 0; }
    fclose(f);

#ifdef _WIN32
    if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(path);
        if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(tmp);
            return 0;
        }
    }
#else
    {
        char *a = qp_wide_to_utf8_alloc(tmp);
        char *b = qp_wide_to_utf8_alloc(path);
        int ok = (a && b) ? (rename(a, b) == 0) : 0;
        free(a);
        free(b);
        if (!ok) return 0;
    }
#endif
    return 1;
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
        /* 首次运行：写入示例数据 */
        d = appdata_create_seed();
        if (created) *created = 1;
        if (d) storage_save(d);
        return d;
    }
    d = appdata_from_json(text);
    free(text);
    if (!d) {
        /* JSON 损坏：备份原文件后使用示例数据，避免用户数据被静默清空 */
        wchar_t bak[700];
        swprintf(bak, 700, L"%ls.corrupt", path);
#ifdef _WIN32
        MoveFileExW(path, bak, MOVEFILE_REPLACE_EXISTING);
#endif
        d = appdata_create_seed();
        if (created) *created = 1;
    }
    return d;
}

int storage_save(const AppData *d)
{
    wchar_t path[600];
    char *text;
    int ok;

    if (!d) return 0;
    storage_ensure_dir();
    if (!storage_get_data_path(path, 600)) return 0;
    text = appdata_to_json(d);
    if (!text) return 0;
    ok = storage_write_text(path, text);
    free(text);
    return ok;
}

int storage_export(const AppData *d, const wchar_t *path)
{
    char *text;
    int ok;
    if (!d || !path) return 0;
    text = appdata_to_json(d);
    if (!text) return 0;
    ok = storage_write_text(path, text);
    free(text);
    return ok;
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
    incoming = appdata_from_json(text);
    free(text);
    if (!incoming) return 0;

    if (mode == QP_IMPORT_REPLACE) {
        /* 整体替换：直接接管 incoming 的链表 */
        Group *og = d->groups;
        Template *ot = d->templates;
        while (og) { Group *n = og->next; group_free(og); og = n; }
        while (ot) { Template *n = ot->next; tmpl_free(ot); ot = n; }
        d->groups = incoming->groups;
        d->templates = incoming->templates;
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
