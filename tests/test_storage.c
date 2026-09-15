#include "../src/storage.h"
#include "../src/cJSON.h"
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int P=0,F=0,S=0;
static void ck(int c,const char*w){ if(c){++P;printf("  [ok]   %s\n",w);} else {++F;printf("  [FAIL] %s\n",w);} }
static void cks(const wchar_t*g,const wchar_t*w,const char*n){ if(g&&w&&wcscmp(g,w)==0){++P;printf("  [ok]   %s\n",n);} else {++F;printf("  [FAIL] %s  期望[%ls] 实际[%ls]\n",n,w?w:L"",g?g:L"");} }
/* 跳过的用例（本机自备的验收夹具不在时用）：既不算通过也不算失败，但会报出来 */
static void skip(const char*w){ ++S; printf("  [跳过] %s\n",w); }
/* 取第 0 个模板、第 si 个片段的第 ri 条条件规则对应的 JSON 对象 */
static cJSON *cond_json(const char *json,int si,int ri,cJSON **root_out){
  cJSON *root=cJSON_Parse(json),*t,*s;
  *root_out=root;
  if(!root) return NULL;
  t=cJSON_GetArrayItem(cJSON_GetObjectItem(root,"templates"),0);
  s=cJSON_GetArrayItem(cJSON_GetObjectItem(t,"segments"),si);
  return cJSON_GetArrayItem(cJSON_GetObjectItem(s,"conditionRules"),ri);
}
/* Fixture acceptance catches truncating device IDs, missing batch/noOutput flags,
   off-by-one split indices, and data appearing before source input is entered. */
static Template *inspection_fixture_template(AppData *d) {
  Template *t;
  for(t=d?d->templates:NULL;t;t=t->next) {
    Segment *s;
    for(s=t->segments;s;s=s->next)
      if(s->type==SEG_TEXT && s->multiline &&
         wcscmp(s->label,L"巡检数据")==0 && s->parse_rules)
        return t;
  }
  return NULL;
}
static Template *fixture_template(AppData *d,const wchar_t *name) {
  Template *t;
  for(t=d?d->templates:NULL;t;t=t->next)
    if(wcscmp(t->name,name)==0) return t;
  return NULL;
}
static int has_synthetic_table_row(const Template *t) {
  const Segment *s;
  for(s=t?t->segments:NULL;s;s=s->next) {
    const ConditionRule *r;
    if(s->type!=SEG_STATIC) continue;
    for(r=s->condition_rules;r;r=r->next)
      if(wcsstr(r->result_value,L"\\t")) return 1;
  }
  return 0;
}
static int uses_legacy_direct_description(const Template *t) {
  const Segment *s;
  for(s=t?t->segments:NULL;s;s=s->next) {
    const ParseRule *r;
    if(wcscmp(s->label,L"巡检数据")!=0) continue;
    for(r=s->parse_rules;r;r=r->next)
      if(wcscmp(r->keyword,L"经核查")==0 &&
         wcscmp(r->target_segment_id,L"seg_6aa0fe8d_ee")==0)
        return 1;
  }
  return 0;
}
static wchar_t *next_tsv_cell(wchar_t **cursor) {
  wchar_t *start,*end;
  if(!cursor||!*cursor) return NULL;
  start=*cursor; end=wcspbrk(start,L"\t\n");
  if(end) { *end=0; *cursor=end+1; }
  else *cursor=NULL;
  return start;
}
static void assert_inspection_cells(const wchar_t *actual,const wchar_t *delivery,
                                    int legacy) {
  const wchar_t *first_cells[]={
    L"NFV-D-HNGZ-03A",
    L"NFV-D-HNZJI-11A",
    L"NFV-D-HNGZ-07A",
    L"NFV-D-HNGZ-00A",
    L"NFV-D-HNGZ-08A",
    L"GDGZ-PC-CMNET-RT24-HWNE40EX8C"
  };
  const wchar_t *legacy_desc[]={
    L"网络设备NFV-D-HNGZ-03A-2402-AH10-S-TOR-02的10GE1/0/28端口收光低引起，不影响业务，计划下个窗口处理",
    L"网络设备NFV-D-HNZJI-11A-1401-AF09-S-FW-01单板故障导致，不影响业务，待后续厂家安排更换",
    L"风扇故障导致主机离线引起，已联系厂家处理，无业务影响。",
    L"因巡检账号密码过期导致，已联系厂家处理，不影响业务。",
    L"风扇故障导致主机离线引起，已联系厂家处理，无业务影响。",
    L"网络设备GDGZ-PC-CMNET-RT24-HWNE40EX8C板卡故障导致，不影响业务，待后续厂家安排更换"
  };
  wchar_t *got=qp_wcsdup(actual),*want=qp_wcsdup(delivery),*gc=got,*wc=want;
  int row,col;
  for(row=0;row<6;++row) for(col=0;col<13;++col) {
    wchar_t *g=next_tsv_cell(&gc),*w=next_tsv_cell(&wc);
    char label[96];
    if(col==0) w=(wchar_t *)first_cells[row];
    if(legacy&&col==12) w=(wchar_t *)legacy_desc[row];
    snprintf(label,sizeof(label),"%s inspection row %d column %d preserves exact content",
             legacy?"legacy":"delivery",row+1,col+1);
    cks(g,w,label);
  }
  free(got); free(want);
}
/* 验收样例的位置：收进 examples/ 之后先找新路径，找不到再退回老的根目录位置 */
static char *read_sample(void) {
  char *s=storage_read_text(L"examples/示例完整.txt");
  if(!s) s=storage_read_text(L"示例完整.txt");
  return s;
}
/* 验收夹具（data.json / QuickPhrase-模板.json / 示例完整.txt）是**本机自备**的，
   已从版本库移除。都不在就整组跳过 —— 不算通过也不算失败，但要报出来，
   免得「测试全绿」其实什么都没验。 */
