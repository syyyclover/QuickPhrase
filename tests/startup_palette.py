"""Run startup's real palette-to-window gate with failed first/second GDI allocation."""
from pathlib import Path
import runpy,subprocess,tempfile
base=runpy.run_path(str(Path(__file__).with_name('theme_transactions.py')))
s=base['s'];start=s.index('    g_dark = (g_settings.theme == THEME_DARK)',s.index('int WINAPI wWinMain'))
gate=s[start:s.index('    s_accel = ui_create_accelerators();',start)]
extra=r'''
#define QP_APP_TITLE L"Test"
#define MB_OK 1
#define MB_ICONERROR 2
static int classes,windows,errors,s_mutex,g_fontUI,g_fontBold,g_fontMono;
static AppData *g_app;
static void qp_table_set_refs(void *p){(void)p;}
void appdata_free(AppData *p){(void)p;}
static int CloseHandle(int h){(void)h;return 1;}
static int MessageBoxW(void *h,const wchar_t *s,const wchar_t *t,int f){(void)h;(void)s;(void)t;(void)f;++errors;return 0;}
static int ui_register_classes(void){++classes;return 1;}
static int ui_create_main_window(void){++windows;return 1;}
'''
driver=r'''
int main(void){
    for(int i=1;i<=2;++i){
        allocations=classes=windows=errors=0;fail_at=i;g_brushBk=g_brushCtl=0;
        g_settings.theme=THEME_LIGHT;
        assert(startup_gate()==1 && !classes && !windows && errors==1);
        assert(!g_brushBk && !g_brushCtl);
    }
    allocations=classes=windows=errors=fail_at=0;
    assert(!startup_gate() && classes==1 && windows==1 && g_brushBk && g_brushCtl);
    puts("startup: failed initial palette refuses window creation; complete palette starts");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='qp-start-palette-') as tmp:
    exe=Path(tmp)/'test';code=base['pre']+extra+base['function']('create_brushes')+'\nstatic int startup_gate(void){\n'+gate+'return 0;\n}\n'+driver
    subprocess.run(['cc','-std=gnu11','-Wall','-Wno-unused-function','-Wno-unused-variable',*base['INCLUDE_FLAGS'],'-x','c','-','-o',str(exe)],input=code,text=True,check=True)
    subprocess.run([str(exe)],check=True)
