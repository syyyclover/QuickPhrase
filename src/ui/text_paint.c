/* Measured text and local buffered painting; no whole-window composition. */
#include "ui.h"
#include <stdlib.h>
#include <string.h>

void qp_show_if_needed(HWND h,int visible)
{
    if(h && !!(GetWindowLongPtrW(h,GWL_STYLE)&WS_VISIBLE)!=!!visible)
        ShowWindow(h,visible?SW_SHOW:SW_HIDE);
}

void qp_set_column_width_if_needed(HWND h,int col,int width)
{
    if(width<0) width=0;
    if(h && ListView_GetColumnWidth(h,col)!=width) ListView_SetColumnWidth(h,col,width);
}

void qp_set_scroll_info_if_needed(HWND h,int bar,const SCROLLINFO *info)
{
    SCROLLINFO cur={0}, want=*info;
    int range=want.nMax-want.nMin+1, maxpos;
    if(range<1) range=1;
    if(want.nPage>(UINT)range) want.nPage=(UINT)range;
    maxpos=want.nMax-(want.nPage?(int)want.nPage-1:0);
    if(maxpos<want.nMin) maxpos=want.nMin;
    if(want.nPos>maxpos) want.nPos=maxpos;
    if(want.nPos<want.nMin) want.nPos=want.nMin;
    cur.cbSize=sizeof(cur); cur.fMask=SIF_RANGE|SIF_PAGE|SIF_POS;
    if(GetScrollInfo(h,bar,&cur) && cur.nMin==want.nMin && cur.nMax==want.nMax &&
       cur.nPage==want.nPage && cur.nPos==want.nPos) return;
    SetScrollInfo(h,bar,&want,TRUE);
}

int qp_line_height(HWND h)
{
    TEXTMETRICW tm={0}; HDC dc=GetDC(h);
    if(!dc) return DPIX(18);
    HFONT font=(HFONT)SendMessageW(h,WM_GETFONT,0,0);
    HGDIOBJ old=SelectObject(dc,font?font:g_fontUI);
    GetTextMetricsW(dc,&tm); SelectObject(dc,old); ReleaseDC(h,dc);
    return tm.tmHeight>0?tm.tmHeight:DPIX(18);
}

