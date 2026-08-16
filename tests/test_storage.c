#include "../src/storage.h"
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int P=0,F=0;
static void ck(int c,const char*w){ if(c){++P;printf("  [ok]   %s\n",w);} else {++F;printf("  [FAIL] %s\n",w);} }
static void cks(const wchar_t*g,const wchar_t*w,const char*n){ if(g&&w&&wcscmp(g,w)==0){++P;printf("  [ok]   %s\n",n);} else {++F;printf("  [FAIL] %s  期望[%ls] 实际[%ls]\n",n,w?w:L"",g?g:L"");} }
int main(void){
  setlocale(LC_ALL,"");
  printf("存储层自测\n==========\n");
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
    ck(tmpl_list_count(d2->templates)==1,"模板数量一致");
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
      ck(cond_rule_at(b->condition_rules,0)->op==OP_IN,"运算符保持 in"); }
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
  /* OP_IN 列表往返 */
  { AppData *d=appdata_create(); Template *t=tmpl_create(L"T"); Segment *s=seg_create(SEG_STATIC);
    ConditionRule *r=cond_rule_create(); char *j; AppData *d2; ConditionRule *r2;
    r->op=OP_IN; cond_rule_set_match(r,L"甲,乙,丙"); qp_strlcpy(r->result_value,L"R",QP_RESULT_MAX);
    cond_rule_append(&s->condition_rules,r); seg_list_append(&t->segments,s); tmpl_list_append(&d->templates,t);
    j=appdata_to_json(d); ck(strstr(j,"matchValues")!=NULL,"OP_IN 写出 matchValues 数组");
    d2=appdata_from_json(j); r2=d2->templates->segments->condition_rules;
    ck(r2->op==OP_IN && r2->match_value_count==3,"OP_IN 列表往返 3 项");
    cks(r2->match_values[2],L"丙","OP_IN 第三项一致");
    free(j); appdata_free(d); appdata_free(d2); }
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
         wcscmp(p,L"C:\\CustomDir\\data.json")==0,"自定义 data.json 路径"); }
    storage_set_data_dir(NULL);
    { wchar_t p[QP_PATH_MAX];
      ck(storage_get_data_dir(p,QP_PATH_MAX)&&
         wcscmp(p,L"C:\\CustomDir")!=0,"清除后恢复默认目录"); } }
  /* 容错 */
  { ck(appdata_from_json("{ 坏的 json")==NULL,"非法 JSON 返回 NULL");
    AppData *e=appdata_from_json("{}"); ck(e!=NULL&&e->templates==NULL,"空对象得到空数据"); appdata_free(e);
    AppData *m=appdata_from_json("{\"templates\":[{\"name\":\"仅名字\"}]}");
    ck(m&&m->templates&&m->templates->id[0]!=0,"缺失 id 自动生成");
    cks(m->templates->name,L"仅名字","缺字段模板可读"); appdata_free(m); }
  /* 文件读写 + 导入导出 */
  { AppData *d=appdata_create_seed(); AppData *d2=appdata_create(); int a=0,u=0;
    ck(storage_save(d)==1,"保存到数据目录");
    { int cr=0; AppData *l=storage_load(&cr); ck(l&&tmpl_list_count(l->templates)==1,"从文件载入 1 个模板"); ck(cr==0,"已存在文件不算首次创建"); appdata_free(l);}
    ck(storage_export(d,L"/tmp/qp_export.json")==1,"导出到指定路径");
    ck(storage_import(d2,L"/tmp/qp_export.json",QP_IMPORT_MERGE,&a,&u)==1,"导入成功");
    ck(a==1&&u==0,"合并导入：新增 1 更新 0");
    a=u=0; storage_import(d2,L"/tmp/qp_export.json",QP_IMPORT_MERGE,&a,&u);
    ck(a==0&&u==1,"再次合并导入：新增 0 更新 1");
    ck(tmpl_list_count(d2->templates)==1,"合并后不产生重复模板");
    { Template *extra=tmpl_create(L"额外模板"); tmpl_list_append(&d2->templates,extra); }
    a=u=0; storage_import(d2,L"/tmp/qp_export.json",QP_IMPORT_REPLACE,&a,&u);
    ck(tmpl_list_count(d2->templates)==1,"覆盖导入后仅剩文件内模板");
    ck(storage_import(d2,L"/tmp/不存在的文件.json",QP_IMPORT_MERGE,&a,&u)==0,"导入不存在文件返回失败");
    appdata_free(d); appdata_free(d2); }
  printf("\n==========\n通过 %d 项，失败 %d 项\n",P,F);
  return F?1:0;
}
