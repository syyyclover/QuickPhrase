/*
 * engine.h —— 口径生成器 数据模型 + 规则引擎
 *
 * 本文件与本层实现 (engine.c) 不依赖任何 Windows API，仅使用 C11 标准库，
 * 因此可以在任意平台上独立编译并单元测试（见 tests/test_engine.c）。
 *
 * 字符串统一使用 wchar_t（Windows 下为 UTF-16），仅在 storage.c 读写文件时
 * 与 UTF-8 互转。
 */
#ifndef QP_ENGINE_H
#define QP_ENGINE_H

#include <stddef.h>
#include <wchar.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* 容量常量                                                            */
/* ------------------------------------------------------------------ */
#define QP_ID_MAX        64
#define QP_NAME_MAX      256
#define QP_LABEL_MAX     128
#define QP_CONTENT_MAX   1024
#define QP_PLACEHOLD_MAX 128
#define QP_SEP_MAX       16
#define QP_KEYWORD_MAX   128
#define QP_MATCH_MAX     256
#define QP_VALUE_MAX     2048         /* 变量取值：要装得下多行大段文字 */
#define QP_RESULT_MAX    2048
#define QP_TIME_MAX      32
#define QP_FORMAT_MAX    64           /* 时间格式串                     */
#define QP_OUTPUT_MAX    32768
#define QP_PATH_MAX      520          /* 文件路径缓冲（engine 层不依赖 windows.h） */

/* 规则引擎迭代上限：防止规则互相触发导致死循环 */
#define QP_EVAL_MAX_PASS 8
/* 引用链的最大深度：A 引用 B、B 引用 C…… 同时也是成环检测的上限 */
#define QP_REF_MAX_DEPTH 16

/* ------------------------------------------------------------------ */
/* 枚举                                                                */
/* ------------------------------------------------------------------ */
typedef enum {
    SEG_STATIC = 0,       /* 固定片段  */
    SEG_TEXT = 1,         /* 文本输入  */
    SEG_SELECT = 2,       /* 下拉单选  */
    SEG_MULTI_SELECT = 3, /* 下拉多选  */
    SEG_DATETIME = 4      /* 时间日期  */
} SegmentType;

/* 引用取法：整体 / 按分隔符取第 N 段 / 按字截取 */
typedef enum {
    REF_WHOLE = 0,
    REF_SPLIT = 1,
    REF_SUBSTR = 2
} RefMode;

typedef enum {
    PARSE_SPLIT = 0,      /* 按分隔符切割取第 N 段 */
    PARSE_CONTAINS = 1    /* 包含关键词            */
} ParseRuleType;

typedef enum {
    OP_CONTAINS = 0,
    OP_EQUALS = 1,
    OP_STARTS_WITH = 2,
    OP_IN = 3,
    OP_ENDS_WITH = 4,
    OP_EMPTY = 5          /* 为空；配合 negate 表达「不为空」 */
} CondOperator;

/* 条件项之间的组合方式 */
typedef enum {
    COND_JOIN_ALL = 0,    /* 全部满足（且） */
    COND_JOIN_ANY = 1     /* 任意满足（或） */
} CondJoin;

/* ------------------------------------------------------------------ */
/* 解析规则：挂在 text 变量上，从输入值提取信息去填充「目标变量」        */
/* ------------------------------------------------------------------ */
typedef struct ParseRule {
    wchar_t id[QP_ID_MAX];
    ParseRuleType type;
    wchar_t delimiter[QP_SEP_MAX];        /* split: 分隔符            */
    int     segment_index;                /* split: 第 N 段, 从 1 开始 */
    wchar_t keyword[QP_KEYWORD_MAX];      /* 命中关键词（空=任意命中） */
    wchar_t target_segment_id[QP_ID_MAX]; /* 目标变量片段 id          */
    wchar_t fill_value[QP_VALUE_MAX];     /* 填充值（空=用提取到的值） */
    struct ParseRule *next;
} ParseRule;

