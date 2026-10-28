#include "rule_layout.h"
#include <string.h>

static int scale(int v,int dpi) { return (v*dpi+48)/96; }
static int larger(int a,int b) { return a>b?a:b; }

void qp_rule_layout(const QpRuleLayoutInput *in,QpRuleLayout *out)
{
    int dpi=in->dpi>0?in->dpi:96, m=scale(14,dpi), gap=scale(8,dpi);
    int labelw=scale(84,dpi), x=m+labelw+gap, full=in->width-2*m;
    int row=larger(scale(24,dpi),in->line_height+scale(8,dpi));
    int labelh=larger(scale(18,dpi),in->line_height), y=m;
    int kind=in->kind>=0 && in->kind<=4?in->kind:0;
    unsigned rows=1;
    if(kind!=4) rows|=(1u<<3)|(1u<<5)|(1u<<6);
    if(kind==0) rows|=(1u<<1)|(1u<<2);
    if(kind==2) rows|=1u<<4;
    if(kind==3) rows|=1u<<2;
    memset(out,0,sizeof(*out)); out->visible_rows=rows;
    for(int k=0;k<7;++k) if(rows&(1u<<k)) {
        out->labels[k]=(QpRect){m,y+(row-labelh)/2,labelw,labelh};
        out->fields[k]=(QpRect){x,y,(k==1 || k==2)?scale(90,dpi):in->width-m-x,row};
        y+=row+gap;
    }
    if(kind==4) {
        out->lookup_label=(QpRect){m,y+(row-labelh)/2,labelw,labelh};
        out->lookup_button=(QpRect){x,y,scale(110,dpi),row};
        y+=row+gap;
        out->summary=(QpRect){m,y,full,larger(in->summary_height,in->line_height*2+gap)};
        y+=out->summary.h+gap;
    }
    out->paragraph=(QpRect){m,y,full,larger(scale(22,dpi),in->line_height+scale(4,dpi))};
    y+=out->paragraph.h+scale(4,dpi);
    out->paragraph_help=(QpRect){m+scale(22,dpi),y,full-scale(22,dpi),in->paragraph_height};
    y+=in->paragraph_height+gap;
    out->paragraph_mode_label=(QpRect){m,y+(row-labelh)/2,scale(56,dpi),labelh};
    int mx=m+scale(56,dpi)+gap, mw=scale(110,dpi), sx=mx+mw+gap;
    out->paragraph_mode=(QpRect){mx,y,mw,row};
    out->paragraph_separator_label=(QpRect){sx,y+(row-labelh)/2,scale(56,dpi),labelh};
    sx+=scale(56,dpi)+gap;
    out->paragraph_separator=(QpRect){sx,y,in->width-m-sx,row};
    y+=row+gap;
    out->tip=(QpRect){m,y,full,in->tip_height};
    y+=in->tip_height+scale(12,dpi);
    int buttonh=larger(scale(26,dpi),in->line_height+gap), buttonw=scale(86,dpi);
    out->minimum_height=y+buttonh+m;
    int height=larger(in->height,out->minimum_height), by=height-m-buttonh;
    out->cancel=(QpRect){in->width-m-buttonw,by,buttonw,buttonh};
    out->ok=(QpRect){out->cancel.x-gap-buttonw,by,buttonw,buttonh};
}
