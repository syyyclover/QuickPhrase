/* Real Win32 message/GDI tests; runnable on Windows or Wine, not an OS mock. */
#include "ui_internal.h"
#include "dialogs_internal.h"
#include "textview.h"
#include <assert.h>

static int checks, failures;
static const wchar_t *modal_class;
static int modal_ticks;
static HFONT previous_ui, previous_bold, previous_mono;
static wchar_t artifact_dir[32768];
static int captures, skipped;
static int editor_time_row=-1;
static int editor_choice_row=-1,input_custom_mode;
static const wchar_t *input_test_text=L"Other";
static void check(int yes, const char *name)
{
    ++checks;
    if (!yes) { ++failures; printf("FAIL: %s\n", name); }
}

static void pump(void)
{
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) continue;
        TranslateMessage(&m); DispatchMessageW(&m);
    }
}

typedef struct { BYTE *pixels; int width,height; } ScreenFrame;

static void settle(HWND h)
{
    SetFocus(h); /* Avoid edit-caret blink in image comparison. */
    POINT origin={0,0}; ClientToScreen(h,&origin);
    SetCursorPos(origin.x-20,origin.y-20);
    /* Keep this short: capture_screen() already waits for the desktop to stop
       changing, so a long fixed delay here only adds dead time. Measured runs with
       180 ms were no worse than 400/600 ms. */
    DWORD until=GetTickCount()+180;
    do { pump(); Sleep(10); } while((LONG)(GetTickCount()-until)<0);
    GdiFlush();
}

static const char *capture_blocked_reason;
static int skips_oversized, skips_unsettled, skips_other;
static size_t frame_difference(const ScreenFrame *a,const ScreenFrame *b);

static ScreenFrame blit_client(const RECT *rc,POINT pt)
{
    ScreenFrame frame={0}; BITMAPINFO info={0};
    HDC screen=NULL,mem=NULL; HBITMAP bitmap=NULL; HGDIOBJ old=NULL; BYTE *pixels=NULL;
    info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=rc->right;
    info.bmiHeader.biHeight=-rc->bottom;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    screen=GetDC(NULL); if(!screen) goto done;
    mem=CreateCompatibleDC(screen); if(!mem) goto done;
    bitmap=CreateDIBSection(mem,&info,DIB_RGB_COLORS,(void **)&pixels,NULL,0);if(!bitmap)goto done;
    old=SelectObject(mem,bitmap);
    if(BitBlt(mem,0,0,rc->right,rc->bottom,screen,pt.x,pt.y,SRCCOPY|CAPTUREBLT) && GdiFlush()) {
        frame.pixels=malloc((size_t)rc->right*rc->bottom*4);
        if(frame.pixels) {
            frame.width=rc->right;frame.height=rc->bottom;
            memcpy(frame.pixels,pixels,(size_t)rc->right*rc->bottom*4);
            for(int i=0;i<rc->right*rc->bottom;++i)frame.pixels[i*4+3]=255;
        }
    }
done:
    if(old)SelectObject(mem,old);
    if(bitmap)DeleteObject(bitmap);
    if(mem)DeleteDC(mem);
    if(screen)ReleaseDC(NULL,screen);
    return frame;
}

/* WindowFromPoint names the window that owns a screen point right now. When that is
   not our own window the blit would compare somebody else's pixels. */
static int point_shows_window(HWND h,POINT p)
{
    HWND top=WindowFromPoint(p);
    if(!top)return 0;
    return GetAncestor(top,GA_ROOT)==GetAncestor(h,GA_ROOT);
}

static int region_shows_window(HWND h,const RECT *rc,POINT origin)
{
    /* A sparse corner sample misses small overlays - a browser tooltip strip across
       the top of the client area slipped through a 5-point check. Walk a grid. */
    enum { N=7 };
    for(int iy=0;iy<N;++iy) {
        for(int ix=0;ix<N;++ix) {
            POINT p;
            p.x=origin.x+1+(rc->right-3)*ix/(N-1);
            p.y=origin.y+1+(rc->bottom-3)*iy/(N-1);
            if(!point_shows_window(h,p))return 0;
        }
    }
    return 1;
}

/* A screen blit is only comparable once the composited desktop really shows our
   window over the whole client region. Directly after SW_MAXIMIZE/SW_RESTORE the
   window already reports itself visible and foreground while the compositor still
   displays the previous frame - in the worst case another application's window.
   Comparing that frame produced "painting" failures that had nothing to do with our
   drawing. So wait until the z-order is ours AND several consecutive blits agree,
   and otherwise report the capture as unavailable instead of as a paint mismatch. */
