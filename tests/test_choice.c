#include "storage.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int main(void)
{
    AppData *data=appdata_from_json("{\"templates\":[{\"segments\":[{\"type\":\"multi-select\","
        "\"label\":\"choice\",\"options\":[\"A\",\"Other\"],\"customOptionIndex\":1}]}]}");
    assert(data);
    char *json=appdata_to_json(data);
    assert(json && strstr(json,"customOptionIndex"));
    Segment *seg=data->templates->segments;
    assert(qp_choice_custom_index(seg)==1);
    Segment *clone=seg_clone(seg);assert(clone && clone->custom_option==2);
    assert(seg_move_option(clone,1,-1) && clone->custom_option==1);
    seg_remove_option(clone,0);assert(!clone->custom_option);seg_free(clone);
    EvalContext *ctx=eval_create(data->templates);assert(ctx);
    unsigned char selected[2]={1,1};
    assert(qp_choice_set(ctx,0,selected,2,L"A / extra",L" / "));
    assert(!wcscmp(ctx->fields[0].user_value,L"A / A / extra"));
    assert(!wcscmp(ctx->fields[0].choice_base,L"A"));
    assert(!wcscmp(ctx->fields[0].custom_value,L"A / extra") && ctx->fields[0].custom_selected);
    assert(qp_choice_is_checked(&ctx->fields[0],0,L" / ") && qp_choice_is_checked(&ctx->fields[0],1,L" / "));
    selected[0]=0;assert(qp_choice_set(ctx,0,selected,2,L"A / extra",L" / "));
    eval_run(ctx);
    assert(!qp_choice_is_checked(&ctx->fields[0],0,L" / ") && qp_choice_is_checked(&ctx->fields[0],1,L" / "));
    selected[0]=1;assert(qp_choice_set(ctx,0,selected,2,L"A / extra",L" / "));
    eval_run(ctx);assert(!wcscmp(ctx->fields[0].value,L"A / A / extra"));
    assert(!wcscmp(seg->options[1],L"Other"));
    selected[1]=0;assert(qp_choice_set(ctx,0,selected,2,ctx->fields[0].custom_value,L" / "));
    assert(!ctx->fields[0].custom_selected && !wcscmp(ctx->fields[0].custom_value,L"A / extra"));
    wchar_t huge[QP_VALUE_MAX];wmemset(huge,L'x',QP_VALUE_MAX-1);huge[QP_VALUE_MAX-1]=0;
    selected[1]=1;
    assert(!qp_choice_set(ctx,0,selected,2,huge,L" / "));
    assert(!wcscmp(ctx->fields[0].user_value,L"A") && !ctx->fields[0].custom_selected);
    assert(!qp_choice_set(ctx,0,selected,1,L"wrong",L" / "));
    eval_reset(ctx);assert(!ctx->fields[0].choice_tracked && !ctx->fields[0].custom_value[0]);
    eval_destroy(ctx);

    /* ---- 口径 A：模板改了选项之后，旧值必须向新选项集对齐 ---- */
    {
        unsigned char ck[8];
        wchar_t cout[QP_VALUE_MAX];
        const wchar_t *abc[3]={L"A",L"B",L"C"};
        const wchar_t *ac[2]={L"A",L"C"};
        const wchar_t *xy[2]={L"X",L"Y"};

        /* 多选：拆得开就按新选项集留；拆不开（分隔符认不出）原样退回 */
        Segment *m=seg_clone(seg);assert(m);
        assert(qp_seg_set_options(m,abc,3));
        m->custom_option=0;
        assert(qp_choice_realign(m,L"A、B、C",L"",L"、",ck,cout,QP_VALUE_MAX)==3); /* 三项都还在 */
        assert(qp_seg_set_options(m,ac,2));                            /* B 被删，只剩 A、C */
        assert(qp_choice_realign(m,L"A、B、C",L"",L"、",ck,cout,QP_VALUE_MAX)==2); /* 旧值里的 B 丢掉 */
        assert(ck[0]&&ck[1]);                                          /* A、C 都留下 */
        assert(qp_choice_realign(m,L"A、C",L"",L"、",ck,cout,QP_VALUE_MAX)==2);
        assert(qp_choice_realign(m,L"P、Q",L"",L"、",ck,cout,QP_VALUE_MAX)==0);   /* 拆得开但全不命中 */
        assert(qp_choice_realign(m,L"A | B",L"",L"、",ck,cout,QP_VALUE_MAX)==-1); /* 认不出：保持原值 */
        assert(qp_choice_realign(m,L"",L"",L"、",ck,cout,QP_VALUE_MAX)==0);       /* 没填过 */
        assert(qp_seg_set_options(m,NULL,0));                          /* 选项删光 */
        assert(qp_choice_realign(m,L"A",L"",L"、",ck,cout,QP_VALUE_MAX)==0);
        seg_free(m);

        /* 单选：值还在就留，被删就丢（没有分隔符歧义） */
        Segment *s1=seg_clone(seg);assert(s1);
        s1->type=SEG_SELECT;s1->custom_option=0;
        assert(qp_seg_set_options(s1,xy,2));
        assert(qp_choice_realign(s1,L"Y",L"",L"、",ck,cout,QP_VALUE_MAX)==1);
        assert(!ck[0]&&ck[1]);
        assert(qp_choice_realign(s1,L"Z",L"",L"、",ck,cout,QP_VALUE_MAX)==0);
        seg_free(s1);

        /* 多选 + 自主补充：补充文字不在选项库里，原样保留；新模板没了补充项就丢掉 */
        Segment *m2=seg_clone(seg);assert(m2);
        assert(qp_seg_set_options(m2,ac,2));
        m2->custom_option=2;
        assert(qp_choice_realign(m2,L"A",L"临时补充",L"、",ck,cout,QP_VALUE_MAX)==2);
        assert(ck[0]&&ck[1]&&!wcscmp(cout,L"临时补充"));
        assert(qp_choice_realign(m2,L"",L"临时补充",L"、",ck,cout,QP_VALUE_MAX)==1);
        assert(ck[1]&&!wcscmp(cout,L"临时补充"));                       /* 只填了补充文字也不丢 */
        m2->custom_option=0;
        assert(qp_choice_realign(m2,L"A",L"临时补充",L"、",ck,cout,QP_VALUE_MAX)==1);
        assert(ck[0]&&!ck[1]&&!cout[0]);
        seg_free(m2);
    }

    free(json);appdata_free(data);
    puts("custom choice JSON preserved");
    return 0;
}