/* ------------------------------------------------------------------ */
/* 条件规则：挂在任意片段上，控制该片段最终输出内容                     */
/*                                                                    */
/* 一条规则 = 若干「条件项」 + 一个结果值。条件项之间按 join 组合        */
/* （全部满足 / 任意满足），每个条件项还可以单独取反（negate）。         */
/* 只有一个未取反的条件项时，语义与老版本完全一致。                     */
/* ------------------------------------------------------------------ */
typedef struct CondTerm {
    wchar_t trigger_segment_id[QP_ID_MAX];
    CondOperator op;
    int     negate;                       /* 1 = 不满足该条件时才算成立 */
    wchar_t match_value[QP_MATCH_MAX];    /* 单值 / OP_IN 时为原始文本 */
    wchar_t **match_values;               /* OP_IN 拆分后的列表        */
    int     match_value_count;
    struct CondTerm *next;
} CondTerm;

typedef struct ConditionRule {
    wchar_t id[QP_ID_MAX];
    CondJoin join;
    CondTerm *terms;
    wchar_t result_value[QP_RESULT_MAX];
    struct ConditionRule *next;
} ConditionRule;

/* ------------------------------------------------------------------ */
/* 片段                                                                */
/* ------------------------------------------------------------------ */
typedef struct Segment {
    wchar_t id[QP_ID_MAX];
    SegmentType type;
    wchar_t label[QP_LABEL_MAX];          /* 变量名（static 为空）     */
    wchar_t content[QP_CONTENT_MAX];      /* static 的固定内容         */
    wchar_t placeholder[QP_PLACEHOLD_MAX];
    wchar_t **options;                    /* select / multi-select 选项 */
    int     option_count;
    wchar_t separator[QP_SEP_MAX];        /* multi-select 拼接分隔符   */
    wchar_t default_value[QP_VALUE_MAX];
    int     hidden;                       /* 1 = 填写界面不显示，只靠
                                             变量匹配/默认值填充      */
    int     multiline;                    /* text：1 = 多行大文本框    */
    wchar_t time_format[QP_FORMAT_MAX];   /* datetime：格式串，空=默认 */
    /* 引用：本片段的值直接取自另一个变量（整体或其中一部分）。
       引用来的值算「推导值」，与规则同级 → 目标在填写界面只读。 */
    wchar_t ref_segment_id[QP_ID_MAX];    /* 空 = 不引用               */
    RefMode ref_mode;
    wchar_t ref_delimiter[QP_SEP_MAX];    /* REF_SPLIT：分隔符         */
    int     ref_index;                    /* REF_SPLIT：第 N 段，1 起  */
    int     ref_start;                    /* REF_SUBSTR：第 N 个字，1 起 */
    int     ref_len;                      /* REF_SUBSTR：取几个字，
                                             <=0 = 一直取到末尾       */
    ParseRule *parse_rules;
    ConditionRule *condition_rules;
    struct Segment *next;
} Segment;

typedef struct Template {
    wchar_t id[QP_ID_MAX];
    wchar_t name[QP_NAME_MAX];
    wchar_t group_id[QP_ID_MAX];          /* 空 = 未分组 */
    Segment *segments;
    wchar_t created_at[QP_TIME_MAX];
    wchar_t updated_at[QP_TIME_MAX];
    struct Template *next;
} Template;

typedef struct Group {
    wchar_t id[QP_ID_MAX];
    wchar_t name[QP_NAME_MAX];
    int     order;
    struct Group *next;
} Group;

typedef struct AppData {
    Group    *groups;
    Template *templates;
} AppData;

/* 主题 */
typedef enum { THEME_LIGHT = 0, THEME_DARK = 1, THEME_SYSTEM = 2 } ThemeMode;

typedef struct Settings {
    unsigned hotkey_mods;                 /* MOD_ALT | MOD_CONTROL ... */
    unsigned hotkey_vk;                   /* 虚拟键码                  */
    int      autostart;
    ThemeMode theme;
    wchar_t  default_separator[QP_SEP_MAX];
    int      win_x, win_y, win_w, win_h;  /* 上次窗口位置/尺寸         */
    int      win_maximized;
    int      tree_width;                  /* 左侧模板树宽度            */
    int      preview_height;              /* 口径预览区高度；0 = 自动  */
    int      editor_w, editor_h;          /* 模板编辑器客户区尺寸；0 = 默认 */
    int      editor_preview_height;       /* 编辑器里默认值预览的高度；0 = 默认 */
    int      minimize_to_tray_hint;       /* 是否已提示过「已最小化到托盘」*/
    wchar_t  data_dir[QP_PATH_MAX];       /* 自定义数据目录；空 = 默认 %APPDATA%\QuickPhrase */
} Settings;

