/* Native editor regression: synthetic model, real HWNDs and desktop pixels.
   Include the editor to instantiate its actual private procedure without adding
   a production test API. Its normal object is excluded from this test's link. */
#define COBJMACROS
#include "editor_internal.h"
#include <stdio.h>
#include <dwmapi.h>
#include "win32_desktop_capture.h"
#include "../src/ui/dialog_editor.c"

static CRITICAL_SECTION gate;
static HANDLE ready;
static volatile LONG stopping;
static RECT area;
static LARGE_INTEGER started;
static int sampling, capture_ok, capture_error, frames, bad, maximum, width, height;
static int checks, failures;
static BYTE *worst;
static char artifact[1024];
static int trace;
static LRESULT CALLBACK probe_proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    if(trace && (msg==WM_WINDOWPOSCHANGING || msg==WM_WINDOWPOSCHANGED || msg==WM_SIZE || msg==WM_ERASEBKGND)) {
        printf("TRACE msg=%x zoomed=%d flags=%x size=%lu\n",msg,!!IsZoomed(hwnd),
            (msg==WM_WINDOWPOSCHANGING || msg==WM_WINDOWPOSCHANGED)?((WINDOWPOS*)lp)->flags:0,(unsigned long)wp);
    }
    return editor_proc(hwnd,msg,wp,lp);
}
int __wrap_settings_save(const Settings *s) { (void)s; return 1; }
int __wrap_storage_save(const AppData *a) { (void)a; return 1; }
static void check(int ok,const char *name) {
    ++checks;if(!ok){++failures;printf("FAIL: %s\n",name);}
}
static void pump(int ms) {
    DWORD end=GetTickCount()+ms;MSG msg;
    do {
        while(PeekMessageW(&msg,NULL,0,0,PM_REMOVE))
            if(msg.message!=WM_QUIT){TranslateMessage(&msg);DispatchMessageW(&msg);}
        Sleep(1);
    } while((LONG)(GetTickCount()-end)<0);
}
static DWORD WINAPI capture(void *unused) {
    (void)unused;
    DXCapture dx={0};BYTE *pixels=malloc((size_t)width*height*4);
    capture_ok=pixels && dx_init(&dx,area);SetEvent(ready);
    if(!capture_ok){free(pixels);dx_free(&dx);return 1;}
    while(!InterlockedCompareExchange(&stopping,0,0)) {
        LARGE_INTEGER present;
        int got=dx_frame(&dx,area,pixels,&present);
        if(got<0){capture_error=1;break;}
        if(!got)continue;
        EnterCriticalSection(&gate);
        if(sampling && present.QuadPart>=started.QuadPart) {
            ++frames;int black=0;
            for(int i=0;i<width*height;i++)
                if(pixels[i*4]<20 && pixels[i*4+1]<20 && pixels[i*4+2]<20)++black;
            /* All underlying pixels are white; fixture ink is well below 2%.
               Count animation frames too, including partially exposed editors. */
            if(black>width*height/50)++bad;
            if(black>maximum){maximum=black;memcpy(worst,pixels,(size_t)width*height*4);}
        }
        LeaveCriticalSection(&gate);
    }
    free(pixels);dx_free(&dx);return 0;
}
static void sample(int on) {
    EnterCriticalSection(&gate);
    if(on)QueryPerformanceCounter(&started);
    sampling=on;LeaveCriticalSection(&gate);
}
static void check_editor(EditorCtx *c,int zoomed) {
    DWORD cloaked=0;
    check(IsWindowVisible(c->hwnd) && !!IsZoomed(c->hwnd)==zoomed,"editor remains visible in requested state");
    check(SUCCEEDED(DwmGetWindowAttribute(c->hwnd,DWMWA_CLOAKED,&cloaked,sizeof(cloaked))) && !(cloaked&DWM_CLOAKED_APP),"editor is not stranded behind a cloak");
    check(GetWindowTextLengthW(c->hName)>0,"template name survives resizing");
    check(!wcscmp(c->t->name,L"Editor resize synthetic fixture"),"resizing preserves model");
}
static void save_frame(void) {
    FILE *out=fopen(artifact,"wb");if(!out){check(0,"write desktop frame");return;}
    BITMAPFILEHEADER fh={0};BITMAPINFOHEADER ih={0};
    fh.bfType=0x4d42;fh.bfOffBits=sizeof(fh)+sizeof(ih);fh.bfSize=fh.bfOffBits+width*height*4;
    ih.biSize=sizeof(ih);ih.biWidth=width;ih.biHeight=-height;ih.biPlanes=1;ih.biBitCount=32;
    int ok=fwrite(&fh,sizeof(fh),1,out)==1 && fwrite(&ih,sizeof(ih),1,out)==1 && fwrite(worst,(size_t)width*height*4,1,out)==1;
    if(fclose(out))ok=0;
    check(ok,"saved worst compositor frame");
}
static void check_surface(HWND hwnd) {
    RECT rc,clip={13,11,241,109},before,after;
    GetClientRect(hwnd,&rc);
    BITMAPINFO info={0};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=rc.right;info.bmiHeader.biHeight=-rc.bottom;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    HDC screen=GetDC(NULL),dc=CreateCompatibleDC(screen);BYTE *pixels=NULL;
    HBITMAP bitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,(void**)&pixels,NULL,0);
    ReleaseDC(NULL,screen);
    check(dc && bitmap && pixels,"allocate native editor surface target");
    if(!dc || !bitmap || !pixels){if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);return;}
    HGDIOBJ old=SelectObject(dc,bitmap);
    size_t size=(size_t)rc.right*rc.bottom*4;BYTE *reference=malloc(size);
    check(reference!=NULL,"allocate native surface reference");
    if(reference) {
        for(int clipped=0;clipped<2;clipped++) {
            HRGN region=clipped?CreateRectRgnIndirect(&clip):NULL;
            SelectClipRgn(dc,region);if(region)DeleteObject(region);
            GetClipBox(dc,&before);memset(pixels,0,size);
            SendMessageW(hwnd,WM_PRINTCLIENT,(WPARAM)dc,PRF_CLIENT);GdiFlush();memcpy(reference,pixels,size);
            COLORREF bk=theme_bk();int filled=0;
            for(size_t i=0;i<size;i+=4)
                if(pixels[i]==GetBValue(bk)&&pixels[i+1]==GetGValue(bk)&&pixels[i+2]==GetRValue(bk))++filled;
            check(filled>0,"print produces real themed pixels");
            InvalidateRect(hwnd,NULL,TRUE);int pending=GetUpdateRect(hwnd,NULL,FALSE);
            memset(pixels,0,size);
            LRESULT erased=SendMessageW(hwnd,WM_ERASEBKGND,(WPARAM)dc,0);GdiFlush();
            check(erased && !memcmp(reference,pixels,size),"erase equals complete editor background including splitter");
            GetClipBox(dc,&after);check(EqualRect(&before,&after),"surface render preserves caller clip");
            check(GetUpdateRect(hwnd,NULL,FALSE)==pending,"surface render does not consume normal paint");
        }
        free(reference);
    }
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
}
int wmain(int argc,wchar_t **argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    if(argc<2 || WideCharToMultiByte(CP_UTF8,0,argv[1],-1,artifact,sizeof(artifact),NULL,NULL)<=0)return 2;
    INITCOMMONCONTROLSEX ic={sizeof(ic),ICC_WIN95_CLASSES};InitCommonControlsEx(&ic);
    g_hInst=GetModuleHandleW(NULL);settings_defaults(&g_settings);g_dpi=96;
    if(argc>2 && !wcscmp(argv[2],L"--dark")){g_dark=1;g_settings.theme=THEME_DARK;}
    int diagnostic=argc>2 && !wcscmp(argv[2],L"--trace");
    g_app=appdata_create_seed();if(!g_app)return 2;
    g_fontUI=g_fontBold=g_fontMono=CreateFontW(-16,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,0,0,L"Segoe UI");
    g_brushBk=CreateSolidBrush(theme_bk());g_brushCtl=CreateSolidBrush(theme_ctl_bk());
    if(!g_fontUI||!g_brushBk||!g_brushCtl||!ui_register_classes())return 2;
    SystemParametersInfoW(SPI_GETWORKAREA,0,&area,0);
    width=area.right-area.left;height=area.bottom-area.top;
    worst=calloc((size_t)width*height,4);if(!worst)return 2;
    HWND backdrop=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"Static",L"Editor test white backdrop",WS_POPUP|SS_WHITERECT,area.left,area.top,width,height,NULL,NULL,g_hInst,NULL);
    ShowWindow(backdrop,SW_SHOWNOACTIVATE);UpdateWindow(backdrop);
    EditorCtx c={0};c.t=g_app->templates;c.cur=-1;c.splitLastY=-1;
    qp_strlcpy(c.t->name,L"Editor resize synthetic fixture",QP_NAME_MAX);
    HWND editor=make_dlg_ex(L"QuickPhraseEditor",probe_proc,L"Editor resize regression - synthetic data",ED_MIN_W,ED_MIN_H,backdrop,&c,WS_THICKFRAME|WS_MAXIMIZEBOX);
    if(!editor)return 2;
    SetWindowPos(editor,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    ShowWindow(editor,SW_SHOW);pump(250);
    check_surface(editor);check_surface(c.hPropsHost);pump(100);
    InitializeCriticalSection(&gate);ready=CreateEventW(NULL,TRUE,FALSE,NULL);
    HANDLE worker=CreateThread(NULL,0,capture,NULL,0,NULL);if(!worker)return 2;
    while(WaitForSingleObject(ready,0)==WAIT_TIMEOUT)pump(10);
    if(!capture_ok){puts("SKIP: desktop duplication unavailable");failures=2;goto cleanup;}
    sample(1);RedrawWindow(editor,NULL,NULL,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_UPDATENOW);pump(150);sample(0);
    check(frames>0 && !bad,"initial editor has no large black regions");
    maximum=bad=frames=0;
    DWORD handles=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    for(int i=0;i<(diagnostic?1:16);i++) {
        trace=diagnostic;
        int before=frames;sample(1);
        if(i&1)ShowWindow(editor,SW_MAXIMIZE);
        else SendMessageW(editor,WM_SYSCOMMAND,SC_MAXIMIZE,0);
        pump(100);sample(0);check_editor(&c,1);check(frames>before,"captured this maximize transition");
        before=frames;sample(1);
        if(i&1)ShowWindow(editor,SW_RESTORE);
        else SendMessageW(editor,WM_SYSCOMMAND,SC_RESTORE,0);
        pump(100);sample(0);check_editor(&c,0);check(frames>before,"captured this restore transition");
        before=frames;sample(1);
        SetWindowPos(editor,NULL,0,0,1100+(i%3)*90,850+(i%2)*80,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        pump(50);sample(0);check_editor(&c,0);check(frames>before,"captured this border resize");
    }
    check(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)<=handles+3,"resizing does not leak GDI handles");
    check(!capture_error && frames>48,"desktop capture remains active");
    check(bad==0,"no black child surfaces presented while editor maximizes or resizes");
    save_frame();
cleanup:
    InterlockedExchange(&stopping,1);WaitForSingleObject(worker,INFINITE);CloseHandle(worker);CloseHandle(ready);
    DeleteCriticalSection(&gate);free(worst);
    DestroyWindow(editor);eval_destroy(c.previewCtx);DestroyWindow(backdrop);appdata_free(g_app);
    DeleteObject(g_fontUI);DeleteObject(g_brushBk);DeleteObject(g_brushCtl);
    printf("editor resize: checks=%d failures=%d frames=%d bad=%d max_black=%d\n",checks,failures,frames,bad,maximum);
    return failures?1:0;
}
