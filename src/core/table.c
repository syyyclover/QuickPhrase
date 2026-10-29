/*
 * table.c —— 数据表（名单 / 联系表）：解析 CSV/TSV、建索引、按键查询
 *
 * 三条要求各自怎么落实：
 *   快：文件只解析一次（按 mtime/size 判断要不要重读），键列建哈希索引，
 *       查表 O(1)；索引按列缓存，几万行也只是几 MB 内存。
 *   准：键先按「去首尾空白（含全角空格与 U+00A0）+ 大小写不敏感」的字符串比，
 *       再按数值比（1.38E+10 / 13800138000 / 13800138000.0 视为同一个键）；
 *       列优先按表头名绑定，避免每周换表列序一变就悄悄取错列。
 *   稳：CSV 按 RFC4180 处理引号、字段内逗号与换行、CRLF/LF、BOM；
 *       编码自动识别（BOM → 合法 UTF-8 → GBK）；读不通就明确报错，不猜。
 */
#include "table.h"
#include "storage.h"
#include "table_key.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>       /* 只用 MultiByteToWideChar 做 GBK 解码 */
#endif

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <wchar.h>
#include <wctype.h>
#include <limits.h>

#define QP_TABLE_CACHE_MAX   4      /* 同时缓存几张表 */
#define QP_TABLE_MAX_IDX     4      /* 每张表最多缓存几个「键列」的索引 */
#define QP_TABLE_HITS_INLINE 512    /* 命中行先放这么多在栈上，再多就往堆上长 */

/* ------------------------------------------------------------------ */
/* 引用登记                                                            */
/* ------------------------------------------------------------------ */

static DataTableRef *s_refs;
static DataTableRef *s_read_refs;
static unsigned s_read_depth;

void qp_table_set_refs(DataTableRef *refs)
{
    s_refs = refs;
    qp_table_invalidate_all();
}

void qp_table_forget_refs(const DataTableRef *refs)
{
    if (s_refs == refs) s_refs = NULL;
}

DataTableRef *qp_table_find_ref(const wchar_t *id)
{
    return dtref_list_find(s_read_depth ? s_read_refs : s_refs, id);
}

const wchar_t *qp_table_ref_name(const wchar_t *id)
{
    DataTableRef *r = qp_table_find_ref(id);
    return r ? r->name : NULL;
}

const wchar_t *qp_table_encoding_name(int enc)
{
    switch (enc) {
    case QP_ENC_UTF8:  return L"UTF-8";
    case QP_ENC_GBK:   return L"GBK";
    case QP_ENC_UTF16: return L"UTF-16";
    default:           return L"自动识别";
    }
}

const wchar_t *qp_table_delim_name(int delim)
{
    switch (delim) {
    case QP_DELIM_COMMA: return L"逗号";
    case QP_DELIM_TAB:   return L"制表符";
    case QP_DELIM_SEMI:  return L"分号";
    default:             return L"自动识别";
    }
}

const wchar_t *qp_table_hit_name(int hit)
{
    switch (hit) {
    case LOOKUP_LAST: return L"取最后一条";
    case LOOKUP_ALL:  return L"全部取出";
    default:          return L"取第一条";
    }
}

/* 存档键名：故意与界面文字分开，改文案不影响旧档读取 */
const char *qp_table_encoding_key(int enc)
{
    switch (enc) {
    case QP_ENC_UTF8:  return "utf8";
    case QP_ENC_GBK:   return "gbk";
    case QP_ENC_UTF16: return "utf16";
    default:           return "auto";
    }
}

const char *qp_table_delim_key(int delim)
{
    switch (delim) {
    case QP_DELIM_COMMA: return "comma";
    case QP_DELIM_TAB:   return "tab";
    case QP_DELIM_SEMI:  return "semi";
    default:             return "auto";
    }
}

const char *qp_table_hit_key(int hit)
{
    switch (hit) {
    case LOOKUP_LAST: return "last";
    case LOOKUP_ALL:  return "all";
    default:          return "first";
    }
}

TableEncoding qp_table_encoding_from_key(const char *k)
{
    if (!k) return QP_ENC_AUTO;
    if (strcmp(k, "utf8") == 0)  return QP_ENC_UTF8;
    if (strcmp(k, "gbk") == 0)   return QP_ENC_GBK;
    if (strcmp(k, "utf16") == 0) return QP_ENC_UTF16;
    return QP_ENC_AUTO;
}

TableDelim qp_table_delim_from_key(const char *k)
{
    if (!k) return QP_DELIM_AUTO;
    if (strcmp(k, "comma") == 0) return QP_DELIM_COMMA;
    if (strcmp(k, "tab") == 0)   return QP_DELIM_TAB;
    if (strcmp(k, "semi") == 0)  return QP_DELIM_SEMI;
    return QP_DELIM_AUTO;
}

LookupHitMode qp_table_hit_from_key(const char *k)
{
    if (!k) return LOOKUP_FIRST;
    if (strcmp(k, "last") == 0) return LOOKUP_LAST;
    if (strcmp(k, "all") == 0)  return LOOKUP_ALL;
    return LOOKUP_FIRST;
}

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static void set_err(wchar_t *err, size_t cap, const wchar_t *fmt, ...)
{
    va_list ap;
    if (!err || cap == 0) return;
    va_start(ap, fmt);
    vswprintf(err, cap, fmt, ap);
    err[cap - 1] = L'\0';
    va_end(ap);
}

static int is_blank_w(wchar_t c)
{
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' ||
           c == 0x3000 || c == 0x00A0;
}

/* Full header identity without allocation or prefix clipping. */
static int normalized_equal(const wchar_t *a, const wchar_t *b)
{
    if(!a || !b)return 0;
    while(*a && is_blank_w(*a))++a;
    while(*b && is_blank_w(*b))++b;
    size_t an=wcslen(a),bn=wcslen(b);
    while(an && is_blank_w(a[an-1]))--an;
    while(bn && is_blank_w(b[bn-1]))--bn;
    if(an!=bn)return 0;
    for(size_t i=0;i<an;++i)if(towlower(a[i])!=towlower(b[i]))return 0;
    return 1;
}


