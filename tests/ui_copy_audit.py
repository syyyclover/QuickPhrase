"""The copy checker must detect missing/stale coverage and wrong button guides."""
from pathlib import Path
import hashlib
import json
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
tool=root/'tools/audit_ui_copy.py'
def entry(anchor,text):
    return {'file':'src/ui/demo.c','anchor':anchor,'kind':'guide','source_hash':hashlib.sha256(text.encode()).hexdigest(),
            'text':text,'previous_text':'','decision':'retain','reason':'fixture','behavior_test':'fixture',
            'layout_check':'fixture only'}
with tempfile.TemporaryDirectory(prefix='qp-copy-audit-') as tmp:
    base=Path(tmp);(base/'src/ui').mkdir(parents=True);(base/'docs').mkdir()
    source=base/'src/ui/demo.c';manifest=base/'docs/ui-copy-review.json'
    source.write_text('void demo(void) { mk_btn(h,L"刷新",1); mk_wrapped_label(h,L"点击「刷新」更新数据。",2); }\n')
    def run():
        return subprocess.run(['python3',str(tool),'--root',str(base),'--check'],capture_output=True,text=True)
    assert run().returncode!=0
    entries=[entry('demo:1','刷新'),entry('demo:2','点击「刷新」更新数据。')]
    manifest.write_text(json.dumps({'schema_version':1,'entries':entries},ensure_ascii=False))
    result=run();assert result.returncode==0,result.stdout+result.stderr
    source.write_text('void demo(void) { mk_btn(h,L"刷新",1); mk_wrapped_label(h,L"点击「不存在」更新数据。",2); }\n')
    assert run().returncode!=0
    entries[1]=entry('demo:2','点击「不存在」更新数据。');manifest.write_text(json.dumps({'schema_version':1,'entries':entries},ensure_ascii=False))
    assert run().returncode!=0
    source.write_text('void demo(void) { /* L"不是界面" */ mk_btn(h,L"刷新",1); mk_wrapped_label(h,L"点击「刷新」" L"更新数据。",2); }\n')
    entries[1]=entry('demo:2','点击「刷新」更新数据。');manifest.write_text(json.dumps({'schema_version':1,'entries':entries},ensure_ascii=False))
    assert run().returncode==0
print('copy audit: missing/stale/new guides, wrong control references, comments and adjacent literals checked')
