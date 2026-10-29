/* Catches allocation failures silently deleting fields, collections or models. */
#include "storage.h"
#include "test_fail_alloc.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static AppData *fixture(void)
{
    const wchar_t *opts[]={L"Alpha",L"Beta"};
    AppData *d=appdata_create();
    Template *t=tmpl_create(L"KEEP_NAME");
    Segment *text=seg_create(SEG_STATIC),*choice=seg_create(SEG_MULTI_SELECT);
    ParseRule *pr=parse_rule_create();
    ConditionRule *cr=cond_rule_create();
    d->groups=group_create(L"KEEP_GROUP");
    d->tables=dtref_create(L"KEEP_TABLE",L"/tmp/never-read-qp.csv");
    wcscpy(t->group_id,d->groups->id);
    t->batch_mode=1; t->table_output_enabled=1;
    wcscpy(t->table_output_format,L"{Choice}\\t中文");
    wcscpy(text->content,L"KEEP_CONTENT");
    wcscpy(choice->label,L"Choice");
    seg_set_options(choice,opts,2);
    pr->type=PARSE_LOOKUP; pr->lk_key_mode=LOOKUP_KEY_FIX;
    wcscpy(pr->lk_key_value,L"KEY"); wcscpy(pr->lk_table_id,d->tables->id);
    pr->lk_outs=lookup_out_create();
    wcscpy(pr->lk_outs->col,L"COL"); wcscpy(pr->lk_outs->target_segment_id,choice->id);
    choice->parse_rules=pr;
    cr->terms->op=OP_IN; cr->terms->negate=1;
    cond_term_set_match(cr->terms,L"One,Two");
    wcscpy(cr->result_value,L"RESULT"); choice->condition_rules=cr;
    seg_list_append(&t->segments,text); seg_list_append(&t->segments,choice);
    d->templates=t;
    return d;
}
static void serialize_failure_is_not_partial(void)
{
    AppData *d=fixture();
    fa_inject(0); char *reference=appdata_to_json(d); size_t count=fa_calls;
    assert(reference); size_t base=fa_live;
    for(size_t i=1;i<=count;++i) {
        fa_inject(i); char *json=appdata_to_json(d); fa_disable();
        assert(fa_hit);
        if(json && strcmp(json,reference)) fprintf(stderr,"partial JSON at allocation %zu: %s\n",i,json);
        if(json) assert(!strcmp(json,reference));
        free(json); assert(fa_live==base);
    }
    free(reference); appdata_free(d); assert(!fa_live);
    printf("JSON serialize: %zu allocation failures cannot return partial data\n",count);
}
static void deserialize_failure_is_not_partial(void)
{
    AppData *d=fixture(); char *reference=appdata_to_json(d);
    assert(reference); appdata_free(d);
    fa_inject(0); d=appdata_from_json(reference); size_t count=fa_calls;
    assert(d); appdata_free(d); size_t base=fa_live;
    for(size_t i=1;i<=count;++i) {
        fa_inject(i); d=appdata_from_json(reference); fa_disable();
        assert(fa_hit);
        if(d) {
            char *json=appdata_to_json(d); assert(json && !strcmp(json,reference));
            free(json);
        }
        appdata_free(d); assert(fa_live==base);
    }
    free(reference); assert(!fa_live);
    printf("JSON deserialize: %zu allocation failures cannot publish partial model\n",count);
}
static void settings_failure_is_not_partial(void)
{
    Settings s,out;
    settings_defaults(&s); s.hotkey_vk='X'; s.hotkey_mods=3; s.theme=THEME_DARK;
    s.autostart=1; s.tree_width=333; s.win_w=1500; s.win_h=950;
    s.editor_w=1200; s.editor_h=720; s.editor_preview_height=100;
    wcscpy(s.default_separator,L"SEP"); wcscpy(s.data_dir,L"/tmp/custom");
    fa_inject(0); char *reference=settings_to_json(&s); size_t count=fa_calls;
    assert(reference); size_t base=fa_live;
    for(size_t i=1;i<=count;++i) {
        fa_inject(i); char *json=settings_to_json(&s); fa_disable();
        assert(fa_hit);
        if(json) {
            assert(!strcmp(json,reference)); assert(settings_from_json(json,&out));
            assert(!memcmp(&s,&out,sizeof(s)));
        }
        free(json); assert(fa_live==base);
    }
    free(reference); assert(!fa_live);
    printf("settings serialize: %zu allocation failures cannot return partial settings\n",count);
}
static void save_failure_preserves_file(void)
{
    char dir[]="/tmp/qp-json-failure-XXXXXX"; wchar_t wide[600],path[600];
    assert(mkdtemp(dir)); qp_utf8_to_wide(dir,wide,600); storage_set_data_dir(wide);
    assert(storage_get_data_path(path,600));
    AppData *d=fixture(); size_t count;
    fa_inject(0); assert(storage_save(d)); count=fa_calls;
    const char *old="ORIGINAL_FILE_BYTES";
    for(size_t i=1;i<=count;++i) {
        assert(storage_write_text(path,old)); size_t base=fa_live;
        fa_inject(i); int saved=storage_save(d); fa_disable();
        char *bytes=storage_read_text(path); assert(bytes);
        if(!saved) assert(!strcmp(bytes,old));
        else {
            char *expected=appdata_to_json(d);
            assert(expected && !strcmp(bytes,expected)); free(expected);
        }
        free(bytes); assert(fa_live==base);
        char temporary[700]; snprintf(temporary,sizeof(temporary),"%s/data.json.tmp",dir);
        assert(access(temporary,F_OK)!=0); /* Every failed write removes its own temporary. */
    }
    appdata_free(d); storage_set_data_dir(NULL); assert(!fa_live);
    char *file=qp_wide_to_utf8_alloc(path); assert(file);
    assert(!unlink(file)); free(file); assert(!rmdir(dir)); assert(!fa_live);
    printf("JSON save: %zu allocation failures preserve original or complete data\n",count);
}
static void malformed_collections_are_not_silently_omitted(void)
{
    const char *bad[]={
        "{\"templates\":[{\"segments\":[42]}]}",
        "{\"templates\":[{\"segments\":{}}]}",
        "{\"templates\":[{\"segments\":[{\"type\":\"select\",\"options\":[42]}]}]}",
        "{\"templates\":[{\"segments\":[{\"parseRules\":[null]}]}]}",
        "{\"templates\":[{\"segments\":[{\"conditionRules\":[{\"conditions\":[false]}]}]}]}",
        "{\"templates\":[{\"segments\":[{\"parseRules\":[{\"type\":\"lookup\",\"outputs\":[42]}]}]}]}"
    };
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);++i) {
        AppData *d=appdata_from_json(bad[i]); assert(!d); assert(!fa_live);
    }
    Settings original,out; settings_defaults(&original); original.hotkey_vk='X';
    out=original; assert(!settings_from_json("[]",&out));
    assert(!memcmp(&original,&out,sizeof(out)));
    assert(!settings_from_json("{}garbage",&out)); assert(!fa_live);
}
static void explicit_membership_values_survive(void)
{
    char value[301],json[1400]; memset(value,'a',300); value[300]=0;
    snprintf(json,sizeof(json),"{\"templates\":[{\"segments\":[{\"conditionRules\":[{\"conditions\":[{\"operator\":\"in\",\"matchValues\":[\"North,South\",\"%s\"]}]}]}]}]}",value);
    AppData *d=appdata_from_json(json); assert(d);
    CondTerm *term=d->templates->segments->condition_rules->terms;
    assert(term->match_value_count==2);
    assert(!wcscmp(term->match_values[0],L"North,South"));
    assert(wcslen(term->match_values[1])==300);
    char *serialized=appdata_to_json(d); assert(serialized); appdata_free(d);
    d=appdata_from_json(serialized); free(serialized);
    assert(d && d->templates->segments->condition_rules->terms->match_value_count==2);
    appdata_free(d); assert(!fa_live);
}
int main(int argc,char **argv)
{
    if(argc==2) {
        if(!strcmp(argv[1],"read")) deserialize_failure_is_not_partial();
        else if(!strcmp(argv[1],"settings")) settings_failure_is_not_partial();
        else if(!strcmp(argv[1],"save")) save_failure_preserves_file();
        else if(!strcmp(argv[1],"malformed")) malformed_collections_are_not_silently_omitted();
        else if(!strcmp(argv[1],"membership")) explicit_membership_values_survive();
        else return 2;
        return 0;
    }
    serialize_failure_is_not_partial();
    deserialize_failure_is_not_partial();
    settings_failure_is_not_partial();
    save_failure_preserves_file();
    malformed_collections_are_not_silently_omitted();
    explicit_membership_values_survive();
    return 0;
}
