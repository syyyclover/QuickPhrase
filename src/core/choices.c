#include "engine.h"
#include <string.h>

int qp_choice_custom_index(const Segment *s)
{
    if(!s || (s->type!=SEG_SELECT && s->type!=SEG_MULTI_SELECT) ||
       s->custom_option<=0 || s->custom_option>s->option_count)return -1;
    return s->custom_option-1;
}

int qp_choice_join(const Segment *s,const unsigned char *checked,int count,
                   const wchar_t *custom,const wchar_t *sep,wchar_t *out,size_t cap)
{
    size_t used=0;
    int ci=qp_choice_custom_index(s);
    if(!s || !checked || count!=s->option_count || !out || !cap)return 0;
    if(!custom)custom=L"";
    if(!sep)sep=L"";
    out[0]=0;
    for(int i=0;i<count;++i)if(checked[i]) {
        const wchar_t *v=i==ci?custom:s->options[i];
        if(i==ci && !v[0])continue;
        size_t n=wcslen(v),gap=used?wcslen(sep):0;
        if(gap>=cap-used || n>=cap-used-gap)return 0;
        if(gap){wmemcpy(out+used,sep,gap);used+=gap;}
        wmemcpy(out+used,v,n+1);used+=n;
    }
    return 1;
}

int qp_choice_is_checked(const EvalField *field,int option,const wchar_t *sep)
{
    if(!field || !field->seg || option<0 || option>=field->seg->option_count)return 0;
    if(option==qp_choice_custom_index(field->seg))return !field->rule_driven && field->custom_selected;
    if(field->seg->type==SEG_SELECT) {
        if(field->custom_selected && !field->rule_driven)return 0;
        return qp_icmp(field->value,field->seg->options[option])==0;
    }
    const wchar_t *value=field->choice_tracked && !field->rule_driven?field->choice_base:field->value;
    const wchar_t *needle=field->seg->options[option];
    if(!value[0] || !needle)return 0;
    wchar_t part[QP_VALUE_MAX];
    for(int i=1;i<=128 && qp_split_take(value,sep && sep[0]?sep:L"、",i,part,QP_VALUE_MAX);++i) {
        qp_trim(part);
        if(qp_icmp(part,needle)==0)return 1;
    }
    return 0;
}

int qp_choice_set(EvalContext *ctx,int index,const unsigned char *checked,int count,
                  const wchar_t *custom,const wchar_t *sep)
{
    wchar_t value[QP_VALUE_MAX],base[QP_VALUE_MAX],custom_copy[QP_VALUE_MAX];
    if(!ctx || index<0 || index>=ctx->count)return 0;
    EvalField *f=&ctx->fields[index];
    int ci=qp_choice_custom_index(f->seg);
    if(!custom)custom=L"";
    if(wcslen(custom)>=QP_VALUE_MAX)return 0;
    qp_strlcpy(custom_copy,custom,QP_VALUE_MAX);custom=custom_copy;
    if(
       !qp_choice_join(f->seg,checked,count,custom,sep,value,QP_VALUE_MAX) ||
       !qp_choice_join(f->seg,checked,count,L"",sep,base,QP_VALUE_MAX))return 0;
    eval_set_user_value(ctx,index,value);
    f->choice_tracked=ci>=0;
    f->custom_selected=ci>=0 && checked[ci] && custom[0];
    qp_strlcpy(f->custom_value,ci>=0?custom:L"",QP_VALUE_MAX);
    qp_strlcpy(f->choice_base,ci>=0?base:L"",QP_VALUE_MAX);
    return 1;
}

int qp_choice_realign(const Segment *seg,const wchar_t *value,
                      const wchar_t *custom_in,const wchar_t *sep,
                      unsigned char *checked,wchar_t *custom_out,size_t cap)
{
    int n,ci,kept=0,segments=0;
    if(!seg||!checked||(seg->type!=SEG_SELECT&&seg->type!=SEG_MULTI_SELECT))return -1;
    if(custom_out&&cap)custom_out[0]=0;
    n=seg->option_count;
    if(n<=0)return 0;                       /* 新模板把选项删光了：值无处可依 */
    memset(checked,0,(size_t)n);
    ci=qp_choice_custom_index(seg);
    if(!value)value=L"";
    if(!custom_in)custom_in=L"";
    if(!sep||!sep[0])sep=L"、";
    if(!value[0] && !(ci>=0&&custom_in[0]))return 0;   /* 本来就没填过 */

    if(seg->type==SEG_SELECT) {
        /* 单选没有分隔符问题：旧值要么等于某一项，要么是自主补充文字，
           都对不上就是「这一项已被删/改名」，按口径 A 丢弃。 */
        for(int i=0;i<n;++i)if(i!=ci&&seg->options[i]&&qp_icmp(value,seg->options[i])==0) {
            checked[i]=1;return 1;
        }
        if(ci>=0&&custom_in[0]&&(!value[0]||qp_icmp(value,custom_in)==0)) {
            checked[ci]=1;
            if(custom_out&&cap)qp_strlcpy(custom_out,custom_in,cap);
            return 1;
        }
        return 0;
    }

    {
        wchar_t part[QP_VALUE_MAX];
        for(int k=1;k<=512&&qp_split_take(value,sep,k,part,QP_VALUE_MAX);++k) {
            ++segments;
            qp_trim(part);
            if(!part[0])continue;
            for(int i=0;i<n;++i) {
                if(i==ci||!seg->options[i])continue;
                if(qp_icmp(part,seg->options[i])==0) { checked[i]=1;break; }
            }
        }
    }
    if(ci>=0&&custom_in[0]) {               /* 补充文字不在选项库里，原样保留 */
        checked[ci]=1;
        if(custom_out&&cap)qp_strlcpy(custom_out,custom_in,cap);
    }
    for(int i=0;i<n;++i)if(checked[i])++kept;
    if(kept>0)return kept;
    if(segments>=2)return 0;                /* 分隔符拆得开 → 旧值确实一项都没留住 */
    return -1;                              /* 拆不开又全不命中 → 认不出，宁可保持原值 */
}
