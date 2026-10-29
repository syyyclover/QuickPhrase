#include "editor_layout.h"
#include <assert.h>
#include <stdio.h>
static int px(int n,int dpi){return (n*dpi+48)/96;}
static void fits(QpRect r,int w,int h){assert(r.w>=0 && r.h>=0 && r.x>=0 && r.y>=0 && r.x+r.w<=w && r.y+r.h<=h);}
int main(void)
{
    const int scales[]={96,120,144,192};int cases=0;
    for(int d=0;d<4;++d)for(int narrow=0;narrow<2;++narrow)for(int page=0;page<2;++page) {
        int dpi=scales[d],w=narrow?1340:px(880,dpi),h=narrow?670:px(690,dpi);
        if(!narrow)w=px(880,dpi);
        for(int preview=0;preview<2;++preview) {
            QpEditorLayoutInput in={dpi,w,h,preview?10000:0,page};QpEditorLayout out;
            qp_editor_layout(&in,&out);
            fits(out.name,w,h);fits(out.group,w,h);fits(out.batch,w,h);
            fits(out.segment,w,h);fits(out.properties,w,h);fits(out.preview,w,h);
            fits(out.ok,w,h);fits(out.cancel,w,h);fits(out.page_tabs[0],w,h);fits(out.page_tabs[1],w,h);
            assert(out.preview.y+out.preview.h<=out.ok.y-px(8,dpi));
            assert(out.narrow==(w<px(800,dpi)));
            if(out.narrow)assert(out.page_tabs[0].h>0);
            ++cases;
        }
    }
    printf("editor layout: %d DPI/workarea/page/preview cases fit client\n",cases);
    return 0;
}