static ScreenFrame capture_screen(HWND h)
{
    RECT rc; POINT pt={0,0};
    ScreenFrame prev={0};
    capture_blocked_reason=NULL;
    GetClientRect(h,&rc); ClientToScreen(h,&pt);
    int vx=GetSystemMetrics(SM_XVIRTUALSCREEN),vy=GetSystemMetrics(SM_YVIRTUALSCREEN);
    if(!IsWindowVisible(h) || IsIconic(h) || GetAncestor(h,GA_ROOT)!=GetForegroundWindow()) {
        capture_blocked_reason="window is not the visible foreground window";
        ++skips_other;
        return prev;
    }
    if(rc.right<=0 || rc.bottom<=0 || (size_t)rc.right*rc.bottom>20000000) {
        capture_blocked_reason="client area is empty or larger than the capture limit";
        ++skips_other;
        return prev;
    }
    if(pt.x<vx || pt.y<vy || pt.x+rc.right>vx+GetSystemMetrics(SM_CXVIRTUALSCREEN) ||
       pt.y+rc.bottom>vy+GetSystemMetrics(SM_CYVIRTUALSCREEN)) {
        /* Environment limitation (display too small / too much scaling for the
           requested window size), not a property of our painting. */
        capture_blocked_reason="client area does not fit the actual desktop";
        ++skips_oversized;
        return prev;
    }
    int stable=0;
    for(int attempt=0;attempt<28;++attempt) {
        if(!region_shows_window(h,&rc,pt)) {
            free(prev.pixels); prev=(ScreenFrame){0}; stable=0;
            capture_blocked_reason="another window still covers the client area";
            pump(); Sleep(25); continue;
        }
        ScreenFrame cur=blit_client(&rc,pt);
        if(!cur.pixels) {
            free(prev.pixels); capture_blocked_reason="screen blit failed"; ++skips_other; return cur;
        }
        stable=(prev.pixels && frame_difference(&prev,&cur)==0)?stable+1:0;
        free(prev.pixels); prev=cur;   /* prev now owns the buffer */
        if(stable>=3) { capture_blocked_reason=NULL; return prev; }
        capture_blocked_reason="desktop has not settled on the test window";
        pump(); Sleep(50);
    }
    free(prev.pixels);
    ++skips_unsettled;
    return (ScreenFrame){0};
}

static size_t frame_difference(const ScreenFrame *a,const ScreenFrame *b)
{
    if(!a->pixels || !b->pixels || a->width!=b->width || a->height!=b->height) return (size_t)-1;
    size_t count=0;
    for(int i=0;i<a->width*a->height;++i)
        if(memcmp(a->pixels+i*4,b->pixels+i*4,3)) ++count;
    return count;
}

/* One-off environment dump: a real capture acceptance needs a display that can hold
   the requested window sizes at the requested scaling. */
static void report_environment(void)
{
    RECT wr,cr;
    HDC screen=GetDC(NULL);
    int dpi=screen?GetDeviceCaps(screen,LOGPIXELSX):0;
    if(screen)ReleaseDC(NULL,screen);
    GetWindowRect(g_hwndMain,&wr); GetClientRect(g_hwndMain,&cr);
    printf("env: system dpi=%d primary=%dx%d virtual=%d,%d %dx%d main window=%ld,%ld,%ld,%ld client=%ld,%ld\n",
           dpi,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN),
           GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),
           GetSystemMetrics(SM_CXVIRTUALSCREEN),GetSystemMetrics(SM_CYVIRTUALSCREEN),
           wr.left,wr.top,wr.right,wr.bottom,cr.right,cr.bottom);
}

static size_t natural_paint_difference(HWND h,const char *case_name,int report);

/* Test the supplied-DC path before waiting for ordinary WM_PAINT. The old
   final-frame comparison could miss an empty animation/print surface. */
static void check_print_client(HWND h,COLORREF expected)
{
    RECT rc;
    GetClientRect(h,&rc);
    if(rc.right<=8 || rc.bottom<=8)return;
    HDC screen=GetDC(h),dc=screen?CreateCompatibleDC(screen):NULL;
    HBITMAP bitmap=dc?CreateCompatibleBitmap(screen,rc.right,rc.bottom):NULL;
    if(!bitmap) {
        ++skipped;
        if(dc)DeleteDC(dc);
        if(screen)ReleaseDC(h,screen);
        return;
    }
    HGDIOBJ old=SelectObject(dc,bitmap);
    HBRUSH marker=CreateSolidBrush(RGB(255,0,255));
    FillRect(dc,&rc,marker);DeleteObject(marker);
    InvalidateRect(h,NULL,FALSE);
    RECT pending;
    check(GetUpdateRect(h,&pending,FALSE),"print probe starts with pending paint");
    SendMessageW(h,WM_PRINTCLIENT,(WPARAM)dc,PRF_CLIENT);
    GdiFlush();
    check(GetPixel(dc,0,0)==expected,"supplied-DC client has theme background immediately");
    check(GetUpdateRect(h,&pending,FALSE),"print leaves pending ordinary paint intact");
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(h,screen);
}

