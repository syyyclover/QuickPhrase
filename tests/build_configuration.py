"""Build isolation contracts; dry-run checks are not binary or Windows tests."""
from pathlib import Path
import re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='qp-build-config-') as tmp:
    build=Path(tmp)/'build';dist=Path(tmp)/'dist'
    def commands(core,ui,lto):
        return subprocess.run(['make','-n','setup',f'BUILD={build}',f'DIST={dist}',f'CORE_OPT={core}',
                               f'UI_OPT={ui}',f'LTO={lto}'],cwd=ROOT,capture_output=True,text=True,check=True).stdout
    a=commands('-O2','-Os',0);b=commands('-Os','-Os',0);c=commands('-Os','-Os',1)
    assert 'QP_EMBED_APP' in a and str(build/'QuickPhrase.exe') in a,'installer must embed selected build'
    assert 'CJSON_HIDE_SYMBOLS' in a,'disable accidental DLL exports'
    def object_dirs(text):return set(re.findall(re.escape(str(build))+r'/app/([^/\s]+)/',text))
    assert object_dirs(a) and object_dirs(a).isdisjoint(object_dirs(b)),'core optimization must have a distinct object fingerprint'
    assert object_dirs(b).isdisjoint(object_dirs(c)),'LTO must have a distinct object fingerprint'
    assert str(dist/'QuickPhrase-3.5.0-setup.exe') in a
    make=(ROOT/'Makefile').read_text()
    assert re.search(r'app/.*%\.o:\s*%\.c.*\$\(APP_HDR\)',make),'header/version changes must rebuild objects'
    assert 'ACTIVE_CONFIG' in make and 'cmp -s' in make,'switching back must relink, not just reuse older matching objects'
    assert ".tmp.'$$$$" in make,'configuration-stamp temporaries must not collide between checks/builds'
    assert 'resource.rc res/app.manifest res/app.ico' in make
    assert 'installer/setup.rc installer/setup.manifest res/setup.ico' in make
print('build configuration: selected embed paths, object fingerprints and resource/header dependencies checked')
