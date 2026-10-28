#ifndef QP_ENGINE_INTERNAL_H
#define QP_ENGINE_INTERNAL_H
#include "engine.h"
typedef struct { const EvalContext *ctx; wchar_t (*work)[QP_VALUE_MAX]; int self; const int *driven; } PlaceholderCtx;
enum { QP_TEXT_TRUNCATED = 1u, QP_TEXT_NOMEM = 2u };
unsigned qp_eval_output_flags(EvalContext *ctx, OutputMode mode);
size_t qp_append_checked(wchar_t *out, size_t cap, size_t used, const wchar_t *src, size_t n, unsigned *flags);
size_t qp_expand_placeholders_status(const wchar_t *src, QpPlaceholderLookup lookup, void *user,
    wchar_t *out, size_t cap, int base, int *marks, int mark_cap, int *mark_count,
    int (*missing)(void *, const wchar_t *), unsigned *flags);
size_t append_n(wchar_t *out, size_t cap, size_t used,
                       const wchar_t *src, size_t n);
size_t qp_expand_placeholders_marked(const wchar_t *src, QpPlaceholderLookup lookup,
                                            void *user, wchar_t *out, size_t cap,
                                            int out_base, int *marks, int mark_cap,
                                            int *mark_count);
int cond_term_hit(const CondTerm *t, const wchar_t *trigger_value);
const wchar_t *eval_placeholder_lookup(void *user, const wchar_t *name);
int eval_placeholder_missing(void *user, const wchar_t *name);
size_t qp_expand_placeholders_checked(const wchar_t *src, QpPlaceholderLookup lookup,
    void *user, wchar_t *out, size_t cap, int out_base, int *marks, int mark_cap,
    int *mark_count, int (*missing)(void *, const wchar_t *));
#endif
