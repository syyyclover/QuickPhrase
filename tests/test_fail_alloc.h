/* Test-only allocation injection. GNU ld --wrap leaves production unchanged. */
#ifndef QP_TEST_FAIL_ALLOC_H
#define QP_TEST_FAIL_ALLOC_H
#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <errno.h>

static void *fa_blocks[16384];
/* Interposition is a linker operation: the compiler cannot infer that libc
   calls update these private counters. Volatile makes observation explicit. */
static volatile size_t fa_live, fa_calls, fa_fail_at;
static volatile int fa_hit;
void *__real_malloc(size_t);
void *__real_calloc(size_t,size_t);
void *__real_realloc(void *,size_t);
void __real_free(void *);
static void fa_add(void *p)
{
    if(!p) return;
    for(size_t i=0;i<sizeof(fa_blocks)/sizeof(*fa_blocks);++i)
        if(!fa_blocks[i]) { fa_blocks[i]=p; ++fa_live; return; }
    assert(!"allocation tracking exhausted");
}
static void fa_remove(void *p)
{
    if(!p) return;
    for(size_t i=0;i<sizeof(fa_blocks)/sizeof(*fa_blocks);++i)
        if(fa_blocks[i]==p) { fa_blocks[i]=NULL; --fa_live; return; }
    assert(!"free of untracked allocation");
}
static int fa_fails(void)
{
    ++fa_calls;
    if(fa_fail_at && fa_calls==fa_fail_at) { fa_hit=1; errno=ENOMEM; return 1; }
    return 0;
}
void *__wrap_malloc(size_t n)
{ void *p; if(fa_fails()) return NULL; p=__real_malloc(n); fa_add(p); return p; }
void *__wrap_calloc(size_t n,size_t size)
{ void *p; if(fa_fails()) return NULL; p=__real_calloc(n,size); fa_add(p); return p; }
void *__wrap_realloc(void *p,size_t n)
{
    void *next;
    if(fa_fails()) return NULL;
    next=__real_realloc(p,n);
    if(next || !n) { fa_remove(p); fa_add(next); }
    return next;
}
void __wrap_free(void *p) { fa_remove(p); __real_free(p); }
static void fa_inject(size_t n) { fa_calls=0; fa_fail_at=n; fa_hit=0; }
static void fa_disable(void) { fa_fail_at=0; }
#endif
