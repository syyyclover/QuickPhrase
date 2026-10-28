/*
 * regex.h —— QuickPhrase 内置正则引擎（纯 C11，无第三方依赖）
 *
 * 一个自包含的回溯正则引擎，供解析规则第四种类型「正则匹配」使用。
 * 与 engine.c 一样不依赖任何 Windows API，可在任意平台编译并单元测试。
 *
 * 支持的语法（涵盖日常提取所需的全部能力）：
 *   字面字符、.、[...] / [^...]（含范围 a-z、转义 \d \w \s 及其取反）、
 *   ^ $ 锚点、* + ? {m} {m,} {m,n} 量词（贪婪与懒惰 *? +? ?? {m,n}?）、
 *   分支 |、捕获组 (…)、非捕获组 (?:…)、反向引用 \1…\9、
 *   转义 \. \* \+ \? \( \) \[ \] \{ \} \| \^ \$ \\ 以及 \t \n \r \f \v。
 *
 * 匹配语义：从左到右搜索第一个匹配；整体大小写不敏感（与引擎其它规则
 * 「大小写不敏感」的全局口径一致）；^ 匹配文本开头、$ 匹配文本结尾。
 */
#ifndef QP_REGEX_H
#define QP_REGEX_H

#include <stddef.h>
#include <wchar.h>

/* 一次匹配里能记录的捕获组数量上限（第 0 组 = 整体匹配） */
#define QP_REGEX_MAX_GROUPS 16

/* 一次匹配里捕获到的区间。未参与的组 begin = NULL、len = 0。 */
typedef struct {
    const wchar_t *begin;
    size_t         len;
} QpRegexSpan;

/* 在 text 中搜索 pattern 的第一次匹配。成功返回 1，并把第 0 组（整体）
   与各捕获组写进 spans[0..span_cap-1]；失败或语法错误返回 0。 */
int qp_regex_search(const wchar_t *pattern, const wchar_t *text,
                    QpRegexSpan *spans, int span_cap);

/* 校验 pattern 是否合法。返回 1 表示合法；返回 0 时，若 errpos 非空则填
   入出错处的字符下标（相对 pattern 开头，0 起）。 */
int qp_regex_valid(const wchar_t *pattern, int *errpos);

#endif /* QP_REGEX_H */
