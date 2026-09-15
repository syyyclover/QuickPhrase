"""Exercise extracted editor combo insertion and dialog-group helper behavior.

This is source-executed evidence for model-backed combo refresh and the Win32
WS_GROUP contract. It does not claim native Windows message dispatch.
"""
import pathlib
import re
import subprocess
import sys
import tempfile

repo = pathlib.Path(__file__).resolve().parents[1]
source = (repo / "src/ui_dialogs.c").read_text()

def function(name, required=True):
    match = re.search(r"static [^\n]+\b" + name + r"\([^;]+?\)\n\{", source)
    if not match:
        if required:
            print("RED: missing production helper " + name)
            sys.exit(1)
        return ""
    return source[match.start():source.index("\n}", match.end()) + 2] + "\n"

end = source.index("} EditorCtx;") + len("} EditorCtx;")
ctx = source[source.rfind("typedef struct {", 0, end):end]

combo_preamble = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "engine.h"
#include "resource.h"
typedef unsigned long WPARAM;
typedef long LPARAM;
typedef struct Ctl {
    int count, sel;
    wchar_t label[8][QP_LABEL_MAX];
    const Segment *data[8];
    wchar_t text[QP_OUTPUT_MAX];
} Ctl;
typedef Ctl *HWND;
#define CB_ERR (-1)
#define CB_RESETCONTENT 0x014B
#define CB_ADDSTRING 0x0143
#define CB_SETITEMDATA 0x0151
#define CB_SETCURSEL 0x014E
#define CB_GETCURSEL 0x0147
#define CB_GETITEMDATA 0x0150
#define EM_REPLACESEL 0x00C2
#define TRUE 1
#define _snwprintf swprintf
static int failures;
static long SendMessageW(HWND h, unsigned msg, WPARAM wp, LPARAM lp) {
    int i;
    switch (msg) {
    case CB_RESETCONTENT: h->count=0;h->sel=CB_ERR;return 0;
    case CB_ADDSTRING:
        i=h->count++; wcsncpy(h->label[i], (const wchar_t *)lp, QP_LABEL_MAX-1);
        h->label[i][QP_LABEL_MAX-1]=0; return i;
    case CB_SETITEMDATA: h->data[wp]=(const Segment *)lp;return 0;
    case CB_SETCURSEL: h->sel=(int)wp;return h->sel;
    case CB_GETCURSEL: return h->sel;
    case CB_GETITEMDATA: return (long)h->data[wp];
    case EM_REPLACESEL:
        wcsncat(h->text, (const wchar_t *)lp, QP_OUTPUT_MAX-wcslen(h->text)-1);return 0;
    default: return 0;
    }
}
static void SetFocus(HWND h) {(void)h;}
'''

combo_driver = r'''
static void require(int yes, const char *what) {
    if (!yes) { printf("FAIL: %s\n", what); ++failures; }
}
int main(void) {
    EditorCtx c={0}; Template t={0}; Segment first={0}, second={0};
    Ctl combo={.sel=CB_ERR}, format={0};
    first.type=SEG_TEXT; second.type=SEG_TEXT; first.next=&second;
    wcscpy(first.label, L"变量1"); wcscpy(second.label, L"数量");
    t.segments=&first; c.t=&t; c.hTableInsert=&combo; c.hTableFmt=&format;
    ed_fill_table_combo(&c);
    combo.sel=1; wcscpy(second.label, L"金额");
    ed_fill_table_combo(&c);
    require(combo.sel==1 && combo.data[combo.sel]==&second,
            "renamed selected variable remains selected by Segment pointer");
    require(wcscmp(combo.label[1], L"金额")==0,
            "renamed variable display label refreshes immediately");
    ed_insert_table_variable(&c);
    require(wcscmp(format.text, L"{金额}")==0,
            "insert immediately after rename uses current braced label");
    second.label[0]=0; ed_fill_table_combo(&c);
    require(combo.count==1 && combo.data[0]==&first && combo.sel==0,
            "clearing selected label removes it and selects remaining variable");
    wcscpy(second.label, L"金额"); ed_fill_table_combo(&c);
    require(combo.count==2 && combo.data[combo.sel]==&first,
            "repopulating a label restores it without losing current selection");
    if (!failures) printf("combo rename/clear/repopulate interaction checks passed\n");
    return failures?1:0;
}
'''

combo_code = (combo_preamble + ctx +
              "\nstatic void ed_table_format_changed(EditorCtx *c) {(void)c;}\n" +
              function("combo_selected_seg") +
              function("ed_fill_table_combo") + function("ed_insert_table_variable") + combo_driver)
with tempfile.TemporaryDirectory(prefix="qp-task3-interaction-") as tmp:
    exe = pathlib.Path(tmp) / "combo"
    result = subprocess.run(["cc", "-std=gnu11", "-O0", "-I", str(repo / "src"), "-x", "c", "-", "-o", str(exe)], input=combo_code, text=True)
    if result.returncode:
        sys.exit(result.returncode)
    result = subprocess.run([str(exe)])
    if result.returncode:
        sys.exit(result.returncode)

radio = function("mk_radio", required=False)
next_group = function("mk_combo_group", required=False)
if not radio or not next_group:
    print("RED: preview radio group is not terminated by a grouped next control")
    sys.exit(1)

group_preamble = r'''
#include <stdio.h>
#include <wchar.h>
typedef unsigned long DWORD;
typedef struct Ctl {DWORD style;} Ctl;
typedef Ctl *HWND;
#define BS_AUTORADIOBUTTON 0x00000009L
#define CBS_DROPDOWNLIST 0x0003L
#define WS_VSCROLL 0x00200000L
#define WS_TABSTOP 0x00010000L
#define WS_GROUP 0x00020000L
static Ctl made[3]; static int nmade;
static int DPIX(int x) { return x; }
static HWND qp_create(const wchar_t *cls,const wchar_t *text,DWORD style,DWORD ex,
                      int x,int y,int w,int h,HWND parent,int id) {
    (void)cls;(void)text;(void)ex;(void)x;(void)y;(void)w;(void)h;(void)parent;(void)id;
    made[nmade].style=style; return &made[nmade++];
}
'''
group_driver = r'''
int main(void) {
    Ctl parent={0};
    mk_radio(&parent, L"文本预览", 1, 1);
    mk_radio(&parent, L"表格预览", 2, 0);
    mk_combo_group(&parent, 3);
    if (!(made[0].style&WS_GROUP) || !(made[0].style&WS_TABSTOP) ||
        (made[1].style&WS_GROUP) || !(made[1].style&WS_TABSTOP) ||
        !(made[2].style&WS_GROUP) || !(made[2].style&WS_TABSTOP)) {
        puts("FAIL: preview radio pair must be one tab-stop group terminated by segment type");
        return 1;
    }
    puts("preview radio group interaction checks passed");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="qp-task3-group-") as tmp:
    exe = pathlib.Path(tmp) / "group"
    result = subprocess.run(["cc", "-std=gnu11", "-O0", "-x", "c", "-", "-o", str(exe)], input=group_preamble + radio + next_group + group_driver, text=True)
    if result.returncode:
        sys.exit(result.returncode)
    sys.exit(subprocess.run([str(exe)]).returncode)
