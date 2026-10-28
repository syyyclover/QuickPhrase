"""Execute the real settings OK handler; double platform operations, not logic."""
from pathlib import Path
import subprocess
import tempfile
from source_helpers import INCLUDE_FLAGS
root=Path(__file__).resolve().parents[1]
source=(root/'src/ui/dialog_settings.c').read_text()
start=source.index('        if (LOWORD(wp) == IDOK) {')
end=source.index('        if (LOWORD(wp) == IDCANCEL)',start)
body=source[start:end]
apply_path=root/'src/platform/settings_apply.c'
helper=apply_path.read_text().replace('#include "app.h"','') if apply_path.exists() else ''
types=''
header=(root/'src/platform/app.h').read_text()
if 'QpSettingsResult' in header:
    begin=header.index('/* Settings transaction result')
    stop=header.index('/* ----------------------',begin)
    types=header[begin:stop]
preamble=r'''
#include "engine.h"
void settings_defaults(Settings *);
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void *HWND;
typedef struct {unsigned mods,vk;int result;HWND hTheme,hAuto,hSep,hHotkey;} SetCtx;
static Settings g_settings,persisted;
static unsigned registered_vk;
static int auto_enabled,actual_theme,g_dark,fail_save,fail_new_key,fail_old_key,fail_auto,fail_restore_auto,fail_theme,fail_restore_theme,closed;
static char registry_command[32];
#define CB_GETCURSEL 1
#define BM_GETCHECK 2
#define BST_CHECKED 1
#define CB_ERR (-1)
#define MB_OK 1
#define MB_ICONWARNING 2
#define MB_ICONERROR 4
#define IDOK 1
#define LOWORD(v) ((v)&65535)
static long SendMessageW(HWND h,int m,unsigned long w,long l) {(void)h;(void)w;(void)l;return m==CB_GETCURSEL?THEME_DARK:BST_CHECKED;}
static void SetFocus(HWND h) {(void)h;}
static void SetWindowTextW(HWND h,const wchar_t *s) {(void)h;(void)s;}
static void qp_get_text(HWND h,wchar_t *s,size_t n) {(void)h;qp_strlcpy(s,L"NEW_SEP",n);}
static void hotkey_text(unsigned m,unsigned v,wchar_t *s,size_t n) {(void)m;(void)v;qp_strlcpy(s,L"KEY",n);}
static int qp_msgbox(HWND h,unsigned f,const wchar_t *t,const wchar_t *s,...) {(void)h;(void)f;(void)t;(void)s;return 0;}
static void dlg_close(HWND h) {(void)h;closed=1;}
static void app_unregister_hotkey(void) {registered_vk=0;}
static int app_register_hotkey(void) {
    registered_vk=0;
    if((g_settings.hotkey_vk=='X' && fail_new_key)||(g_settings.hotkey_vk=='Q' && fail_old_key)) return 0;
    registered_vk=g_settings.hotkey_vk;return 1;
}
static int app_get_autostart(void) {return auto_enabled;}
static int app_set_autostart(int on) {
    if((on && fail_auto)||(!on && fail_restore_auto)) return 0;
    auto_enabled=on;strcpy(registry_command,on?"OWN_EXE":"ABSENT");return 1;
}
static int app_apply_theme(void) {
    if((g_settings.theme==THEME_DARK && fail_theme)||(g_settings.theme==THEME_LIGHT && fail_restore_theme))return 0;
    actual_theme=g_settings.theme;g_dark=actual_theme==THEME_DARK;return 1;
}
static int settings_save(const Settings *s) {if(fail_save)return 0;persisted=*s;return 1;}
static int app_save_settings(void) {return settings_save(&g_settings);}
'''
fake_snapshot=r'''
int qp_autostart_capture(QpAutostartSnapshot *out) {
    memset(out,0,sizeof(*out));out->enabled=auto_enabled;out->present=1;
    out->size=strlen(registry_command)+1;out->data=malloc(out->size);
    if(!out->data)return 0;
    memcpy(out->data,registry_command,out->size);return 1;
}
int qp_autostart_restore(const QpAutostartSnapshot *old) {
    if(fail_restore_auto)return 0;
    auto_enabled=old->enabled;strcpy(registry_command,(const char *)old->data);return 1;
}
void qp_autostart_dispose(QpAutostartSnapshot *old) {free(old->data);old->data=NULL;}
'''
driver=r'''
static void setup(SetCtx *c) {
    settings_defaults(&g_settings);g_settings.theme=THEME_LIGHT;persisted=g_settings;
    registered_vk='Q';auto_enabled=0;actual_theme=THEME_LIGHT;g_dark=0;closed=0;
    strcpy(registry_command,"ORIGINAL_OTHER_EXE");
    memset(c,0,sizeof(*c));c->mods=2;c->vk='X';
}
static int save_ui(HWND hwnd,SetCtx *c,unsigned wp) {
'''+body+r'''
    return 0;
}
int main(void) {
    SetCtx c;setup(&c);Settings old=g_settings;
    fail_save=1;save_ui(NULL,&c,IDOK);
    assert(!closed && !c.result && !memcmp(&old,&g_settings,sizeof(old)));
    assert(registered_vk=='Q' && !auto_enabled && actual_theme==THEME_LIGHT);
    assert(!strcmp(registry_command,"ORIGINAL_OTHER_EXE"));
    assert(!memcmp(&old,&persisted,sizeof(old)));
    fail_save=0;fail_new_key=1;setup(&c);save_ui(NULL,&c,IDOK);
    assert(!closed && !c.result && registered_vk=='Q' && !auto_enabled);
    fail_new_key=0;fail_auto=1;setup(&c);save_ui(NULL,&c,IDOK);
    assert(!closed && !c.result && registered_vk=='Q' && !auto_enabled && actual_theme==THEME_LIGHT);
    fail_auto=0;setup(&c);save_ui(NULL,&c,IDOK);
    assert(closed && c.result && registered_vk=='X' && auto_enabled && actual_theme==THEME_DARK);
    assert(g_settings.hotkey_vk=='X' && !wcscmp(g_settings.default_separator,L"NEW_SEP"));
    setup(&c);Settings candidate=g_settings;candidate.hotkey_mods=2;candidate.hotkey_vk='X';
    candidate.theme=THEME_DARK;candidate.autostart=1;QpSettingsResult result;
    fail_save=1;fail_old_key=1;
    assert(!qp_settings_commit(&candidate,&result));
    assert((result.failures & QP_SETTINGS_SAVE) && (result.rollback_failures & QP_SETTINGS_HOTKEY));
    assert(!registered_vk && !g_settings.hotkey_vk);
    fail_old_key=0;fail_restore_auto=1;setup(&c);candidate=g_settings;
    candidate.hotkey_mods=2;candidate.hotkey_vk='X';candidate.autostart=1;
    assert(!qp_settings_commit(&candidate,&result));
    assert(result.rollback_failures & QP_SETTINGS_AUTOSTART);
    assert(auto_enabled && g_settings.autostart && !strcmp(registry_command,"OWN_EXE"));
    fail_save=fail_restore_auto=0;fail_theme=1;setup(&c);candidate=g_settings;candidate.theme=THEME_DARK;
    assert(!qp_settings_commit(&candidate,&result) && (result.failures & QP_SETTINGS_THEME));
    assert(actual_theme==THEME_LIGHT && g_settings.theme==THEME_LIGHT);
    fail_theme=0;fail_save=fail_restore_theme=1;setup(&c);candidate=g_settings;candidate.theme=THEME_DARK;
    assert(!qp_settings_commit(&candidate,&result) && (result.rollback_failures & QP_SETTINGS_THEME));
    assert(actual_theme==THEME_DARK && g_settings.theme==THEME_DARK);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='qp-settings-txn-') as tmp:
    exe=Path(tmp)/'probe'
    sources=[str(p) for p in (root/'src/core').glob('*.c')]+[str(root/'third_party/cjson/cJSON.c')]
    subprocess.run(['cc','-std=gnu11','-Wall','-Wno-unused-function',*INCLUDE_FLAGS,'-x','c','-',*sources,
                    '-o',str(exe)],input=preamble+types+fake_snapshot+helper+driver,text=True,check=True)
    subprocess.run([str(exe)],cwd=tmp,check=True)
print('settings: save/key/autostart failures preserve prior state; success commits candidate')
