/* Native desktop-composition regression. Synthetic model only; saves are wrapped.
   Requires an unlocked Windows 8+ desktop and DXGI duplication. No profile read,
   clipboard, tray registration, or input injection. Not part of Linux make test. */
#define COBJMACROS
#include "ui_internal.h"
#include <stdio.h>
#include <dwmapi.h>
#include "win32_desktop_capture.h"

static CRITICAL_SECTION lock;
static HANDLE ready;
static volatile LONG stopping;
static int sampling, capture_ok, capture_error, frames, owned_frames, bad_frames;
static LARGE_INTEGER sample_started;
static int max_black, width, height, checks, failures;
static RECT target;
static BYTE *worst;
static char artifact[1024];

int __wrap_settings_save(const Settings *s) { (void)s; return 1; }
int __wrap_storage_save(const AppData *a) { (void)a; return 1; }

static void check(int yes,const char *name) {
    ++checks;if(!yes){++failures;printf("FAIL: %s\n",name);}
}
static void pump(int ms) {
    DWORD end=GetTickCount()+ms;MSG m;
    do {
        while(PeekMessageW(&m,NULL,0,0,PM_REMOVE))
            if(m.message!=WM_QUIT){TranslateMessage(&m);DispatchMessageW(&m);}
        Sleep(1);
    } while((LONG)(GetTickCount()-end)<0);
}
static DWORD WINAPI capture(void *unused) {
    DXCapture dx={0};BYTE *pixels=malloc((size_t)width*height*4);
    capture_ok=pixels && dx_init(&dx,target);
    SetEvent(ready);
    if(!capture_ok){free(pixels);dx_free(&dx);return 1;}
    while(!InterlockedCompareExchange(&stopping,0,0)) {
        /* Acquisition happens outside the gate, so UI transitions don't wait for
           a desktop frame. The gate protects attribution and in-flight copies. */
        LARGE_INTEGER presented;
        int got=dx_frame(&dx,target,pixels,&presented);
        if(got<0){capture_error=1;break;}
        if(!got)continue;
        EnterCriticalSection(&lock);
        if(sampling && presented.QuadPart>=sample_started.QuadPart) {
            ++frames;
            BYTE *p=pixels+4*(4*width+4);
            COLORREF bk=theme_bk();
            if(p[0]==GetBValue(bk)&&p[1]==GetGValue(bk)&&p[2]==GetRValue(bk)) {
                ++owned_frames;int black=0;
                for(int i=0;i<width*height;i++)
                    if(pixels[i*4]<20 && pixels[i*4+1]<20 && pixels[i*4+2]<20)++black;
                /* Fixture text occupies <1%; >10% is a missing child surface. */
                if(black>width*height/10)++bad_frames;
                if(black>max_black){max_black=black;memcpy(worst,pixels,(size_t)width*height*4);}
            }
        }
        LeaveCriticalSection(&lock);
    }
    free(pixels);dx_free(&dx);return 0;
}
static void sample(int on) {
    EnterCriticalSection(&lock);
    if(on)QueryPerformanceCounter(&sample_started);
    sampling=on;LeaveCriticalSection(&lock);
}
static void check_restored(void) {
    DWORD cloaked=0;
    HRESULT hr=DwmGetWindowAttribute(g_hwndMain,DWMWA_CLOAKED,&cloaked,sizeof(cloaked));
    check(SUCCEEDED(hr) && !(cloaked&DWM_CLOAKED_APP),"restored window is not left app-cloaked");
    check(IsWindowVisible(g_hwndMain)&&!IsIconic(g_hwndMain),"restored window remains visible and non-iconic");
    check(GetWindowTextLengthW(g_fields[0].hCtl)>0,"restore preserves fixture input");
}
static void save_frame(void) {
    FILE *out=fopen(artifact,"wb");if(!out){check(0,"write frame artifact");return;}
    BITMAPFILEHEADER fh={0};BITMAPINFOHEADER ih={0};
    fh.bfType=0x4d42;fh.bfOffBits=sizeof(fh)+sizeof(ih);fh.bfSize=fh.bfOffBits+width*height*4;
    ih.biSize=sizeof(ih);ih.biWidth=width;ih.biHeight=-height;ih.biPlanes=1;ih.biBitCount=32;
    int ok=fwrite(&fh,sizeof(fh),1,out)==1 && fwrite(&ih,sizeof(ih),1,out)==1 &&
           fwrite(worst,(size_t)width*height*4,1,out)==1;
    if(fclose(out))ok=0;
    check(ok,"save captured desktop pixels");
}
int wmain(int argc,wchar_t **argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    if(argc<2 || WideCharToMultiByte(CP_UTF8,0,argv[1],-1,artifact,sizeof(artifact),NULL,NULL)<=0) {
        puts("usage: test_restore_visibility_win32.exe OUTPUT.bmp [--dark] [--maximized]");return 2;
    }
    INITCOMMONCONTROLSEX ic={sizeof(ic),ICC_WIN95_CLASSES};InitCommonControlsEx(&ic);
    g_hInst=GetModuleHandleW(NULL);settings_defaults(&g_settings);
    for(int i=2;i<argc;i++) {
        if(!wcscmp(argv[i],L"--dark")){g_dark=1;g_settings.theme=THEME_DARK;}
        else if(!wcscmp(argv[i],L"--maximized"))g_settings.win_maximized=1;
        else return 2;
    }
    g_settings.win_x=100;g_settings.win_y=100;g_settings.win_w=1100;g_settings.win_h=750;
    g_settings.minimize_to_tray_hint=1;g_app=appdata_create_seed();g_dpi=96;
    g_fontUI=g_fontBold=g_fontMono=CreateFontW(-16,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,0,0,L"Segoe UI");
    g_brushBk=CreateSolidBrush(theme_bk());g_brushCtl=CreateSolidBrush(theme_ctl_bk());
    if(!g_app||!g_fontUI||!g_brushBk||!g_brushCtl||!ui_register_classes())return 2;
    /* A bright underlay separates an absent/animated window from black content. */
    RECT work;SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
    HWND backdrop=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"Static",L"QuickPhrase test backdrop",
        WS_POPUP|SS_WHITERECT,work.left,work.top,work.right-work.left,work.bottom-work.top,NULL,NULL,g_hInst,NULL);
    ShowWindow(backdrop,SW_SHOWNOACTIVATE);UpdateWindow(backdrop);
    g_hwndMain=ui_create_main_window();if(!g_hwndMain)return 2;
    SetWindowTextW(g_hwndMain,L"QuickPhrase restore regression - synthetic data");
    SetWindowPos(g_hwndMain,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    ui_theme_changed();ui_rebuild_tree(g_app->templates->id);
    ShowWindow(g_hwndMain,g_settings.win_maximized?SW_SHOWMAXIMIZED:SW_SHOW);pump(300);
    GetClientRect(g_hwndMain,&target);MapWindowPoints(g_hwndMain,NULL,(POINT*)&target,2);
    width=target.right-target.left;height=target.bottom-target.top;
    worst=calloc((size_t)width*height,4);if(!worst)return 2;
    InitializeCriticalSection(&lock);ready=CreateEventW(NULL,TRUE,FALSE,NULL);
    HANDLE worker=CreateThread(NULL,0,capture,NULL,0,NULL);if(!worker)return 2;
    while(WaitForSingleObject(ready,0)==WAIT_TIMEOUT)pump(10);
    if(!capture_ok){puts("SKIP: desktop duplication unavailable");failures=2;goto cleanup;}
    sample(1);RedrawWindow(g_hwndMain,NULL,NULL,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW);pump(150);sample(0);
    check(owned_frames>0 && max_black<width*height/10,"initial launch has a complete nonblack frame");
    max_black=bad_frames=owned_frames=frames=0;
    DWORD gdi_before=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    for(int i=0;i<20;i++) {
        ShowWindow(g_hwndMain,SW_MINIMIZE);pump(30);
        int before=owned_frames;
        sample(1);
        /* Both shell-style system command and direct Win32 restoration. */
        if(i&1)SendMessageW(g_hwndMain,WM_SYSCOMMAND,SC_RESTORE,0);
        else ShowWindow(g_hwndMain,SW_RESTORE);
        pump(80);sample(0);check_restored();
        check(owned_frames>before,"captured a presented client frame for this minimize/restore cycle");
        app_hide_main();pump(30);before=owned_frames;sample(1);
        app_show_main();app_show_main();app_show_main(); /* tray single/double clicks */
        pump(80);sample(0);check_restored();
        check(owned_frames>before,"captured a presented client frame for this tray/show cycle");
    }
    check(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)<=gdi_before+3,"repeated restore does not leak GDI handles");
    check(!capture_error && owned_frames>=20,"compositor frames were captured throughout restoration");
    check(bad_frames==0,"no presented frame exposes black child rectangles during restore");
    save_frame();
cleanup:
    InterlockedExchange(&stopping,1);WaitForSingleObject(worker,INFINITE);CloseHandle(worker);CloseHandle(ready);
    DeleteCriticalSection(&lock);free(worst);
    DestroyWindow(g_hwndMain);g_hwndMain=NULL;DestroyWindow(backdrop);
    appdata_free(g_app);g_app=NULL;DeleteObject(g_fontUI);DeleteObject(g_brushBk);DeleteObject(g_brushCtl);
    printf("restore visibility: checks=%d failures=%d frames=%d owned=%d bad=%d max_black=%d\n",checks,failures,frames,owned_frames,bad_frames,max_black);
    return failures?1:0;
}
