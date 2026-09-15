"""Exercise the template-editor production layout through a Win32 geometry adapter.

This checks rectangles, clipping, and output-on/output-off state transitions at
minimum and maximized editor sizes.  It does not claim native Windows painting,
message dispatch, keyboard behavior, or combo-popup behavior.  Run with:
  python3 .superpowers/sdd/2026-09-10-dual-text-table-output/task-3-geometry.py
"""
import pathlib
import re
import subprocess
import sys
import tempfile

repo = pathlib.Path(__file__).resolve().parents[1]
source = (repo / "src/ui_dialogs.c").read_text()

expected = ["hTableOutput", "hTableFmtLbl", "hTableFmt", "hTableInsert",
            "hTableInsertBtn", "hPreviewText", "hPreviewTable"]
missing = [name for name in expected if not re.search(r"\b" + name + r"\b", source)]
if missing:
    print("RED: table editor layout/state is not implemented: " + ", ".join(missing))
    sys.exit(1)

def function(name):
    match = re.search(r"static [^\n]+\b" + name + r"\([^;]+?\)\n\{", source)
    if not match:
        print("RED: missing production function " + name)
        sys.exit(1)
    return source[match.start():source.index("\n}", match.end()) + 2] + "\n"

end = source.index("} EditorCtx;") + len("} EditorCtx;")
ctx = source[source.rfind("typedef struct {", 0, end):end]
fields = [f.strip() for decl in re.findall(r"HWND ([^;]+);", ctx) for f in decl.split(",")]
props = fields[fields.index("hPropTitle"):fields.index("hPreviewLbl")]
if any(name not in props for name in expected):
    print("RED: table controls must be children of the clipped property host")
    sys.exit(1)