/* FNV-1a（wchar_t 版） */
static unsigned hash_w(const wchar_t *s)
{
    unsigned h = 2166136261u;
    for (; s && *s; ++s) {
        h ^= (unsigned)(*s & 0xffffu);
        h *= 16777619u;
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* 读文件 + 编码识别 + 解码                                             */
/* ------------------------------------------------------------------ */

static int valid_utf8(const unsigned char *p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned c = p[i];
        int extra;
        if (c < 0x80) { ++i; continue; }
        else if ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        else return 0;
        if (i + (size_t)extra >= n) return 0;
        while (extra-- > 0) {
            ++i;
            if ((p[i] & 0xC0) != 0x80) return 0;
        }
        ++i;
    }
    return 1;
}

#if defined(_WIN32)
/* GBK/GB18030 -> wide：交给系统（中文 Windows 上 Excel 存的「CSV」就是这个） */
static wchar_t *decode_codepage(const char *p, size_t n, unsigned cp)
{
    int need = MultiByteToWideChar(cp, 0, p, (int)n, NULL, 0);
    wchar_t *out;
    if (need <= 0) return NULL;
    out = (wchar_t *)malloc(((size_t)need + 1) * sizeof(wchar_t));
    if (!out) return NULL;
    MultiByteToWideChar(cp, 0, p, (int)n, out, need);
    out[need] = L'\0';
    return out;
}
#endif

static wchar_t *decode_file(const wchar_t *path, int enc_opt, int *used,
                            wchar_t *err, size_t errcap)
{
    size_t n = 0;
    char *raw = storage_read_bytes(path, &n);
    const unsigned char *p;
    wchar_t *out = NULL;
    int enc = enc_opt;

    if (used) *used = enc;
    if (!raw) {
        set_err(err, errcap, L"无法读取文件：%ls", path);
        return NULL;
    }
    p = (const unsigned char *)raw;

    if (enc == QP_ENC_AUTO) {
        if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) enc = QP_ENC_UTF8;
        else if (n >= 2 && ((p[0] == 0xFF && p[1] == 0xFE) ||
                            (p[0] == 0xFE && p[1] == 0xFF))) enc = QP_ENC_UTF16;
        else if (valid_utf8(p, n)) enc = QP_ENC_UTF8;
        else enc = QP_ENC_GBK;
    }
    if (used) *used = enc;

    if (enc == QP_ENC_UTF8) {
        size_t off = (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) ? 3 : 0;
        out = qp_utf8_to_wide_alloc(raw + off);
        if (!out) set_err(err, errcap, L"UTF-8 解码失败（文件可能不是 UTF-8）");
    } else if (enc == QP_ENC_UTF16) {
        int be = (n >= 2 && p[0] == 0xFE && p[1] == 0xFF);
        size_t off = 2, cnt = (n > off) ? (n - off) / 2 : 0, i;
        out = (wchar_t *)malloc((cnt + 1) * sizeof(wchar_t));
        if (out) {
            for (i = 0; i < cnt; ++i) {
                unsigned lo = p[off + i * 2], hi = p[off + i * 2 + 1];
                out[i] = (wchar_t)(be ? ((lo << 8) | hi) : ((hi << 8) | lo));
            }
            out[cnt] = L'\0';
        }
    } else {
#if defined(_WIN32)
        out = decode_codepage(raw, n, 936);
        if (!out) set_err(err, errcap, L"GBK 解码失败");
#else
        set_err(err, errcap,
                L"本机不支持 GBK 解码：请把「编码」改成 UTF-8，"
                L"或把文件另存为「CSV UTF-8」");
#endif
    }
    free(raw);
    return out;
}

/* ------------------------------------------------------------------ */
/* 分隔符识别                                                          */
/* ------------------------------------------------------------------ */

static int detect_delim(const wchar_t *t)
{
    static const wchar_t cand[3] = { L',', L'\t', L';' };
    int cnt[3] = { 0, 0, 0 }, k, i, in_q = 0, lines = 0;
    for (i = 0; t[i] && lines < 10; ++i) {
        wchar_t c = t[i];
        if (c == L'"') { in_q = !in_q; continue; }
        if (c == L'\n') { ++lines; continue; }
        if (in_q) continue;
        for (k = 0; k < 3; ++k) if (c == cand[k]) ++cnt[k];
    }
    k = 0;
    for (i = 1; i < 3; ++i) if (cnt[i] > cnt[k]) k = i;
    if (cnt[k] == 0) return QP_DELIM_COMMA;
    return (k == 0) ? QP_DELIM_COMMA : (k == 1) ? QP_DELIM_TAB : QP_DELIM_SEMI;
}

static wchar_t delim_char(int delim)
{
    switch (delim) {
    case QP_DELIM_TAB:  return L'\t';
    case QP_DELIM_SEMI: return L';';
    default:            return L',';
    }
}

/* ------------------------------------------------------------------ */
/* CSV / TSV 解析（RFC4180）                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    wchar_t *buf;      /* 所有单元格文本，各自以 0 结尾 */
    size_t   blen, bcap;
    int     *off;      /* 单元格 → 文本在 buf 里的偏移（-1 = 空） */
    int     *len;
    int      n, cap;
    int     *row_start;/* 每行第一个单元格在 off/len 里的下标 */
    int      nrows, rcap;
} CsvGrid;

static int grid_push_cell(CsvGrid *g, int off, int len)
{
    if (g->n >= g->cap) {
        int nc = g->cap ? g->cap * 2 : 256;
        int *no = (int *)realloc(g->off, (size_t)nc * sizeof(int));
        int *nl;
        if (!no) return 0;
        g->off = no;
        nl = (int *)realloc(g->len, (size_t)nc * sizeof(int));
        if (!nl) return 0;
        g->len = nl;
        g->cap = nc;
    }
    g->off[g->n] = off;
    g->len[g->n] = len;
    ++g->n;
    return 1;
}

