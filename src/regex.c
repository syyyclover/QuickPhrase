/*
 * regex.c —— 内置回溯正则引擎实现
 *
 * 采用「回溯虚拟机」：先把模式编译成一条指令序列，再用显式栈解释执行。
 * 相比递归下降，回溯栈能统一处理贪婪/懒惰量词、捕获组与反向引用的回退，
 * 且不受 C 递归深度限制。匹配全程大小写不敏感（与引擎其它规则一致）。
 */
#include "regex.h"

#include <stdlib.h>
#include <string.h>
#include <wctype.h>

/* ------------------------------------------------------------------ */
/* 指令                                                                */
/* ------------------------------------------------------------------ */

/* 预定义类的谓词位 */
#define PRED_WORD  1
#define PRED_SPACE 2
#define PRED_DIGIT 4

typedef enum {
    OP_CHAR,      /* 匹配单个字符（大小写不敏感）        ch */
    OP_ANY,       /* . 匹配任意一个字符                  */
    OP_CLASS,     /* 字符类：区间池 [a, a+b) ∪ 谓词位    a,b,negate,predpos,predneg */
    OP_BOL,       /* ^ 文本开头                        */
    OP_EOL,       /* $ 文本结尾                        */
    OP_SPLIT,     /* 分支：先走 a，回退时走 b            a,b */
    OP_JMP,       /* 无条件跳转                         a */
    OP_SAVE,      /* 记录当前位置到捕获槽               a=槽号 */
    OP_BACKREF,   /* 反向引用                          a=组号 */
    OP_SAVEPOS,   /* 记下循环体入口的位置               a=循环槽号 */
    OP_PROGRESS,  /* 循环体没消耗字符（零宽）就退出循环  a=循环槽号 */
    OP_MATCH      /* 成功                              */
} RxOp;

typedef struct { wchar_t lo, hi; } RxRange;

typedef struct {
    RxOp    op;
    int     a, b;
    wchar_t ch;
    int     negate;
    int     predpos, predneg;
} RxIns;

/* 编译产物 */
typedef struct {
    RxIns   *ins; int len, cap;
    RxRange *ranges; int nranges, rcap;   /* 字符类区间共享池（OP_CLASS 只存下标） */
    int      ngroup;                       /* 已创建的捕获组数（含整体=0） */
    int      nloops;                       /* 已分配的循环槽数（零宽守卫用） */
    int      errpos;                       /* 语法错误位置，-1 = 无 */
} RxProg;

/* 一段已编译好的指令（编译量词/分支时先摘下来再重新排布）。
   base = 摘下来之前它在程序里的起始位置，重新发出时据此平移跳转目标。 */
typedef struct { RxIns *ins; int n; int base; } RxBlock;

#define MAX_CAP (2 * QP_REGEX_MAX_GROUPS)
#define RX_MAX_SLOTS 32                      /* 零宽守卫的循环槽数量上限 */
#define RX_MAX_FRAMES 100000                 /* 回溯栈帧上限：兜住病态模式的显存 */
#ifndef RX_ATTEMPT_BUDGET
#  define RX_ATTEMPT_BUDGET 2000000L         /* 单个起点的一次尝试最多走这么多步 */
#endif
#ifndef RX_TOTAL_BUDGET
#  define RX_TOTAL_BUDGET   20000000L        /* 一次搜索（所有起点合计）的上限 */
#endif

/* ------------------------------------------------------------------ */
/* 字符判定（不依赖 locale，保证各平台一致）                            */
/* ------------------------------------------------------------------ */

static int rx_lower(wchar_t c)
{
    wint_t w = towlower((wint_t)c);
    return (w == WEOF) ? (int)c : (int)w;
}

static int rx_upper(wchar_t c)
{
    wint_t w = towupper((wint_t)c);
    return (w == WEOF) ? (int)c : (int)w;
}

static int rx_char_eq(wchar_t a, wchar_t b) { return rx_lower(a) == rx_lower(b); }

static int rx_str_ieq(const wchar_t *a, const wchar_t *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i)
        if (!rx_char_eq(a[i], b[i])) return 0;
    return 1;
}

