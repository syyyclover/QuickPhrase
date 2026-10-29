/* Read-only icon diagnostics. --refresh notifies Shell about this file only. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <wincrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int file_hash(const wchar_t *path)
{
    HCRYPTPROV provider=0; HCRYPTHASH hash=0;
    BYTE buffer[65536], digest[32]; DWORD read=0,size=sizeof(digest); int ok=0;
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                            NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE) return 0;
    if(!CryptAcquireContextW(&provider,NULL,NULL,PROV_RSA_AES,CRYPT_VERIFYCONTEXT) ||
       !CryptCreateHash(provider,CALG_SHA_256,0,0,&hash)) goto done;
    for(;;) {
        if(!ReadFile(file,buffer,sizeof(buffer),&read,NULL)) goto done;
        if(!read) break;
        if(!CryptHashData(hash,buffer,read,0)) goto done;
    }
    if(!CryptGetHashParam(hash,HP_HASHVAL,digest,&size,0)) goto done;
    wprintf(L"SHA256: ");
    for(DWORD i=0;i<size;++i) wprintf(L"%02x",digest[i]);
    wprintf(L"\n"); ok=1;
done:
    if(hash) CryptDestroyHash(hash);
    if(provider) CryptReleaseContext(provider,0);
    CloseHandle(file); return ok;
}

static BYTE *raster(HICON icon,int size)
{
    BITMAPINFO info={0}; BYTE *bits=NULL,*copy=NULL;
    HDC dc=CreateCompatibleDC(NULL); HBITMAP image; HGDIOBJ old;
    if(!dc || !icon) { if(dc) DeleteDC(dc); return NULL; }
    info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth=size;
    info.bmiHeader.biHeight=-size; info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32;
    image=CreateDIBSection(dc,&info,DIB_RGB_COLORS,(void **)&bits,NULL,0);
    if(!image) { DeleteDC(dc); return NULL; }
    old=SelectObject(dc,image); memset(bits,255,(size_t)size*size*4);
    if(DrawIconEx(dc,0,0,icon,size,size,0,NULL,DI_NORMAL) && GdiFlush()) {
        copy=(BYTE *)malloc((size_t)size*size*4);
        if(copy) {
            memcpy(copy,bits,(size_t)size*size*4);
            for(int i=0;i<size*size;++i) copy[i*4+3]=255; /* Compare rendered RGB, not HICON alpha conventions. */
        }
    }
    SelectObject(dc,old); DeleteObject(image); DeleteDC(dc); return copy;
}

static int save_bitmap(const wchar_t *dir,const wchar_t *name,const BYTE *bits,int size)
{
    wchar_t path[1024]; BITMAPFILEHEADER file={0}; BITMAPINFOHEADER info={0}; FILE *out;
    if(!bits || swprintf(path,1024,L"%ls\\%ls",dir,name)<0) return 0;
    file.bfType=0x4D42; file.bfOffBits=sizeof(file)+sizeof(info);
    file.bfSize=file.bfOffBits+(DWORD)size*size*4;
    info.biSize=sizeof(info); info.biWidth=size; info.biHeight=-size;
    info.biPlanes=1; info.biBitCount=32; info.biSizeImage=(DWORD)size*size*4;
    out=_wfopen(path,L"wb"); if(!out) return 0;
    int ok=fwrite(&file,1,sizeof(file),out)==sizeof(file) &&
           fwrite(&info,1,sizeof(info),out)==sizeof(info) &&
           fwrite(bits,1,info.biSizeImage,out)==info.biSizeImage;
    if(fclose(out)) ok=0;
    return ok;
}