static void test_repeated_transitions(void)
{
    ShowWindow(g_hwndMain,SW_RESTORE);settle(g_hwndMain);
    DWORD before=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    for(int i=0;i<60;++i) {
        ShowWindow(g_hwndMain,SW_MAXIMIZE);
        ShowWindow(g_hwndMain,SW_MINIMIZE);
        ShowWindow(g_hwndMain,SW_RESTORE); /* direct restore, no tray helper */
        check_print_client(g_hwndMain,theme_bk());
        check_print_client(g_hHost,theme_bk());
        check_print_client(g_hPreview,theme_ctl_bk());
        if(g_hTabs && IsWindowVisible(g_hTabs))check_print_client(g_hTabs,theme_bk());
        pump();
        ShowWindow(g_hwndMain,SW_RESTORE);
        if(i%10==0)natural_paint_difference(g_hwndMain,"repeated transitions",1);
    }
    settle(g_hwndMain);
    DWORD after=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    printf("transition stress: GDI objects before=%lu after=%lu\n",(unsigned long)before,(unsigned long)after);
    check(after<=before+4,"repeated transitions do not accumulate GDI resources");
}

static void save_frame(const ScreenFrame *f,int id,const wchar_t *kind)
{
    wchar_t path[32768]; BITMAPFILEHEADER file={0}; BITMAPINFOHEADER info={0};
    if(!f->pixels || !artifact_dir[0])return;
    if(swprintf(path,32768,L"%ls\\%03d-%ls.bmp",artifact_dir,id,kind)<0)return;
    FILE *out=_wfopen(path,L"wb");if(!out)return;
    file.bfType=0x4D42;file.bfOffBits=sizeof(file)+sizeof(info);
    file.bfSize=file.bfOffBits+(DWORD)f->width*f->height*4;
    info.biSize=sizeof(info);info.biWidth=f->width;info.biHeight=-f->height;
    info.biPlanes=1;info.biBitCount=32;info.biSizeImage=(DWORD)f->width*f->height*4;
    if(fwrite(&file,1,sizeof(file),out)!=sizeof(file) || fwrite(&info,1,sizeof(info),out)!=sizeof(info) ||
       fwrite(f->pixels,1,info.biSizeImage,out)!=info.biSizeImage)check(0,"write capture artifact");
    if(fclose(out))check(0,"close capture artifact");
}

static size_t natural_paint_difference(HWND h,const char *case_name,int report)
{
    settle(h);
    ScreenFrame natural=capture_screen(h);
    /* Full invalidation is ONLY the reference step, after natural pixels exist. */
    RedrawWindow(h,NULL,NULL,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_FRAME|RDW_UPDATENOW);
    settle(h);ScreenFrame reference=capture_screen(h);
    size_t differences=frame_difference(&natural,&reference);
    if(differences==(size_t)-1) {
        ++skipped;
        printf("SKIP capture: %s (%s)\n",case_name,
               capture_blocked_reason?capture_blocked_reason:"desktop visibility/resources");
    } else if(report) {
        ++captures;
        printf("natural/reference: %s dpi=%d size=%dx%d different=%zu\n",case_name,g_dpi,natural.width,natural.height,differences);
        check(!differences,"natural paint equals same-geometry full-redraw reference");
        if(differences) {save_frame(&natural,captures,L"natural");save_frame(&reference,captures,L"reference");}
    }
    free(natural.pixels);free(reference.pixels);
    return differences;
}

static LRESULT CALLBACK stale_probe(HWND h,UINT m,WPARAM w,LPARAM l)
{
    if(m==WM_ERASEBKGND)return 1;
    if(m==WM_PAINT) {
        PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);RECT r;GetClientRect(h,&r);
        SetDCBrushColor(dc,GetWindowLongPtrW(h,GWLP_USERDATA)?RGB(240,40,40):RGB(40,40,240));
        FillRect(dc,&r,(HBRUSH)GetStockObject(DC_BRUSH));EndPaint(h,&ps);return 0;
    }
    return DefWindowProcW(h,m,w,l);
}

static void test_paint_detector(void)
{
    WNDCLASSW wc={0};wc.lpfnWndProc=stale_probe;wc.hInstance=g_hInst;wc.lpszClassName=L"QPStalePaintProbe";
    RegisterClassW(&wc);
    HWND h=CreateWindowW(wc.lpszClassName,L"",WS_CHILD|WS_VISIBLE,10,10,140,100,g_hwndMain,NULL,g_hInst,NULL);
    check(h!=NULL,"create intentionally stale drawing probe");
    if(!h)return;
    settle(h);
    RedrawWindow(h,NULL,NULL,RDW_INVALIDATE|RDW_UPDATENOW);
    settle(h);
    SetWindowLongPtrW(h,GWLP_USERDATA,1); /* Change render state without invalidation. */
    size_t diff=natural_paint_difference(h,"intentional stale probe",0);
    if(diff!=(size_t)-1)check(diff>0,"detector exposes retained old pixels before reference repaint");
    DestroyWindow(h);pump();
}