/* \d：ASCII 数字，外加 iswdigit 认可的其它数字 */
static int rx_is_digit(wchar_t c)
{
    if (c >= L'0' && c <= L'9') return 1;
    return iswdigit((wint_t)c) != 0;
}

/* \s：空白，含全角空格（中文文本常见） */
static int rx_is_space(wchar_t c)
{
    if (c == 0x3000) return 1;
    return iswspace((wint_t)c) != 0;
}

/* \w：字母数字下划线。非 ASCII 里显式认下 CJK / 假名 / 谚文 / 全角字母数字，
   这样中文能被 \w 命中，又不会把全角标点当成词字符，且不依赖进程 locale。 */
static int rx_is_word(wchar_t c)
{
    if (c == L'_') return 1;
    if (c < 128)
        return (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') ||
               (c >= L'a' && c <= L'z');
    if (c >= 0x2E80 && c <= 0x9FFF) return 1;   /* CJK 部首扩展 ~ 统一表意 */
    if (c >= 0x3040 && c <= 0x30FF) return 1;   /* 平假名 / 片假名 */
    if (c >= 0xAC00 && c <= 0xD7AF) return 1;   /* 谚文音节 */
    if (c >= 0xF900 && c <= 0xFAFF) return 1;   /* CJK 兼容表意 */
    if (c >= 0xFF10 && c <= 0xFF19) return 1;   /* 全角数字 */
    if (c >= 0xFF21 && c <= 0xFF3A) return 1;   /* 全角大写 */
    if (c >= 0xFF41 && c <= 0xFF5A) return 1;   /* 全角小写 */
    return iswalnum((wint_t)c) != 0;
}

static int rx_pred_match(int mask, wchar_t c)
{
    int r = 0;
    if (mask & PRED_WORD)  r |= rx_is_word(c);
    if (mask & PRED_SPACE) r |= rx_is_space(c);
    if (mask & PRED_DIGIT) r |= rx_is_digit(c);
    return r;
}

/* 转义字母 → 谓词位与是否取反；非预定义类返回 0 */
static int rx_predef_bit(wchar_t e, int *negate)
{
    *negate = 0;
    switch (e) {
    case L'd': return PRED_DIGIT;
    case L'D': *negate = 1; return PRED_DIGIT;
    case L'w': return PRED_WORD;
    case L'W': *negate = 1; return PRED_WORD;
    case L's': return PRED_SPACE;
    case L'S': *negate = 1; return PRED_SPACE;
    default:   return 0;
    }
}

/* ------------------------------------------------------------------ */
/* 编译：指令与区间池                                                   */
/* ------------------------------------------------------------------ */

static void prog_init(RxProg *p)
{
    memset(p, 0, sizeof(*p));
    p->errpos = -1;
    p->ngroup = 1;        /* 第 0 组 = 整体匹配 */
    p->nloops = 0;
}

static void prog_free(RxProg *p)
{
    free(p->ins);
    free(p->ranges);
    p->ins = NULL; p->len = p->cap = 0;
    p->ranges = NULL; p->nranges = p->rcap = 0;
}

static int prog_emit(RxProg *p, RxIns in)
{
    if (p->len >= p->cap) {
        int nc = p->cap ? p->cap * 2 : 64;
        RxIns *ni = (RxIns *)realloc(p->ins, (size_t)nc * sizeof(RxIns));
        if (!ni) return -1;
        p->ins = ni;
        p->cap = nc;
    }
    p->ins[p->len++] = in;
    return p->len - 1;
}

static int prog_emit_op(RxProg *p, RxOp op)
{
    RxIns in;
    memset(&in, 0, sizeof(in));
    in.op = op;
    return prog_emit(p, in);
}

static void prog_patch(RxProg *p, int idx, int a, int b)
{
    if (idx >= 0 && idx < p->len) { p->ins[idx].a = a; p->ins[idx].b = b; }
}

static void prog_add_range(RxProg *p, wchar_t lo, wchar_t hi)
{
    if (p->nranges >= p->rcap) {
        int nc = p->rcap ? p->rcap * 2 : 32;
        RxRange *nr = (RxRange *)realloc(p->ranges, (size_t)nc * sizeof(RxRange));
        if (!nr) return;
        p->ranges = nr;
        p->rcap = nc;
    }
    p->ranges[p->nranges].lo = lo;
    p->ranges[p->nranges].hi = hi;
    p->nranges++;
}

/* 把一段指令再发一遍。SPLIT / JMP 的目标都在块内（含块尾），按 delta 平移；
   其余指令的 a/b 是槽号、区间下标、组号，绝不能动。 */
static int prog_emit_block(RxProg *p, const RxBlock *b)
{
    int i, delta = p->len - b->base;
    for (i = 0; i < b->n; ++i) {
        RxIns in = b->ins[i];
        if (in.op == OP_SPLIT) { in.a += delta; in.b += delta; }
        else if (in.op == OP_JMP) { in.a += delta; }
        else if (in.op == OP_PROGRESS) { in.b += delta; }   /* a 是循环槽号，不能动 */
        if (prog_emit(p, in) < 0) return -1;
    }
    return 0;
}

/* 量词：把 b 按 {min,max} 重排后发出。max = -1 表示无限。b 本身不再内联。 */
static int prog_emit_repeat(RxProg *p, const RxBlock *b, int min, int max, int greedy)
{
    int i, *split_at = NULL, nsplit = 0;

    if (max == 0) return 0;                    /* {0}：匹配空 */

    for (i = 0; i < min; ++i)
        if (prog_emit_block(p, b) < 0) goto fail;

    if (max < 0) {
        /* 无限循环：SPLIT(more, exit); more: SAVEPOS; b; PROGRESS; JMP 回来; exit:
           SAVEPOS / PROGRESS 是一对零宽守卫：循环体这一趟没消耗任何字符时，
           就接受这一趟（捕获照记）并直接走到循环出口 —— 与主流实现一致，
           既不会 (a*)* 原地打转，也不会把这一趟的捕获丢掉。 */
        int l2, sp, more, exit, slot, prog;
        l2 = p->len;
        sp = prog_emit_op(p, OP_SPLIT);
        if (sp < 0) goto fail;
        more = p->len;
        slot = (p->nloops++) % RX_MAX_SLOTS;
        if (prog_emit(p, (RxIns){ .op = OP_SAVEPOS, .a = slot }) < 0) goto fail;
        if (prog_emit_block(p, b) < 0) goto fail;
        prog = prog_emit(p, (RxIns){ .op = OP_PROGRESS, .a = slot });
        if (prog < 0) goto fail;
        if (prog_emit(p, (RxIns){ .op = OP_JMP, .a = l2 }) < 0) goto fail;
        exit = p->len;
        prog_patch(p, prog, slot, exit);       /* b = 循环出口 */
        if (greedy) prog_patch(p, sp, more, exit);
        else        prog_patch(p, sp, exit, more);
    } else {
        /* 有限：max-min 个可选副本，容量不够时的统一出口都是 end。
           次数有界，不会打转，所以不需要零宽守卫。 */
        nsplit = max - min;
        if (nsplit > 0) {
            split_at = (int *)malloc((size_t)nsplit * sizeof(int));
            if (!split_at) goto fail;
        }
        for (i = 0; i < nsplit; ++i) {
            int sp = prog_emit_op(p, OP_SPLIT);
            if (sp < 0) goto fail;
            split_at[i] = sp;
            if (prog_emit_block(p, b) < 0) goto fail;
        }
        for (i = 0; i < nsplit; ++i) {
            int body = split_at[i] + 1, end = p->len;
            if (greedy) prog_patch(p, split_at[i], body, end);
            else        prog_patch(p, split_at[i], end, body);
        }
    }
    free(split_at);
    return 0;
fail:
    free(split_at);
    return -1;
}

/* ------------------------------------------------------------------ */
/* 语法分析                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    const wchar_t *s;
    int pos;
} RxParser;

static int parse_alt(RxProg *p, RxParser *ps);
static int parse_seq(RxProg *p, RxParser *ps);
static int parse_repeat(RxProg *p, RxParser *ps);
static int parse_atom(RxProg *p, RxParser *ps);

/* 把一次 parse_seq 的产物摘成独立块（调用后 p 恢复到起始长度） */
static int parse_seq_block(RxProg *p, RxParser *ps, RxBlock *b)
{
    int s = p->len;
    b->ins = NULL;
    b->n = 0;
    b->base = s;
    if (parse_seq(p, ps) < 0) return -1;
    b->n = p->len - s;
    if (b->n > 0) {
        b->ins = (RxIns *)malloc((size_t)b->n * sizeof(RxIns));
        if (!b->ins) return -1;
        memcpy(b->ins, p->ins + s, (size_t)b->n * sizeof(RxIns));
    }
    p->len = s;
    return 0;
}

/* 把单个原子摘成独立块 */
static int parse_atom_block(RxProg *p, RxParser *ps, RxBlock *b)
{
    int s = p->len;
    b->ins = NULL;
    b->n = 0;
    b->base = s;
    if (parse_atom(p, ps) < 0) return -1;
    b->n = p->len - s;
    if (b->n > 0) {
        b->ins = (RxIns *)malloc((size_t)b->n * sizeof(RxIns));
        if (!b->ins) return -1;
        memcpy(b->ins, p->ins + s, (size_t)b->n * sizeof(RxIns));
    }
    p->len = s;
    return 0;
}

/* A|B|C 编译成右嵌套：
     SPLIT(A, SPLIT(B, C)) ; A ; JMP end ; B ; JMP end ; C ; end
   入口先试 A，失败才回退到后一个分支。 */
static int parse_alt(RxProg *p, RxParser *ps)
{
    RxBlock *alts = NULL;
    int n = 0, cap = 0, i, rc = -1;
    int *split_at = NULL, *body_at = NULL, *jmp_at = NULL;
    int end;

    for (;;) {
        RxBlock b;
        if (parse_seq_block(p, ps, &b) < 0) { free(b.ins); goto done; }
        if (n >= cap) {
            int nc = cap ? cap * 2 : 4;
            RxBlock *na = (RxBlock *)realloc(alts, (size_t)nc * sizeof(RxBlock));
            if (!na) { free(b.ins); goto done; }
            alts = na; cap = nc;
        }
        alts[n++] = b;
        if (ps->s[ps->pos] != L'|') break;
        ps->pos++;
    }

    split_at = (int *)calloc((size_t)n, sizeof(int));
    body_at  = (int *)calloc((size_t)n, sizeof(int));
    jmp_at   = (int *)calloc((size_t)n, sizeof(int));
    if (!split_at || !body_at || !jmp_at) goto done;

    for (i = 0; i < n; ++i) {
        split_at[i] = -1;
        jmp_at[i] = -1;
        if (i < n - 1) {                       /* 最后一个分支后面不需要 SPLIT */
            split_at[i] = prog_emit_op(p, OP_SPLIT);
            if (split_at[i] < 0) goto done;
        }
        body_at[i] = p->len;
        if (prog_emit_block(p, &alts[i]) < 0) goto done;
        if (i < n - 1) {
            jmp_at[i] = prog_emit(p, (RxIns){ .op = OP_JMP });
            if (jmp_at[i] < 0) goto done;
        }
    }
    end = p->len;

    for (i = 0; i < n - 1; ++i) {
        int next = (i + 1 < n - 1) ? split_at[i + 1] : body_at[n - 1];
        prog_patch(p, split_at[i], body_at[i], next);
        if (jmp_at[i] >= 0) prog_patch(p, jmp_at[i], end, 0);
    }
    rc = 0;
done:
    for (i = 0; i < n; ++i) free(alts[i].ins);
    free(alts);
    free(split_at);
    free(body_at);
    free(jmp_at);
    return rc;
}

static int parse_seq(RxProg *p, RxParser *ps)
{
    for (;;) {
        wchar_t c = ps->s[ps->pos];
        if (c == L'\0' || c == L')' || c == L'|') break;
        if (parse_repeat(p, ps) < 0) return -1;
    }
    return 0;
}

/* 读 {m} {m,} {m,n}。是量词返回 1 并填 min/max，否则返回 0（当字面量处理）。 */
static int parse_braces(RxParser *ps, int *min, int *max)
{
    int save = ps->pos, m = 0, n = -1, digits = 0;
    if (ps->s[ps->pos] != L'{') return 0;
    ps->pos++;
    while (ps->s[ps->pos] >= L'0' && ps->s[ps->pos] <= L'9') {
        m = m * 10 + (ps->s[ps->pos] - L'0');
        ps->pos++; digits++;
        if (m > 100000) m = 100000;
    }
    if (!digits) { ps->pos = save; return 0; }
    if (ps->s[ps->pos] == L'}') {
        ps->pos++; n = m;
    } else if (ps->s[ps->pos] == L',') {
        ps->pos++;
        if (ps->s[ps->pos] == L'}') { ps->pos++; n = -1; }
        else {
            int dn = 0; n = 0;
            while (ps->s[ps->pos] >= L'0' && ps->s[ps->pos] <= L'9') {
                n = n * 10 + (ps->s[ps->pos] - L'0');
                ps->pos++; dn++;
                if (n > 100000) n = 100000;
            }
            if (!dn || ps->s[ps->pos] != L'}') { ps->pos = save; return 0; }
            ps->pos++;
        }
    } else {
        ps->pos = save; return 0;
    }
    if (n >= 0 && n < m) { ps->pos = save; return 0; }   /* {m,n} 且 n<m → 字面量 */
    *min = m; *max = n;
    return 1;
}

static int parse_repeat(RxProg *p, RxParser *ps)
{
    RxBlock b;
    int min = -1, max = -1, greedy = 1, rc;

    if (parse_atom_block(p, ps, &b) < 0) return -1;
    if (ps->s[ps->pos] == L'*')      { min = 0; max = -1; ps->pos++; }
    else if (ps->s[ps->pos] == L'+') { min = 1; max = -1; ps->pos++; }
    else if (ps->s[ps->pos] == L'?') { min = 0; max = 1;  ps->pos++; }
    else if (!parse_braces(ps, &min, &max)) min = -1;

    if (min >= 0) {
        if (ps->s[ps->pos] == L'?') { greedy = 0; ps->pos++; }
        rc = prog_emit_repeat(p, &b, min, max, greedy);
    } else {
        rc = prog_emit_block(p, &b);
    }
    free(b.ins);
    return rc;
}

/* 解析 [] 字符类：区间 + \d \w \s 谓词（可混合、可取反）。 */
static int parse_class(RxProg *p, RxParser *ps)
{
    int negate = 0, first = 1, rstart = p->nranges;
    int predpos = 0, predneg = 0;

    if (ps->s[ps->pos] == L'^') { negate = 1; ps->pos++; }
    for (;;) {
        wchar_t c = ps->s[ps->pos], lo;
        if (c == L'\0') { p->errpos = ps->pos; return -1; }
        if (c == L']' && !first) break;

        if (c == L'\\') {
            wchar_t e = ps->s[ps->pos + 1];
            int neg = 0, bit;
            if (e == L'\0') { p->errpos = ps->pos; return -1; }
            bit = rx_predef_bit(e, &neg);
            if (bit) {
                if (neg) predneg |= bit; else predpos |= bit;
                ps->pos += 2;
                first = 0;
                continue;
            }
            lo = e;
            ps->pos += 2;
        } else {
            lo = c;
            ps->pos++;
        }
        /* 是否构成 a-z 范围 */
        if (ps->s[ps->pos] == L'-' && ps->s[ps->pos + 1] != L']' &&
            ps->s[ps->pos + 1] != L'\0') {
            wchar_t hi;
            ps->pos++;
            if (ps->s[ps->pos] == L'\\') {
                hi = ps->s[ps->pos + 1];
                if (hi == L'\0') { p->errpos = ps->pos; return -1; }
                ps->pos += 2;
            } else {
                hi = ps->s[ps->pos];
                ps->pos++;
            }
            prog_add_range(p, lo, hi);
        } else {
            prog_add_range(p, lo, lo);
        }
        first = 0;
    }
    ps->pos++;   /* 跳过 ] */
    {
        RxIns in;
        memset(&in, 0, sizeof(in));
        in.op = OP_CLASS;
        in.a = rstart;
        in.b = p->nranges - rstart;
        in.negate = negate;
        in.predpos = predpos;
        in.predneg = predneg;
        return prog_emit(p, in);
    }
}

static int parse_atom(RxProg *p, RxParser *ps)
{
    wchar_t c = ps->s[ps->pos];
    int rstart;

    if (c == L'\0') { p->errpos = ps->pos; return -1; }

    if (c == L'(') {
        int group = -1;
        if (ps->s[ps->pos + 1] == L'?') {
            if (ps->s[ps->pos + 2] == L':') { ps->pos += 3; }
            else { p->errpos = ps->pos; return -1; }   /* 仅支持 (?:…) */
        } else {
            group = p->ngroup++;
            if (p->ngroup > QP_REGEX_MAX_GROUPS) { p->errpos = ps->pos; return -1; }
            ps->pos++;
        }
        if (group >= 0) {
            if (prog_emit(p, (RxIns){ .op = OP_SAVE, .a = 2 * group }) < 0) return -1;
        }
        if (parse_alt(p, ps) < 0) return -1;
        if (ps->s[ps->pos] != L')') { p->errpos = ps->pos; return -1; }
        ps->pos++;
        if (group >= 0) {
            if (prog_emit(p, (RxIns){ .op = OP_SAVE, .a = 2 * group + 1 }) < 0) return -1;
        }
        return 0;
    }

    if (c == L'[') { ps->pos++; return parse_class(p, ps); }
    if (c == L'.') { ps->pos++; return prog_emit_op(p, OP_ANY); }
    if (c == L'^') { ps->pos++; return prog_emit_op(p, OP_BOL); }
    if (c == L'$') { ps->pos++; return prog_emit_op(p, OP_EOL); }

    if (c == L'\\') {
        wchar_t e = ps->s[ps->pos + 1];
        int neg = 0, bit;
        if (e == L'\0') { p->errpos = ps->pos; return -1; }
        bit = rx_predef_bit(e, &neg);
        if (bit) {
            rstart = p->nranges;
            ps->pos += 2;
            {
                RxIns in;
                memset(&in, 0, sizeof(in));
                in.op = OP_CLASS;
                in.a = rstart;
                in.b = 0;
                if (neg) in.predneg = bit; else in.predpos = bit;
                return prog_emit(p, in);
            }
        }
        switch (e) {
        case L'n': e = L'\n'; break;
        case L'r': e = L'\r'; break;
        case L't': e = L'\t'; break;
        case L'f': e = L'\f'; break;
        case L'v': e = L'\v'; break;
        default: break;
        }
        if (e >= L'1' && e <= L'9') {              /* 反向引用（组已存在时） */
            int g = e - L'0';
            if (g < p->ngroup) {
                ps->pos += 2;
                return prog_emit(p, (RxIns){ .op = OP_BACKREF, .a = g });
            }
        }
        ps->pos += 2;
        return prog_emit(p, (RxIns){ .op = OP_CHAR, .ch = e });
    }

    ps->pos++;
    return prog_emit(p, (RxIns){ .op = OP_CHAR, .ch = c });
}

static int rx_compile(const wchar_t *pattern, RxProg *p)
{
    RxParser ps;

    prog_init(p);
    ps.s = pattern ? pattern : L"";
    ps.pos = 0;

    if (prog_emit(p, (RxIns){ .op = OP_SAVE, .a = 0 }) < 0) return -1;
    if (parse_alt(p, &ps) < 0) {
        if (p->errpos < 0) p->errpos = ps.pos;
        return 0;
    }
    if (ps.s[ps.pos] != L'\0') { p->errpos = ps.pos; return 0; }
    if (prog_emit(p, (RxIns){ .op = OP_SAVE, .a = 1 }) < 0) return -1;
    if (prog_emit_op(p, OP_MATCH) < 0) return -1;
    return 1;
}

/* ------------------------------------------------------------------ */
/* 执行（显式回溯栈）                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    int pc;
    const wchar_t *sp;
    const wchar_t *cap[MAX_CAP];
    /* 各循环体入口位置（相对 text 的偏移，-1 = 未设）。必须随帧一起回退：
       否则回退到循环体内部的老帧时，零宽守卫会拿着「后一轮」的入口位置去比，
       把本该算作「没前进」的一趟当成前进了，于是反复重进循环体。 */
    int vmoff[RX_MAX_SLOTS];
} RxFrame;