static int raster_self_test(void)
{
    BYTE mask[128]={0},red[4096],blue[4096];
    DWORD old_limit=GdiSetBatchLimit(1000);
    for(int i=0;i<1024;++i) {
        red[i*4]=0; red[i*4+1]=0; red[i*4+2]=255; red[i*4+3]=255;
        blue[i*4]=255; blue[i*4+1]=0; blue[i*4+2]=0; blue[i*4+3]=255;
    }
    HICON a=CreateIcon(GetModuleHandleW(NULL),32,32,1,32,mask,red);
    HICON b=CreateIcon(GetModuleHandleW(NULL),32,32,1,32,mask,blue);
    BYTE *ra=raster(a,32),*rb=raster(b,32);
    int colored=0;
    if(ra && rb) for(int i=0;i<1024;++i)
        if(ra[i*4]!=255 || ra[i*4+1]!=255 || ra[i*4+2]!=255) ++colored;
    int ok=ra && rb && colored>0 && memcmp(ra,rb,4096)!=0;
    free(ra); free(rb); if(a) DestroyIcon(a); if(b) DestroyIcon(b);
    GdiSetBatchLimit(old_limit);
    wprintf(L"Batched icon raster regression: %ls\n",ok?L"PASS":L"FAIL");
    return ok?0:1;
}

int wmain(int argc,wchar_t **argv)
{
    wchar_t path[32768],temp[MAX_PATH],dir[1024];
    HICON direct_large=NULL,direct_small=NULL; int failures=0;
    if(argc==2 && !wcscmp(argv[1],L"--self-test")) return raster_self_test();
    if(argc<2) {
        wprintf(L"Usage: QuickPhrase-icon-check.exe <full path to EXE> [--refresh]\n");
        return 2;
    }
    DWORD n=GetFullPathNameW(argv[1],32768,path,NULL);
    if(!n || n>=32768 || !file_hash(path)) {
        wprintf(L"Cannot read target EXE; Windows error %lu\n",GetLastError()); return 1;
    }
    wprintf(L"EXE: %ls\n",path);
    CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
    if(argc>2 && !wcscmp(argv[2],L"--refresh")) {
        SHChangeNotify(SHCNE_UPDATEITEM,SHCNF_PATHW|SHCNF_FLUSH,path,NULL);
        wprintf(L"Requested a Shell refresh for this EXE only.\n");
    }
    if(!GetTempPathW(MAX_PATH,temp)) { CoUninitialize(); return 1; }
    swprintf(dir,1024,L"%lsQuickPhrase-icons-%lu-%llu",temp,GetCurrentProcessId(),
             (unsigned long long)GetTickCount64());
    if(!CreateDirectoryW(dir,NULL)) { CoUninitialize(); return 1; }
    if(!ExtractIconExW(path,0,&direct_large,&direct_small,1)) {
        wprintf(L"No directly extractable EXE icon.\n"); CoUninitialize(); return 1;
    }
    for(int small=0;small<2;++small) {
        SHFILEINFOW shell={0};
        int size=GetSystemMetrics(small?SM_CXSMICON:SM_CXICON);
        DWORD flags=SHGFI_ICON|(small?SHGFI_SMALLICON:SHGFI_LARGEICON);
        BYTE *direct=raster(small?direct_small:direct_large,size),*shown=NULL;
        if(SHGetFileInfoW(path,0,&shell,sizeof(shell),flags)) shown=raster(shell.hIcon,size);
        if(!direct || !shown) { wprintf(L"Icon rendering failed.\n"); ++failures; }
        else wprintf(L"%ls (%dpx): Shell_matches_direct=%ls\n",small?L"small":L"large",size,
                    memcmp(direct,shown,(size_t)size*size*4)?L"NO":L"YES");
        save_bitmap(dir,small?L"direct-small.bmp":L"direct-large.bmp",direct,size);
        save_bitmap(dir,small?L"shell-small.bmp":L"shell-large.bmp",shown,size);
        free(direct); free(shown); if(shell.hIcon) DestroyIcon(shell.hIcon);
    }
    if(direct_large) DestroyIcon(direct_large);
    if(direct_small) DestroyIcon(direct_small);
    wprintf(L"Icon previews: %ls\n",dir);
    wprintf(L"Compare the SHA256 with the shipped checksum before attributing a difference to caching.\n");
    CoUninitialize(); return failures?1:0;
}
