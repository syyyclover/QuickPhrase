"""Execute editor insertion and capacity handling; Win32 calls are adapted."""
from pathlib import Path
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/ui/dialog_editor.c').read_text()
def function(name):
    m=re.search(r'^static void '+name+r'\([^;]+?\)\n\{',s,re.M)
    assert m, 'missing insertion helper: '+name
    return s[m.start():s.index('\n}',m.end())+2]
functions=function('ed_insert_table_text')+'\n'+function('ed_insert_table_variable')
pre=r'''
#include <assert.h>
#include <wchar.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "engine.h"
typedef unsigned long DWORD;
typedef intptr_t LPARAM;
typedef intptr_t WPARAM;
typedef struct {int hTableFmt,hTableInsert,hwnd;} EditorCtx;
typedef struct {wchar_t label[QP_LABEL_MAX];} DummySegment;
static DummySegment selected;
static wchar_t text[QP_OUTPUT_MAX];static DWORD a,b;static int changes,errors;
#define EM_GETSEL 1
#define EM_REPLACESEL 2
#define TRUE 1
#define MB_OK 1
#define MB_ICONWARNING 2
#define _snwprintf swprintf
static const void *combo_selected_seg(int h){(void)h;return &selected;}
static int GetWindowTextLengthW(int h){(void)h;return (int)wcslen(text);}
static void SendMessageW(int h,int msg,intptr_t wp,LPARAM lp){(void)h;
if(msg==EM_GETSEL){*(DWORD*)wp=a;*(DWORD*)lp=b;}
if(msg==EM_REPLACESEL){const wchar_t *v=(const wchar_t*)lp;size_t n=wcslen(v);
memmove(text+a+n,text+b,(wcslen(text)-b+1)*sizeof(wchar_t));wmemcpy(text+a,v,n);a=b=a+n;}}
static void ed_table_format_changed(EditorCtx *c){(void)c;++changes;}
static void SetFocus(int h){(void)h;}
static void qp_msgbox(int h,int flags,const wchar_t *title,const wchar_t *msg){(void)h;(void)flags;(void)title;(void)msg;++errors;}
'''.replace('typedef struct {wchar_t label[QP_LABEL_MAX];} DummySegment;','typedef Segment DummySegment;')
driver=r'''
int main(void){EditorCtx c={0};wcscpy(selected.label,L"test");
ed_insert_table_variable(&c);assert(wcscmp(text,L"{test}")==0);
ed_insert_table_text(&c,L"\\t");assert(wcscmp(text,L"{test}\\t")==0 && changes==2);
a=0;b=6;ed_insert_table_text(&c,L"{other}");assert(wcscmp(text,L"{other}\\t")==0);
for(int i=0;i<QP_OUTPUT_MAX-2;++i)text[i]=L'x';text[QP_OUTPUT_MAX-2]=0;a=b=QP_OUTPUT_MAX-2;
int before=changes;ed_insert_table_text(&c,L"\\t");assert(errors==1 && changes==before && wcslen(text)==QP_OUTPUT_MAX-2);
a=b=QP_OUTPUT_MAX-3; b=QP_OUTPUT_MAX-2;ed_insert_table_text(&c,L"\\t");assert(wcslen(text)==QP_OUTPUT_MAX-1 && changes==before+1);}
'''
with tempfile.TemporaryDirectory(prefix='qp-table-insert-') as tmp:
    exe=Path(tmp)/'probe'
    subprocess.run(['cc','-std=gnu11','-I',str(root/'src/core'),'-x','c','-','-o',str(exe)],input=pre+functions+driver,text=True,check=True)
    subprocess.run([str(exe)],check=True)
print('table insertion: placeholder, cell delimiter, selection, overflow refusal and exact capacity')
