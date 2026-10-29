#include "dialogs_internal.h"
#include "dialog_viewport.h"

/* Oversized modal layouts retain a virtual content canvas. The table/parser
   layouts use a minimum canvas when the owner's physical workarea is smaller;
   ordinary windows stay unchanged. State belongs to the root subclass. */
typedef struct {
    int width,height,x,y,min_width,min_height,resizable;
} QpDialogViewport;
typedef struct { HWND root; int okay; } QpDialogViewHook;
enum { QP_DLG_VIEW_FOCUS=WM_APP+71, QP_DLG_VIEW_ID=0x5156 };
static LRESULT CALLBACK dialog_view_proc(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);

static QpDialogViewport *dialog_view_state(HWND hwnd)
{
    DWORD_PTR ref=0;
    return GetWindowSubclass(hwnd,dialog_view_proc,QP_DLG_VIEW_ID,&ref)
        ? (QpDialogViewport *)ref : NULL;
}

void qp_dialog_content_rect(HWND hwnd,RECT *r)
{
    GetClientRect(hwnd,r);
    QpDialogViewport *v=dialog_view_state(hwnd);
    if(v && v->resizable) {
        if(r->right<v->min_width)r->right=v->min_width;
        if(r->bottom<v->min_height)r->bottom=v->min_height;
        v->width=r->right;v->height=r->bottom;
    }
}

int qp_dialog_require_size(HWND hwnd,int width,int height)
{
    QpDialogViewport *v=dialog_view_state(hwnd);
    if(!v || !v->resizable)return 0;
    v->min_width=width;v->min_height=height;
    return 1;
}

HDWP qp_dialog_defer(HDWP queue,HWND child,int x,int y,int width,int height)
{
    QpDialogViewport *v=dialog_view_state(GetParent(child));
    if(v){x-=v->x;y-=v->y;}
    return qp_defer(queue,child,x,y,width,height);
}

void qp_dialog_limit_tracking(HWND hwnd,MINMAXINFO *mmi)
{
    RECT work;qp_window_work_area(GetWindow(hwnd,GW_OWNER),&work);
    int width=work.right-work.left,height=work.bottom-work.top;
    if(mmi->ptMinTrackSize.x>width)mmi->ptMinTrackSize.x=width;
    if(mmi->ptMinTrackSize.y>height)mmi->ptMinTrackSize.y=height;
}

static void dialog_view_scroll(HWND hwnd,QpDialogViewport *v,int x,int y)
{
    RECT view;
    GetClientRect(hwnd,&view);
    int maxx=qp_dialog_scroll_limit(v->width,view.right);
    int maxy=qp_dialog_scroll_limit(v->height,view.bottom);
    if(x<0)x=0;
    if(x>maxx)x=maxx;
    if(y<0)y=0;
    if(y>maxy)y=maxy;
    int dx=v->x-x,dy=v->y-y;
    if(dx || dy) {
        int count=0;
        for(HWND child=GetWindow(hwnd,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))++count;
        HDWP queue=qp_layout_begin(count);
        for(HWND child=GetWindow(hwnd,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
            RECT r;
            if(!GetWindowRect(child,&r))continue;
            MapWindowPoints(NULL,hwnd,(POINT *)&r,2);
            queue=qp_defer(queue,child,r.left+dx,r.top+dy,r.right-r.left,r.bottom-r.top);
        }
        v->x=x;v->y=y;
        qp_layout_end(queue);
    }
    SCROLLINFO si={0};
    si.cbSize=sizeof(si);si.fMask=SIF_RANGE|SIF_PAGE|SIF_POS|SIF_DISABLENOSCROLL;
    si.nMax=v->width-1;si.nPage=(UINT)view.right;si.nPos=v->x;
    SetScrollInfo(hwnd,SB_HORZ,&si,TRUE);
    si.nMax=v->height-1;si.nPage=(UINT)view.bottom;si.nPos=v->y;
    SetScrollInfo(hwnd,SB_VERT,&si,TRUE);
}

void qp_dialog_update_view(HWND hwnd)
{
    QpDialogViewport *v=dialog_view_state(hwnd);
    if(v)dialog_view_scroll(hwnd,v,v->x,v->y);
}

static LRESULT CALLBACK dialog_view_proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,
                                        UINT_PTR id,DWORD_PTR ref)
{
    QpDialogViewport *v=(QpDialogViewport *)ref;
    if(msg==WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd,dialog_view_proc,id);
        free(v);
        return DefSubclassProc(hwnd,msg,wp,lp);
    }
    if(msg==WM_SIZE) {
        LRESULT result=DefSubclassProc(hwnd,msg,wp,lp);
        dialog_view_scroll(hwnd,v,v->x,v->y);
        return result;
    }
    if(v->resizable && msg==WM_GETMINMAXINFO) {
        LRESULT result=DefSubclassProc(hwnd,msg,wp,lp);
        qp_dialog_limit_tracking(hwnd,(MINMAXINFO *)lp);
        return result;
    }
    if(v->resizable && msg==WM_WINDOWPOSCHANGING) {
        WINDOWPOS *pos=(WINDOWPOS *)lp;
        if(!(pos->flags&SWP_NOSIZE)) {
            RECT work;qp_window_work_area(GetWindow(hwnd,GW_OWNER),&work);
            if(pos->cx>work.right-work.left)pos->cx=work.right-work.left;
            if(pos->cy>work.bottom-work.top)pos->cy=work.bottom-work.top;
        }
    }
    if(msg==QP_DLG_VIEW_FOCUS) {
        HWND child=(HWND)lp;RECT r,view;
        if(IsChild(hwnd,child) && GetWindowRect(child,&r)) {
            MapWindowPoints(NULL,hwnd,(POINT *)&r,2);GetClientRect(hwnd,&view);
            int x=qp_dialog_focus_scroll(v->width,view.right,v->x,r.left+v->x,r.right+v->x);
            int y=qp_dialog_focus_scroll(v->height,view.bottom,v->y,r.top+v->y,r.bottom+v->y);
            dialog_view_scroll(hwnd,v,x,y);
        }
        return 0;
    }
    if(msg==WM_MOUSEWHEEL) {
        int delta=(short)HIWORD(wp);
        dialog_view_scroll(hwnd,v,v->x,v->y+(delta>0?-DPIX(48):DPIX(48)));
        return 0;
    }
    if(msg==WM_VSCROLL || msg==WM_HSCROLL) {
        int vertical=msg==WM_VSCROLL,position=vertical?v->y:v->x;
        SCROLLINFO si={0};si.cbSize=sizeof(si);si.fMask=SIF_TRACKPOS|SIF_PAGE;
        GetScrollInfo(hwnd,vertical?SB_VERT:SB_HORZ,&si);
        switch(LOWORD(wp)) {
        case SB_LINEUP:position-=DPIX(24);break;
        case SB_LINEDOWN:position+=DPIX(24);break;
        case SB_PAGEUP:position-=(int)si.nPage;break;
        case SB_PAGEDOWN:position+=(int)si.nPage;break;
        case SB_TOP:position=0;break;
        case SB_BOTTOM:position=vertical?v->height:v->width;break;
        case SB_THUMBTRACK:case SB_THUMBPOSITION:position=si.nTrackPos;break;
        default:return 0;
        }
        dialog_view_scroll(hwnd,v,vertical?v->x:position,vertical?position:v->y);
        return 0;
    }
    return DefSubclassProc(hwnd,msg,wp,lp);
}

