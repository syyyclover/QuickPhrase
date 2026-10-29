/* Linker --wrap must be visible to optimizer-sensitive bookkeeping. */
#include "test_fail_alloc.h"
int main(void)
{
    fa_inject(0);
    void *p=malloc(8);assert(p);
    assert(fa_calls==1 && fa_live==1);
    free(p);
    assert(fa_live==0);
    fa_disable();
    return 0;
}
