#include "dialog_viewport.h"

int qp_dialog_scroll_limit(int content, int view)
{
    if(view<1)view=1;
    return content>view ? content-view : 0;
}

int qp_dialog_focus_scroll(int content,int view,int current,int first,int last)
{
    int limit=qp_dialog_scroll_limit(content,view);
    if(view<1)view=1;
    if(last-first>view)current=first;
    else if(first<current)current=first;
    else if(last>current+view)current=last-view;
    if(current<0)current=0;
    if(current>limit)current=limit;
    return current;
}
