#include "ui_internal.h"

static int pending_index=-1;
static HWND pending_anchor;
static EvalContext *pending_context;

void ui_choice_cancel_pending(void)
{
    pending_index=-1;pending_anchor=NULL;pending_context=NULL;
}

static HWND choice_anchor(int index)
{
    for(int i=0;i<g_fieldCount;++i)if(g_fields[i].field_index==index)return g_fields[i].hCtl;
    return NULL;
}

void ui_choice_single_queue(int index,HWND anchor)
{
    int queued=pending_index>=0;
    pending_index=index;pending_anchor=anchor;pending_context=g_ctx;
    if(!queued && !PostMessageW(g_hwndMain,WM_QP_CHOICE_INPUT,0,0))ui_choice_cancel_pending();
}

void ui_choice_dispatch(void)
{
    int index=pending_index;
    HWND anchor=pending_anchor;
    EvalContext *context=pending_context;
    ui_choice_cancel_pending();
    if(index<0 || g_ctx!=context || !g_ctx || index>=g_ctx->count ||
       !anchor || choice_anchor(index)!=anchor || !IsWindow(anchor))return;
    if(!IsWindowEnabled(g_hwndMain) || GetFocus()!=anchor) {ui_sync_controls();return;}
    int ci=qp_choice_custom_index(g_ctx->fields[index].seg);
    int selected=(int)SendMessageW(anchor,CB_GETCURSEL,0,0);
    if(ci<0)return;
    for(int i=0;i<g_fieldCount;++i)if(g_fields[i].field_index==index) {
        if(selected==ci || (g_ctx->fields[index].custom_selected && selected==g_fields[i].extraIndex))
            ui_choice_single(index);
        return;
    }
}

void ui_choice_single(int index)
{
    if(!g_ctx || index<0 || index>=g_ctx->count || g_ctx->fields[index].rule_driven)return;
    EvalContext *context=g_ctx;
    const Segment *seg=context->fields[index].seg;
    if(qp_choice_custom_index(seg)<0)return;
    wchar_t text[QP_VALUE_MAX];
    qp_strlcpy(text,context->fields[index].custom_value,QP_VALUE_MAX);
    int accepted=dlg_input(g_hwndMain,L"自主补充",L"请输入自己的内容：",text,QP_VALUE_MAX);
    if(g_ctx!=context || index>=g_ctx->count || g_ctx->fields[index].seg!=seg)return;
    if(accepted) {
        EvalField *field=&g_ctx->fields[index];
        eval_set_user_value(g_ctx,index,text);
        field->choice_tracked=1;field->custom_selected=text[0]!=0;
        qp_strlcpy(field->custom_value,text,QP_VALUE_MAX);field->choice_base[0]=0;
        ui_refresh_current();
    } else ui_sync_controls();
}

void ui_choice_multi(int index,const unsigned char *checked,int count)
{
    if(!g_ctx || index<0 || index>=g_ctx->count || g_ctx->fields[index].rule_driven)return;
    EvalContext *context=g_ctx;
    const Segment *seg=context->fields[index].seg;
    int ci=qp_choice_custom_index(seg);
    if(ci<0 || count!=seg->option_count || !checked)return;
    unsigned char *selected=malloc((size_t)count);
    if(!selected) {
        qp_set_text_if_diff(g_hHint,L"内存不足，补充窗口未打开。请关闭不需要的窗口后重试。");
        return;
    }
    memcpy(selected,checked,(size_t)count);
    HWND anchor=choice_anchor(index);
    wchar_t text[QP_VALUE_MAX];
    qp_strlcpy(text,context->fields[index].custom_value,QP_VALUE_MAX);
    /* The input dialog disables its owner; close the capture popup first and
       own the copied mask before synchronous cancellation destroys that popup. */
    popup_close();
    int accepted=dlg_input(g_hwndMain,L"自主补充",L"请输入自己的内容：",text,QP_VALUE_MAX);
    if(g_ctx==context && index<g_ctx->count && g_ctx->fields[index].seg==seg &&
       qp_choice_custom_index(seg)==ci && seg->option_count==count) {
        if(accepted) {
            selected[ci]=(unsigned char)(text[0]!=0);
            if(qp_choice_set(g_ctx,index,selected,count,text,multi_sep(seg)))ui_refresh_current();
            else qp_set_text_if_diff(g_hHint,L"补充后内容超过变量长度上限，本次修改未生效。请缩短内容后重试。");
        }
        if(anchor && IsWindow(anchor))popup_open(index,anchor);
    }
    free(selected);
}