preamble = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "engine.h"
#include "resource.h"
typedef struct {int left,top,right,bottom;} RECT;
typedef struct {unsigned cbSize,fMask;int nMin,nMax;unsigned nPage;int nPos,nTrackPos;} SCROLLINFO;
typedef unsigned UINT;
typedef struct Ctl {RECT rect; struct Ctl *parent; int visible,enabled,scrollbar,checked; SCROLLINFO si; const char *name;} Ctl;
typedef Ctl *HWND;
typedef int HDWP;
#define TRUE 1
#define SW_SHOW 1
#define SW_HIDE 0
#define SB_VERT 1
#define SIF_RANGE 1
#define SIF_PAGE 2
#define SIF_POS 4
#define IDOK 1
#define IDCANCEL 2
#define BST_CHECKED 1
#define BST_UNCHECKED 0
#define BM_GETCHECK 0x00F0
#define BM_SETCHECK 0x00F1
#define RDW_INVALIDATE 1
#define RDW_NOERASE 2
#define RDW_UPDATENOW 4
#define ED_MIN_W 880
#define ED_MIN_H 690
#define ED_PREVIEW_DEF 46
#define ED_SPLIT_HIT DPIX(8)
static int dpi=96, failures, cases;
static struct {int editor_preview_height;} g_settings;
static int DPIX(int x) {return (x*dpi+48)/96;}
static Ctl root, controls[160], extras[3];
static int ncontrols;
static HWND newctl(const char *name, HWND parent) {
    Ctl *c=&controls[ncontrols++]; *c=(Ctl){0}; c->parent=parent;c->name=name;c->visible=1;c->enabled=1;return c;
}
static void GetClientRect(HWND c,RECT *r) {
    *r=(RECT){0,0,c->rect.right-c->rect.left-(c->scrollbar?DPIX(17):0),c->rect.bottom-c->rect.top};
}
static int BeginDeferWindowPos(int n) {(void)n;return 1;}
static void EndDeferWindowPos(int d) {(void)d;}
static int qp_defer(int d,HWND c,int x,int y,int w,int h) {
    if(w<0||h<0){printf("negative size %s %d %d\n",c->name,w,h);++failures;}
    c->rect=(RECT){x,y,x+w,y+h};return d;
}
static void ShowWindow(HWND c,int s){c->visible=s;}
static int EnableWindow(HWND c,int enabled){c->enabled=enabled;return 1;}
static long SendMessageW(HWND c,unsigned msg,unsigned long wp,long lp){
    (void)wp;(void)lp;if(msg==BM_GETCHECK)return c->checked;return 0;
}
static void ListView_SetColumnWidth(HWND c,int n,int w){(void)c;(void)n;(void)w;}
static void SetScrollInfo(HWND c,int bar,SCROLLINFO *s,int redraw){
    (void)bar;(void)redraw;c->si=*s;c->scrollbar=(s->nMax>0 && s->nMax+1>(int)s->nPage);
}
static HWND GetDlgItem(HWND c,int id){(void)c;return &extras[id==IDOK?0:id==IDCANCEL?1:2];}
static void RedrawWindow(HWND c,RECT *r,void *a,int f){(void)c;(void)r;(void)a;(void)f;}
static const Segment *combo_selected_seg(HWND c){(void)c;return NULL;}
void qp_strlcpy(wchar_t *d,const wchar_t *s,size_t n){if(n){wcsncpy(d,s,n-1);d[n-1]=0;}}
'''

helpers = "\n".join(function(n) for n in ["ed_preview_bottom", "ed_splitter_base", "ed_preview_bounds", "ed_preview_height_for", "ed_splitter_y", "ed_metrics"])
layout = function("ed_layout_props") + function("ed_layout") + function("ed_sync_table")
init = "\n".join(f'c.{f}=newctl("{f}", &root);' for f in fields if f != "hwnd")
init += "\n" + "\n".join(f"c.{f}->parent=c.hPropsHost;" for f in props)

driver = r'''
static void check(EditorCtx *c, int table_on) {
    int boundary=ed_splitter_y(root.rect.bottom),i;
    ++cases;
    for(i=0;i<ncontrols;++i) {
        Ctl *a=&controls[i];RECT r=a->rect;Ctl *p;
        if(!a->visible || a==c->hPreview || a==c->hPreviewLbl)continue;
        for(p=a->parent;p&&p!=&root;p=p->parent) {
            RECT v;GetClientRect(p,&v);
            if(r.top<0)r.top=0;if(r.bottom>v.bottom)r.bottom=v.bottom;
            r.top+=p->rect.top;r.bottom+=p->rect.top;
        }
        if(r.bottom>r.top && r.bottom>boundary) {
            if(failures<12)printf("overflow dpi=%d H=%d table=%d %s bottom=%d boundary=%d\n",dpi,root.rect.bottom,table_on,a->name,r.bottom,boundary);
            ++failures;
        }
    }
    if(c->hPropsHost->rect.bottom>boundary)++failures;
    if(c->hPreview->rect.bottom>extras[0].rect.top-DPIX(8))++failures;
    if(!c->hTableOutput->visible || !c->hTableFmt->visible || !c->hPreviewText->visible)++failures;
    if(table_on) {
        if(!c->hTableFmt->enabled || !c->hTableInsert->enabled ||
           !c->hTableInsertBtn->enabled || !c->hPreviewTable->enabled)++failures;
    } else {
        if(c->hTableFmt->enabled || c->hTableInsert->enabled ||
           c->hTableInsertBtn->enabled || c->hPreviewTable->enabled ||
           c->previewMode != OUTPUT_TEXT)++failures;
    }
}
int main(void) {
    int size,setting,table_on,round;
    for(dpi=96;dpi<=144;dpi+=24) {
        EditorCtx c={0};Template t={0};Segment *s=calloc(1,sizeof(*s));
        c.hwnd=&root;c.t=&t;t.segments=s;c.splitLastY=-1;ncontrols=0;
        INIT
        for(size=0;size<2;++size) for(setting=0;setting<2;++setting) for(table_on=0;table_on<2;++table_on) {
            root.rect=(RECT){0,0,DPIX(size?1600:880),DPIX(size?1100:690)};
            g_settings.editor_preview_height=setting?100000:-300;
            c.hTableOutput->checked=table_on?BST_CHECKED:BST_UNCHECKED;
            c.previewMode=OUTPUT_TABLE;
            ed_sync_table(&c);
            for(round=0;round<2;++round) {
                c.propsScroll=round?100000:0;
                ed_layout(&c);check(&c,table_on);
            }
        }
        free(s);
    }
    printf("%d production-layout table geometry cases, %d failures\n",cases,failures);
    return failures?1:0;
}
'''.replace("INIT", init)

code = (preamble + ctx + helpers +
        "\nstatic void ed_update_preview(EditorCtx *c){(void)c;}\n"
        "static void ed_commit_table(EditorCtx *c){(void)c;}\n"
        "static Segment *ed_cur_seg(EditorCtx *c){return c->t->segments;}\n" +
        layout + driver)
with tempfile.TemporaryDirectory(prefix="qp-task3-geometry-") as tmp:
    exe = pathlib.Path(tmp) / "geometry"
    result = subprocess.run(["cc", "-std=gnu11", "-O0", "-g", "-I", str(repo / "src"), "-x", "c", "-", "-o", str(exe)], input=code, text=True)
    if result.returncode:
        sys.exit(result.returncode)
    sys.exit(subprocess.run([str(exe)]).returncode)
