"""Real extractor with filesystem OS double: failed upgrade keeps the old EXE."""
from pathlib import Path
import subprocess,tempfile
from source_helpers import ROOT
s=(ROOT/'installer/installer.c').read_text();start=s.index('static int extract(')
body=s[start:s.index('\n}',start)+2]
pre=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>
#include <unistd.h>
#define MAX_PATH 260
#define _snwprintf swprintf
typedef void *HRSRC,*HGLOBAL;
typedef FILE *HANDLE;
typedef unsigned long DWORD;
#define INVALID_HANDLE_VALUE NULL
#define MAKEINTRESOURCEW(v) ((void *)(long)(v))
#define RT_RCDATA NULL
#define GENERIC_WRITE 1
#define CREATE_ALWAYS 2
#define CREATE_NEW 3
#define FILE_ATTRIBUTE_NORMAL 0
#define MOVEFILE_REPLACE_EXISTING 1
#define MOVEFILE_WRITE_THROUGH 2
#define ERROR_FILE_EXISTS 80
#define ERROR_ALREADY_EXISTS 183
static int fault;
static HRSRC FindResourceW(void *a,void *b,void *c){(void)a;(void)b;(void)c;return (void *)1;}
static DWORD SizeofResource(void *a,HRSRC b){(void)a;(void)b;return 11;}
static HGLOBAL LoadResource(void *a,HRSRC b){(void)a;(void)b;return (void *)1;}
static void *LockResource(HGLOBAL h){(void)h;return "replacement";}
static DWORD GetCurrentProcessId(void){return 42;}
static DWORD GetLastError(void){return 5;}
static HANDLE CreateFileW(const wchar_t *w,int access,int share,void *sec,int mode,int flags,void *t){
    (void)access;(void)share;(void)sec;(void)flags;(void)t;char p[600];wcstombs(p,w,600);
    return fopen(p,mode==CREATE_NEW?"wbx":"wb");
}
static int WriteFile(HANDLE f,const void *p,DWORD n,DWORD *written,void *o){
    (void)o;*written=fwrite(p,1,fault==1?n/2:n,f);return 1;
}
static int FlushFileBuffers(HANDLE f){return fault==2?0:fflush(f)==0;}
static int CloseHandle(HANDLE f){int okay=fclose(f)==0;return fault==3?0:okay;}
static int DeleteFileW(const wchar_t *w){char p[600];wcstombs(p,w,600);return !unlink(p);}
static int MoveFileExW(const wchar_t *a,const wchar_t *b,int flags){
    (void)flags;char src[600],dst[600];wcstombs(src,a,600);wcstombs(dst,b,600);return fault==4?0:rename(src,dst)==0;
}
'''
driver=r'''
int main(void){
    for(fault=1;fault<=4;++fault){
        FILE *f=fopen("app.exe","wb");assert(f);fputs("original",f);fclose(f);
        assert(!extract(1,L"app.exe"));char old[32]={0};f=fopen("app.exe","rb");assert(f);
        fread(old,1,31,f);fclose(f);assert(!strcmp(old,"original"));
    }
    fault=0;assert(extract(1,L"app.exe"));FILE *f=fopen("app.exe","rb");char s[32]={0};
    assert(f);fread(s,1,31,f);fclose(f);assert(!strcmp(s,"replacement"));
    puts("installer: short write/flush/close/replace preserve old EXE; successful replacement complete");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='qp-extract-') as tmp:
    exe=Path(tmp)/'test'
    subprocess.run(['cc','-std=gnu11','-Wall','-Wno-unused-function','-x','c','-','-o',str(exe)],input=pre+body+driver,text=True,check=True)
    subprocess.run([str(exe)],cwd=tmp,check=True)
    assert sorted(p.name for p in Path(tmp).iterdir())==['app.exe','test'], 'failed staging left temporary files'