typedef struct {
    RxFrame *stk; int top, cap;
    long steps;
} RxStack;

/* 压栈成功返回 1；栈满 / 分配失败返回 0（调用方应当放弃这条尝试，
   而不是悄悄丢掉一个备用分支 —— 那会算出错误的结果）。 */
static int stack_push(RxStack *s, int pc, const wchar_t *sp,
                      const wchar_t **cap, const int *vmoff)
{
    if (s->top >= s->cap) {
        int nc = s->cap ? s->cap * 2 : 64;
        RxFrame *nf;
        if (nc > RX_MAX_FRAMES) return 0;
        nf = (RxFrame *)realloc(s->stk, (size_t)nc * sizeof(RxFrame));
        if (!nf) return 0;
        s->stk = nf; s->cap = nc;
    }
    s->stk[s->top].pc = pc;
    s->stk[s->top].sp = sp;
    memcpy(s->stk[s->top].cap, cap, MAX_CAP * sizeof(const wchar_t *));
    memcpy(s->stk[s->top].vmoff, vmoff, RX_MAX_SLOTS * sizeof(int));
    s->top++;
    return 1;
}

static int stack_pop(RxStack *s, int *pc, const wchar_t **sp,
                     const wchar_t **cap, int *vmoff)
{
    if (s->top <= 0) return 0;
    s->top--;
    *pc = s->stk[s->top].pc;
    *sp = s->stk[s->top].sp;
    memcpy(cap, s->stk[s->top].cap, MAX_CAP * sizeof(const wchar_t *));
    memcpy(vmoff, s->stk[s->top].vmoff, RX_MAX_SLOTS * sizeof(int));
    return 1;
}

