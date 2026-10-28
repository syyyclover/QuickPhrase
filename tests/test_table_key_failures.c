#include "table_key.h"
#include "test_fail_alloc.h"
#include <stdio.h>
#include <wchar.h>
int main(void)
{
    int valid=0;fa_inject(0);wchar_t *key=qp_table_numeric_key_checked(L"123.4500e-99999999999999999999",&valid);
    size_t count=fa_calls;assert(key && valid);free(key);assert(!fa_live);
    for(size_t i=1;i<=count;++i) {
        fa_inject(i);key=qp_table_numeric_key_checked(L"123.4500e-99999999999999999999",&valid);fa_disable();
        assert(fa_hit && valid && !key && !fa_live);
    }
    fa_inject(1);key=qp_table_text_key(L"  FULL KEY  ");fa_disable();assert(!key && fa_hit && !fa_live);
    fa_inject(0);key=qp_table_numeric_key_checked(L"NaN",&valid);assert(!key && !valid && !fa_calls);
    key=qp_table_text_key(L"  FULL KEY  ");assert(key && !wcscmp(key,L"full key"));free(key);assert(!fa_live);
    printf("key failures: %zu numeric allocations distinguish OOM from nonnumeric input\n",count);return 0;
}