static int file_here(const wchar_t *path) {
  char *u8=qp_wide_to_utf8_alloc(path);
  FILE *f;
  if(!u8) return 0;
  f=fopen(u8,"rb");            /* 本机跑测试（含 Linux）：走窄路径 fopen */
  free(u8);
  if(!f) return 0;
  fclose(f);
  return 1;
}
static int fixtures_present(void) {
  int sample=file_here(L"examples/示例完整.txt")||file_here(L"示例完整.txt");
  int json=file_here(L"data.json")||file_here(L"QuickPhrase-模板.json");
  return sample && json;
}
static void fixture_acceptance(void) {
  const wchar_t *paths[]={L"data.json",L"QuickPhrase-模板.json"};
  const wchar_t *derived[]={L"资源池",L"地区",L"隐患归属类型（计算/存储/网络/安全/虚拟层）",L"涉及设备",L"首列",L"问题跟进人",L"问题定位描述",L"设备后正文",L"描述起始词",L"资源池第二段",L"资源池第三段",L"资源池第四段",L"故障状态",L"隐患级别"};
  const wchar_t *row_format=L"{首列}\\t{地区}\\t{故障状态}\\t{隐患级别}\\t{隐患归属类型（计算/存储/网络/安全/虚拟层）}\\t{涉及设备}\\t\\t{时间}\\t\\t{问题跟进人}\\t\\t处理中\\t{描述起始词}{问题定位描述}";
  int p;
  char *sample;
  wchar_t *wide;
  wchar_t input[QP_VALUE_MAX]=L"",expected[QP_OUTPUT_MAX]=L"";
  wchar_t *line; int output_section=0,input_rows=0;
  if(!fixtures_present()) {
    skip("验收夹具不在本机（data.json / QuickPhrase-模板.json / 示例完整.txt），跳过交付模板验收");
    return;
  }
  sample=read_sample();
  wide=sample?qp_utf8_to_wide_alloc(sample):NULL;
  line=wide;
  ck(wide!=NULL,"acceptance fixture loads");
  while(line&&*line) {
    wchar_t *end=wcschr(line,L'\n'); if(end) *end=0;
    size_t n=wcslen(line); if(n&&line[n-1]==L'\r') line[--n]=0;
    if(wcsstr(line,L"输出模板：")) output_section=1;
    else if(n&&output_section) {
      if(expected[0]) wcscat(expected,L"\n");
      wcscat(expected,line);
    }
    else if(wcsncmp(line,L"1、",2)==0 && !output_section) {
      if(input[0]) wcscat(input,L"\n");
      wcscat(input,line); ++input_rows;
    }
    line=end?end+1:NULL;
  }
  ck(input_rows==6,"fixture has six input rows");
  { wchar_t *last=wcsstr(expected,L"GDGZ-PC-CMNET-RT24-HWNE40EX8C");
    wchar_t *description=last?wcsstr(last,L"单板故障"):NULL;
    ck(description!=NULL,"known router description discrepancy is explicit");
    if(description) { description[0]=L'板'; description[1]=L'卡'; } }
  for(p=0;p<2;++p) {
    AppData *d=appdata_create(); int added=0,updated=0;
    ck(storage_import(d,paths[p],QP_IMPORT_REPLACE,&added,&updated),"delivery JSON imports through real storage");
    if(d) {
      Template *t=inspection_fixture_template(d);
      ck(t!=NULL,"inspection template found by multiline parsed source");
      if(t) {
        EvalContext *c=eval_create(t); wchar_t text[QP_OUTPUT_MAX],table[QP_OUTPUT_MAX];
        int i,si=eval_index_of_label(c,L"巡检数据");
        const Segment *src=si>=0?c->fields[si].seg:NULL;
        LintIssue *issues=(LintIssue *)calloc(QP_LINT_MAX,sizeof(LintIssue));
        ck(issues&&tmpl_lint(t,issues,QP_LINT_MAX)==0,
           "inspection template passes generic lint after import");
        free(issues);
        ck(src && !src->hidden && !src->no_output,"inspection source is visible and participates in text output");
        ck(t->batch_mode && src && wcscmp(t->batch_source_id,src->id)==0,"inspection source drives batch rendering");
        ck(t->table_output_enabled,"inspection table output is explicitly enabled");
        cks(t->table_output_format,row_format,"inspection table format preserves the delivered row expression");
        ck(!has_synthetic_table_row(t),"inspection has no synthetic conditional table-row segment");
        for(i=0;i<(int)(sizeof(derived)/sizeof(derived[0]));++i) {
          int at=eval_index_of_label(c,derived[i]);
          ck(at>=0 && c->fields[at].seg->hidden && c->fields[at].seg->no_output,
             "inspection derived fields remain hidden and excluded from text output");
        }
        { int hi=eval_index_of_label(c,L"首列");
          const Segment *h=hi>=0?c->fields[hi].seg:NULL;
          const ConditionRule *nfv=h?cond_rule_at(h->condition_rules,0):NULL;
          const ConditionRule *fallback=h?cond_rule_at(h->condition_rules,1):NULL;
          const CondTerm *nt=nfv?cond_term_at(nfv->terms,0):NULL;
          const CondTerm *ft=fallback?cond_term_at(fallback->terms,0):NULL;
          ck(h&&h->type==SEG_TEXT&&h->hidden&&h->no_output,
             "inspection first-column helper is hidden no-output text");
          ck(cond_rule_count(h?h->condition_rules:NULL)==2&&nt&&nt->op==OP_STARTS_WITH&&
             wcscmp(nt->match_value,L"NFV-")==0&&
             wcscmp(nfv->result_value,L"{资源池}-{资源池第二段}-{资源池第三段}-{资源池第四段}")==0,
             "inspection first-column helper composes NFV resource fields");
          ck(ft&&ft->op==OP_EMPTY&&ft->negate&&
             wcscmp(fallback->result_value,L"{涉及设备}")==0,
             "inspection first-column helper falls back to the full nonempty device");
        }
        eval_set_user_value(c,eval_index_of_label(c,L"时间"),L"2026/9/1");
        eval_set_user_value(c,si,input); eval_run(c);
        eval_render_batch_mode(c,OUTPUT_TEXT,text,QP_OUTPUT_MAX);
        { wchar_t rows[QP_VALUE_MAX],*q,*next; int found=0;
          qp_strlcpy(rows,input,QP_VALUE_MAX);
          for(q=rows;q&&*q;q=next) {
            next=wcschr(q,L'\n'); if(next) *next++=0;
            if(*q && wcsstr(text,q)) ++found;
          }
          ck(found==input_rows,"ordinary output contains every untouched inspection input row");
        }
        eval_render_batch_mode(c,OUTPUT_TABLE,table,QP_OUTPUT_MAX);
        ck(wcsstr(table,L"\\t")==NULL,"inspection table renders actual tabs, not backslash-t text");
        ck(wcschr(table,L'{')==NULL && wcschr(table,L'}')==NULL,
           "inspection table has no unresolved placeholders");
        { int rows=table[0]?1:0,tabs=0,uniform=1; wchar_t *q;
          for(q=table;*q;++q) {
            if(*q==L'\t') ++tabs;
            if(*q==L'\n') { if(tabs!=12) uniform=0; tabs=0; ++rows; }
          }
          ck(rows==6 && uniform && tabs==12,"inspection table has six rows with thirteen columns each");
        }
        assert_inspection_cells(table,expected,uses_legacy_direct_description(t));
        { char *rendered=qp_wide_to_utf8_alloc(table);
          ck(storage_write_text(p==0?L"/tmp/qp-task5-data.tsv":L"/tmp/qp-task5-delivery.tsv",rendered),
             "save rendered TSV for separator inspection");
          free(rendered);
        }
        { wchar_t negative[QP_OUTPUT_MAX],*cursor=negative;
          eval_set_user_value(c,si,
            L"1、边缘任务：经排查为网络设备EDGE-NFV-RT-01-EXTRA因测试。");
          eval_run(c);
          eval_render_batch_mode(c,OUTPUT_TABLE,negative,QP_OUTPUT_MAX);
          cks(next_tsv_cell(&cursor),L"EDGE-NFV-RT-01-EXTRA",
              "interior NFV text keeps the whole device in the non-prefix fallback");
        }
        eval_destroy(c);
      }
    }
    appdata_free(d);
  }
  free(wide);free(sample);
}
/* Retain the pre-dual-output fixture regression coverage: exact derived cells,
   single-row evaluation, clearing/restoration, round-trip, and the day template. */
