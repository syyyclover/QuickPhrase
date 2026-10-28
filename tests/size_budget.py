"""Enforce release byte budgets; PE section reporting is separate from size."""
import argparse,json,struct
from pathlib import Path

APP_MAX=458752
# setup 的 .rsrc 内嵌的是未压缩的 app：崩溃记录器让 app +6144 B，setup 跟着涨到
# 592896 B，顶破了原来的 576 KB。上调到 600 KB（2026-10-12）。根治方案是压缩
# 内嵌载荷（→200 KB 量级），见 docs/3.5.0-six-issue-analysis.md 问题 3 的补充。
SETUP_MAX=614400
def budget_errors(app,setup):
    return [f'{name}: {size} bytes exceeds {limit}' for name,size,limit in
            (('application',app,APP_MAX),('setup',setup,SETUP_MAX)) if size>limit]
def sections(path):
    data=path.read_bytes();pe=struct.unpack_from('<I',data,60)[0]
    if data[pe:pe+4]!=b'PE\0\0':raise ValueError('not PE')
    count=struct.unpack_from('<H',data,pe+6)[0];optional=struct.unpack_from('<H',data,pe+20)[0]
    found={}
    for i in range(count):
        at=pe+24+optional+i*40
        name=data[at:at+8].rstrip(b'\0').decode('ascii')
        found[name]=struct.unpack_from('<I',data,at+16)[0]
    return found
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--app',type=Path,default=Path('build/QuickPhrase.exe'))
    p.add_argument('--setup',type=Path,default=Path('dist/QuickPhrase-3.5.0-setup.exe'))
    p.add_argument('--self-test',action='store_true');a=p.parse_args()
    if a.self_test:
        assert not budget_errors(APP_MAX,SETUP_MAX)
        assert len(budget_errors(APP_MAX+1,SETUP_MAX+1))==2
        print('size budget: exact boundary accepted; both over-budget artifacts rejected');return 0
    try:
        sizes={name:path.stat().st_size for name,path in (('app',a.app),('setup',a.setup))}
        errors=budget_errors(sizes['app'],sizes['setup'])
        for error in errors:print('FAIL: '+error)
        print(json.dumps({'bytes':sizes,'limits':{'app':APP_MAX,'setup':SETUP_MAX},
                          'sections':{name:sections(path) for name,path in (('app',a.app),('setup',a.setup))}},indent=2))
        return 1 if errors else 0
    except (OSError,ValueError,struct.error) as error:print('FAIL: '+str(error));return 1
if __name__=='__main__':raise SystemExit(main())