static int rx_class_has(const RxIns *ins, const RxRange *ranges, wchar_t ch)
{
    int matched = 0, i;
    int c = (int)ch, lo = rx_lower(ch), up = rx_upper(ch);
    for (i = 0; i < ins->b; ++i) {
        const RxRange *r = &ranges[ins->a + i];
        int rl = (int)r->lo, rh = (int)r->hi;
        /* 大小写不敏感：字符本身、它的小写形、它的大写形，任一落在区间内即算命中。
           只比较小写形是不够的 —— 像 [@-_]（含 A-Z）配上输入 'b'，要认的是
           'b' 的大写形 'B' 落在区间里。 */
        if ((c >= rl && c <= rh) || (lo >= rl && lo <= rh) || (up >= rl && up <= rh)) {
            matched = 1;
            break;
        }
    }
    if (!matched && ins->predpos && rx_pred_match(ins->predpos, ch)) matched = 1;
    if (!matched && ins->predneg && !rx_pred_match(ins->predneg, ch)) matched = 1;
    return ins->negate ? !matched : matched;
}

static int rx_run_at(const RxIns *ins, const RxRange *ranges,
                     const wchar_t *text, size_t tlen, size_t start,
                     QpRegexSpan *spans, int span_cap, long *steps)
{
    RxStack s;
    const wchar_t *end = text + tlen;
    const wchar_t *cap[MAX_CAP];
    int vmoff[RX_MAX_SLOTS];                  /* 各循环体入口位置（相对 text），-1 = 未设 */
    const wchar_t *sp = text + start;
    int pc = 0, i;

    memset(&s, 0, sizeof(s));
    for (i = 0; i < MAX_CAP; ++i) cap[i] = NULL;
    for (i = 0; i < RX_MAX_SLOTS; ++i) vmoff[i] = -1;

    for (;;) {
        RxIns in;
        if (++(*steps) > RX_ATTEMPT_BUDGET) { free(s.stk); return 0; }
        in = ins[pc];
        switch (in.op) {
        case OP_CHAR:
            if (sp < end && rx_char_eq(in.ch, *sp)) { sp++; pc++; }
            else goto fail;
            break;
        case OP_ANY:
            if (sp < end) { sp++; pc++; } else goto fail;
            break;
        case OP_CLASS:
            if (sp < end && rx_class_has(&in, ranges, *sp)) { sp++; pc++; }
            else goto fail;
            break;
        case OP_BOL:
            if (sp == text) pc++; else goto fail;
            break;
        case OP_EOL:
            if (sp == end) pc++; else goto fail;
            break;
        case OP_SAVE:
            cap[in.a] = sp;
            pc++;
            break;
        case OP_BACKREF: {
            const wchar_t *cs = cap[2 * in.a], *ce = cap[2 * in.a + 1];
            size_t blen;
            if (!cs || !ce) goto fail;
            blen = (size_t)(ce - cs);
            if ((size_t)(end - sp) >= blen && rx_str_ieq(sp, cs, blen)) {
                sp += blen; pc++;
            } else goto fail;
            break;
        }
        case OP_SPLIT:
            if (!stack_push(&s, in.b, sp, cap, vmoff)) { free(s.stk); return 0; }
            pc = in.a;
            break;
        case OP_JMP:
            pc = in.a;
            break;
        case OP_SAVEPOS:
            vmoff[in.a] = (int)(sp - text);
            pc++;
            break;
        case OP_PROGRESS:
            /* 循环体这一趟没前进：接受这一趟、直接去循环出口（不再 JMP 回来），
               从而既不死循环，也不丢这一趟的捕获 */
            if (vmoff[in.a] >= 0 && (int)(sp - text) == vmoff[in.a]) {
                if (in.b > 0) pc = in.b;
                else goto fail;
            } else {
                pc++;
            }
            break;
        case OP_MATCH: {
            int g;
            for (g = 0; g < span_cap && g < QP_REGEX_MAX_GROUPS; ++g) {
                const wchar_t *gs = cap[2 * g], *ge = cap[2 * g + 1];
                if (gs && ge) { spans[g].begin = gs; spans[g].len = (size_t)(ge - gs); }
                else          { spans[g].begin = NULL; spans[g].len = 0; }
            }
            free(s.stk);
            return 1;
        }
        default:
            goto fail;
        }
        continue;
    fail:
        if (!stack_pop(&s, &pc, &sp, cap, vmoff)) { free(s.stk); return 0; }
    }
}

