"""Real registry snapshot/restore with a deterministic Win32 boundary adapter."""
from pathlib import Path
import re
import subprocess
import tempfile
from source_helpers import INCLUDE_FLAGS
root=Path(__file__).resolve().parents[1]
s=(root/'src/platform/main.c').read_text()
def function(name):
    m=re.search(r'^(?:void|int) '+name+r'\([^;]+?\)\n\{',s,re.M)
    return s[m.start():s.index('\n}',m.end())+2]
h=(root/'src/platform/app.h').read_text();a=h.index('typedef struct {\n    unsigned type;');b=h.index('int qp_settings_commit',a)
types=h[a:b]
pre=r'''
#include <wchar.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
typedef int HKEY;
typedef long LONG;
typedef unsigned DWORD;
typedef unsigned char BYTE;
#define HKEY_CURRENT_USER 1
#define KEY_QUERY_VALUE 1
#define KEY_SET_VALUE 2
#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_ACCESS_DENIED 5
#define ERROR_MORE_DATA 234
#define REG_SZ 1
#define REG_EXPAND_SZ 2
#define QP_RUN_KEY L"run"
#define QP_APP_NAME L"QuickPhrase"
#define _wcsnicmp wcsncasecmp
static int mode,reads,closed,restored;
static BYTE recorded[1024];static DWORD recorded_type,recorded_size;
static const wchar_t *command=L"\"C:\\QP\\QuickPhrase.exe\" --tray";
static int exe_path(wchar_t *out,DWORD cap) {swprintf(out,cap,L"C:\\QP\\QuickPhrase.exe");return 1;}
static LONG RegOpenKeyExW(int root,const wchar_t *p,int f,int access,HKEY *k) {
    (void)root;(void)p;(void)f;(void)access;*k=1;return mode==1?ERROR_FILE_NOT_FOUND:mode==2?ERROR_ACCESS_DENIED:ERROR_SUCCESS;
}
static int RegCloseKey(HKEY k) {(void)k;++closed;return 0;}
static LONG RegQueryValueExW(HKEY k,const wchar_t *name,void *r,DWORD *type,BYTE *data,DWORD *size) {
    (void)k;(void)name;(void)r;*type=REG_SZ;
    DWORD need=(DWORD)wcslen(command)*sizeof(wchar_t); /* deliberately no NUL */
    if(!data){*size=mode==4?70000:need;return ERROR_SUCCESS;}
    ++reads;if(mode==3){*size=need+100;return ERROR_MORE_DATA;}
    assert(*size>=need);memcpy(data,command,need);*size=need;return ERROR_SUCCESS;
}
static DWORD ExpandEnvironmentStringsW(const wchar_t *from,wchar_t *to,DWORD n) {
    (void)from;wcscpy(to,command);return n?(DWORD)wcslen(to)+1:0;
}
static LONG RegCreateKeyExW(int a,const wchar_t *b,int c,void *d,int e,int f,void *g,HKEY *k,void *i) {
    (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)i;*k=1;return ERROR_SUCCESS;
}
static LONG RegSetValueExW(HKEY k,const wchar_t *n,int r,DWORD type,const BYTE *data,DWORD size) {
    (void)k;(void)n;(void)r;assert(size<=sizeof(recorded));memcpy(recorded,data,size);
    recorded_type=type;recorded_size=size;++restored;return ERROR_SUCCESS;
}
static LONG RegDeleteValueW(HKEY k,const wchar_t *n) {(void)k;(void)n;++restored;return ERROR_FILE_NOT_FOUND;}
'''
driver=r'''
int main(void) {
    QpAutostartSnapshot saved;
    assert(qp_autostart_capture(&saved) && saved.present && saved.enabled);
    assert(saved.size==wcslen(command)*sizeof(wchar_t));
    assert(qp_autostart_restore(&saved) && recorded_type==REG_SZ && recorded_size==saved.size);
    assert(!memcmp(recorded,command,saved.size));qp_autostart_dispose(&saved);
    command=L"\"C:\\OTHER\\QuickPhrase.exe\" --tray";
    assert(qp_autostart_capture(&saved) && !saved.enabled);qp_autostart_dispose(&saved);
    mode=1;assert(qp_autostart_capture(&saved) && !saved.present);
    assert(qp_autostart_restore(&saved));qp_autostart_dispose(&saved);
    mode=2;assert(!qp_autostart_capture(&saved));
    mode=3;reads=closed=0;assert(!qp_autostart_capture(&saved) && reads==3 && closed==1);
    mode=4;assert(!qp_autostart_capture(&saved));
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='qp-startup-snapshot-') as tmp:
    exe=Path(tmp)/'probe'
    code=pre+types+function('qp_autostart_dispose')+function('qp_autostart_capture')+function('qp_autostart_restore')+driver
    subprocess.run(['cc','-std=gnu11','-Wall',*INCLUDE_FLAGS,'-x','c','-','-o',str(exe)],input=code,text=True,check=True)
    subprocess.run([str(exe)],cwd=tmp,check=True)
print('startup snapshot: exact bytes/type, nonterminated strings, absent/denied/changing/oversize values')