static void check_children(HWND parent)
{
    HWND child;
    RECT client;
    GetClientRect(parent, &client);
    for (child = GetWindow(parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        RECT r;
        if (!(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE)) continue;
        GetWindowRect(child, &r);
        MapWindowPoints(NULL, parent, (POINT *)&r, 2);
        if (r.left < 0 || r.top < 0 || r.right > client.right + 1 || r.bottom > client.bottom + 1) {
            wchar_t cls[80]; GetClassNameW(child, cls, 80);
            printf("clipped id=%d rect=%ld,%ld,%ld,%ld client=%ld,%ld\n",
                   GetDlgCtrlID(child),r.left,r.top,r.right,r.bottom,client.right,client.bottom);
            check(0, "visible direct child fits client area");
        } else check(1, "visible direct child fits client area");
    }
}

static void check_wrapped_text(HWND h)
{
    wchar_t text[4096],cls[32]; RECT rc,needed; HDC dc;
    if(!h || !IsWindowVisible(h)) return;
    GetClassNameW(h,cls,32);
    int checkbox=!wcscmp(cls,L"Button") && (GetWindowLongPtrW(h,GWL_STYLE)&BS_MULTILINE);
    if(wcscmp(cls,L"Static") && !checkbox) return;
    GetClientRect(h,&rc); needed=rc; needed.bottom=0;
    if(checkbox) needed.right-=DPIX(22); /* Same glyph/padding allowance as editor layout. */
    GetWindowTextW(h,text,4096);
    if(!checkbox) check(!(GetWindowLongPtrW(h,GWL_STYLE)&SS_ENDELLIPSIS),"instruction label must not ellipsize");
    dc=GetDC(h);
    HGDIOBJ old=SelectObject(dc,(HFONT)SendMessageW(h,WM_GETFONT,0,0));
    DrawTextW(dc,text,-1,&needed,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
    check(needed.bottom<=rc.bottom,"all instruction lines fit actual font/control height");
    SelectObject(dc,old); ReleaseDC(h,dc);
}

static BOOL CALLBACK check_multiline_button(HWND h,LPARAM unused)
{
    wchar_t cls[32]; (void)unused;
    GetClassNameW(h,cls,32);
    if(!wcscmp(cls,L"Button") && (GetWindowLongPtrW(h,GWL_STYLE)&BS_MULTILINE)) check_wrapped_text(h);
    return TRUE;
}

static BOOL CALLBACK apply_scaled_font(HWND h,LPARAM unused)
{
    HFONT old=(HFONT)SendMessageW(h,WM_GETFONT,0,0); (void)unused;
    HFONT font=old==previous_bold?g_fontBold:old==previous_mono?g_fontMono:g_fontUI;
    SendMessageW(h,WM_SETFONT,(WPARAM)font,TRUE); return TRUE;
}

static void use_scaled_fonts(void)
{
    LOGFONTW lf={0}; NONCLIENTMETRICSW ncm={0};
    previous_ui=g_fontUI; previous_bold=g_fontBold; previous_mono=g_fontMono;
    ncm.cbSize=sizeof(ncm);
    if(SystemParametersInfoW(SPI_GETNONCLIENTMETRICS,sizeof(ncm),&ncm,0)) lf=ncm.lfMessageFont;
    else wcscpy(lf.lfFaceName,L"Microsoft YaHei UI");
    lf.lfHeight=-MulDiv(9,g_dpi,72); lf.lfWeight=FW_NORMAL;
    g_fontUI=CreateFontIndirectW(&lf);
    lf.lfHeight=-MulDiv(10,g_dpi,72); lf.lfWeight=FW_BOLD;
    g_fontBold=CreateFontIndirectW(&lf);
    lf.lfWeight=FW_NORMAL; wcscpy(lf.lfFaceName,L"Consolas");
    g_fontMono=CreateFontIndirectW(&lf);
    check(g_fontUI && g_fontBold && g_fontMono,"create actual DPI-scaled test fonts");
    if(g_hwndMain) EnumChildWindows(g_hwndMain,apply_scaled_font,0);
    if(previous_ui) DeleteObject(previous_ui);
    if(previous_bold) DeleteObject(previous_bold);
    if(previous_mono) DeleteObject(previous_mono);
}

static void check_erase_keeps_text(HWND h)
{
    RECT rc; HDC dc; COLORREF *before; int count;
    if(!h || !IsWindowVisible(h)) return;
    RedrawWindow(h,NULL,NULL,RDW_INVALIDATE|RDW_UPDATENOW);
    GetClientRect(h,&rc); count=rc.right*rc.bottom;
    if(count<=0 || count>500000) return;
    before=malloc((size_t)count*sizeof(*before)); if(!before) return;
    dc=GetDC(h);
    for(int y=0;y<rc.bottom;++y) for(int x=0;x<rc.right;++x) before[y*rc.right+x]=GetPixel(dc,x,y);
    SendMessageW(h,WM_ERASEBKGND,(WPARAM)dc,0);
    int same=1;
    for(int y=0;y<rc.bottom;++y) for(int x=0;x<rc.right;++x)
        if(before[y*rc.right+x]!=GetPixel(dc,x,y)) same=0;
    check(same,"label erase does not wipe visible glyph pixels");
    ReleaseDC(h,dc); free(before);
}

static void check_time_edit_keys(HWND editor)
{
    HWND list=GetDlgItem(editor,IDC_ED_SEGLIST);
    if(editor_time_row<0 || !list)return;
    ListView_SetItemState(list,editor_time_row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
    HWND props=FindWindowExW(editor,NULL,L"QuickPhraseEditorProps",NULL);
    if(!IsWindowVisible(props))SendMessageW(editor,WM_COMMAND,
        MAKEWPARAM(IDC_ED_PAGE_PROPS,BN_CLICKED),(LPARAM)GetDlgItem(editor,IDC_ED_PAGE_PROPS));
    HWND combo=GetDlgItem(props,IDC_ED_SEG_FORMAT);
    COMBOBOXINFO info={0};info.cbSize=sizeof(info);
    check(combo && GetComboBoxInfo(combo,&info),"find native editable time-format control");
    if(!info.hwndItem)return;
    SetFocus(info.hwndItem);
    SendMessageW(info.hwndItem,EM_SETSEL,8,8);
    SendMessageW(info.hwndItem,WM_KEYDOWN,VK_DELETE,0);
    DWORD start=0,end=0;
    SendMessageW(info.hwndItem,EM_GETSEL,(WPARAM)&start,(LPARAM)&end);
    check(start==8 && end==8,"native Delete preserves time-format caret");
    wchar_t text[QP_FORMAT_MAX];GetWindowTextW(combo,text,QP_FORMAT_MAX);
    check(!wcscmp(text,L"yyyyMMddHmmss"),"native Delete removes only one format character");
    SendMessageW(info.hwndItem,WM_CHAR,VK_BACK,0);
    SendMessageW(info.hwndItem,EM_GETSEL,(WPARAM)&start,(LPARAM)&end);
    check(start==7 && end==7,"native Backspace preserves time-format caret");
    SendMessageW(info.hwndItem,EM_SETSEL,0,-1);
    SendMessageW(info.hwndItem,WM_KEYDOWN,VK_DELETE,0);
    check(GetWindowTextLengthW(combo)==0,"time format can be cleared before entering a replacement");
    SendMessageW(info.hwndItem,WM_CHAR,L'y',0);
    SendMessageW(info.hwndItem,EM_GETSEL,(WPARAM)&start,(LPARAM)&end);
    check(start==1 && end==1,"typing after clear keeps one caret instead of selecting all");
}

static void CALLBACK modal_probe(HWND unused, UINT msg, UINT_PTR id, DWORD time)
{
    HWND h = FindWindowW(modal_class, NULL);
    (void)unused; (void)msg; (void)time;
    if (!h) {
        if (++modal_ticks > 30) { check(0, "modal window created"); KillTimer(NULL,id); PostQuitMessage(1); }
        return;
    }
    KillTimer(NULL,id);
    check_children(h);
    if(!wcscmp(modal_class,L"QuickPhraseInput")) {
        HWND custom=GetDlgItem(h,IDC_INPUT_CUSTOM);
        if(input_custom_mode) {
            check(custom && IsWindowVisible(custom) && IsWindowEnabled(custom),"author custom-option checkbox is visible and enabled");
            CheckDlgButton(h,IDC_INPUT_CUSTOM,BST_CHECKED);
        } else check(custom==NULL,"filler input has no author-only checkbox");
        SetWindowTextW(GetDlgItem(h,IDC_INPUT_EDIT),input_test_text);
        SendMessageW(h,WM_COMMAND,MAKEWPARAM(IDOK,BN_CLICKED),0);
        return;
    }
    if(!wcscmp(modal_class,L"QuickPhraseLookup")) {
        HWND mode=GetDlgItem(h,IDC_LK_KEYMODE);
        SendMessageW(mode,CB_SETCURSEL,2,0);
        SendMessageW(h,WM_COMMAND,MAKEWPARAM(IDC_LK_KEYMODE,CBN_SELCHANGE),(LPARAM)mode);
    }
    if(!wcscmp(modal_class,L"QuickPhraseParseRule")) {
        HWND type=GetDlgItem(h,IDC_RL_TYPE);
        for(int kind=0;kind<5;++kind) {
            SendMessageW(type,CB_SETCURSEL,kind,0);
            SendMessageW(h,WM_COMMAND,MAKEWPARAM(IDC_RL_TYPE,CBN_SELCHANGE),(LPARAM)type);
            natural_paint_difference(h,"parse type switch",1);
            check_children(h);
            check_wrapped_text(GetDlgItem(h,IDC_RL_TIP));
            check_wrapped_text(GetDlgItem(h,IDC_RL_PARA_HELP));
            if(kind==4) {
                LONG_PTR style=GetWindowLongPtrW(GetDlgItem(h,IDC_RL_LOOKUP_DESC),GWL_STYLE);
                check((style&ES_MULTILINE) && (style&ES_READONLY) && (style&WS_VSCROLL) && !(style&ES_AUTOHSCROLL),
                      "lookup summary is wrapped/selectable/scrollable");
                RECT summary,para;
                GetWindowRect(GetDlgItem(h,IDC_RL_LOOKUP_DESC),&summary);
                GetWindowRect(GetDlgItem(h,IDC_RL_PARA),&para);
                check(para.top-summary.bottom<=DPIX(12),"lookup controls have no hidden-row gap");
            }
        }
    }
    if (GetWindowLongPtrW(h, GWL_STYLE) & WS_THICKFRAME) {
        RECT r;
        GetWindowRect(h,&r);
        SendMessageW(h, WM_ENTERSIZEMOVE, 0, 0);
        SetWindowPos(h,NULL,0,0,r.right-r.left+DPIX(160),r.bottom-r.top+DPIX(100),SWP_NOMOVE|SWP_NOZORDER);
        natural_paint_difference(h,"modal grow",1);
        check_children(h);
        if(!wcscmp(modal_class,L"QuickPhraseEditor")) {
            check_time_edit_keys(h);
            if(editor_choice_row>=0) {
                ListView_SetItemState(GetDlgItem(h,IDC_ED_SEGLIST),editor_choice_row,
                    LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
                const wchar_t *saved_class=modal_class;
                input_custom_mode=1;input_test_text=L"Other";
                modal_class=L"QuickPhraseInput";modal_ticks=0;
                UINT_PTR nested_timer=SetTimer(NULL,0,30,modal_probe);
                SendMessageW(h,WM_COMMAND,MAKEWPARAM(IDC_ED_OPT_ADD,BN_CLICKED),0);
                if(nested_timer)KillTimer(NULL,nested_timer);
                modal_class=saved_class;input_custom_mode=0;
            }
            SetWindowPos(h,NULL,0,0,r.right-r.left+DPIX(100),r.bottom-r.top,SWP_NOMOVE|SWP_NOZORDER);
            natural_paint_difference(h,"editor width only",1);
            SetWindowPos(h,NULL,0,0,r.right-r.left,r.bottom-r.top+DPIX(100),SWP_NOMOVE|SWP_NOZORDER);
            natural_paint_difference(h,"editor height only",1);
            for(int i=0;i<30;++i) {
                SetWindowPos(h,NULL,0,0,r.right-r.left+DPIX((i%2)?180:20),
                             r.bottom-r.top+DPIX((i%2)?120:20),SWP_NOMOVE|SWP_NOZORDER);
                if(i%5==0)natural_paint_difference(h,"editor alternating resize",1);
                else pump();
            }
            check_erase_keeps_text(GetDlgItem(h,IDC_ED_NAME+1000));
            EnumChildWindows(h,check_multiline_button,0);
            HWND preview=GetDlgItem(h,IDC_ED_PREVIEW);
            RECT area;GetWindowRect(preview,&area);MapWindowPoints(NULL,h,(POINT *)&area,2);
            int divider=area.top-DPIX(18)-DPIX(4);
            SendMessageW(h,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(DPIX(20),divider));
            SendMessageW(h,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(DPIX(20),divider-DPIX(20)));
            SendMessageW(h,WM_LBUTTONUP,0,MAKELPARAM(DPIX(20),divider-DPIX(20)));
            natural_paint_difference(h,"editor preview divider",1);
            HWND props=FindWindowExW(h,NULL,L"QuickPhraseEditorProps",NULL);
            SendMessageW(props,WM_VSCROLL,SB_BOTTOM,0);
            natural_paint_difference(h,"editor properties bottom",1);
            SendMessageW(props,WM_VSCROLL,SB_TOP,0);
            natural_paint_difference(h,"editor properties top",1);
        }
        SendMessageW(h,WM_EXITSIZEMOVE,0,0);
    }
    PostMessageW(h,WM_CLOSE,0,0);
}

static void prepare_modal(const wchar_t *cls)
{
    modal_class=cls; modal_ticks=0;
    SetTimer(NULL,0,30,modal_probe);
}

static void test_preview(void)
{
    HWND p=qp_preview_create(g_hwndMain,999,0);
    const wchar_t *raw=L"a\t\t\t\tb\r\n中文\xD83D\xDE00";
    int marks[]={1};
    wchar_t copied[128]={0};
    SetWindowPos(p,NULL,10,10,DPIX(180),DPIX(100),SWP_NOZORDER);
    qp_preview_set_text(p,raw,marks,1);
    SendMessageW(p,WM_KEYDOWN,'A',0); /* Ctrl+A requires actual modifier; use mouse + key below. */
    {
    BYTE state[256], saved[256];
    GetKeyboardState(saved); memcpy(state,saved,sizeof(state)); state[VK_CONTROL]=0x80;
    SetKeyboardState(state);
    SendMessageW(p,WM_KEYDOWN,'A',0);
    SendMessageW(p,WM_KEYDOWN,'C',0);
    SetKeyboardState(saved);
    }
    if (OpenClipboard(p)) {
        HANDLE data=GetClipboardData(CF_UNICODETEXT);
        const wchar_t *s=data ? GlobalLock(data) : NULL;
        if(s) { qp_strlcpy(copied,s,128); GlobalUnlock(data); }
        CloseClipboard();
    }
    check(wcscmp(copied,raw)==0,"preview copy preserves raw tabs/CRLF and excludes error hints");
    qp_preview_set_mode(p,QP_PREVIEW_TABLE);
    qp_preview_set_table(p,L"甲\t乙\r\n丙\t丁",L"{一}\\t{二}");
    RedrawWindow(p,NULL,NULL,RDW_INVALIDATE|RDW_UPDATENOW);
    SendMessageW(p,WM_VSCROLL,SB_BOTTOM,0);
    SendMessageW(p,WM_HSCROLL,SB_RIGHT,0);
    DestroyWindow(p);
}

int wmain(int argc,wchar_t **argv)
{
    int scales[]={96,120,144,192}, i;
    setvbuf(stdout,NULL,_IONBF,0); /* A crash must not swallow the progress log. */
    INITCOMMONCONTROLSEX ic={sizeof(ic),ICC_WIN95_CLASSES|ICC_DATE_CLASSES};
    Template *t;
    ParseRule *pr;
    ConditionRule *cr;
    HWND input;
    wchar_t profile_path[600];
    wchar_t base[32768];
    if(argc==3 && !wcscmp(argv[1],L"--artifacts")) {
        qp_strlcpy(base,argv[2],32768);
        if(!CreateDirectoryW(base,NULL) && GetLastError()!=ERROR_ALREADY_EXISTS)return 2;
    } else if(!GetTempPathW(32768,base))return 2;
    if(swprintf(artifact_dir,32768,L"%ls\\QuickPhrase-check-%lu-%llu",base,GetCurrentProcessId(),
                (unsigned long long)GetTickCount64())<0 || !CreateDirectoryW(artifact_dir,NULL))return 2;
    /* Modal geometry checks persist settings: never run over a real user profile. */
    if (FindWindowW(L"QuickPhraseMain",NULL) ||
        (storage_get_settings_path(profile_path,600) && GetFileAttributesW(profile_path)!=INVALID_FILE_ATTRIBUTES) ||
        (storage_get_data_path(profile_path,600) && GetFileAttributesW(profile_path)!=INVALID_FILE_ATTRIBUTES)) {
        puts("Refusing to run over an existing QuickPhrase profile/window; use a fresh test account or isolated Wine profile.");
        return 2;
    }
    InitCommonControlsEx(&ic);
    g_hInst=GetModuleHandleW(NULL);
    settings_defaults(&g_settings);
    g_app=appdata_create_seed();
    qp_table_set_refs(g_app->tables);
    g_dpi=96; use_scaled_fonts();
    g_brushBk=CreateSolidBrush(theme_bk());
    g_brushCtl=CreateSolidBrush(theme_ctl_bk());
    check(ui_register_classes(),"register real window classes");
    g_hwndMain=ui_create_main_window();
    check(g_hwndMain!=NULL,"create main window");
    ShowWindow(g_hwndMain,SW_SHOW);
    SetForegroundWindow(g_hwndMain);test_paint_detector();
    ui_rebuild_tree(g_app->templates->id);
    report_environment();
    test_repeated_transitions();
    input=qp_create(L"Edit",L"",ES_AUTOHSCROLL,0,0,0,10,10,g_hwndMain,998);
    check(SendMessageW(input,EM_GETLIMITTEXT,0,0)==QP_VALUE_MAX-1,"input limit matches model capacity");
    DestroyWindow(input);
    for(i=0;i<4;++i) {
        g_dpi=scales[i];
        use_scaled_fonts();
        SetWindowPos(g_hwndMain,NULL,0,0,DPIX(1100),DPIX(800),SWP_NOMOVE|SWP_NOZORDER);
        natural_paint_difference(g_hwndMain,"main large",1);check_children(g_hwndMain);
        SetWindowPos(g_hwndMain,NULL,0,0,DPIX(720),DPIX(520),SWP_NOMOVE|SWP_NOZORDER);
        natural_paint_difference(g_hwndMain,"main small",1);check_children(g_hwndMain);
        RECT before_minimize, after_minimize;
        GetWindowRect(g_hHost,&before_minimize);
        ShowWindow(g_hwndMain,SW_MINIMIZE);settle(g_hwndMain);
        GetWindowRect(g_hHost,&after_minimize);
        check(before_minimize.right-before_minimize.left==after_minimize.right-after_minimize.left &&
              before_minimize.bottom-before_minimize.top==after_minimize.bottom-after_minimize.top,
              "minimize preserves field viewport dimensions");
        ShowWindow(g_hwndMain,SW_RESTORE);
        natural_paint_difference(g_hwndMain,"main restore without tray helper",1);
        ShowWindow(g_hwndMain,SW_MAXIMIZE);
        natural_paint_difference(g_hwndMain,"main maximize",1);
        ShowWindow(g_hwndMain,SW_MINIMIZE);settle(g_hwndMain);
        ShowWindow(g_hwndMain,SW_RESTORE);
        natural_paint_difference(g_hwndMain,"main restore maximized",1);
        ShowWindow(g_hwndMain,SW_RESTORE);
        natural_paint_difference(g_hwndMain,"main restore normal",1);
        test_preview();
        t=tmpl_clone(g_app->templates);
        Segment *time_edit=seg_create(SEG_DATETIME);
        check(time_edit!=NULL,"allocate isolated time-edit regression fixture");
        if(time_edit) {
            qp_strlcpy(time_edit->label,L"测试日期",QP_LABEL_MAX);
            qp_strlcpy(time_edit->time_format,L"yyyyMMddHHmmss",QP_FORMAT_MAX);
            editor_time_row=seg_list_count(t->segments);
            seg_list_append(&t->segments,time_edit);
        }
        Segment *choice_edit=seg_create(SEG_MULTI_SELECT);
        check(choice_edit!=NULL,"allocate isolated custom-option author fixture");
        if(choice_edit) {
            qp_strlcpy(choice_edit->label,L"补充选项测试",QP_LABEL_MAX);
            seg_add_option(choice_edit,L"A");editor_choice_row=seg_list_count(t->segments);
            seg_list_append(&t->segments,choice_edit);
        }
        prepare_modal(L"QuickPhraseEditor"); dlg_edit_template(g_hwndMain,t,0);
        if(choice_edit)check(choice_edit->custom_option==2 && choice_edit->option_count==2,
            "native add-option command stores custom marker with new option");
        prepare_modal(L"QuickPhraseTables"); dlg_data_tables(g_hwndMain,g_app);
        pr=parse_rule_create();
        int original_mode=pr->lk_key_mode;
        prepare_modal(L"QuickPhraseLookup");
        check(!dlg_lookup_rule(g_hwndMain,pr,t),"lookup close is cancellation");
        check(pr->lk_key_mode==original_mode,"lookup cancel discards key-source change");
        prepare_modal(L"QuickPhraseParseRule"); dlg_parse_rule(g_hwndMain,pr,t,t->segments);
        cr=cond_rule_create();
        prepare_modal(L"QuickPhraseCondRule"); dlg_cond_rule(g_hwndMain,cr,t,t->segments);
        prepare_modal(L"QuickPhraseSettings"); dlg_settings(g_hwndMain);
        parse_rule_free(pr); cond_rule_free(cr); tmpl_free(t);
        g_settings.theme=g_dark?THEME_LIGHT:THEME_DARK; app_apply_theme(); pump();
    }
    ui_reset_selection();
    DestroyWindow(g_hwndMain); g_hwndMain=NULL;
    DeleteObject(g_fontUI); DeleteObject(g_fontBold); DeleteObject(g_fontMono);
    appdata_free(g_app); g_app=NULL;
    printf("Win32 checks: %d, failures: %d, captures: %d, skipped: %d"
           " (desktop-too-small=%d unsettled=%d other=%d)\n",
           checks,failures,captures,skipped,skips_oversized,skips_unsettled,skips_other);
    if(skipped)puts("Visual acceptance is incomplete: one or more actual-screen captures were unavailable.");
    return failures?1:skipped?3:0;
}
