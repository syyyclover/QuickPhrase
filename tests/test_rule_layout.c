#include "rule_layout.h"
#include <assert.h>
#include <stdio.h>

static int px(int n,int dpi) { return (n*dpi+48)/96; }
static void inside(QpRect r,int w,int h)
{ assert(r.x>=0 && r.y>=0 && r.w>=0 && r.h>=0 && r.x+r.w<=w && r.y+r.h<=h); }
int main(void)
{
    int scales[]={96,120,144,192}, widths[]={440,520,800};
    int cases=0;
    for(int s=0;s<4;++s) for(int w=0;w<3;++w) for(int kind=0;kind<5;++kind) {
        int dpi=scales[s];
        QpRuleLayoutInput in={dpi,px(widths[w],dpi),px(330,dpi),kind,
                             px(15,dpi),px(40,dpi),px(30,dpi),px(60,dpi)};
        QpRuleLayout out;
        qp_rule_layout(&in,&out);
        int h=in.height>out.minimum_height?in.height:out.minimum_height;
        for(int k=0;k<7;++k) if(out.visible_rows&(1u<<k)) {
            inside(out.labels[k],in.width,h); inside(out.fields[k],in.width,h);
            assert(out.fields[k].h>=in.line_height);
        }
        if(kind==4) {
            assert(out.visible_rows==1);
            inside(out.lookup_button,in.width,h); inside(out.summary,in.width,h);
            assert(out.summary.w>=in.width-px(30,dpi));
            assert(out.paragraph.y<=px(160,dpi));
            assert(out.paragraph.y-out.summary.y-out.summary.h<=px(12,dpi));
        } else {
            assert((out.visible_rows&(1u<<4))==(kind==2?1u<<4:0));
            assert((out.visible_rows&(1u<<1))==(kind==0?1u<<1:0));
        }
        inside(out.paragraph,in.width,h); inside(out.paragraph_help,in.width,h);
        inside(out.paragraph_mode,in.width,h); inside(out.paragraph_separator,in.width,h);
        assert(out.paragraph_mode.y>=out.paragraph_help.y+out.paragraph_help.h);
        assert(out.paragraph_separator.x>=out.paragraph_mode.x+out.paragraph_mode.w);
        assert(out.tip.y>=out.paragraph_mode.y+out.paragraph_mode.h);
        inside(out.tip,in.width,h); inside(out.ok,in.width,h); inside(out.cancel,in.width,h);
        assert(out.paragraph_help.h>=in.paragraph_height);
        assert(out.tip.h>=in.tip_height);
        assert(out.paragraph_help.y>=out.paragraph.y+out.paragraph.h);
        assert(out.tip.y>=out.paragraph_help.y+out.paragraph_help.h);
        assert(out.ok.y>=out.tip.y+out.tip.h+px(8,dpi));
        ++cases;
    }
    printf("parse rule layout: %d DPI/type/width cases passed\n",cases);
    return 0;
}
