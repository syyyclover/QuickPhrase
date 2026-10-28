/* Catches partial clones and destructive option/match mutation on OOM. */
#include "engine.h"
#include "test_fail_alloc.h"
#include <stdio.h>
#include <string.h>

static Template *fixture(void)
{
    const wchar_t *opts[]={L"Alpha",L"Beta"};
    Template *t=tmpl_create(L"KEEP");
    Segment *a=seg_create(SEG_SELECT),*b=seg_create(SEG_TEXT);
    ParseRule *p=parse_rule_create();
    ConditionRule *c=cond_rule_create();
    CondTerm *term=cond_term_create();
    p->type=PARSE_LOOKUP;
    p->lk_outs=lookup_out_create(); p->lk_outs->next=lookup_out_create();
    wcscpy(p->lk_outs->col,L"FIRST"); wcscpy(p->lk_outs->next->col,L"LAST");
    a->parse_rules=p;
    cond_term_set_match(c->terms,L"One,Two");
    cond_term_set_match(term,L"Three;Four"); cond_term_append(&c->terms,term);
    b->condition_rules=c;
    seg_set_options(a,opts,2);
    wcscpy(a->label,L"SOURCE"); wcscpy(b->label,L"TARGET");
    seg_list_append(&t->segments,a); seg_list_append(&t->segments,b);
    return t;
}
static void check_fixture(const Template *t)
{
    assert(t && seg_list_count(t->segments)==2);
    const Segment *a=t->segments,*b=a->next;
    assert(!wcscmp(t->name,L"KEEP") && !wcscmp(a->label,L"SOURCE"));
    assert(a->option_count==2 && a->options[0] && a->options[1]);
    assert(!wcscmp(a->options[0],L"Alpha") && !wcscmp(a->options[1],L"Beta"));
    assert(lookup_out_count(a->parse_rules->lk_outs)==2);
    assert(!wcscmp(a->parse_rules->lk_outs->col,L"FIRST"));
    assert(!wcscmp(a->parse_rules->lk_outs->next->col,L"LAST"));
    assert(cond_term_count(b->condition_rules->terms)==2);
    for(const CondTerm *x=b->condition_rules->terms;x;x=x->next)
        assert(x->match_value_count==2 && x->match_values[0] && x->match_values[1]);
    assert(!wcscmp(b->condition_rules->terms->match_values[0],L"One"));
    assert(!wcscmp(b->condition_rules->terms->next->match_values[1],L"Four"));
}
static void clone_every_allocation_failure_is_atomic(void)
{
    Template *t=fixture(),*copy;
    size_t base=fa_live,count;
    fa_inject(0); copy=tmpl_clone(t); count=fa_calls; check_fixture(copy); tmpl_free(copy);
    assert(fa_live==base);
    for(size_t i=1;i<=count;++i) {
        fa_inject(i); copy=tmpl_clone(t); fa_disable();
        if(copy) check_fixture(copy);
        assert(fa_hit && !copy); /* Every clone allocation is required. */
        tmpl_free(copy); check_fixture(t); assert(fa_live==base);
    }
    tmpl_free(t); assert(fa_live==0);
    printf("model clone: %zu required allocation failures preserve original\n",count);
}
static void option_replacement_preserves_old_on_failure(void)
{
    const wchar_t *old[]={L"Old"},*next[]={L"New",L"Second"};
    for(size_t i=1;i<=3;++i) {
        Segment *s=seg_create(SEG_SELECT); seg_set_options(s,old,1);
        size_t base=fa_live;
        fa_inject(i); seg_set_options(s,next,2); fa_disable();
        assert(fa_hit && s->option_count==1 && s->options[0]);
        assert(!wcscmp(s->options[0],L"Old") && fa_live==base);
        seg_free(s); assert(!fa_live);
    }
}
static void option_append_preserves_old_on_failure(void)
{
    const wchar_t *old[]={L"Old"};
    for(size_t i=1;i<=2;++i) {
        Segment *s=seg_create(SEG_SELECT); seg_set_options(s,old,1);
        size_t base=fa_live;
        fa_inject(i); seg_add_option(s,L"New"); fa_disable();
        assert(fa_hit && s->option_count==1 && !wcscmp(s->options[0],L"Old"));
        assert(fa_live==base); seg_free(s); assert(!fa_live);
    }
}
static void match_replacement_preserves_old_on_failure(void)
{
    CondTerm *probe=cond_term_create(); size_t count;
    fa_inject(0); cond_term_set_match(probe,L"New,Second"); count=fa_calls;
    cond_term_free(probe);
    for(size_t i=1;i<=count;++i) {
        CondTerm *term=cond_term_create(); cond_term_set_match(term,L"Old");
        size_t base=fa_live;
        fa_inject(i); cond_term_set_match(term,L"New,Second"); fa_disable();
        assert(fa_hit && !wcscmp(term->match_value,L"Old"));
        assert(term->match_value_count==1 && !wcscmp(term->match_values[0],L"Old"));
        assert(fa_live==base); cond_term_free(term); assert(!fa_live);
    }
}
int main(int argc,char **argv)
{
    if(argc==2) {
        if(!strcmp(argv[1],"replace")) option_replacement_preserves_old_on_failure();
        else if(!strcmp(argv[1],"append")) option_append_preserves_old_on_failure();
        else if(!strcmp(argv[1],"match")) match_replacement_preserves_old_on_failure();
        else return 2;
        return 0;
    }
    clone_every_allocation_failure_is_atomic();
    option_replacement_preserves_old_on_failure();
    option_append_preserves_old_on_failure();
    match_replacement_preserves_old_on_failure();
    puts("model mutation: replacement/append/match failures are atomic");
    return 0;
}