int qp_regex_search(const wchar_t *pattern, const wchar_t *text,
                    QpRegexSpan *spans, int span_cap)
{
    RxProg p;
    size_t tlen, i;
    long total = 0;
    int rc = 0;

    if (!spans || span_cap <= 0) return 0;
    if (!text) text = L"";
    tlen = wcslen(text);
    if (span_cap > QP_REGEX_MAX_GROUPS) span_cap = QP_REGEX_MAX_GROUPS;

    if (rx_compile(pattern, &p) != 1) { prog_free(&p); return 0; }

    /* 预算按「每个起点一次尝试」计，再另加一个总量兜底：这样某个起点上的
       病态回溯不会把后面所有起点的机会一起吃掉（否则会漏掉真实存在的匹配）。 */
    for (i = 0; i <= tlen; ++i) {
        long steps = 0;
        if (rx_run_at(p.ins, p.ranges, text, tlen, i, spans, span_cap, &steps)) {
            rc = 1;
            break;
        }
        total += steps;
        if (total >= RX_TOTAL_BUDGET) break;
    }
    prog_free(&p);
    return rc;
}

int qp_regex_valid(const wchar_t *pattern, int *errpos)
{
    RxProg p;
    int r = rx_compile(pattern, &p);
    if (errpos) *errpos = (r == 0) ? p.errpos : -1;
    prog_free(&p);
    return r == 1;
}
