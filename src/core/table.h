/*
 * table.h —— 数据表（名单 / 联系表）：读 CSV/TSV、建索引、按键查询
 *
 * 设计要点（都为了「快、准、稳」）：
 *   · 只有「引用」进 data.json（名字 / 路径 / 编码 / 分隔符 / 有无表头），
 *     表内容永远以磁盘上那个文件为准，每周覆盖同一个文件即可。
 *   · 表按名字（id）引用，规则里不写文件路径；文件换了内容，规则自动用新的。
 *   · 解析一次 → 常驻内存 → 按「键列」建哈希索引；之后每次查表是 O(1)，
 *     与表有多少行无关。数万行对内存和 CPU 都不算大。
 *   · 命中语义写死并可单测：键先按「去空白 + 大小写不敏感」的字符串比，
 *     再按数值比（这样 1.38E+10 / 13800138000 / 13800138000.0 视为同一个键）。
 *   · 键可能不唯一：命中策略有 取第一条 / 取最后一条 / 全部按分隔符拼接（可去重）。
 *
 * 本文件与实现都不依赖任何 Windows API（GBK 解码走 Windows API 的那一小段
 * 用 #ifdef 圈起来），因此可以在本机直接单元测试。
 */
#ifndef QP_TABLE_H
#define QP_TABLE_H

#include "engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 已加载的表（不透明） */
typedef struct QpTable QpTable;

/* 解析参数：导入时可以先自动识别，再由用户覆盖 */
typedef struct {
    int encoding;      /* TableEncoding */
    int delimiter;     /* TableDelim    */
    int has_header;    /* 1 = 第一行是列名 */
} QpTableOpts;

/* --------------------------- 引用登记 ----------------------------- */
/* 把 data.json 里的表清单交给本模块（只借指针，不接管释放）。
   引擎查表、摘要显示表名、体检判断「表在不在」都靠它。 */
void            qp_table_set_refs(DataTableRef *refs);
/* 表清单要被释放时调用：只有当前登记的就是这一份才清空，避免误伤别的 AppData */
void            qp_table_forget_refs(const DataTableRef *refs);
DataTableRef   *qp_table_find_ref(const wchar_t *id);
const wchar_t  *qp_table_ref_name(const wchar_t *id);   /* 找不到返回 NULL */

/* --------------------------- 加载与缓存 --------------------------- */
/* 按 id 取表（带缓存，文件被覆盖后需 invalidate）。失败返回 NULL 并写 err。 */
QpTable *qp_table_get(const wchar_t *id, wchar_t *err, size_t errcap);
/* Freeze refs/first table access for a nested evaluation range. On OOM begin
   returns 0 without entering; borrowed tables remain alive until final end. */
int      qp_table_read_begin(void);
void     qp_table_read_end(void);
/* 丢掉某张表（或全部）的缓存：文件换内容后调用 */
void     qp_table_invalidate(const wchar_t *id);
void     qp_table_invalidate_all(void);
/* 只读地看一下「表能不能读、有多少行」，不改变缓存；返回 1 = 可读 */
int      qp_table_probe(const wchar_t *id, int *rows, int *cols,
                        wchar_t *err, size_t errcap);

/* --------------------------- 读表信息 ----------------------------- */
int             qp_table_rows(const QpTable *t);
int             qp_table_cols(const QpTable *t);
int             qp_table_has_header(const QpTable *t);
/* 第 col 列的列名（没表头返回 NULL） */
const wchar_t  *qp_table_col_name(const QpTable *t, int col);
/* 按列名找列下标（大小写不敏感、去首尾空白）；找不到返回 -1。
   没有表头时返回 fallback（通常是规则里存的列号）。 */
int             qp_table_col_index(const QpTable *t, const wchar_t *name,
                                   int fallback);
/* 单元格文本；越界返回 L"" */
const wchar_t  *qp_table_cell(const QpTable *t, int row, int col);

/* --------------------------- 查询 -------------------------------- */
/* 命中策略见 engine.h 的 LookupHitMode */
/* 按 key 在 key_col 列里查，取 val_col 列的值写进 out。
   找到返回 1；没找到返回 0（out 置空）。
   mode == LOOKUP_ALL 时用 sep 拼接所有命中行；unique 为真则同一个值只留一份。
   命中行数没有上限；结果装不下 cap 时，停在最后一个**完整**的取值上，
   不会写出被截断的半个值。 */
int qp_table_lookup(QpTable *t, int key_col, const wchar_t *key, int val_col,
                    int mode, const wchar_t *sep, int unique,
                    wchar_t *out, size_t cap);
typedef struct { int found, truncated, failed; size_t written; } QpTableLookupResult;
/* Owned query; the table is borrowed and must outlive the match. Within an
   evaluation read scope, free every match before qp_table_read_end(). ALL's
   ordered row list is materialized once; FIRST/LAST do not allocate that list. */
typedef struct QpTableMatch QpTableMatch;
QpTableMatch *qp_table_match(QpTable *table,int key_col,const wchar_t *key);
void qp_table_match_free(QpTableMatch *match);
QpTableLookupResult qp_table_match_value(const QpTableMatch *match,int val_col,int mode,
    const wchar_t *sep,int unique,wchar_t *out,size_t cap);
QpTableLookupResult qp_table_lookup_checked(QpTable *t,int key_col,const wchar_t *key,int val_col,
    int mode,const wchar_t *sep,int unique,wchar_t *out,size_t cap);
/* 这个键在表里命中几行（体检与「为什么没查到」的诊断用）。精确计数，不封顶 */
int qp_table_count(QpTable *t, int key_col, const wchar_t *key);

/* --------------------------- 导入预览 ----------------------------- */
/* 试读一个 CSV/TSV 文件：自动识别参数 + 列名 + 前若干行，给导入对话框确认用。
   失败返回 NULL 并写 err。max_rows <= 0 表示「只要能读通的头部」。 */
typedef struct {
    int       encoding;      /* 实际采用的编码（可能是识别结果） */
    int       delimiter;     /* 实际采用的分隔符 */
    int       has_header;
    int       rows;          /* preview 里有几行 */
    int       total_rows;    /* 文件里一共有几行（不含表头） */
    int       cols;
    wchar_t **col_name;      /* cols 个：列名（没表头时是「列 1」这种占位） */
    wchar_t **preview;       /* rows*cols 个单元格文本 */
    /* 表头重复列名：每个列名出现的次数（>1 即重名，体检会提示按名取列有歧义） */
    int      *col_dup;
    void     *priv;
} QpTablePreview;

QpTablePreview *qp_table_preview(const wchar_t *path, const QpTableOpts *opts,
                                 int max_rows, wchar_t *err, size_t errcap);
void            qp_table_preview_free(QpTablePreview *p);

/* 枚举名称（界面下拉用） */
const wchar_t *qp_table_encoding_name(int enc);
const wchar_t *qp_table_delim_name(int delim);
const wchar_t *qp_table_hit_name(int hit);       /* 第一条 / 最后一条 / 全部 */

/* 存档用的稳定键名（与界面文字解耦，改文案不影响读档） */
const char *qp_table_encoding_key(int enc);
const char *qp_table_delim_key(int delim);
const char *qp_table_hit_key(int hit);

/* 键名 -> 枚举；认不出来时回落到“自动识别 / 取第一条” */
TableEncoding qp_table_encoding_from_key(const char *k);
TableDelim    qp_table_delim_from_key(const char *k);
LookupHitMode qp_table_hit_from_key(const char *k);

#ifdef __cplusplus
}
#endif

#endif /* QP_TABLE_H */
