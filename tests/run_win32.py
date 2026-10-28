"""Run Win32 integration checks in a disposable Wine profile and X display."""
from pathlib import Path
import os
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='qp-wine-350-') as tmp:
    env=os.environ.copy()
    env.update(WINEPREFIX=str(Path(tmp)/'profile'), WINEDEBUG='-all', WINEARCH='win64')
    try:
        result=subprocess.run(['xvfb-run','-a','-s','-screen 0 3840x2160x24','wine64',str(root/'build/test_win32.exe'),
                               '--artifacts',str(root/'build/win32-artifacts')],
                              cwd=tmp,env=env,timeout=600)
    finally:
        subprocess.run(['wineserver','-k'],env=env,check=False)
        subprocess.run(['wineserver','-w'],env=env,timeout=30,check=False)
    if result.returncode: raise SystemExit(result.returncode)