static void fixture_regression_acceptance(void) {
  const wchar_t *paths[]={L"data.json",L"QuickPhrase-模板.json"};
  int p,round;
  char *sample;
  wchar_t *wide;
  wchar_t input[QP_VALUE_MAX]=L"",expected[QP_OUTPUT_MAX]=L"";
  wchar_t *line; int output_section=0,input_rows=0;
  if(!fixtures_present()) {
    skip("验收夹具不在本机，跳过旧版回归覆盖（表格列 / 空值清空 / 日间管控）");
    return;
  }
  sample=read_sample();
  wide=sample?qp_utf8_to_wide_alloc(sample):NULL;
  line=wide;
  ck(wide!=NULL,"regression fixture loads");
  while(line&&*line) {
    wchar_t *end=wcschr(line,L'\n'); if(end) *end=0;
    size_t n=wcslen(line); if(n&&line[n-1]==L'\r') line[--n]=0;
    if(wcsstr(line,L"输出模板：")) output_section=1;
    else if(n&&output_section) {
      if(expected[0]) wcscat(expected,L"\n");
      wcscat(expected,line);
    } else if(wcsncmp(line,L"1、",2)==0) {
      if(input[0]) wcscat(input,L"\n");
      wcscat(input,line); ++input_rows;
    }
    line=end?end+1:NULL;
  }
  ck(input_rows==6,"regression fixture has six input rows");
  /* Source conservation is binding where the illustrative output says 单板. */
  { wchar_t *last=wcsstr(expected,L"GDGZ-PC-CMNET-RT24-HWNE40EX8C");
    wchar_t *description=last?wcsstr(last,L"单板故障"):NULL;
    ck(description!=NULL,"known router description discrepancy remains explicit");
    if(description) { description[0]=L'板'; description[1]=L'卡'; }
  }
  for(p=0;p<2;++p) {
    AppData *d=appdata_create(); int added=0,updated=0;
    ck(storage_import(d,paths[p],QP_IMPORT_REPLACE,&added,&updated),"regression JSON imports through real storage");
    for(round=0;d&&round<2;++round) {
      Template *day=fixture_template(d,L"日间管控");
      ck(day!=NULL,"day template remains importable");
      if(day) {
        EvalContext *c=eval_create(day); Segment *src=seg_list_at(day->segments,18),*dst=seg_list_at(day->segments,4);
        int si=eval_index_of_id(c,src->id),di=eval_index_of_id(c,dst->id);
        eval_set_user_value(c,si,L"NFV-D-HNGZ-03A-2402-AH10-S-TOR-02"); eval_run(c);
        cks(c->fields[di].value,L"03A","day single row extracts strict fourth dash field");
        eval_set_user_value(c,si,L"NFV-D-HNGZ-03A-2402-AH10-S-TOR-02\r\n\r\nNFV-D-HNZJI-11A-1401-AF09-S-FW-01\nshort-row"); eval_run(c);
        cks(c->fields[di].value,L"03A\n11A","day multiline extracts fourth field per nonempty valid row");
        eval_set_user_value(c,si,L"short-row"); eval_run(c);
        cks(c->fields[di].value,L"","day short row does not reuse previous extraction");
        eval_destroy(c);
      }
      if(p==1) {
        Template *t=fixture_template(d,L"巡检"); ck(t!=NULL,"inspection regression template remains importable");
        if(t) {
          EvalContext *c=eval_create(t); wchar_t out[QP_OUTPUT_MAX]; int i,si=eval_index_of_label(c,L"巡检数据");
          const wchar_t *derived[]={L"资源池",L"地区",L"隐患归属类型（计算/存储/网络/安全/虚拟层）",L"涉及设备",L"问题跟进人",L"问题定位描述",L"故障状态",L"隐患级别"};
          eval_run(c);
          for(i=0;i<(int)(sizeof(derived)/sizeof(derived[0]));++i) {
            int at=eval_index_of_label(c,derived[i]);
            ck(at>=0&&c->fields[at].value[0]==0,"empty source leaves derived inspection field empty");
          }
          eval_set_user_value(c,eval_index_of_label(c,L"时间"),L"2026/9/1");
          eval_set_user_value(c,si,input); eval_run(c);
          eval_render_batch_mode(c,OUTPUT_TABLE,out,QP_OUTPUT_MAX);
          cks(out,expected,"six inspection rows retain exact delivered cell content");
          ck(wcsstr(out,L"\\t")==NULL,"inspection regression output has actual tabs");
          { int rows=1,tabs=0,uniform=1; wchar_t *q;
            for(q=out;*q;++q) { if(*q==L'\t') ++tabs; if(*q==L'\n') {if(tabs!=12) uniform=0; tabs=0;++rows;} }
            ck(rows==6&&uniform&&tabs==12,"inspection regression output remains six by thirteen"); }
          ck(!c->unstable,"inspection evaluation converges");
          eval_set_user_value(c,si,L""); eval_run(c);
          for(i=0;i<(int)(sizeof(derived)/sizeof(derived[0]));++i) {
            int at=eval_index_of_label(c,derived[i]);
            ck(at>=0&&c->fields[at].value[0]==0,"clearing source removes derived inspection values");
          }
          for(i=0;i<c->count;++i)
            if(c->fields[i].seg->type!=SEG_DATETIME)
              ck(!c->fields[i].value[0],"all source and intermediate values clear after batch");
          { wchar_t first_input[QP_VALUE_MAX],first_expected[QP_OUTPUT_MAX],*end;
            qp_strlcpy(first_input,input,QP_VALUE_MAX); end=wcschr(first_input,L'\n'); if(end) *end=0;
            qp_strlcpy(first_expected,expected,QP_OUTPUT_MAX); end=wcschr(first_expected,L'\n'); if(end) *end=0;
            eval_set_user_value(c,si,first_input); eval_run(c);
            eval_render_batch_mode(c,OUTPUT_TABLE,out,QP_OUTPUT_MAX);
            cks(out,first_expected,"single inspection source renders one exact table row");
            eval_set_user_value(c,eval_index_of_label(c,L"时间"),L"2026/9/2"); eval_run(c);
            eval_render_batch_mode(c,OUTPUT_TABLE,out,QP_OUTPUT_MAX);
            ck(wcsstr(out,L"\t2026/9/2\t")!=NULL,"registration date remains editable");
          }
          eval_destroy(c);
        }
      }
      if(round==0) {
        char *saved=appdata_to_json(d); AppData *restored=appdata_from_json(saved);
        free(saved); appdata_free(d); d=restored;
        ck(d!=NULL,"regression fixtures survive storage round trip");
      }
    }
    appdata_free(d);
  }
  free(wide);free(sample);
}
int main(void){
  setlocale(LC_ALL,"");
  printf("存储层自测\n==========\n");
  fixture_acceptance();
  fixture_regression_acceptance();
  /* 编码往返 */
  { const wchar_t *src=L"经核查由于存储服务器一硬盘异常，不影响业务。ABC-123 🙂";
    char *u8=qp_wide_to_utf8_alloc(src); wchar_t *back=qp_utf8_to_wide_alloc(u8);
    cks(back,src,"UTF-16 -> UTF-8 -> UTF-16 往返一致");
    ck(strlen(u8)>40,"UTF-8 编码长度合理"); free(u8); free(back); }
  /* AppData 往返 */
  { AppData *d=appdata_create_seed(); char *json=appdata_to_json(d);
    AppData *d2=appdata_from_json(json);
    ck(json&&strstr(json,"\"templates\"")!=NULL,"生成 JSON 含 templates");
    ck(d2!=NULL,"JSON 解析成功");
    ck(tmpl_list_count(d2->templates)==2,"模板数量一致（基础 + 复杂示例）");
    ck(group_list_count(d2->groups)==1,"分组数量一致");
    cks(d2->templates->name,d->templates->name,"模板名称一致");
    ck(seg_list_count(d2->templates->segments)==seg_list_count(d->templates->segments),"片段数量一致");
    { Segment *a=seg_list_at(d->templates->segments,1), *b=seg_list_at(d2->templates->segments,1);
      cks(b->label,a->label,"变量名一致");
      ck(b->option_count==a->option_count,"选项数量一致");
      cks(b->options[0],a->options[0],"首个选项一致");
      ck(b->type==a->type,"片段类型一致"); }
    { Segment *a=seg_list_at(d->templates->segments,3), *b=seg_list_at(d2->templates->segments,3);
      ck(parse_rule_count(b->parse_rules)==3,"解析规则 3 条");
      ck(parse_rule_at(b->parse_rules,0)->segment_index==3,"解析规则段号=3");
      cks(parse_rule_at(b->parse_rules,0)->keyword,L"rel","解析规则关键词");
      cks(parse_rule_at(b->parse_rules,0)->target_segment_id,parse_rule_at(a->parse_rules,0)->target_segment_id,"目标片段 id 保持");}
    { Segment *b=seg_list_at(d2->templates->segments,9);
      ck(cond_rule_count(b->condition_rules)==2,"影响范围条件规则 2 条");
      cks(cond_rule_at(b->condition_rules,0)->result_value,L"全部用户（安全加固强制升级）","条件规则结果值");
      ck(cond_term_at(cond_rule_at(b->condition_rules,0)->terms,0)->op==OP_IN,"运算符保持 in"); }
    /* 反序列化后引擎仍能正确求值 */
    { EvalContext *c=eval_create(d2->templates); wchar_t out[QP_OUTPUT_MAX];
      int iv=eval_index_of_label(c,L"版本号"), ic=eval_index_of_label(c,L"变更类型");
      eval_set_user_value(c,iv,L"V3.0.0-20240901-alpha");
      eval_set_user_value(c,ic,L"安全加固");
      eval_run(c); eval_render(c,out,QP_OUTPUT_MAX);
      printf("  渲染: %ls\n", out);
      cks(out,L"本次发布「订单系统」V3.0.0-20240901-alpha（内测版），主要变更：安全加固；影响范围：全部用户（安全加固强制升级）；回滚方案：已准备一键回滚。","往返后引擎输出与文档一致");
      eval_destroy(c); }
    free(json); appdata_free(d); appdata_free(d2); }
  /* 复杂示例模板往返：隐藏 / 多行 / 多条件项（全部满足 / 任意满足 / 取反） */
  { AppData *d=appdata_create(); char *json; AppData *d2;
    tmpl_list_append(&d->templates,tmpl_create_demo());
    json=appdata_to_json(d); d2=appdata_from_json(json);
    ck(d2&&tmpl_list_count(d2->templates)==1,"复杂示例可序列化并读回");
    if(d2&&d2->templates){
      EvalContext *c=eval_create(d2->templates);
      int idet=eval_index_of_label(c,L"处置说明"), idate=eval_index_of_label(c,L"发生日期");
      int ilv=eval_index_of_label(c,L"违规等级"), ifu=eval_index_of_label(c,L"后续要求");
      int irv=eval_index_of_label(c,L"复核人"), irs=eval_index_of_label(c,L"复核状态");
      ck(c!=NULL&&idet>=0&&idate>=0&&ilv>=0&&ifu>=0&&irv>=0&&irs>=0,"往返后各变量齐全");
      if(c&&idet>=0&&idate>=0&&ilv>=0&&ifu>=0&&irv>=0&&irs>=0){
        ck(c->fields[idet].seg->multiline==1,"多行标记往返保持");
        ck(c->fields[idate].seg->hidden==1,"隐藏标记往返保持");
        eval_run(c);
        cks(c->fields[ilv].value,L"严重","往返后解析规则仍生效");
        cks(c->fields[ifu].value,L"2 小时内同步风控组，纳入平台黑名单，并在群内公示",
            "往返后「全部满足」规则仍生效");
        eval_set_user_value(c,irv,L"李四"); eval_run(c);
        cks(c->fields[irs].value,L"（已复核）","往返后「不满足」取反仍生效"); }
      eval_destroy(c); }
    free(json); appdata_free(d); appdata_free(d2); }
  /* OP_IN 列表往返 */
  { AppData *d=appdata_create(); Template *t=tmpl_create(L"T"); Segment *s=seg_create(SEG_STATIC);
    ConditionRule *r=cond_rule_create(); char *j; AppData *d2; ConditionRule *r2; CondTerm *t2;
    cond_rule_set_simple(r,L"",OP_IN,0,L"甲,乙,丙"); qp_strlcpy(r->result_value,L"R",QP_RESULT_MAX);
    cond_rule_append(&s->condition_rules,r); seg_list_append(&t->segments,s); tmpl_list_append(&d->templates,t);
    j=appdata_to_json(d); ck(strstr(j,"matchValues")!=NULL,"OP_IN 写出 matchValues 数组");
    ck(strstr(j,"\"conditions\"")!=NULL,"条件项写成 conditions 数组");
    ck(strstr(j,"\"join\"")!=NULL,"组合方式写入 join 字段");
    { cJSON *root=NULL,*o=cond_json(j,0,0,&root);
      ck(o&&cJSON_GetObjectItem(o,"matchValue")!=NULL,
         "单条件规则同时平铺老字段（旧版本仍可读）");
      ck(o&&cJSON_GetArraySize(cJSON_GetObjectItem(o,"conditions"))==1,"conditions 数组 1 项");
      cJSON_Delete(root); }
    d2=appdata_from_json(j); r2=d2->templates->segments->condition_rules;
    t2=cond_term_at(r2->terms,0);
    ck(cond_term_count(r2->terms)==1,"往返后仍是 1 个条件项");
    ck(t2->op==OP_IN && t2->match_value_count==3,"OP_IN 列表往返 3 项");
    cks(t2->match_values[2],L"丙","OP_IN 第三项一致");
    free(j); appdata_free(d); appdata_free(d2); }
  /* 多条件项 / 取反 / 新运算符 往返 */
  { AppData *d=appdata_create(); Template *t=tmpl_create(L"T");
    Segment *a=seg_create(SEG_TEXT), *s=seg_create(SEG_STATIC);
    ConditionRule *r=cond_rule_create(); CondTerm *tm; char *j; AppData *d2; ConditionRule *r2;
    qp_strlcpy(a->label,L"输入",QP_LABEL_MAX);
    r->join=COND_JOIN_ANY;
    qp_strlcpy(r->terms->trigger_segment_id,a->id,QP_ID_MAX);
    r->terms->op=OP_ENDS_WITH; cond_term_set_match(r->terms,L"-END");
    tm=cond_term_create(); qp_strlcpy(tm->trigger_segment_id,a->id,QP_ID_MAX);
    tm->op=OP_EMPTY; tm->negate=1; cond_term_append(&r->terms,tm);
    qp_strlcpy(r->result_value,L"命中",QP_RESULT_MAX);
    cond_rule_append(&s->condition_rules,r);
    seg_list_append(&t->segments,a); seg_list_append(&t->segments,s);
    tmpl_list_append(&d->templates,t);
    j=appdata_to_json(d);
    ck(strstr(j,"\"any\"")!=NULL,"任意满足写成 join=any");
    ck(strstr(j,"endsWith")!=NULL,"新运算符 endsWith 写入");
    ck(strstr(j,"\"empty\"")!=NULL,"新运算符 empty 写入");
    ck(strstr(j,"\"negate\"")!=NULL,"取反标记写入");
    { cJSON *root=NULL,*o=cond_json(j,1,0,&root);
      ck(o&&cJSON_GetObjectItem(o,"matchValue")==NULL,"多条件规则不再平铺老字段");
      ck(o&&cJSON_GetArraySize(cJSON_GetObjectItem(o,"conditions"))==2,"conditions 数组 2 项");
      cJSON_Delete(root); }
    d2=appdata_from_json(j); r2=d2->templates->segments->next->condition_rules;
    ck(r2&&r2->join==COND_JOIN_ANY,"join=any 往返");
    ck(cond_term_count(r2->terms)==2,"两个条件项往返");
    ck(cond_term_at(r2->terms,0)->op==OP_ENDS_WITH,"第一项运算符往返");
    cks(cond_term_at(r2->terms,0)->match_value,L"-END","第一项匹配值往返");
    ck(cond_term_at(r2->terms,1)->op==OP_EMPTY&&cond_term_at(r2->terms,1)->negate==1,
       "第二项「不为空」往返");
    free(j); appdata_free(d); appdata_free(d2); }
  /* 老格式（单条件平铺字段）仍能读入 */
  { AppData *o=appdata_from_json(
      "{\"templates\":[{\"name\":\"旧\",\"segments\":[{\"type\":\"static\",\"content\":\"X\","
      "\"conditionRules\":[{\"triggerSegmentId\":\"故障类型\",\"operator\":\"contains\","
      "\"matchValue\":\"硬件\",\"resultValue\":\"老规则\"}]}]}]}");
    ConditionRule *r=o&&o->templates&&o->templates->segments
                     ? o->templates->segments->condition_rules : NULL;
    ck(r!=NULL,"老格式条件规则读入成功");
    ck(r&&r->join==COND_JOIN_ALL,"老格式默认全部满足");
    ck(r&&cond_term_count(r->terms)==1,"老格式读成 1 个条件项");
    ck(r&&cond_term_at(r->terms,0)->op==OP_CONTAINS&&cond_term_at(r->terms,0)->negate==0,
       "老格式运算符与取反默认值");
    cks(r?cond_term_at(r->terms,0)->match_value:NULL,L"硬件","老格式匹配值读入");
    cks(r?cond_term_at(r->terms,0)->trigger_segment_id:NULL,L"故障类型","老格式触发变量读入");
    appdata_free(o); }
  /* 设置往返 */
  { Settings s,s2; char *j; settings_defaults(&s);
    s.hotkey_mods=3; s.hotkey_vk='K'; s.theme=THEME_DARK; s.autostart=1;
    s.win_x=100;s.win_y=50;s.win_w=1000;s.win_h=700; s.tree_width=260;
    qp_strlcpy(s.default_separator,L"; ",QP_SEP_MAX);
    j=settings_to_json(&s); ck(settings_from_json(j,&s2)==1,"设置 JSON 解析成功");
    ck(s2.hotkey_mods==3&&s2.hotkey_vk=='K',"热键往返");
    ck(s2.theme==THEME_DARK&&s2.autostart==1,"主题/自启动往返");
    ck(s2.win_w==1000&&s2.win_h==700&&s2.tree_width==260,"窗口尺寸往返");
    cks(s2.default_separator,L"; ","分隔符往返"); free(j); }
  /* 预览高度往返（0 = 自动） */
  { Settings s,s2; char *j; settings_defaults(&s);
    ck(s.preview_height==0,"预览高度默认为 0（自动）");
    s.preview_height=320;
    j=settings_to_json(&s); ck(strstr(j,"previewHeight")!=NULL,"预览高度写入 JSON");
    ck(settings_from_json(j,&s2)==1&&s2.preview_height==320,"预览高度往返"); free(j);
    { Settings s3; settings_defaults(&s3);
      ck(settings_from_json("{\"hotkeyVk\":65}",&s3)==1&&s3.preview_height==0,
         "老设置缺 previewHeight 时仍是自动"); } }
  /* 编辑器窗口尺寸与预览高度往返（同样 0 = 用设计值，负数按 0 收） */
  { Settings s,s2,s3; char *j; settings_defaults(&s);
    ck(s.editor_w==0&&s.editor_h==0&&s.editor_preview_height==0,
       "编辑器几何默认全 0（用设计值）");
    s.editor_w=1200; s.editor_h=800; s.editor_preview_height=180;
    j=settings_to_json(&s);
    ck(strstr(j,"editorW")!=NULL&&strstr(j,"editorPreviewHeight")!=NULL,
       "编辑器几何写入 JSON");
    ck(settings_from_json(j,&s2)==1&&s2.editor_w==1200&&s2.editor_h==800&&
       s2.editor_preview_height==180,"编辑器几何往返"); free(j);
    settings_defaults(&s3);
    ck(settings_from_json("{\"editorW\":-5,\"editorPreviewHeight\":-1}",&s3)==1&&
       s3.editor_w==0&&s3.editor_preview_height==0,"负数几何收成 0");
    settings_defaults(&s3);
    ck(settings_from_json("{\"hotkeyVk\":65}",&s3)==1&&s3.editor_h==0,
       "老设置缺编辑器几何时用设计值"); }
  /* 数据目录往返 */
  { Settings s,s2; char *j; settings_defaults(&s);
    qp_strlcpy(s.data_dir,L"C:\\MyData\\QuickPhrase",QP_PATH_MAX);
    j=settings_to_json(&s); ck(settings_from_json(j,&s2)==1,"dataDir JSON 解析成功");
    cks(s2.data_dir,L"C:\\MyData\\QuickPhrase","dataDir 往返一致");
    free(j);
    j=settings_to_json(&s2); ck(strstr(j,"dataDir")!=NULL,"dataDir 写入 JSON"); free(j); }
  /* 自定义数据目录路径 */
  { Settings s; settings_defaults(&s);
    storage_set_data_dir(L"C:\\CustomDir");
    { wchar_t p[QP_PATH_MAX];
      ck(storage_get_data_dir(p,QP_PATH_MAX)&&wcscmp(p,L"C:\\CustomDir")==0,
         "自定义数据目录生效");
      ck(storage_get_data_path(p,QP_PATH_MAX)&&
#ifdef _WIN32
         wcscmp(p,L"C:\\CustomDir\\data.json")==0,
#else
         /* 本机（Linux）自测时路径分隔符是 /，Windows 上才是 \ */
         wcscmp(p,L"C:\\CustomDir/data.json")==0,
#endif
         "自定义 data.json 路径"); }
    storage_set_data_dir(NULL);
    { wchar_t p[QP_PATH_MAX];
      ck(storage_get_data_dir(p,QP_PATH_MAX)&&
         wcscmp(p,L"C:\\CustomDir")!=0,"清除后恢复默认目录"); } }
  /* 隐藏开关往返 */
  { AppData *d=appdata_create(); Template *t=tmpl_create(L"T");
    Segment *a=seg_create(SEG_TEXT), *b=seg_create(SEG_SELECT), *c=seg_create(SEG_STATIC);
    char *j; AppData *d2; Segment *a2,*b2,*c2;
    qp_strlcpy(a->label,L"设备编号",QP_LABEL_MAX);
    qp_strlcpy(b->label,L"设备类型",QP_LABEL_MAX); b->hidden=1;
    qp_strlcpy(c->content,L"。",QP_CONTENT_MAX);
    seg_list_append(&t->segments,a); seg_list_append(&t->segments,b);
    seg_list_append(&t->segments,c); tmpl_list_append(&d->templates,t);
    j=appdata_to_json(d); ck(strstr(j,"hidden")!=NULL,"隐藏变量写入 hidden 字段");
    d2=appdata_from_json(j);
    a2=seg_list_at(d2->templates->segments,0);
    b2=seg_list_at(d2->templates->segments,1);
    c2=seg_list_at(d2->templates->segments,2);
    ck(a2->hidden==0,"未隐藏的变量往返仍为 0");
    ck(b2->hidden==1,"隐藏的变量往返仍为 1");
    ck(c2->hidden==0,"固定句不带隐藏标记");
    free(j); appdata_free(d); appdata_free(d2);
    /* 旧数据（没有 hidden 字段）默认不隐藏 */
    { AppData *o=appdata_from_json("{\"templates\":[{\"name\":\"旧\",\"segments\":[{\"type\":\"select\",\"label\":\"L\"}]}]}");
      ck(o&&o->templates&&o->templates->segments&&o->templates->segments->hidden==0,
         "旧数据缺 hidden 字段时默认不隐藏"); appdata_free(o); } }
  /* 时间变量往返 */
  { AppData *d=appdata_create(); Template *t=tmpl_create(L"T");
    Segment *a=seg_create(SEG_DATETIME), *b=seg_create(SEG_TEXT);
    char *j; AppData *d2; Segment *a2,*b2;
    qp_strlcpy(a->label,L"发生时间",QP_LABEL_MAX);
    qp_strlcpy(a->time_format,L"yyyy年M月d日 HH:mm",QP_FORMAT_MAX);
    qp_strlcpy(b->label,L"设备编号",QP_LABEL_MAX);
    seg_list_append(&t->segments,a); seg_list_append(&t->segments,b);
    tmpl_list_append(&d->templates,t);
    j=appdata_to_json(d);
    ck(strstr(j,"\"datetime\"")!=NULL,"时间变量写成 type=datetime");
    ck(strstr(j,"timeFormat")!=NULL,"时间格式写入 JSON");
    d2=appdata_from_json(j);
    a2=seg_list_at(d2->templates->segments,0);
    b2=seg_list_at(d2->templates->segments,1);
    ck(a2->type==SEG_DATETIME,"时间变量类型往返");
    cks(a2->time_format,L"yyyy年M月d日 HH:mm","时间格式往返");
    ck(b2->time_format[0]==0,"非时间变量不带格式");
    free(j); appdata_free(d); appdata_free(d2);
    /* 老数据没有 timeFormat（或人手改坏了）→ 回落到第一个预置格式 */
    { AppData *o=appdata_from_json("{\"templates\":[{\"name\":\"旧\",\"segments\":[{\"type\":\"datetime\",\"label\":\"时间\"}]}]}");
      cks(o&&o->templates&&o->templates->segments?o->templates->segments->time_format:NULL,
          qp_time_format_at(0),"缺 timeFormat 时回落默认格式"); appdata_free(o); } }
  /* 引用往返 */
  { AppData *d=appdata_create(); Template *t=tmpl_create(L"T");
    Segment *src=seg_create(SEG_TEXT), *whole=seg_create(SEG_TEXT),
            *part=seg_create(SEG_TEXT), *sub=seg_create(SEG_SELECT),
            *fix=seg_create(SEG_STATIC);
    char *j; AppData *d2; Segment *w2,*p2,*s2,*f2;
    qp_strlcpy(src->label,L"联系电话",QP_LABEL_MAX);
    qp_strlcpy(whole->label,L"整体引用",QP_LABEL_MAX);
    qp_strlcpy(whole->ref_segment_id,src->id,QP_ID_MAX);
    qp_strlcpy(part->label,L"分段引用",QP_LABEL_MAX);
    qp_strlcpy(part->ref_segment_id,src->id,QP_ID_MAX);
    part->ref_mode=REF_SPLIT; qp_strlcpy(part->ref_delimiter,L"-",QP_SEP_MAX);
    part->ref_index=3;
    qp_strlcpy(sub->label,L"按字引用",QP_LABEL_MAX);
    qp_strlcpy(sub->ref_segment_id,src->id,QP_ID_MAX);
    sub->ref_mode=REF_SUBSTR; sub->ref_start=8; sub->ref_len=4;
    qp_strlcpy(fix->content,L"。",QP_CONTENT_MAX);
    qp_strlcpy(fix->ref_segment_id,src->id,QP_ID_MAX);   /* 固定句不该有引用 */
    seg_list_append(&t->segments,src); seg_list_append(&t->segments,whole);
    seg_list_append(&t->segments,part); seg_list_append(&t->segments,sub);
    seg_list_append(&t->segments,fix); tmpl_list_append(&d->templates,t);
    j=appdata_to_json(d);
    ck(strstr(j,"refSegmentId")!=NULL,"引用来源写入 JSON");
    ck(strstr(j,"\"split\"")!=NULL&&strstr(j,"\"substr\"")!=NULL,"取法写入 JSON");
    d2=appdata_from_json(j);
    w2=seg_list_at(d2->templates->segments,1);
    p2=seg_list_at(d2->templates->segments,2);
    s2=seg_list_at(d2->templates->segments,3);
    f2=seg_list_at(d2->templates->segments,4);
    cks(w2->ref_segment_id,src->id,"引用来源 id 往返");
    ck(w2->ref_mode==REF_WHOLE,"整体取法往返");
    ck(p2->ref_mode==REF_SPLIT&&p2->ref_index==3,"分段取法与段号往返");
    cks(p2->ref_delimiter,L"-","分隔符往返");
    ck(s2->ref_mode==REF_SUBSTR&&s2->ref_start==8&&s2->ref_len==4,"按字取法参数往返");
    ck(f2->ref_segment_id[0]==0,"固定句的引用一律丢掉");
    free(j); appdata_free(d); appdata_free(d2);
    /* 没配引用的变量不写这一组字段 */
    { AppData *e=appdata_create(); Template *et=tmpl_create(L"E");
      Segment *es=seg_create(SEG_TEXT); char *ej;
      qp_strlcpy(es->label,L"普通变量",QP_LABEL_MAX);
      seg_list_append(&et->segments,es); tmpl_list_append(&e->templates,et);
      ej=appdata_to_json(e);
      ck(strstr(ej,"refSegmentId")==NULL&&strstr(ej,"refMode")==NULL,
         "不引用时整组字段都不写"); free(ej); appdata_free(e); }
    /* 老数据没有引用字段 → 不引用，且参数取合法默认值 */
    { AppData *o=appdata_from_json("{\"templates\":[{\"name\":\"旧\",\"segments\":[{\"type\":\"text\",\"label\":\"L\"}]}]}");
      Segment *os=o&&o->templates?o->templates->segments:NULL;
      ck(os&&os->ref_segment_id[0]==0&&os->ref_mode==REF_WHOLE,
         "老数据缺引用字段时默认不引用");
      ck(os&&os->ref_index>=1&&os->ref_start>=1&&os->ref_len==0,
         "老数据的取法参数取合法默认值"); appdata_free(o); }
    /* 存档里写了非法参数（手改坏了）也要夹回合法范围 */
    { AppData *o=appdata_from_json("{\"templates\":[{\"name\":\"坏\",\"segments\":["
        "{\"type\":\"text\",\"label\":\"A\",\"refSegmentId\":\"seg-1\",\"refMode\":\"split\","
        "\"refIndex\":0,\"refStart\":-5,\"refLen\":-9}]}]}");
      Segment *os=o&&o->templates?o->templates->segments:NULL;
      ck(os&&os->ref_index==1&&os->ref_start==1&&os->ref_len==0,
         "非法的取法参数被夹回合法范围"); appdata_free(o); } }
  /* 取两标记之间 + 逐段 + 批量 + 不计入输出 往返 */
  { AppData *d=appdata_create(); Template *t=tmpl_create(L"批量T");
    Segment *src=seg_create(SEG_TEXT), *dev=seg_create(SEG_TEXT);
    ParseRule *pr; char *j,*j2; AppData *d2; Template *t2; Segment *src2,*dev2; ParseRule *pr2;
    qp_strlcpy(src->label,L"告警",QP_LABEL_MAX);
    qp_strlcpy(dev->label,L"设备名",QP_LABEL_MAX); dev->hidden=1; dev->no_output=1;
    pr=parse_rule_create(); pr->type=PARSE_BETWEEN;
    qp_strlcpy(pr->keyword,L"网络设备",QP_KEYWORD_MAX);
    qp_strlcpy(pr->marker_after,L"经核查",QP_KEYWORD_MAX);
    pr->per_paragraph=1;
    qp_strlcpy(pr->target_segment_id,dev->id,QP_ID_MAX);
    parse_rule_append(&src->parse_rules,pr);
    seg_list_append(&t->segments,src); seg_list_append(&t->segments,dev);
    t->batch_mode=1; qp_strlcpy(t->batch_source_id,src->id,QP_ID_MAX);
    t->table_output_enabled=1;
    qp_strlcpy(t->table_output_format,L"{甲}\\t{乙}",QP_OUTPUT_MAX);
    tmpl_list_append(&d->templates,t);
    j=appdata_to_json(d);
    ck(strstr(j,"\"between\"")!=NULL,"取两标记之间写成 type=between");
    ck(strstr(j,"markerAfter")!=NULL,"后标记写入 JSON");
    ck(strstr(j,"perParagraph")!=NULL,"逐段标记写入 JSON");
    ck(strstr(j,"noOutput")!=NULL,"不计入输出写入 JSON");
    ck(strstr(j,"batchMode")!=NULL,"批量开关写入 JSON");
    ck(strstr(j,"batchSourceId")!=NULL,"批量源写入 JSON");
    ck(strstr(j,"tableOutputEnabled")!=NULL,"表格输出开关写入 JSON");
    ck(strstr(j,"tableOutputFormat")!=NULL,"表格输出格式写入 JSON");
    d2=appdata_from_json(j); t2=d2->templates;
    src2=seg_list_at(t2->segments,0); dev2=seg_list_at(t2->segments,1);
    pr2=parse_rule_at(src2->parse_rules,0);
    ck(pr2&&pr2->type==PARSE_BETWEEN,"取法往返为 between");
    cks(pr2?pr2->keyword:NULL,L"网络设备","前标记往返");
    cks(pr2?pr2->marker_after:NULL,L"经核查","后标记往返");
    ck(pr2&&pr2->per_paragraph==1,"逐段标记往返");
    ck(dev2->no_output==1,"不计入输出往返仍为 1");
    ck(src2->no_output==0,"源变量未设不计入输出仍为 0");
    ck(t2->batch_mode==1,"批量开关往返");
    cks(t2->batch_source_id,src->id,"批量源 id 往返");
    ck(t2->table_output_enabled==1,"表格输出开关往返");
    cks(t2->table_output_format,L"{甲}\\t{乙}","表格输出格式往返");
    j2=appdata_to_json(d2);
    ck(j2&&strstr(j2,"markerAfter")&&strstr(j2,"perParagraph")&&
       strstr(j2,"noOutput")&&strstr(j2,"batchMode")&&strstr(j2,"batchSourceId")&&
       strstr(j2,"tableOutputEnabled")&&strstr(j2,"tableOutputFormat"),
       "新字段读回后再写仍保留");
    free(j); free(j2); appdata_free(d); appdata_free(d2);
  /* 正则解析规则往返：模式里的反斜杠不能丢 */
  { AppData *d=appdata_create(); Template *t=tmpl_create(L"正则T");
    Segment *src=seg_create(SEG_TEXT), *dev=seg_create(SEG_TEXT);
    ParseRule *pr; char *j; AppData *d2; ParseRule *pr2;
    qp_strlcpy(src->label,L"原文",QP_LABEL_MAX);
    qp_strlcpy(dev->label,L"设备",QP_LABEL_MAX);
    pr=parse_rule_create(); pr->type=PARSE_REGEX;
    qp_strlcpy(pr->pattern,L"设备编号[:：]\\s*([A-Za-z0-9\\-]+)",QP_PATTERN_MAX);
    pr->capture_group=1;
    qp_strlcpy(pr->target_segment_id,dev->id,QP_ID_MAX);
    parse_rule_append(&src->parse_rules,pr);
    seg_list_append(&t->segments,src); seg_list_append(&t->segments,dev);
    tmpl_list_append(&d->templates,t);
    j=appdata_to_json(d);
    ck(strstr(j,"\"regex\"")!=NULL,"正则规则写成 type=regex");
    ck(strstr(j,"\"pattern\"")!=NULL,"正则模式写入 JSON");
    ck(strstr(j,"captureGroup")!=NULL,"捕获组写入 JSON");
    d2=appdata_from_json(j);
    pr2=parse_rule_at(d2->templates->segments->parse_rules,0);
    ck(pr2&&pr2->type==PARSE_REGEX,"正则类型往返");
    cks(pr2?pr2->pattern:NULL,L"设备编号[:：]\\s*([A-Za-z0-9\\-]+)",
       "正则模式往返（反斜杠不丢）");
    ck(pr2&&pr2->capture_group==1,"捕获组往返");
    free(j); appdata_free(d); appdata_free(d2); }
  /* 非正则规则不写 pattern / captureGroup；旧数据缺这两键取默认 */
  { AppData *o=appdata_from_json("{\"templates\":[{\"name\":\"旧正则\",\"segments\":["
      "{\"type\":\"text\",\"label\":\"S\",\"parseRules\":[{\"type\":\"split\","
      "\"keyword\":\"A\",\"targetSegmentId\":\"x\"}]}]}]}");
    Segment *os=o&&o->templates?o->templates->segments:NULL;
    ParseRule *opr=os?parse_rule_at(os->parse_rules,0):NULL;
    ck(opr&&opr->pattern[0]==0&&opr->capture_group==0,
       "旧数据缺 pattern/captureGroup 取默认");
    appdata_free(o); }
  { AppData *e=appdata_create(); Template *et=tmpl_create(L"普通2");
    Segment *es=seg_create(SEG_TEXT); ParseRule *er=parse_rule_create(); char *ej;
    qp_strlcpy(es->label,L"变量",QP_LABEL_MAX);
    parse_rule_append(&es->parse_rules,er);
    seg_list_append(&et->segments,es); tmpl_list_append(&e->templates,et);
    ej=appdata_to_json(e);
    ck(strstr(ej,"pattern")==NULL&&strstr(ej,"captureGroup")==NULL,
       "非正则规则不写 pattern / captureGroup");
    free(ej); appdata_free(e); }
    /* 不开批量的模板不写 batchMode / batchSourceId（旧数据保持字节一致） */
    { AppData *e=appdata_create(); Template *et=tmpl_create(L"普通");
      Segment *es=seg_create(SEG_TEXT); ParseRule *er=parse_rule_create(); char *ej;
      qp_strlcpy(es->label,L"变量",QP_LABEL_MAX);
      qp_strlcpy(et->batch_source_id,es->id,QP_ID_MAX);
      parse_rule_append(&es->parse_rules,er);
      seg_list_append(&et->segments,es); tmpl_list_append(&e->templates,et);
      ej=appdata_to_json(e);
      ck(strstr(ej,"batchMode")==NULL&&strstr(ej,"batchSourceId")==NULL,
         "不开批量时两个字段都不写");
      ck(strstr(ej,"tableOutputEnabled")==NULL&&strstr(ej,"tableOutputFormat")==NULL,
         "未配置表格输出时两个字段都不写");
      ck(strstr(ej,"perParagraph")==NULL&&strstr(ej,"noOutput")==NULL,
         "为假的可选布尔字段不写");
      free(ej); appdata_free(e); }
    /* 旧数据：缺这些键一律默认关闭 */
    { AppData *o=appdata_from_json("{\"templates\":[{\"name\":\"旧\",\"segments\":["
        "{\"type\":\"text\",\"label\":\"S\",\"parseRules\":[{\"type\":\"split\","
        "\"keyword\":\"A\",\"targetSegmentId\":\"x\"}]}]}]}");
      Segment *os=o&&o->templates?o->templates->segments:NULL;
      ParseRule *opr=os?parse_rule_at(os->parse_rules,0):NULL;
      ck(o&&o->templates&&o->templates->batch_mode==0,"旧数据缺 batchMode 默认关闭");
      ck(o&&o->templates&&o->templates->batch_source_id[0]==0,"旧数据缺 batchSourceId 为空");
      ck(o&&o->templates&&o->templates->table_output_enabled==0,
         "旧数据缺 tableOutputEnabled 默认关闭");
      ck(o&&o->templates&&o->templates->table_output_format[0]==0,
         "旧数据缺 tableOutputFormat 默认为空");
      ck(os&&os->no_output==0,"旧数据缺 noOutput 默认关闭");
      ck(opr&&opr->type==PARSE_SPLIT&&opr->marker_after[0]==0&&opr->per_paragraph==0,
         "旧数据缺 markerAfter/perParagraph 取默认");
      appdata_free(o); } }
  /* 手改坏的解析规则不能把非法类型/段号带进引擎 */
  { AppData *o=appdata_from_json("{\"templates\":[{\"name\":\"容错\",\"segments\":["
      "{\"type\":\"text\",\"label\":\"S\",\"parseRules\":["
      "{\"type\":\"unknown\",\"segmentIndex\":0},"
      "{\"type\":17,\"segmentIndex\":-4}]}]}]}");
    Segment *os=o&&o->templates?o->templates->segments:NULL;
    ParseRule *a=os?parse_rule_at(os->parse_rules,0):NULL;
    ParseRule *b=os?parse_rule_at(os->parse_rules,1):NULL;
    ck(a&&a->type==PARSE_SPLIT&&b&&b->type==PARSE_SPLIT,
       "非法解析类型回落 split");
    ck(a&&a->segment_index==1&&b&&b->segment_index==1,
       "非法解析段号夹到 1");
    appdata_free(o); }
  /* 容错 */
  { ck(appdata_from_json("{ 坏的 json")==NULL,"非法 JSON 返回 NULL");
    AppData *e=appdata_from_json("{}"); ck(e!=NULL&&e->templates==NULL,"空对象得到空数据"); appdata_free(e);
    AppData *m=appdata_from_json("{\"templates\":[{\"name\":\"仅名字\"}]}");
    ck(m&&m->templates&&m->templates->id[0]!=0,"缺失 id 自动生成");
    cks(m->templates->name,L"仅名字","缺字段模板可读"); appdata_free(m); }
  /* 文件读写 + 导入导出 */
  { AppData *d=appdata_create_seed(); AppData *d2=appdata_create(); int a=0,u=0;
    remove("/tmp/quickphrase-storage-test/data.json");
    remove("/tmp/quickphrase-storage-test/data.json.tmp");
    remove("/tmp/quickphrase-storage-test/export.json");
    remove("/tmp/quickphrase-storage-test/export.json.tmp");
    storage_set_data_dir(L"/tmp/quickphrase-storage-test");
    ck(storage_save(d)==1,"保存到数据目录");
    { int cr=0; AppData *l=storage_load(&cr); ck(l&&tmpl_list_count(l->templates)==2,"从文件载入 2 个模板"); ck(cr==0,"已存在文件不算首次创建"); appdata_free(l);}
    ck(storage_export(d,L"/tmp/quickphrase-storage-test/export.json")==1,"导出到指定路径");
    ck(storage_import(d2,L"/tmp/quickphrase-storage-test/export.json",QP_IMPORT_MERGE,&a,&u)==1,"导入成功");
    ck(a==2&&u==0,"合并导入：新增 2 更新 0");
    a=u=0; storage_import(d2,L"/tmp/quickphrase-storage-test/export.json",QP_IMPORT_MERGE,&a,&u);
    ck(a==0&&u==2,"再次合并导入：新增 0 更新 2");
    ck(tmpl_list_count(d2->templates)==2,"合并后不产生重复模板");
    { Template *extra=tmpl_create(L"额外模板"); tmpl_list_append(&d2->templates,extra); }
    a=u=0; storage_import(d2,L"/tmp/quickphrase-storage-test/export.json",QP_IMPORT_REPLACE,&a,&u);
    ck(tmpl_list_count(d2->templates)==2,"覆盖导入后仅剩文件内模板");
    ck(storage_import(d2,L"/tmp/quickphrase-storage-test/missing.json",QP_IMPORT_MERGE,&a,&u)==0,"导入不存在文件返回失败");
    appdata_free(d); appdata_free(d2);
    storage_set_data_dir(NULL);
    remove("/tmp/quickphrase-storage-test/data.json");
    remove("/tmp/quickphrase-storage-test/data.json.tmp");
    remove("/tmp/quickphrase-storage-test/export.json");
    remove("/tmp/quickphrase-storage-test/export.json.tmp");
    { wchar_t path[QP_PATH_MAX];
      ck(storage_get_data_dir(path,QP_PATH_MAX)&&
         wcscmp(path,L"/tmp/quickphrase-storage-test")!=0,
         "文件测试后恢复默认数据目录"); } }
  printf("\n==========\n通过 %d 项，失败 %d 项",P,F);
  if(S) printf("，跳过 %d 组（本机没有验收夹具）",S);
  printf("\n");
  return F?1:0;
}
