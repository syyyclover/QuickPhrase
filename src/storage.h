/*
 * storage.h —— 模板/分组/设置的 JSON 持久化（cJSON）
 *
 * 数据文件：%APPDATA%\QuickPhrase\data.json      （模板 + 分组，UTF-8）
 *           %APPDATA%\QuickPhrase\settings.json  （全局设置，UTF-8）
 *
 * 内部字符串为 wchar_t(UTF-16)，仅在此层与 UTF-8 互转。
 */
#ifndef QP_STORAGE_H
#define QP_STORAGE_H

#include "engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------- 编码转换 ------------------------------- */
/* 返回写入/所需的字符数（不含结尾 NUL）；dst 为 NULL 时仅计算长度 */
size_t   qp_utf8_to_wide(const char *src, wchar_t *dst, size_t cap);
size_t   qp_wide_to_utf8(const wchar_t *src, char *dst, size_t cap);
wchar_t *qp_utf8_to_wide_alloc(const char *src);   /* free() 释放 */
char    *qp_wide_to_utf8_alloc(const wchar_t *src);/* free() 释放 */

/* ------------------------- 序列化（可独立测试） ------------------- */
char    *appdata_to_json(const AppData *d);        /* 返回 UTF-8 文本，free() 释放 */
AppData *appdata_from_json(const char *utf8);      /* 解析失败返回 NULL */
char    *settings_to_json(const Settings *s);
int      settings_from_json(const char *utf8, Settings *out);

/* ------------------------- 路径 ----------------------------------- */
/* 设置自定义数据目录（空字符串或 NULL = 恢复默认 %APPDATA%\QuickPhrase） */
void storage_set_data_dir(const wchar_t *dir);
int storage_get_data_dir(wchar_t *out, size_t cap);
int storage_get_data_path(wchar_t *out, size_t cap);
int storage_get_settings_path(wchar_t *out, size_t cap);
int storage_ensure_dir(void);

/* ------------------------- 文件读写 ------------------------------- */
/* 读取整个 UTF-8 文本文件（自动跳过 BOM），free() 释放 */
char *storage_read_text(const wchar_t *path);
int   storage_write_text(const wchar_t *path, const char *utf8);

/* 载入数据：文件不存在时返回内置示例数据（并置 *created=1） */
AppData *storage_load(int *created);
int      storage_save(const AppData *d);

/* 导入导出 */
int storage_export(const AppData *d, const wchar_t *path);
#define QP_IMPORT_MERGE     0   /* 同 id 覆盖，新 id 追加 */
#define QP_IMPORT_REPLACE   1   /* 清空后整体替换 */
int storage_import(AppData *d, const wchar_t *path, int mode,
                   int *added, int *updated);

/* 设置 */
void settings_defaults(Settings *s);
int  settings_load(Settings *s);
int  settings_save(const Settings *s);

#ifdef __cplusplus
}
#endif

#endif /* QP_STORAGE_H */