/* ------------------------------------------------------------------ */
/* 运行期求值上下文                                                    */
/* ------------------------------------------------------------------ */
typedef struct EvalField {
    const Segment *seg;
    wchar_t value[QP_VALUE_MAX];        /* 当前生效值（UI 显示 / 输出）*/
    wchar_t user_value[QP_VALUE_MAX];   /* 用户最近一次手动输入        */
    int     user_touched;               /* 用户是否手动输入过          */
    wchar_t sticky[QP_VALUE_MAX];       /* 规则曾推导出的值（不随规则
                                           失配而清空，见 eval_run）  */
    int     has_sticky;
    int     rule_driven;                /* 本次求值是否由规则推导得到
                                           （规则值优先，手动输入让位）*/
} EvalField;

typedef struct EvalContext {
    const Template *tmpl;
    EvalField *fields;
    int count;
    int unstable;                       /* 1 = 迭代跑满仍未收敛（规则互相
                                           覆盖），界面据此提示一句     */
} EvalContext;

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------- 字符串工具 --------------------------- */
void     qp_strlcpy(wchar_t *dst, const wchar_t *src, size_t cap);
int      qp_icmp(const wchar_t *a, const wchar_t *b);
const wchar_t *qp_istr(const wchar_t *hay, const wchar_t *needle);
int      qp_istarts(const wchar_t *hay, const wchar_t *prefix);
int      qp_iends(const wchar_t *hay, const wchar_t *suffix);
wchar_t *qp_wcsdup(const wchar_t *s);
void     qp_trim(wchar_t *s);
int      qp_is_blank(const wchar_t *s);
/* 按 delimiter 切割 src，取第 index 段（1 起）。成功返回 1 */
int      qp_split_take(const wchar_t *src, const wchar_t *delim, int index,
                       wchar_t *out, size_t cap);
/* --------------------------- 占位符 --------------------------------
   固定片段的内容里可以写 {变量名}，生成时换成那个变量此刻的值。
   {{ 输出一个字面的 {；认不出的名字原样保留（写错了要看得见）；换进来的
   内容里若还有 {…} 不再展开（只替换一层）。详见 engine.c 里的说明。      */
/* 取值回调：认不出这个名字就返回 NULL */
typedef const wchar_t *(*QpPlaceholderLookup)(void *user, const wchar_t *name);
size_t   qp_expand_placeholders(const wchar_t *src, QpPlaceholderLookup lookup,
                                void *user, wchar_t *out, size_t cap);
/* 依次取出 src 里第 index 个占位符的名字（0 起），没有了返回 0。体检用 */
int      qp_placeholder_at(const wchar_t *src, int index, wchar_t *name, size_t cap);
/* 生成唯一 id（prefix + 计数 + 时间） */
void     qp_new_id(wchar_t *out, size_t cap, const wchar_t *prefix);
/* 生成 ISO 时间戳 */
void     qp_now_iso(wchar_t *out, size_t cap);
/* 按格式串输出时间：yyyy yy MM M dd d HH H mm m ss s ddd(周一)，
   其余字符原样保留；fmt 为空时用 yyyy-MM-dd HH:mm:ss */
void     qp_format_time(const struct tm *tmv, const wchar_t *fmt,
                        wchar_t *out, size_t cap);
void     qp_format_time_now(const wchar_t *fmt, wchar_t *out, size_t cap);
/* 时间格式的预置列表（编辑器下拉用） */
int      qp_time_format_count(void);
const wchar_t *qp_time_format_at(int index);

