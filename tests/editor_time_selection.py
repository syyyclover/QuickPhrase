"""Execute format-edit notification: layout must not change edit selection.

The Win32 adapter reproduces a ComboBox reflow selecting its edit text.
This is not native keyboard/visual acceptance.
"""
from pathlib import Path
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/ui/dialog_editor.c').read_text()
m=re.search(r'^static void ed_commit_format\([^;]+?\)\n\{',s,re.M)
body=s[m.start():s.index('\n}',m.end())+2]
pre=r'''
#include <assert.h>
#include <stddef.h>
#include <wchar.h>
#include "engine.h"
typedef unsigned long DWORD;typedef unsigned long WPARAM;typedef long LPARAM;
typedef struct {int loading,hFmt;Segment *segment;} EditorCtx;
#define CB_ERR (-1)
#define CB_GETCURSEL 1
#define CB_GETLBTEXTLEN 2
#define CB_GETLBTEXT 3
#define CB_GETEDITSEL 4
#define CB_SETEDITSEL 5
#define MAKELPARAM(a,b) ((LPARAM)((unsigned short)(a)|((DWORD)(unsigned short)(b)<<16)))
static wchar_t text[QP_FORMAT_MAX];static DWORD start,end;static int previews;
static Segment *ed_cur_seg(EditorCtx *c){return c->segment;}
static void qp_get_text(int h,wchar_t *out,size_t cap){(void)h;wcsncpy(out,text,cap-1);out[cap-1]=0;}
void qp_strlcpy(wchar_t *out,const wchar_t *src,size_t cap){wcsncpy(out,src,cap-1);out[cap-1]=0;}
static long SendMessageW(int h,int msg,WPARAM wp,LPARAM lp){(void)h;
switch(msg){case CB_GETCURSEL:return 0;case CB_GETLBTEXTLEN:return 8;
case CB_GETLBTEXT:wcscpy((wchar_t *)lp,L"yyyyMMdd");return 8;
case CB_GETEDITSEL:if(wp)*(DWORD *)wp=start;if(lp)*(DWORD *)lp=end;return MAKELPARAM(start,end);
case CB_SETEDITSEL:start=(unsigned short)lp;end=(unsigned short)(lp>>16);return 1;}return 0;}
static void ed_update_fmt_sample(EditorCtx *c){(void)c;}
static void ed_layout(EditorCtx *c){(void)c;start=0;end=(DWORD)wcslen(text);}
static void ed_update_preview(EditorCtx *c){(void)c;++previews;}
'''
driver=r'''
int main(void){Segment segment={.type=SEG_DATETIME};EditorCtx c={.hFmt=1,.segment=&segment};
const wchar_t *values[]={L"yyyyMMddHHmmss",L"yyyyMMddHHmms",L"yyyyMMdd",L"",L"yyyyMM"};
for(int i=0;i<5;++i){wcscpy(text,values[i]);start=end=(DWORD)(wcslen(text)>2?2:wcslen(text));
DWORD caret=start;ed_commit_format(&c,0);assert(start==caret && end==caret);assert(!wcscmp(segment.time_format,text));assert(!c.loading);}
wcscpy(text,L"yyyyMMdd");start=2;end=5;ed_commit_format(&c,0);assert(start==2 && end==5);
c.loading=1;wcscpy(text,L"other");ed_commit_format(&c,0);assert(!wcscmp(segment.time_format,L"yyyyMMdd"));
c.loading=0;ed_commit_format(&c,1);assert(!wcscmp(segment.time_format,L"yyyyMMdd"));assert(previews==7);}
'''
with tempfile.TemporaryDirectory(prefix='qp-time-selection-') as tmp:
    exe=Path(tmp)/'probe'
    subprocess.run(['cc','-std=gnu11','-I',str(root/'src/core'),'-x','c','-','-o',str(exe)],input=pre+body+driver,text=True,check=True)
    subprocess.run([str(exe)],check=True)
print('time format editing: deletion/empty input/selection survive reflow; programmatic loading ignored')
