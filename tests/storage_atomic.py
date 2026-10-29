from source_helpers import read_source, INCLUDE_FLAGS, CORE_SOURCES
"""Execute the production atomic writer with injected Win32 replacement failure.

Only OS replacement/deletion is doubled; stream writes and old-file checks use
real files in a temporary directory. Losing the old file or reporting a failed
close as success must fail this test.
"""
import pathlib
import subprocess
import tempfile

repo = pathlib.Path(__file__).resolve().parents[1]
source = read_source("storage.c")
start = source.index('int storage_write_text(')
body = source[start:source.index('\n}', start) + 2]
preamble = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <unistd.h>
#define _WIN32 1
#define MOVEFILE_REPLACE_EXISTING 1
#define MOVEFILE_WRITE_THROUGH 2
static int fail_move = 1, fail_close, fail_sync;
static int sync_stream(int fd) { return fail_sync ? -1 : fsync(fd); }
#define _fileno fileno
#define _commit sync_stream
static FILE *open_w(const wchar_t *p, const wchar_t *m) {
    char path[1024], mode[32];
    wcstombs(path, p, sizeof(path)); wcstombs(mode, m, sizeof(mode));
    return fopen(path, mode);
}
static int MoveFileExW(const wchar_t *a, const wchar_t *b, int flags) {
    char src[1024], dst[1024]; (void)flags;
    wcstombs(src,a,sizeof(src)); wcstombs(dst,b,sizeof(dst));
    return fail_move ? 0 : rename(src,dst)==0;
}
static int DeleteFileW(const wchar_t *p) {
    char path[1024]; wcstombs(path,p,sizeof(path)); return remove(path)==0;
}
static int close_stream(FILE *f) { int r=fclose(f); return fail_close ? EOF : r; }
#define fclose close_stream
'''
driver = r'''
int main(void) {
    FILE *f=fopen("data.json","wb"); char old[32]={0}; int failures=0;
    fputs("original",f); close_stream(f);
    if(storage_write_text(L"data.json","replacement")) ++failures;
    f=fopen("data.json","rb");
    if(!f) { puts("FAIL: replacement failure deleted original data"); ++failures; }
    else { fread(old,1,31,f); close_stream(f); if(strcmp(old,"original")) ++failures; }
    fail_move=0; fail_close=1;
    if(storage_write_text(L"data.json","replacement")) {
        puts("FAIL: fclose failure reported as successful save"); ++failures;
    }
    fail_close=0;
    fail_sync=1;
    if(storage_write_text(L"data.json","replacement")) {
        puts("FAIL: disk flush failure reported as successful save"); ++failures;
    }
    fail_sync=0;
    if(!storage_write_text(L"data.json","replacement")) ++failures;
    return failures ? 1 : 0;
}
'''
with tempfile.TemporaryDirectory(prefix='qp-atomic-') as tmp:
    src = pathlib.Path(tmp) / 'atomic.c'
    src.write_text(preamble + body + driver)
    exe = pathlib.Path(tmp) / 'atomic'
    subprocess.run(['cc', '-std=gnu11', '-Wall', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], cwd=tmp, check=True)
print('atomic save: replacement/close failures preserve original data')
