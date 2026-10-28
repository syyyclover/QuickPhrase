#include "engine.h"
#include "test_fail_alloc.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    Template *t=tmpl_create(L"Workspace");
    Segment *s=seg_create(SEG_TEXT);
    seg_list_append(&t->segments,s);
    EvalContext *ctx=eval_create(t);
    assert(ctx);
    fa_inject(1);
    for(int i=0;i<100;++i) {
        eval_set_user_value(ctx,0,L"unchanged");
        assert(qp_eval_run_checked(ctx));
    }
    assert(fa_calls==0 && !fa_hit);
    fa_disable();
    eval_destroy(ctx);
    tmpl_free(t);
    assert(!fa_live);
    puts("eval workspace: repeated evaluations allocate no scratch and ownership is released");
    return 0;
}
