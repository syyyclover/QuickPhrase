"""Shared source lookup for legacy extracted-code Win32 contract tests.

These tests execute production helpers with a small OS double; they are not
native Win32 integration tests. Keep lookup independent of old flat paths.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INCLUDE_FLAGS = [flag for p in ('src/core', 'src/ui', 'src/platform', 'third_party/cjson')
                 for flag in ('-I', str(ROOT / p))]
CORE_NAMES = ('strings', 'model', 'eval', 'render', 'summary', 'lint', 'seed', 'unicode', 'table_key', 'choices')
CORE_SOURCES = [str(ROOT / 'src/core' / (name + '.c')) for name in CORE_NAMES]

def read_source(name):
    if name == 'engine.c':
        return '\n'.join(Path(p).read_text() for p in CORE_SOURCES)
    if name == 'ui_dialogs.c':
        context = ROOT / 'src/ui/editor_internal.h'
        files = ('dialog_common', 'dialog_rules', 'dialog_editor', 'dialog_settings',
                 'dialog_tables', 'dialog_lookup')
        text = (context.read_text() + '\n') if context.exists() else ''
        layout = ROOT / 'src/ui/editor_layout.c'
        if layout.exists(): text += layout.read_text() + '\n'
        return text + '\n'.join((ROOT / 'src/ui' / (p + '.c')).read_text() for p in files)
    if name == 'ui.c':
        files = ('state', 'common', 'popup', 'host', 'sessions', 'fields', 'tree',
                 'actions', 'layout', 'window')
        return (ROOT / 'src/ui/ui_internal.h').read_text() + '\n' + '\n'.join(
            (ROOT / 'src/ui' / ('main_' + p + '.c')).read_text() for p in files)
    for folder in ('src/core', 'src/ui', 'src/platform', 'third_party/cjson'):
        p = ROOT / folder / name
        if p.exists(): return p.read_text()
    raise FileNotFoundError(name)
