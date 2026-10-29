"""Fault-sweep actual duplicate/close actions, including pending-session cleanup."""
import runpy,re,subprocess,tempfile
from pathlib import Path
base=runpy.run_path(str(Path(__file__).with_name('session_transactions.py')))
source=base['sessions']
def fn(name):
    m=re.search(r'^(?:static )?[^\n]+\b'+name+r'\([^;]+?\)\n\{',source,re.M)
    assert m,name
    return source[m.start():source.index('\n}',m.end())+2]+'\n'
extra=r'''
#define QP_SESS_MAX 9
#define QP_APP_TITLE L"Test"
#define MB_ICONINFORMATION 4
#define MB_ICONWARNING 8
#define MB_YESNO 16
#define IDYES 6
static Session **g_sess;
static int g_sessCount;
static int choice_mode;
static void popup_close(void) {}
static void ui_focus_first_field(void) {}
'''
state=base['state'].replace('static void sess_discard_pending(Session *s) {(void)s;}','')
state=state.replace('(void)h;(void)f;(void)t;(void)s;return 0;', '(void)h;(void)f;(void)t;(void)s;return 6;')
functions=''.join(fn(n) for n in ('sess_free','sess_visible_count','sess_visible_at','sess_has_input',
    'sess_default_name','sess_pick_color','sess_create','sess_discard_pending','sess_rebind',
    'sess_room_check','sess_duplicate','sess_close'))
driver=r'''
static void prepare(void) {
    original_template=tmpl_create(L"T");
    for(int i=0;i<2;++i)seg_list_append(&original_template->segments,seg_create(SEG_TEXT));
    original_session=sess_create(original_template->id);target_session=sess_create(original_template->id);
    assert(original_session && target_session);
    g_ctx=eval_create(original_template);assert(g_ctx);g_curSess=original_session;g_curTmpl=original_template;
    original_session->used=1;target_session->used=2;
    eval_set_user_value(g_ctx,0,L"LATEST");eval_set_user_value(g_ctx,1,L"");assert(qp_eval_run_checked(g_ctx));
    if(choice_mode) {
        Segment *s=original_template->segments;s->type=SEG_MULTI_SELECT;
        const wchar_t *opts[]={L"LATEST",L"Other"};assert(qp_seg_set_options(s,opts,2));s->custom_option=2;
        g_ctx->fields[0].choice_tracked=1;
        wcscpy(g_ctx->fields[0].custom_value,L"draft / LATEST");
        wcscpy(g_ctx->fields[0].choice_base,L"LATEST");
    }
    clears=builds=0;
}
static void cleanup(void) {
    eval_destroy(g_ctx);g_ctx=NULL;g_curSess=NULL;
    for(int i=0;i<g_sessCount;++i)sess_free(g_sess[i]);
    free(g_sess);g_sess=NULL;g_sessCount=0;
    tmpl_free(original_template);assert(!fa_live);
}
static void action(int close) {if(close)sess_close(0);else sess_duplicate(0);}
int main(int argc,char **argv) {
    int close=argc>1 && !strcmp(argv[1],"close");
    choice_mode=argc>2;
    prepare();fa_inject(0);action(close);size_t count=fa_calls;fa_disable();
    assert(g_sessCount==(close?1:3));
    if(choice_mode && !close)assert(g_ctx->fields[0].choice_tracked && !wcscmp(g_ctx->fields[0].custom_value,L"draft / LATEST"));
    cleanup();
    for(size_t point=1;point<=count;++point) {
        prepare();EvalContext *live=g_ctx;fa_inject(point);action(close);fa_disable();assert(fa_hit);
        assert(g_ctx==live && g_curSess==original_session && g_sessCount==2 && !clears && !builds);
        assert(!wcscmp(g_ctx->fields[0].user_value,L"LATEST") && g_ctx->fields[1].user_touched);
        cleanup();
    }
    printf("session action %s: %zu faults preserve active input and clean pending candidates\n",close?"close":"duplicate",count);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='qp-session-actions-') as tmp:
    exe=Path(tmp)/'test'
    code=base['preamble']+base['ctx']+extra+state+fn('sess_drop_values')+base['snapshot']+functions+driver
    subprocess.run(['cc','-std=gnu11','-Wall','-Wno-unused-function',*base['INCLUDE_FLAGS'],'-I',str(base['root']/'tests'),
                    '-x','c','-',*base['CORE_SOURCES'],str(base['root']/'src/core/table.c'),str(base['root']/'src/core/storage.c'),
                    str(base['root']/'src/core/regex.c'),str(base['root']/'third_party/cjson/cJSON.c'),
                    '-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free','-o',str(exe)],input=code,text=True,check=True)
    for mode in ('duplicate','close'):
        subprocess.run([str(exe),mode],check=True)
        subprocess.run([str(exe),mode,'custom-choice'],check=True)