/* --------------------------- 结构体生命周期 ----------------------- */
Segment       *seg_create(SegmentType type);
void           seg_free(Segment *s);
Segment       *seg_clone(const Segment *s);
void           seg_set_options(Segment *s, const wchar_t *const *opts, int count);
void           seg_add_option(Segment *s, const wchar_t *opt);
void           seg_remove_option(Segment *s, int index);
int            seg_move_option(Segment *s, int index, int delta);
const wchar_t *seg_display_name(const Segment *s);   /* 供 UI 显示 */
/* 按 s 的引用配置从来源值 src 里取出要用的那部分 */
void           seg_ref_extract(const Segment *s, const wchar_t *src,
                               wchar_t *out, size_t cap);
/* 引用摘要文字（「引用[设备编号]按“-”取第 7 段」），无引用时输出空串 */
void           seg_ref_summary(const Segment *s, const Template *t,
                               wchar_t *out, size_t cap);
/* target 若引用 source_id 会不会形成环（含自引用）。1 = 会，别让它保存 */
int            tmpl_ref_would_cycle(const Template *t, const Segment *target,
                                    const wchar_t *source_id);

ParseRule     *parse_rule_create(void);
void           parse_rule_free(ParseRule *r);
ParseRule     *parse_rule_clone(const ParseRule *r);

ConditionRule *cond_rule_create(void);      /* 自带一个空白条件项 */
void           cond_rule_free(ConditionRule *r);
ConditionRule *cond_rule_clone(const ConditionRule *r);
/* 把 src 的内容整体搬进 dst，保留 dst 的 id 与在链表中的位置 */
void           cond_rule_assign(ConditionRule *dst, const ConditionRule *src);
/* 便捷：整条规则只用一个条件项（老版本的单条件写法） */
void           cond_rule_set_simple(ConditionRule *r, const wchar_t *trigger_id,
                                    CondOperator op, int negate, const wchar_t *match);

CondTerm      *cond_term_create(void);
void           cond_term_free(CondTerm *t);
CondTerm      *cond_term_clone(const CondTerm *t);
/* 设置匹配值：OP_IN 时按 , ，、; 拆成列表 */
void           cond_term_set_match(CondTerm *t, const wchar_t *raw);
int            cond_term_count(const CondTerm *head);
CondTerm      *cond_term_at(CondTerm *head, int index);
void           cond_term_append(CondTerm **head, CondTerm *t);
int            cond_term_remove_at(CondTerm **head, int index);
void           cond_term_list_clear(CondTerm **head);

Template      *tmpl_create(const wchar_t *name);
void           tmpl_free(Template *t);
Template      *tmpl_clone(const Template *t);

Group         *group_create(const wchar_t *name);
void           group_free(Group *g);

AppData       *appdata_create(void);
void           appdata_free(AppData *d);
AppData       *appdata_create_seed(void);   /* 首次运行的示例数据 */
/* 复杂示例模板：把全部功能（隐藏变量 / 多行文本 / 两类解析规则 / 多条件项
   规则）都用上，首次运行时随示例数据一起装，也可从「模板」菜单再插一份 */
Template      *tmpl_create_demo(void);

/* --------------------------- 链表操作 ----------------------------- */
int      seg_list_count(const Segment *head);
Segment *seg_list_at(Segment *head, int index);
int      seg_list_index_of(Segment *head, const Segment *s);
void     seg_list_append(Segment **head, Segment *s);
void     seg_list_insert_at(Segment **head, Segment *s, int index);
Segment *seg_list_remove_at(Segment **head, int index);   /* 返回被摘下的节点 */
int      seg_list_move(Segment **head, int from, int to);

int      tmpl_list_count(const Template *head);
Template *tmpl_list_at(Template *head, int index);
void     tmpl_list_append(Template **head, Template *t);
Template *tmpl_list_find(Template *head, const wchar_t *id);
int      tmpl_list_remove(Template **head, const wchar_t *id); /* 并释放 */
int      tmpl_list_replace(Template **head, Template *newer);  /* 按 id 原地替换 */

int      group_list_count(const Group *head);
Group   *group_list_at(Group *head, int index);
void     group_list_append(Group **head, Group *g);
Group   *group_list_find(Group *head, const wchar_t *id);
int      group_list_remove(Group **head, const wchar_t *id);