static int grid_push_row(CsvGrid *g, int start)
{
    if (g->nrows >= g->rcap) {
        int nc = g->rcap ? g->rcap * 2 : 64;
        int *ns = (int *)realloc(g->row_start, (size_t)nc * sizeof(int));
        if (!ns) return 0;
        g->row_start = ns;
        g->rcap = nc;
    }
    g->row_start[g->nrows] = start;
    ++g->nrows;
    return 1;
}

static int grid_put_char(CsvGrid *g, wchar_t c)
{
    if (g->blen + 2 > g->bcap) {
        size_t nc = g->bcap ? g->bcap * 2 : 4096;
        wchar_t *nb;
        if (nc < g->blen + 2) nc = g->blen + 2;
        nb = (wchar_t *)realloc(g->buf, nc * sizeof(wchar_t));
        if (!nb) return 0;
        g->buf = nb;
        g->bcap = nc;
    }
    g->buf[g->blen++] = c;
    return 1;
}

static int csv_parse(const wchar_t *t, int delim, CsvGrid *g)
{
    int i = 0;
    wchar_t d = delim_char(delim);

    memset(g, 0, sizeof(*g));
    if (!grid_push_row(g, 0)) return 0;

    while (t[i]) {
        int field_off = (int)g->blen;
        int field_len = 0;

        if (t[i] == L'"') {
            ++i;
            for (;;) {
                wchar_t c = t[i];
                if (!c) break;
                if (c == L'"') {
                    if (t[i + 1] == L'"') {                 /* "" = 一个字面引号 */
                        if (!grid_put_char(g, L'"')) return 0;
                        ++field_len;
                        i += 2;
                        continue;
                    }
                    ++i;
                    break;                                  /* 引号段结束 */
                }
                if (!grid_put_char(g, c)) return 0;
                ++field_len;
                ++i;
            }
            /* 引号之后、分隔符之前的尾巴宽容地并进来（Excel 偶尔写成 "a"b） */
            while (t[i] && t[i] != d && t[i] != L'\n' && t[i] != L'\r') {
                if (!grid_put_char(g, t[i])) return 0;
                ++field_len;
                ++i;
            }
        } else {
            while (t[i] && t[i] != d && t[i] != L'\n' && t[i] != L'\r') {
                if (!grid_put_char(g, t[i])) return 0;
                ++field_len;
                ++i;
            }
        }
        if (!grid_put_char(g, L'\0')) return 0;             /* 每格自己结尾 */
        if (!grid_push_cell(g, field_off, field_len)) return 0;

        if (t[i] == d) { ++i; continue; }                   /* 同一行的下一格 */
        if (t[i] == L'\r') { ++i; if (t[i] == L'\n') ++i; }
        else if (t[i] == L'\n') { ++i; }
        if (!t[i]) break;
        if (!grid_push_row(g, g->n)) return 0;
    }
    /* 丢掉末尾「整行都是空」的多余行（文件末尾留空行时常见） */
    while (g->nrows > 1) {
        int s = g->row_start[g->nrows - 1];
        int all_empty = 1, k;
        for (k = s; k < g->n; ++k) if (g->len[k] > 0) { all_empty = 0; break; }
        if (!all_empty) break;
        g->nrows--;
        g->n = s;
    }
    return 1;
}

static void csv_free(CsvGrid *g)
{
    free(g->buf);
    free(g->off);
    free(g->len);
    free(g->row_start);
    memset(g, 0, sizeof(*g));
}

/* 求每行最大列数 */
static int grid_cols(const CsvGrid *g)
{
    int r, cols = 0;
    for (r = 0; r < g->nrows; ++r) {
        int s = g->row_start[r];
        int e = (r + 1 < g->nrows) ? g->row_start[r + 1] : g->n;
        if (e - s > cols) cols = e - s;
    }
    return cols;
}

/* ------------------------------------------------------------------ */
/* 已加载的表                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    int  key_col;
    int  nbucket;
    int *bucket;      /* 桶 → 行号+1（0 = 空）；头插，故链上是倒序 */
    int *next;        /* 数据行的下一条（行号+1） */
    int *numbucket;   /* 数值键索引 */
    int *numnext;
    wchar_t **keys, **number_keys;
    int key_count;
} QpTableIndex;

struct QpTable {
    wchar_t *buf;
    int     *off;
    int     *len;
    int      grid_rows;   /* 含表头行 */
    int      rows;        /* 数据行数（不含表头） */
    int      cols;
    int      has_header;
    int     *hdr_off;     /* cols 个：表头文本偏移；-1 = 无 */
    QpTableIndex idx[QP_TABLE_MAX_IDX];
    int      nidx;
};

static void index_clear(QpTableIndex *ix)
{
    for (int i = 0; i < ix->key_count; ++i) {
        if (ix->keys) free(ix->keys[i]);
        if (ix->number_keys) free(ix->number_keys[i]);
    }
    free(ix->keys); free(ix->number_keys);
    free(ix->bucket);
    free(ix->next);
    free(ix->numbucket);
    free(ix->numnext);
    memset(ix, 0, sizeof(*ix));
}

static void table_free(QpTable *t)
{
    int i;
    if (!t) return;
    for (i = 0; i < t->nidx; ++i) index_clear(&t->idx[i]);
    free(t->buf);
    free(t->off);
    free(t->len);
    free(t->hdr_off);
    free(t);
}

static const wchar_t *tcell(const QpTable *t, int grid_row, int col)
{
    int i;
    if (!t || grid_row < 0 || grid_row >= t->grid_rows || col < 0 || col >= t->cols)
        return L"";
    i = grid_row * t->cols + col;
    if (t->off[i] < 0) return L"";
    return t->buf + t->off[i];
}

int             qp_table_rows(const QpTable *t)       { return t ? t->rows : 0; }
int             qp_table_cols(const QpTable *t)       { return t ? t->cols : 0; }
int             qp_table_has_header(const QpTable *t) { return t ? t->has_header : 0; }

const wchar_t *qp_table_col_name(const QpTable *t, int col)
{
    if (!t || !t->has_header || col < 0 || col >= t->cols) return NULL;
    if (t->hdr_off[col] < 0) return NULL;
    return t->buf + t->hdr_off[col];
}

