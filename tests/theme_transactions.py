"""Actual theme application must preserve old GDI objects if replacement fails."""
from pathlib import Path
import re
import subprocess
import tempfile
from source_helpers import INCLUDE_FLAGS
root=Path(__file__).resolve().parents[1]
s=(root/'src/platform/main.c').read_text()
def function(name):
    m=re.search(r'^(?:static )?(?:void|int) '+name+r'\([^;]+?\)\n\{',s,re.M)
    return s[m.start():s.index('\n}',m.end())+2]
pre=r'''
#include "engine.h"
#include <assert.h>
#include <stdio.h>
typedef unsigned long HBRUSH;
static HBRUSH g_brushBk,g_brushCtl;
static int g_dark,g_hwndMain,allocations,fail_at,old_deleted,new_deleted;
static Settings g_settings;
static int theme_bk(void) {return g_dark?1:0;}
static int theme_ctl_bk(void) {return g_dark?3:2;}
static int app_system_dark(void) {return 1;}
static void ui_theme_changed(void) {}
static HBRUSH CreateSolidBrush(int color) {(void)color;++allocations;return allocations==fail_at?0:(HBRUSH)allocations+10;}
static int DeleteObject(HBRUSH b) {if(b==1||b==2)++old_deleted;else if(b)++new_deleted;return 1;}
'''
driver=r'''
int main(void) {
    for(int i=1;i<=2;++i) {
        allocations=old_deleted=new_deleted=0;fail_at=i;g_dark=0;
        g_brushBk=1;g_brushCtl=2;g_settings.theme=THEME_DARK;
        app_apply_theme();
        assert(!g_dark && g_brushBk==1 && g_brushCtl==2 && !old_deleted);
        assert(new_deleted==1);
    }
    allocations=old_deleted=new_deleted=fail_at=0;app_apply_theme();
    assert(g_dark && g_brushBk>2 && g_brushCtl>2 && old_deleted==2);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='qp-theme-txn-') as tmp:
    exe=Path(tmp)/'probe'
    subprocess.run(['cc','-std=gnu11','-Wall',*INCLUDE_FLAGS,'-x','c','-','-o',str(exe)],
                   input=pre+function('create_brushes')+function('app_apply_theme')+driver,text=True,check=True)
    subprocess.run([str(exe)],check=True)
print('theme: failed brush replacements preserve palette and old GDI objects')