int      parse_rule_count(const ParseRule *head);
ParseRule *parse_rule_at(ParseRule *head, int index);
void     parse_rule_append(ParseRule **head, ParseRule *r);
int      parse_rule_remove_at(ParseRule **head, int index);
int      parse_rule_move(ParseRule **head, int from, int to);

int      cond_rule_count(const ConditionRule *head);
ConditionRule *cond_rule_at(ConditionRule *head, int index);
void     cond_rule_append(ConditionRule **head, ConditionRule *r);
int      cond_rule_remove_at(ConditionRule **head, int index);
int      cond_rule_move(ConditionRule **head, int from, int to);

/* --------------------------- 规则引擎 ----------------------------- */
EvalContext *eval_create(const Template *t);
void         eval_destroy(EvalContext *ctx);
void         eval_reset(EvalContext *ctx);                 /* 回到默认值 */
int          eval_index_of_id(const EvalContext *ctx, const wchar_t *id);
int          eval_index_of_label(const EvalContext *ctx, const wchar_t *label);
/* 用户输入（规则推导出值时以规则为准），随后需调用 eval_run */
void         eval_set_user_value(EvalContext *ctx, int idx, const wchar_t *v);
/* 恢复「上次推导出的兜底值」：并行填写切回来时用，见 sess_activate */
void         eval_set_sticky(EvalContext *ctx, int idx, const wchar_t *v);
/* 执行解析规则 + 条件规则，迭代至稳定 */
void         eval_run(EvalContext *ctx);
/* 拼接输出 */
size_t       eval_render(const EvalContext *ctx, wchar_t *out, size_t cap);
/* 便捷：按默认值渲染（模板编辑器预览用） */
size_t       tmpl_render_defaults(const Template *t, wchar_t *out, size_t cap);

/* 供 UI / 存储层复用的枚举名称 */
const wchar_t *seg_type_name(SegmentType t);
const wchar_t *parse_type_name(ParseRuleType t);
const wchar_t *cond_op_name(CondOperator op);              /* 包含 / 等于 …   */
const wchar_t *cond_op_phrase(CondOperator op, int negate);/* 不包含 / 不为空 … */
int            cond_op_needs_value(CondOperator op);       /* OP_EMPTY 不需要值 */
/* 规则摘要文字（列表显示用） */
void parse_rule_summary(const ParseRule *r, const Template *t, wchar_t *out, size_t cap);
void cond_rule_summary(const ConditionRule *r, const Template *t, wchar_t *out, size_t cap);
void cond_term_summary(const CondTerm *term, const Template *t, wchar_t *out, size_t cap);
/* 按 id 找片段（用于把 id 显示成变量名） */
const Segment *tmpl_find_segment(const Template *t, const wchar_t *id);

/* --------------------------- 模板体检 ------------------------------
   保存模板时跑一遍，挑出「配好了但永远不会生效」的规则。只报能证明的：
   条件是否成立由引擎自己的判定函数说话，触发变量的取值穷举不了就不报，
   宁可漏报也不冤枉一条好规则。不阻止保存，只提醒。                     */
#define QP_LINT_MAX 20                /* 一次最多报这么多条，够看了 */

typedef enum {
    LINT_COND_NEVER = 0,      /* 条件永远不成立            */
    LINT_COND_NOTERM,         /* 规则一个条件都没有        */
    LINT_COND_DANGLING,       /* 条件指向的变量没选/已删除 */
    LINT_COND_EMPTY,          /* 命中后把这一段变成空      */
    LINT_PARSE_DANGLING,      /* 解析规则的目标已删除      */
    LINT_PLACEHOLDER          /* 占位符名字认不出来        */
} LintKind;

typedef struct {
    LintKind kind;
    int      seg_index;              /* 片段序号，0 起 —— 界面据此跳过去 */
    int      rule_index;             /* 规则序号，0 起；-1 = 与规则无关   */
    wchar_t  text[QP_RESULT_MAX];    /* 已经写成人话的一句描述            */
} LintIssue;

/* 返回写进 out 的条数（最多 cap 条）。LintIssue 不小，别放栈上 */
int tmpl_lint(const Template *t, LintIssue *out, int cap);

#ifdef __cplusplus
}
#endif

#endif /* QP_ENGINE_H */