const wchar_t *qp_table_cell(const QpTable *t, int row, int col)
{
    int base;
    if (!t) return L"";
    base = t->has_header ? 1 : 0;
    return tcell(t, row + base, col);
}

int qp_table_col_index(const QpTable *t, const wchar_t *name, int fallback)
{
    int i;
    if (!t) return fallback;
    if (!name || !*name) return (fallback >= 0 && fallback < t->cols) ? fallback : -1;
    if (!t->has_header) return (fallback >= 0 && fallback < t->cols) ? fallback : -1;
    for (i = 0; i < t->cols; ++i) {
        const wchar_t *cn = qp_table_col_name(t, i);
        if (!cn) continue;
        if (normalized_equal(cn, name)) return i;
    }
    return -1;                  /* 有表头却找不到：宁可不生效，也不猜一个列号 */
}

static QpTableIndex *table_index(QpTable *t, int key_col)
{
    QpTableIndex *ix;
    int i, r, base;

    if (!t || key_col < 0 || key_col >= t->cols) return NULL;
    for (i = 0; i < t->nidx; ++i)
        if (t->idx[i].key_col == key_col) return &t->idx[i];
    if (t->nidx >= QP_TABLE_MAX_IDX) {          /* 简单淘汰：回收第 0 个 */
        index_clear(&t->idx[0]);
        t->idx[0] = t->idx[--t->nidx];
        memset(&t->idx[t->nidx], 0, sizeof(QpTableIndex));
    }
    ix = &t->idx[t->nidx++];
    memset(ix, 0, sizeof(*ix));
    ix->key_col = key_col;
    ix->nbucket = t->rows * 2 + 16;
    ix->bucket = (int *)calloc((size_t)ix->nbucket, sizeof(int));
    ix->next = (int *)calloc((size_t)(t->rows > 0 ? t->rows : 1), sizeof(int));
    ix->numbucket = (int *)calloc((size_t)ix->nbucket, sizeof(int));
    ix->numnext = (int *)calloc((size_t)(t->rows > 0 ? t->rows : 1), sizeof(int));
    ix->key_count = t->rows;
    ix->keys = calloc((size_t)(t->rows > 0 ? t->rows : 1), sizeof(*ix->keys));
    ix->number_keys = calloc((size_t)(t->rows > 0 ? t->rows : 1), sizeof(*ix->number_keys));
    if (!ix->bucket || !ix->next || !ix->numbucket || !ix->numnext || !ix->keys || !ix->number_keys) {
        index_clear(ix);
        --t->nidx;
        return NULL;
    }
    base = t->has_header ? 1 : 0;
    for (r = 0; r < t->rows; ++r) {
        wchar_t *k, *kn;
        int valid_number = 0;
        unsigned b;
        k = ix->keys[r] = qp_table_text_key(tcell(t, r + base, key_col));
        if (!k) { index_clear(ix); --t->nidx; return NULL; }
        if (!k[0]) continue;                     /* 空键不进索引（空键一律不命中） */
        b = hash_w(k) % (unsigned)ix->nbucket;
        ix->next[r] = ix->bucket[b];
        ix->bucket[b] = r + 1;
        kn = ix->number_keys[r] = qp_table_numeric_key_checked(k, &valid_number);
        if (valid_number && !kn) { index_clear(ix); --t->nidx; return NULL; }
        if (kn) {
            b = hash_w(kn) % (unsigned)ix->nbucket;
            ix->numnext[r] = ix->numbucket[b];
            ix->numbucket[b] = r + 1;
        }
    }
    return ix;
}

/* ------------------------------------------------------------------ */
/* 命中行缓冲                                                          */
/*                                                                    */
/* 「一个键命中几行」不该有上限：名单里同一个部门、同一个班组几百上千人  */
/* 是常态，按上限截断会让「取第一条」悄悄取到中间某一条。所以这里先用手 */
/* 边的小数组接住常见的少数几行，真的超了再往堆上翻倍长 —— 上限只由调用 */
/* 者给的输出缓冲决定，不会少算。                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    int  inline_rows[QP_TABLE_HITS_INLINE];
    int *rows;                 /* 默认指向 inline_rows，溢出后指向堆 */
    int  n;
    int  cap;
} RowHits;

static void hits_init(RowHits *h)
{
    h->rows = h->inline_rows;
    h->n = 0;
    h->cap = QP_TABLE_HITS_INLINE;
}

static void hits_free(RowHits *h)
{
    if (h->rows && h->rows != h->inline_rows) free(h->rows);
    h->rows = h->inline_rows;
    h->n = 0;
    h->cap = QP_TABLE_HITS_INLINE;
}

/* 追加一行；返回 0 = 内存不够（已经收到的仍然有效，调用者据此收尾） */
static int hits_push(RowHits *h, int row)
{
    if (h->n >= h->cap) {
        int nc = h->cap * 2;
        int *nr;
        if (h->rows == h->inline_rows) {
            nr = (int *)malloc((size_t)nc * sizeof(int));
            if (nr) memcpy(nr, h->inline_rows, (size_t)h->n * sizeof(int));
        } else {
            nr = (int *)realloc(h->rows, (size_t)nc * sizeof(int));
        }
        if (!nr) return 0;
        h->rows = nr;
        h->cap = nc;
    }
    h->rows[h->n++] = row;
    return 1;
}

/* 走一条哈希链，把命中行按**文件顺序**收进 hits，返回这次收到几条。
   链是头插的（链头是行号最大的一条），所以边走边收得到的是倒序，
   收完把这一段整体反转一次。没有条数上限。 */
