#include "editor_layout.h"
#include <string.h>
static int px(int n,int dpi){return (n*dpi+48)/96;}
static int min(int a,int b){return a<b?a:b;}
static QpRect rect(int x,int y,int w,int h){return (QpRect){x,y,w>0?w:0,h>0?h:0};}
void qp_editor_preview_bounds(int dpi,int height,int *low,int *high)
{
    *low=px(32,dpi);
    *high=height-px(12,dpi)-px(26,dpi)-px(8,dpi)-px(18,dpi)-px(8,dpi)-px(130,dpi)-px(180,dpi);
    if(*high<*low)*high=*low;
}
int qp_editor_preview_height(int dpi,int height,int wanted)
{
    int lo,hi;qp_editor_preview_bounds(dpi,height,&lo,&hi);
    if(wanted<=0)wanted=px(46,dpi);
    return wanted<lo?lo:wanted>hi?hi:wanted;
}
int qp_editor_splitter_y(int dpi,int height,int wanted)
{
    return height-px(12,dpi)-px(26,dpi)-px(8,dpi)-px(18,dpi)-px(8,dpi)-qp_editor_preview_height(dpi,height,wanted);
}
void qp_editor_layout(const QpEditorLayoutInput *in,QpEditorLayout *out)
{
    memset(out,0,sizeof(*out));if(!in || in->width<=0 || in->height<=0)return;
    int d=in->dpi>0?in->dpi:96,w=in->width,h=in->height,m=px(12,d),y=m;
    int batch_y=px(42,d),top=px(72,d),group_x=m+px(358,d);
    out->name_label=rect(m,y+px(4,d),px(66,d),px(18,d));
    out->name=rect(m+px(72,d),y,px(230,d),px(24,d));
    out->group_label=rect(m+px(310,d),y+px(4,d),px(44,d),px(18,d));
    out->group=rect(group_x,y,min(px(180,d),w-m-group_x),px(24,d));
    if(w<px(562,d)) {
        out->name.w=w-m-out->name.x;
        out->group_label=rect(m,px(44,d),px(66,d),px(18,d));
        out->group=rect(m+px(72,d),px(40,d),w-m*2-px(72,d),px(24,d));
        batch_y=px(70,d);top=px(100,d);
    }
    out->batch=rect(m,batch_y,px(230,d),px(22,d));
    out->batch_label=rect(m+px(236,d),batch_y+px(3,d),px(52,d),px(18,d));
    out->batch_source=rect(m+px(292,d),batch_y,min(px(200,d),w-m*2-px(292,d)),px(24,d));
    out->narrow=w<px(800,d);
    if(out->narrow) {
        int half=(w-m*2-px(4,d))/2;
        out->page_tabs[0]=rect(m,top,half,px(24,d));
        out->page_tabs[1]=rect(m+half+px(4,d),top,half,px(24,d));
        top+=px(26,d);
    }
    int prev=qp_editor_preview_height(d,h,in->preview_height),btn_y=h-m-px(26,d);
    out->preview=rect(m,btn_y-px(8,d)-prev,w-m*2,prev);
    out->preview_label=rect(m,out->preview.y-px(18,d),w-m*2,px(16,d));
    out->body_bottom=out->preview_label.y-px(8,d);
    out->ok=rect(w-m-px(190,d),btn_y,px(92,d),px(26,d));
    out->cancel=rect(w-m-px(92,d),btn_y,px(92,d),px(26,d));
    int extra=w-px(880,d);if(extra<0)extra=0;
    int list_w=out->narrow?w-m*2:px(332,d)+extra*2/5;
    int prop_x=out->narrow?m:m+list_w+px(12,d);
    out->segment_label=rect(m,top,list_w,px(18,d));
    y=top+px(24,d);int bw=(list_w-px(12,d))/5;
    for(int i=0;i<5;++i)out->toolbar[i]=rect(m+i*(bw+px(3,d)),y,bw,px(24,d));
    y+=px(28,d);
    out->toolbar[5]=rect(m,y,bw,px(24,d));
    out->toolbar[6]=rect(m+bw+px(3,d),y,px(40,d),px(24,d));
    out->toolbar[7]=rect(m+bw+px(46,d),y,px(40,d),px(24,d));
    y+=px(30,d);
    out->segment=rect(m,y,list_w,out->body_bottom-y);
    out->properties=rect(prop_x,top,w-prop_x-m,out->body_bottom-top);
}
