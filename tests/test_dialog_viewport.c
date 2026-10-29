#include "dialog_viewport.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    for(int scale=1;scale<=4;++scale) {
        int content=620*scale,view=600,current=0;
        assert(qp_dialog_scroll_limit(content,view)==content-view);
        current=qp_dialog_focus_scroll(content,view,current,580*scale,606*scale);
        assert(current+view>=606*scale && current<=580*scale);
        current=qp_dialog_focus_scroll(content,view,current,16*scale,42*scale);
        assert(current<=16*scale && current+view>=42*scale);
        assert(qp_dialog_focus_scroll(content,view,0,10,30)==0);
    }
    assert(qp_dialog_scroll_limit(100,200)==0);
    assert(qp_dialog_focus_scroll(100,200,100,10,20)==0);
    assert(qp_dialog_focus_scroll(100,10,0,-20,140)==0);
    puts("modal viewport: bottom actions and returning focus stay accessible at four scales");
    return 0;
}