static int index_collect(const QpTable *t, const QpTableIndex *ix, const int *bucket,
                         const int *next, int nbucket, const wchar_t *key,
                         int numeric, RowHits *hits)
{
    unsigned b;
    int r, i, start = hits->n;

    if (!t || !key || !*key || !bucket || nbucket <= 0) return 0;
    b = hash_w(key) % (unsigned)nbucket;
    for (r = bucket[b]; r != 0; r = next[r - 1]) {
        int row = r - 1;
        const wchar_t *cell = numeric ? ix->number_keys[row] : ix->keys[row];
        if (!cell) continue;
        if (wcscmp(cell, key) != 0) continue;    /* 哈希碰撞要再比一次 */
        if (!hits_push(hits, row)) return -1;
    }
    for (i = 0; i < (hits->n - start) / 2; ++i) {
        int a = start + i, z = hits->n - 1 - i;
        int tmp = hits->rows[a];
        hits->rows[a] = hits->rows[z];
        hits->rows[z] = tmp;
    }
    return hits->n - start;
}

/* 先用字符串键查，查不到再用数值键查（这样两种写法都能对上）。
   命中行按文件顺序收进 hits（调用者先 hits_init、用完 hits_free）。 */
static int table_find_rows(QpTable *t, int key_col, const wchar_t *key, RowHits *hits)
{
    QpTableIndex *ix;
    wchar_t *k, *kn = NULL;
    int n;

    hits->n = 0;
    k = qp_table_text_key(key);
    if (!k) return -1;
    if (!k[0]) { free(k); return 0; }
    ix = table_index(t, key_col);
    if (!ix) { free(k); return -1; }
    n = index_collect(t, ix, ix->bucket, ix->next, ix->nbucket, k, 0, hits);
    if (n == 0) {
        int valid_number = 0;
        kn = qp_table_numeric_key_checked(k, &valid_number);
        if (!kn && valid_number) n = -1;
        if (kn) n = index_collect(t, ix, ix->numbucket, ix->numnext, ix->nbucket,
                          kn, 1, hits);
    }
    free(k); free(kn);
    return n;
}

int qp_table_count(QpTable *t, int key_col, const wchar_t *key)
{
    RowHits hits;
    int n;
    if (!t) return 0;
    hits_init(&hits);
    n = table_find_rows(t, key_col, key, &hits);
    hits_free(&hits);
    return n;
}


/* 已写进 out 的每一项，记下它在 out 里的起点与长度。
   去重就比这一份记录（比的是刚写进去的那段原文，不会有子串误判）；
   顺带保证输出末尾永远是完整的一项，而不是被截断的半个值。 */
typedef struct {
    int off;
    int len;
} OutMark;

struct QpTableMatch {
    QpTable *table;
    int key_col, numeric, count, first, last, materialized;
    wchar_t *key;
    RowHits *hits;
};

static void match_bounds(QpTableMatch *m,const QpTableIndex *ix)
{
    const int *bucket=m->numeric?ix->numbucket:ix->bucket;
    const int *next=m->numeric?ix->numnext:ix->next;
    unsigned b=hash_w(m->key)%(unsigned)ix->nbucket;
    for(int r=bucket[b];r;r=next[r-1]) {
        int row=r-1;
        const wchar_t *cell=m->numeric?ix->number_keys[row]:ix->keys[row];
        if(!cell || wcscmp(cell,m->key))continue;
        if(!m->count || row<m->first)m->first=row;
        if(!m->count || row>m->last)m->last=row;
        ++m->count;
    }
}

QpTableMatch *qp_table_match(QpTable *t,int key_col,const wchar_t *key)
{
    QpTableMatch *m=calloc(1,sizeof(*m));
    if(!m)return NULL;
    m->table=t;m->key_col=key_col;
    if(!t || key_col<0 || key_col>=t->cols)return m;
    m->key=qp_table_text_key(key);
    if(!m->key)goto fail;
    if(!m->key[0])return m;
    QpTableIndex *ix=table_index(t,key_col);
    if(!ix)goto fail;
    match_bounds(m,ix);
    if(!m->count) {
        int numeric=0;
        wchar_t *number=qp_table_numeric_key_checked(m->key,&numeric);
        if(numeric && !number)goto fail;
        if(number) {
            free(m->key);m->key=number;m->numeric=1;
            match_bounds(m,ix);
        }
    }
    return m;
fail:
    qp_table_match_free(m);return NULL;
}

void qp_table_match_free(QpTableMatch *m)
{
    if(!m)return;
    if(m->hits){hits_free(m->hits);free(m->hits);}
    free(m->key);free(m);
}

QpTableLookupResult qp_table_match_value(const QpTableMatch *match,int val_col,
    int mode,const wchar_t *sep,int unique,wchar_t *out,size_t cap)
{
    QpTableLookupResult result={0};
    OutMark *marks=NULL;
    int count=0,markcap=0;
    if(!out || !cap){result.failed=1;return result;}
    out[0]=0;
    if(!match){result.failed=1;return result;}
    QpTable *t=match->table;
    if(!t || val_col<0 || val_col>=t->cols)return result;
    int n=match->count,base=t->has_header?1:0;
    if(!n)goto done;
    result.found=1;
    if(mode==LOOKUP_ALL){
        /* Logical const query; only its private ordered-row cache is mutable. */
        QpTableMatch *m=(QpTableMatch *)match;
        if(!m->materialized) {
            if(!m->hits) {
                m->hits=malloc(sizeof(*m->hits));
                if(!m->hits){result.failed=1;goto done;}
                hits_init(m->hits);
            }
            QpTableIndex *ix=table_index(t,m->key_col);
            if(!ix){result.failed=1;goto done;}
            m->hits->n=0;
            int loaded=index_collect(t,ix,m->numeric?ix->numbucket:ix->bucket,
                m->numeric?ix->numnext:ix->next,ix->nbucket,m->key,m->numeric,m->hits);
            if(loaded<0){result.failed=1;goto done;}
            m->materialized=1;
        }
        const wchar_t *separator=sep && *sep?sep:L"、";
        size_t seplen=wcslen(separator);
        for(int i=0;i<n;++i){
            const wchar_t *value=tcell(t,m->hits->rows[i]+base,val_col);
            size_t len=wcslen(value);int duplicate=0;
            if(!len)continue;
            if(unique)for(int j=0;j<count;++j)
                if((size_t)marks[j].len==len && !wcsncmp(out+marks[j].off,value,len)){duplicate=1;break;}
            if(duplicate)continue;
            size_t prefix=result.written?seplen:0;
            if(prefix>cap-result.written-1 || len>cap-result.written-prefix-1){result.truncated=1;break;}
            if(unique && count==markcap){
                int nc=markcap?markcap*2:64;
                OutMark *grown=realloc(marks,(size_t)nc*sizeof(*grown));
                if(!grown){result.failed=1;break;}
                marks=grown;markcap=nc;
            }
            if(prefix){memcpy(out+result.written,separator,seplen*sizeof(*out));result.written+=seplen;}
            if(unique){marks[count].off=(int)result.written;marks[count].len=(int)len;++count;}
            memcpy(out+result.written,value,len*sizeof(*out));result.written+=len;out[result.written]=0;
        }
    }else{
        int row=mode==LOOKUP_LAST?match->last:match->first;
        const wchar_t *value=tcell(t,row+base,val_col);
        size_t len=wcslen(value),take=qp_utf16_prefix(value,len,cap-1);
        memcpy(out,value,take*sizeof(*out));out[take]=0;
        result.written=take;result.truncated=take<len;
    }
done:
    free(marks);
    if(result.failed){out[0]=0;result.written=0;}
    return result;
}

