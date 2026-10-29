#include "storage.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *json = "{\"templates\":[{\"name\":\"join\",\"segments\":["
    "{\"id\":\"src\",\"type\":\"text\",\"noOutput\":true,\"parseRules\":["
    "{\"type\":\"split\",\"delimiter\":\"=\",\"segmentIndex\":2,\"perParagraph\":true,"
    "\"paragraphInline\":true,\"paragraphSeparator\":\" / \",\"targetSegmentId\":\"dst\"}]},"
    "{\"id\":\"dst\",\"type\":\"text\"}]}]}";

int main(void)
{
    AppData *d=appdata_from_json(json);
    assert(d && d->templates);
    Template *t=d->templates;
    EvalContext *c=eval_create(t);
    assert(c);
    eval_set_user_value(c,0,L"x=A\r\n\r\nmiss\nx=B\nx=C");
    eval_run(c);
    assert(wcscmp(c->fields[1].value,L"A / B / C")==0);
    wchar_t out[128];
    QpRenderResult rr=qp_eval_render_checked(c,OUTPUT_TEXT,0,out,128,NULL,0);
    assert(rr.status==QP_RENDER_OK && wcscmp(out,L"A / B / C")==0);
    eval_destroy(c);
    char *saved=appdata_to_json(d);
    assert(saved && strstr(saved,"paragraphInline") && strstr(saved,"paragraphSeparator"));
    AppData *round=appdata_from_json(saved);
    assert(round);
    Template *clone=tmpl_clone(round->templates);
    assert(clone);
    c=eval_create(clone);assert(c);
    eval_set_user_value(c,0,L"x=A\nx=B");eval_run(c);
    assert(wcscmp(c->fields[1].value,L"A / B")==0);
    ParseRule *r=clone->segments->parse_rules;
    r->paragraph_separator[0]=0;
    eval_run(c);assert(wcscmp(c->fields[1].value,L"AB")==0);
    r->paragraph_inline=0;
    wcscpy(r->paragraph_separator,L" / ");
    eval_run(c);assert(wcscmp(c->fields[1].value,L"A\nB")==0);
    r->paragraph_inline=1;
    for(int i=0;i<QP_SEP_MAX-1;++i)r->paragraph_separator[i]=L'|';
    r->paragraph_separator[QP_SEP_MAX-1]=0;
    wchar_t input[256]=L"";
    for(int i=0;i<50;++i)wcscat(input,L"x=A\n");
    eval_set_user_value(c,0,input);
    rr=qp_eval_render_checked(c,OUTPUT_TEXT,0,out,128,NULL,0);
    assert(rr.status==QP_RENDER_TRUNCATED);
    eval_destroy(c);tmpl_free(clone);appdata_free(round);free(saved);appdata_free(d);
    d=appdata_from_json("{\"templates\":[{\"segments\":[{\"type\":\"text\",\"parseRules\":["
        "{\"type\":\"split\",\"paragraphSeparator\":\" / \"}]}]}]}");
    assert(d && !d->templates->segments->parse_rules->paragraph_inline);
    saved=appdata_to_json(d);assert(saved && strstr(saved,"paragraphSeparator"));
    round=appdata_from_json(saved);assert(round);
    assert(wcscmp(round->templates->segments->parse_rules->paragraph_separator,L" / ")==0);
    free(saved);appdata_free(round);appdata_free(d);
    puts("paragraph inline: JSON, skipped rows, CRLF, render and clone passed");
    return 0;
}