int qp_wrapped_text_height(HWND h,int width)
{
    int len=GetWindowTextLengthW(h), height=qp_line_height(h);
    wchar_t *text; HDC dc; RECT r={0,0,width>0?width:1,0};
    if(len<=0) return height;
    text=(wchar_t *)malloc(((size_t)len+1)*sizeof(wchar_t));
    if(!text) return height*(len+1); /* Conservative bound; never silently clip. */
    GetWindowTextW(h,text,len+1); dc=GetDC(h);
    if(dc) {
        HFONT font=(HFONT)SendMessageW(h,WM_GETFONT,0,0);
        HGDIOBJ old=SelectObject(dc,font?font:g_fontUI);
        DrawTextW(dc,text,len,&r,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
        height=r.bottom>height?r.bottom:height;
        SelectObject(dc,old); ReleaseDC(h,dc);
    }
    free(text); return height;
}

/* A print request supplies its own DC and must not consume the update region. */
void qp_draw_background(HWND h,HDC dc)
{
    RECT rc;
    if(!dc)return;
    GetClientRect(h,&rc);
    FillRect(dc,&rc,g_brushBk);
}

/* Shared background painter for custom hosts. Child HWNDs stay clipped. */
void qp_paint_background(HWND h)
{
    PAINTSTRUCT ps; RECT rc; HDC dc=BeginPaint(h,&ps), mem=NULL, target=dc;
    HBITMAP bitmap=NULL; HGDIOBJ old=NULL;
    if(!dc) return;
    GetClientRect(h,&rc);
    if(rc.right>0 && rc.bottom>0) {
        mem=CreateCompatibleDC(dc);
        if(mem) bitmap=CreateCompatibleBitmap(dc,rc.right,rc.bottom);
        if(bitmap) { old=SelectObject(mem,bitmap); target=mem; }
        qp_draw_background(h,target);
        if(bitmap) {
            BitBlt(dc,0,0,rc.right,rc.bottom,mem,0,0,SRCCOPY);
            SelectObject(mem,old); DeleteObject(bitmap);
        }
        if(mem) DeleteDC(mem);
    }
    EndPaint(h,&ps);
}

static LRESULT CALLBACK buffered_static(HWND h,UINT msg,WPARAM wp,LPARAM lp,
                                         UINT_PTR id,DWORD_PTR ref)
{
    (void)ref;
    if(msg==WM_ERASEBKGND) return 1;
    if(msg==WM_PAINT) {
        int len=GetWindowTextLengthW(h);
        wchar_t *text=len>0?malloc(((size_t)len+1)*sizeof(wchar_t)):NULL;
        /* Let the native Static paint from its own text store on application
           allocation failure; never validate an intentionally blank label. */
        if(len>0 && !text) return DefSubclassProc(h,msg,wp,lp);
        if(text) GetWindowTextW(h,text,len+1);
        PAINTSTRUCT ps; RECT rc; HDC dc=BeginPaint(h,&ps), mem=NULL, target=dc;
        HBITMAP bitmap=NULL; HGDIOBJ oldBitmap=NULL,oldFont;
        DWORD style=(DWORD)GetWindowLongPtrW(h,GWL_STYLE);
        UINT flags=style&SS_NOPREFIX?DT_NOPREFIX:0;
        if(!dc) { free(text); return 0; }
        GetClientRect(h,&rc);
        if(rc.right<=0 || rc.bottom<=0) { free(text); EndPaint(h,&ps); return 0; }
        mem=CreateCompatibleDC(dc);
        if(mem) bitmap=CreateCompatibleBitmap(dc,rc.right,rc.bottom);
        if(bitmap) { oldBitmap=SelectObject(mem,bitmap); target=mem; }
        HBRUSH brush=(HBRUSH)SendMessageW(GetParent(h),WM_CTLCOLORSTATIC,(WPARAM)target,(LPARAM)h);
        FillRect(target,&rc,brush?brush:g_brushBk);
        if(!IsWindowEnabled(h)) SetTextColor(target,theme_dim());
        HFONT font=(HFONT)SendMessageW(h,WM_GETFONT,0,0);
        oldFont=SelectObject(target,font?font:g_fontUI);
        SetBkMode(target,TRANSPARENT);
        if((style&SS_TYPEMASK)==SS_RIGHT) flags|=DT_RIGHT;
        else if((style&SS_TYPEMASK)==SS_CENTER) flags|=DT_CENTER;
        if(style&SS_ENDELLIPSIS) flags|=DT_SINGLELINE|DT_END_ELLIPSIS;
        else flags|=DT_WORDBREAK;
        if(style&SS_CENTERIMAGE) flags|=DT_SINGLELINE|DT_VCENTER;
        if(text) { DrawTextW(target,text,len,&rc,flags); free(text); }
        SelectObject(target,oldFont);
        if(bitmap) {
            BitBlt(dc,0,0,rc.right,rc.bottom,mem,0,0,SRCCOPY);
            SelectObject(mem,oldBitmap); DeleteObject(bitmap);
        }
        if(mem) DeleteDC(mem);
        EndPaint(h,&ps); return 0;
    }
    if(msg==WM_NCDESTROY) RemoveWindowSubclass(h,buffered_static,id);
    return DefSubclassProc(h,msg,wp,lp);
}

void qp_buffer_static(HWND h)
{
    DWORD style=(DWORD)GetWindowLongPtrW(h,GWL_STYLE);
    if((style&SS_TYPEMASK)<=SS_RIGHT) SetWindowSubclass(h,buffered_static,1,0);
}