QpTableLookupResult qp_table_lookup_checked(QpTable *t,int key_col,const wchar_t *key,int val_col,
    int mode,const wchar_t *sep,int unique,wchar_t *out,size_t cap)
{
    QpTableMatch *m=qp_table_match(t,key_col,key);
    QpTableLookupResult result=qp_table_match_value(m,val_col,mode,sep,unique,out,cap);
    qp_table_match_free(m);
    return result;
}

int qp_table_lookup(QpTable *t,int key_col,const wchar_t *key,int val_col,
    int mode,const wchar_t *sep,int unique,wchar_t *out,size_t cap)
{
    QpTableLookupResult result=qp_table_lookup_checked(t,key_col,key,val_col,mode,sep,unique,out,cap);
    return result.found && !result.failed;
}

/* ------------------------------------------------------------------ */
/* 从文本造表                                                          */
/* ------------------------------------------------------------------ */

static QpTable *table_build(const wchar_t *text, const QpTableOpts *opts,
                            wchar_t *err, size_t errcap)
{
    CsvGrid g;
    QpTable *t;
    int cols, r, c;

    memset(&g, 0, sizeof(g));
    if (!csv_parse(text, (opts && opts->delimiter != QP_DELIM_AUTO) ? opts->delimiter
                                                                   : detect_delim(text),
                   &g)) {
        set_err(err, errcap, L"无法解析数据表。请检查文件内容及可用内存。");
        csv_free(&g);
        return NULL;
    }
    cols = grid_cols(&g);
    if (g.nrows <= 0 || g.n <= 0) {
        set_err(err, errcap, L"数据表为空。请选择包含数据的文件。");
        csv_free(&g);
        return NULL;
    }
    if (cols <= 0) {
        set_err(err, errcap, L"数据表未包含可读取的列。");
        csv_free(&g);
        return NULL;
    }

    t = (QpTable *)calloc(1, sizeof(QpTable));
    if (!t) { csv_free(&g); set_err(err, errcap, L"内存不足"); return NULL; }
    t->cols = cols;
    t->grid_rows = g.nrows;
    t->has_header = (opts && !opts->has_header) ? 0 : 1;
    if (g.nrows == 1) t->has_header = 0;         /* 只有一行就没表头可言 */
    t->rows = g.nrows - (t->has_header ? 1 : 0);
    t->buf = g.buf;                              /* 接管文本缓冲 */
    g.buf = NULL;
    t->off = (int *)malloc((size_t)t->grid_rows * (size_t)cols * sizeof(int));
    t->len = (int *)malloc((size_t)t->grid_rows * (size_t)cols * sizeof(int));
    t->hdr_off = (int *)calloc((size_t)cols, sizeof(int));
    if (!t->off || !t->len || !t->hdr_off) {
        csv_free(&g);
        table_free(t);
        set_err(err, errcap, L"内存不足");
        return NULL;
    }
    for (r = 0; r < t->grid_rows; ++r) {
        int s = g.row_start[r];
        int e = (r + 1 < g.nrows) ? g.row_start[r + 1] : g.n;
        for (c = 0; c < cols; ++c) {
            int at = r * cols + c;
            if (s + c < e) {
                t->off[at] = g.off[s + c];
                t->len[at] = g.len[s + c];
            } else {
                t->off[at] = -1;                 /* 这一行短了：补空格子 */
                t->len[at] = 0;
            }
        }
    }
    if (t->has_header) {
        for (c = 0; c < cols; ++c) t->hdr_off[c] = t->off[c];
    } else {
        for (c = 0; c < cols; ++c) t->hdr_off[c] = -1;
    }
    csv_free(&g);
    return t;
}

