#include "engine.h"
#include "test_fail_alloc.h"
#include <stdio.h>
#include <string.h>

static void condition_assignment_preserves_old_on_failure(void)
{
    ConditionRule *src=cond_rule_create(),*dst=cond_rule_create();
    wcscpy(src->result_value,L"NEW"); cond_term_set_match(src->terms,L"One,Two");
    wcscpy(dst->result_value,L"OLD"); cond_term_set_match(dst->terms,L"Old");
    fa_inject(0); cond_rule_assign(dst,src); size_t count=fa_calls;
    cond_rule_free(dst);
    for(size_t i=1;i<=count;++i) {
        dst=cond_rule_create(); wcscpy(dst->result_value,L"OLD");
        cond_term_set_match(dst->terms,L"Old"); size_t base=fa_live;
        fa_inject(i); cond_rule_assign(dst,src); fa_disable();
        assert(fa_hit && !wcscmp(dst->result_value,L"OLD"));
        assert(dst->terms && dst->terms->match_value_count==1);
        assert(!wcscmp(dst->terms->match_values[0],L"Old"));
        assert(fa_live==base); cond_rule_free(dst);
    }
    cond_rule_free(src); assert(!fa_live);
    printf("condition assignment: %zu failed allocations preserve destination\n",count);
}
int main(void)
{
    condition_assignment_preserves_old_on_failure();
    return 0;
}