static LRESULT CALLBACK dialog_view_focus(HWND child,UINT msg,WPARAM wp,LPARAM lp,
                                         UINT_PTR id,DWORD_PTR ref)
{
    if(msg==WM_NCDESTROY)RemoveWindowSubclass(child,dialog_view_focus,id);
    LRESULT result=DefSubclassProc(child,msg,wp,lp);
    if(msg==WM_SETFOCUS && IsWindow((HWND)ref))
        SendMessageW((HWND)ref,QP_DLG_VIEW_FOCUS,0,(LPARAM)child);
    return result;
}

static BOOL CALLBACK dialog_view_hook(HWND child,LPARAM data)
{
    QpDialogViewHook *hook=(QpDialogViewHook *)data;
    if(!SetWindowSubclass(child,dialog_view_focus,QP_DLG_VIEW_ID,(DWORD_PTR)hook->root)) {
        hook->okay=0;return FALSE;
    }
    return TRUE;
}

static int dialog_view_fit(HWND hwnd,HWND owner,int min_width,int min_height,int resizable)
{
    RECT work,outer,content;
    qp_window_work_area(owner,&work);GetWindowRect(hwnd,&outer);
    int width=outer.right-outer.left,height=outer.bottom-outer.top;
    if(width<=work.right-work.left && height<=work.bottom-work.top)return 1;
    GetClientRect(hwnd,&content);
    QpDialogViewport *v=calloc(1,sizeof(*v));
    if(!v)return 0;
    v->width=content.right;v->height=content.bottom;
    v->min_width=min_width;v->min_height=min_height;v->resizable=resizable;
    if(!SetWindowSubclass(hwnd,dialog_view_proc,QP_DLG_VIEW_ID,(DWORD_PTR)v)) {free(v);return 0;}
    QpDialogViewHook hook={hwnd,1};
    /* EnumChildWindows' return value is not used; track callback failures. */
    EnumChildWindows(hwnd,dialog_view_hook,(LPARAM)&hook);
    if(!hook.okay)return 0;
    SetWindowLongPtrW(hwnd,GWL_STYLE,GetWindowLongPtrW(hwnd,GWL_STYLE)|WS_HSCROLL|WS_VSCROLL);
    if(width>work.right-work.left)width=work.right-work.left;
    if(height>work.bottom-work.top)height=work.bottom-work.top;
    if(!SetWindowPos(hwnd,NULL,0,0,width,height,
                     SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED))return 0;
    dialog_view_scroll(hwnd,v,0,0);
    HWND focus=GetFocus();
    if(focus && IsChild(hwnd,focus))SendMessageW(hwnd,QP_DLG_VIEW_FOCUS,0,(LPARAM)focus);
    return 1;
}

int qp_dialog_fit_fixed(HWND hwnd,HWND owner)
{
    return dialog_view_fit(hwnd,owner,0,0,0);
}

int qp_dialog_fit_resizable(HWND hwnd,HWND owner,int min_width,int min_height)
{
    return dialog_view_fit(hwnd,owner,min_width,min_height,1);
}
