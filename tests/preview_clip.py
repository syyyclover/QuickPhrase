"""Real table renderer must intersect and preserve the external DC clip."""
from pathlib import Path
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/ui/preview.c').read_text()
m=re.search(r'^static void pv_paint_table\([^;]+?\)\n\{',s,re.M)
body=s[m.start():s.index('\n}',m.end())+2]
pre=r'''
#include <assert.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>
typedef struct {int left,top,right,bottom;} RECT;
typedef struct {RECT clip,saved;int pixels[100][100],x,y;} DC,*HDC;
typedef RECT *HRGN;typedef void *HBRUSH;typedef void *HPEN;typedef void *HGDIOBJ;
typedef struct {int off,len,missing;} PvCell;
typedef struct {int headH,rows,cols,*colw,scrollX,scrollY,rowH,hdrCount;PvCell *cells;wchar_t *hdr,*raw;} Preview;
#define QP_LABEL_MAX 128
#define PV_TABLE_MARK L"missing"
#define PS_SOLID 1
#define DT_SINGLELINE 1
#define DT_NOPREFIX 2
#define DT_END_ELLIPSIS 4
#define DT_VCENTER 8
#define _snwprintf swprintf
#define RGB(r,g,b) ((r)+(g)+(b))
static int g_dark,save_fail;
static int DPIX(int n){return n;}
static int pv_padx(void){return 8;}static int pv_pady(void){return 6;}static int pv_cellpad(void){return 7;}
static int theme_dim(void){return 1;}static int theme_text(void){return 2;}
static int theme_line(void){return 3;}static int theme_error(void){return 4;}
static HBRUSH CreateSolidBrush(int c){(void)c;return malloc(1);}
static HPEN CreatePen(int t,int w,int c){(void)t;(void)w;(void)c;return malloc(1);}
static int DeleteObject(HGDIOBJ o){free(o);return 1;}
static HGDIOBJ SelectObject(HDC d,HGDIOBJ o){(void)d;(void)o;return NULL;}
static int SetTextColor(HDC d,int c){(void)d;(void)c;return 1;}
static int SaveDC(HDC d){d->saved=d->clip;return save_fail?0:1;}
static int RestoreDC(HDC d,int n){(void)n;d->clip=d->saved;return 1;}
static int IntersectClipRect(HDC d,int l,int t,int r,int b){
if(d->clip.left<l)d->clip.left=l;if(d->clip.top<t)d->clip.top=t;
if(d->clip.right>r)d->clip.right=r;if(d->clip.bottom>b)d->clip.bottom=b;return 1;}
static HRGN CreateRectRgn(int l,int t,int r,int b){HRGN a=malloc(sizeof(*a));*a=(RECT){l,t,r,b};return a;}
static int SelectClipRgn(HDC d,HRGN a){d->clip=a?*a:(RECT){0,0,100,100};return 1;}
static int FillRect(HDC d,const RECT *r,HBRUSH brush){(void)brush;
for(int y=r->top;y<r->bottom;++y)for(int x=r->left;x<r->right;++x)
if(x>=0 && y>=0 && x<100 && y<100 && x>=d->clip.left && x<d->clip.right && y>=d->clip.top && y<d->clip.bottom)d->pixels[y][x]=1;return 1;}
static int DrawTextW(HDC d,const wchar_t *text,int n,RECT *r,int flags){(void)text;(void)n;(void)flags;return FillRect(d,r,NULL);}
static int MoveToEx(HDC d,int x,int y,void *p){(void)p;d->x=x;d->y=y;return 1;}
static int LineTo(HDC d,int x,int y){RECT r={d->x<x?d->x:x,d->y<y?d->y:y,d->x>x?d->x:x,d->y>y?d->y:y};r.right++;r.bottom++;FillRect(d,&r,NULL);d->x=x;d->y=y;return 1;}
'''
driver=r'''
int main(void){for(save_fail=0;save_fail<=1;++save_fail){
DC d={.clip={30,30,50,70}};RECT rc={0,0,100,100},original=d.clip;
int widths[1]={100};PvCell cells[4]={{0,1,0},{0,1,0},{0,1,0},{0,1,0}};
Preview p={.headH=20,.rows=4,.cols=1,.rowH=20,.colw=widths,.cells=cells,.raw=L"X"};
pv_paint_table(&d,&p,&rc);assert(!memcmp(&d.clip,&original,sizeof(RECT)));
int painted=0;for(int y=0;y<100;++y)for(int x=0;x<100;++x){
if(x>=30 && x<50 && y>=30 && y<70)painted+=d.pixels[y][x];else assert(!d.pixels[y][x]);}assert(painted>0);}}
'''
with tempfile.TemporaryDirectory(prefix='qp-preview-clip-') as tmp:
    exe=Path(tmp)/'probe'
    subprocess.run(['cc','-std=gnu11','-x','c','-','-o',str(exe)],input=pre+body+driver,text=True,check=True)
    subprocess.run([str(exe)],check=True)
print('table external clip: real renderer respects caller pixels and preserves clip, including SaveDC failure')