/* ------------------------------------------------------------------ */
/* 缓存（含「文件被覆盖了」的自动失效）                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    wchar_t  id[QP_ID_MAX];
    QpTable *t;
    uint64_t mtime;
    uint64_t size;
    int      used;
    int      failed;
    wchar_t  err[256];
} CacheSlot;

static CacheSlot s_cache[QP_TABLE_CACHE_MAX];

typedef struct QpReadTable {
    wchar_t id[QP_ID_MAX];
    QpTable *table;
    int retired;
    wchar_t error[256];
    struct QpReadTable *next;
} QpReadTable;
static QpReadTable *s_read_tables;

int qp_table_read_begin(void)
{
    if (s_read_depth == UINT_MAX) return 0;
    if (s_read_depth) { ++s_read_depth; return 1; }
    DataTableRef *head = NULL, **tail = &head;
    for (const DataTableRef *r = s_refs; r; r = r->next) {
        DataTableRef *copy = malloc(sizeof(*copy));
        if (!copy) { dtref_list_free(head); return 0; }
        *copy = *r; copy->next = NULL;
        *tail = copy; tail = &copy->next;
    }
    s_read_refs = head;
    s_read_depth = 1;
    return 1;
}

void qp_table_read_end(void)
{
    if (!s_read_depth || --s_read_depth) return;
    while (s_read_tables) {
        QpReadTable *next = s_read_tables->next;
        if (s_read_tables->retired) table_free(s_read_tables->table);
        free(s_read_tables); s_read_tables = next;
    }
    dtref_list_free(s_read_refs); s_read_refs = NULL;
}

static void cache_retire(QpTable *table)
{
    if (!table) return;
    for (QpReadTable *r = s_read_tables; r; r = r->next) if (r->table == table) {
        r->retired = 1;
        return;
    }
    table_free(table);
}

static int file_stamp(const wchar_t *path, uint64_t *mtime, uint64_t *size)
{
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA attrs;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &attrs)) return 0;
    if (mtime) *mtime = ((uint64_t)attrs.ftLastWriteTime.dwHighDateTime << 32) |
                        attrs.ftLastWriteTime.dwLowDateTime;
    if (size) *size = ((uint64_t)attrs.nFileSizeHigh << 32) | attrs.nFileSizeLow;
#else
    struct stat st;                        /* 本机（含 Linux）走窄路径 stat */
    char *u8 = qp_wide_to_utf8_alloc(path);
    int rc;
    if (!u8) return 0;
    rc = stat(u8, &st);
    free(u8);
    if (rc != 0) return 0;
    if (mtime) {
#if defined(__APPLE__)
        *mtime = (uint64_t)st.st_mtimespec.tv_sec * 1000000000u + (uint64_t)st.st_mtimespec.tv_nsec;
#else
        *mtime = (uint64_t)st.st_mtim.tv_sec * 1000000000u + (uint64_t)st.st_mtim.tv_nsec;
#endif
    }
    if (size) *size = (uint64_t)st.st_size;
#endif
    return 1;
}

static CacheSlot *cache_find(const wchar_t *id, int *free_slot)
{
    int i;
    if (free_slot) *free_slot = -1;
    for (i = 0; i < QP_TABLE_CACHE_MAX; ++i)
        if (s_cache[i].used && wcscmp(s_cache[i].id, id) == 0) return &s_cache[i];
    for (i = 0; i < QP_TABLE_CACHE_MAX; ++i)
        if (!s_cache[i].used) { if (free_slot) *free_slot = i; return NULL; }
    if (free_slot) *free_slot = 0;               /* 满了：回收第 0 个 */
    return NULL;
}

void qp_table_invalidate(const wchar_t *id)
{
    int i;
    for (i = 0; i < QP_TABLE_CACHE_MAX; ++i) {
        if (!s_cache[i].used) continue;
        if (id && *id && wcscmp(s_cache[i].id, id) != 0) continue;
        cache_retire(s_cache[i].t);
        memset(&s_cache[i], 0, sizeof(s_cache[i]));
    }
}

void qp_table_invalidate_all(void) { qp_table_invalidate(NULL); }

static QpTable *table_get_cached(const wchar_t *id, wchar_t *err, size_t errcap)
{
    CacheSlot *s;
    DataTableRef *ref;
    int slot = -1, need = 0;

    if (err && errcap) err[0] = L'\0';
    if (!id || !*id) { set_err(err, errcap, L"尚未选择数据表。"); return NULL; }
    ref = qp_table_find_ref(id);
    if (!ref) { set_err(err, errcap, L"数据表不存在（可能已被删除）"); return NULL; }

    s = cache_find(id, &slot);
    if (s && s->used) {
        uint64_t mt = 0, sz = 0;
        if (s->failed || !file_stamp(ref->path, &mt, &sz) || mt != s->mtime || sz != s->size)
            need = 1; /* 下一次查询立即检测文件变化，失败的缓存可以恢复。 */
    }
    if (s && !need) {
        if (s->failed) { set_err(err, errcap, L"%ls", s->err); return NULL; }
        return s->t;
    }
    if (!s) {
        if (slot < 0) return NULL;
        s = &s_cache[slot];
        cache_retire(s->t);
        memset(s, 0, sizeof(*s));
        qp_strlcpy(s->id, id, QP_ID_MAX);
        s->used = 1;
    } else {
        cache_retire(s->t);
        s->t = NULL;
    }
    {
        wchar_t e[256];
        wchar_t *text;
        uint64_t mt = 0, sz = 0;
        int used = ref->encoding;
        QpTableOpts opts;
        e[0] = L'\0';
        for (int attempt = 0; attempt < 3; ++attempt) {
            uint64_t before_time = 0, before_size = 0;
            if (!file_stamp(ref->path, &before_time, &before_size)) {
                set_err(e, 256, L"无法读取数据表文件状态。请检查文件位置和读取权限。");
                break;
            }
            text = decode_file(ref->path, ref->encoding, &used, e, 256);
            if (!text) break;
            if (!file_stamp(ref->path, &mt, &sz) || before_time != mt || before_size != sz) {
                free(text);
                set_err(e, 256, L"数据表在读取期间持续变化。请等待文件写入完成后重试。");
                continue;
            }
            opts.encoding = used;
            opts.delimiter = ref->delimiter;
            opts.has_header = ref->has_header;
            s->t = table_build(text, &opts, e, 256);
            free(text);
            break;
        }
        s->mtime = mt;
        s->size = sz;
        if (!s->t) {
            s->failed = 1;
            qp_strlcpy(s->err, e[0] ? e : L"数据表读取失败", 256);
            set_err(err, errcap, L"%ls", s->err);
            return NULL;
        }
        s->failed = 0;
        s->err[0] = L'\0';
    }
    return s->t;
}

QpTable *qp_table_get(const wchar_t *id, wchar_t *err, size_t errcap)
{
    if (!s_read_depth || !id || !*id) return table_get_cached(id, err, errcap);
    for (QpReadTable *r = s_read_tables; r; r = r->next) if (!wcscmp(r->id, id)) {
        if (err && errcap) qp_strlcpy(err, r->error, errcap);
        return r->table;
    }
    QpReadTable *r = calloc(1, sizeof(*r));
    if (!r) { set_err(err, errcap, L"内存不足，无法固定数据表快照。请关闭不需要的窗口后重试。"); return NULL; }
    qp_strlcpy(r->id, id, QP_ID_MAX);
    r->table = table_get_cached(id, r->error, 256);
    r->next = s_read_tables; s_read_tables = r;
    if (err && errcap) qp_strlcpy(err, r->error, errcap);
    return r->table;
}

int qp_table_probe(const wchar_t *id, int *rows, int *cols,
                   wchar_t *err, size_t errcap)
{
    QpTable *t = qp_table_get(id, err, errcap);
    if (!t) return 0;
    if (rows) *rows = t->rows;
    if (cols) *cols = t->cols;
    return 1;
}

/* ------------------------------------------------------------------ */
/* 导入预览                                                            */
/* ------------------------------------------------------------------ */

void qp_table_preview_free(QpTablePreview *p)
{
    int i, n;
    if (!p) return;
    n = p->rows * p->cols;
    for (i = 0; i < n; ++i) free(p->preview[i]);
    for (i = 0; i < p->cols; ++i) free(p->col_name[i]);
    free(p->preview);
    free(p->col_name);
    free(p->col_dup);
    free(p);
}

QpTablePreview *qp_table_preview(const wchar_t *path, const QpTableOpts *opts,
                                 int max_rows, wchar_t *err, size_t errcap)
{
    int used = QP_ENC_AUTO;
    wchar_t *text;
    CsvGrid g;
    QpTablePreview *pv;
    int cols, r, c, show, base;

    if (err && errcap) err[0] = L'\0';
    if (!path || !*path) { set_err(err, errcap, L"尚未指定数据表文件。"); return NULL; }
    text = decode_file(path, opts ? opts->encoding : QP_ENC_AUTO, &used, err, errcap);
    if (!text) return NULL;

    memset(&g, 0, sizeof(g));
    if (!csv_parse(text, (opts && opts->delimiter != QP_DELIM_AUTO) ? opts->delimiter
                                                                   : detect_delim(text),
                   &g)) {
        set_err(err, errcap, L"无法解析数据表。请检查文件内容及可用内存。");
        csv_free(&g);
        free(text);
        return NULL;
    }
    cols = grid_cols(&g);
    if (g.nrows <= 0 || g.n <= 0 || cols <= 0) {
        set_err(err, errcap, L"数据表为空。请选择包含数据的文件。");
        csv_free(&g);
        free(text);
        return NULL;
    }

    pv = (QpTablePreview *)calloc(1, sizeof(QpTablePreview));
    if (!pv) { csv_free(&g); free(text); set_err(err, errcap, L"内存不足"); return NULL; }
    pv->encoding = used;
    pv->delimiter = (opts && opts->delimiter != QP_DELIM_AUTO) ? opts->delimiter
                                                              : detect_delim(text);
    pv->has_header = (opts && !opts->has_header) ? 0 : 1;
    if (g.nrows == 1) pv->has_header = 0;
    pv->cols = cols;
    pv->total_rows = g.nrows - (pv->has_header ? 1 : 0);
    if (pv->total_rows < 0) pv->total_rows = 0;

    pv->col_name = (wchar_t **)calloc((size_t)cols, sizeof(wchar_t *));
    pv->col_dup = (int *)calloc((size_t)cols, sizeof(int));
    if (!pv->col_name || !pv->col_dup) {
        csv_free(&g); free(text); qp_table_preview_free(pv);
        set_err(err, errcap, L"内存不足");
        return NULL;
    }
    for (c = 0; c < cols; ++c) {
        if (pv->has_header && c < g.n && g.off[c] >= 0 && g.len[c] > 0) {
            int len = g.len[c];
            pv->col_name[c] = (wchar_t *)malloc(((size_t)len + 1) * sizeof(wchar_t));
            if (pv->col_name[c]) {
                memcpy(pv->col_name[c], g.buf + g.off[c], (size_t)len * sizeof(wchar_t));
                pv->col_name[c][len] = L'\0';
            }
        }
        if (!pv->col_name[c]) {
            wchar_t buf[64];
            swprintf(buf, 64, L"列 %d", c + 1);
            buf[63] = L'\0';
            pv->col_name[c] = qp_wcsdup(buf);
        }
    }
    for (c = 0; c < cols; ++c) {                 /* 表头重名统计（体检用） */
        int k;
        if (qp_is_blank(pv->col_name[c])) continue;
        for (k = 0; k < cols; ++k) {
            if (normalized_equal(pv->col_name[c], pv->col_name[k])) ++pv->col_dup[c];
        }
    }

    show = pv->total_rows;
    if (max_rows <= 0) max_rows = 20;
    if (show > max_rows) show = max_rows;
    pv->rows = show;
    if (show > 0) {
        pv->preview = (wchar_t **)calloc((size_t)show * (size_t)cols, sizeof(wchar_t *));
        if (!pv->preview) {
            csv_free(&g); free(text); qp_table_preview_free(pv);
            set_err(err, errcap, L"内存不足");
            return NULL;
        }
    }
    base = pv->has_header ? 1 : 0;
    for (r = 0; r < show; ++r) {
        int s = g.row_start[r + base];
        int e = (r + base + 1 < g.nrows) ? g.row_start[r + base + 1] : g.n;
        for (c = 0; c < cols; ++c) {
            int at = r * cols + c;
            if (s + c < e && g.off[s + c] >= 0 && g.len[s + c] > 0) {
                int len = g.len[s + c];
                pv->preview[at] = (wchar_t *)malloc(((size_t)len + 1) * sizeof(wchar_t));
                if (pv->preview[at]) {
                    memcpy(pv->preview[at], g.buf + g.off[s + c],
                           (size_t)len * sizeof(wchar_t));
                    pv->preview[at][len] = L'\0';
                }
            }
            if (!pv->preview[at]) pv->preview[at] = qp_wcsdup(L"");
        }
    }
    csv_free(&g);
    free(text);
    return pv;
}
