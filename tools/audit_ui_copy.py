#!/usr/bin/env python3
"""Inventory product copy; check every visible CJK literal has current review.

No files are written. --inventory emits JSON for human review and apply_patch.
The checker covers strings/anchors, not the truth of all prose or native layout.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re

TOKEN=re.compile(r'/\*.*?\*/|//[^\n]*|(?:L|u8|u|U)?"(?:\\.|[^"\\])*"|(?:L|u|U)?\'(?:\\.|[^\'\\])*\'|[A-Za-z_]\w*|[^\s]',re.S)
FUNCTION=re.compile(r'^[ \t]*(?:(?:static|const|inline|CALLBACK|WINAPI)\s+|[A-Za-z_]\w*[ \t*]+)+([A-Za-z_]\w*)\s*\([^;]*?\)\s*\{',re.M)
CJK=re.compile('[\u3400-\u9fff]')
DECISIONS={'replace','retain','product_name','business_content'}

def unescape(token):
    content=token[token.index('"')+1:-1]
    escapes={'n':'\n','r':'\r','t':'\t','a':'\a','b':'\b','f':'\f','v':'\v','\\':'\\','"':'"',"'":"'",'?':'?'}
    def decode(m):
        value=m.group(1)
        if value in escapes:return escapes[value]
        if value=='\n':return ''
        if value[0] in 'xuU':return chr(int(value[1:],16))
        return chr(int(value,8))
    return re.sub(r'\\(x[0-9A-Fa-f]+|u[0-9A-Fa-f]{4}|U[0-9A-Fa-f]{8}|[0-7]{1,3}|[abfnrtv\\"\'?]|\n)',decode,content)

def scope(root):
    paths=set()
    for folder in ('src/ui','src/platform','installer'):
        paths.update((root/folder).glob('*.c'));paths.update((root/folder).glob('*.h'))
    for name in ('summary.c','lint.c','table.c','seed.c'):
        p=root/'src/core'/name
        if p.exists():paths.add(p)
    if(root/'resource.rc').exists():paths.add(root/'resource.rc')
    return sorted(paths)

def inventory(root):
    entries=[]
    for path in scope(root):
        source=path.read_text(encoding='utf-8')
        tokens=[m for m in TOKEN.finditer(source) if not m.group().startswith(('/*','//'))]
        functions=[]
        # Function bounds are found with lexical braces, never braces inside strings.
        for match in FUNCTION.finditer(source):
            start=source.index('{',match.start(),match.end())
            depth=0;end=len(source)
            for t in tokens:
                if t.start()<start:continue
                if t.group()=='{':depth+=1
                elif t.group()=='}':
                    depth-=1
                    if not depth:end=t.end();break
            functions.append((match.start(),end,match.group(1)))
        counts={};index=0
        while index<len(tokens):
            token=tokens[index]
            if '"' not in token.group() or not re.match(r'^(?:L|u8|u|U)?"',token.group()):index+=1;continue
            text=unescape(token.group());last=index
            while last+1<len(tokens) and re.match(r'^(?:L|u8|u|U)?"',tokens[last+1].group()):
                last+=1;text+=unescape(tokens[last].group())
            index=last+1
            if not CJK.search(text):continue
            function='global'
            for begin,end,name in functions:
                if begin<=token.start()<end:function=name;break
            counts[function]=counts.get(function,0)+1
            prefix=source[max(0,token.start()-160):token.start()]
            kind='control' if re.search(r'(?:mk_btn|mk_check|mk_radio|mk_label(?:_r)?|mk_wrapped_label)\s*\([^;]*$',prefix,re.S) else 'guide'
            file=path.relative_to(root).as_posix()
            if file=='src/core/seed.c':kind='business'
            entries.append({'file':file,'anchor':f'{function}:{counts[function]}','line':source.count('\n',0,token.start())+1,
                            'kind':kind,'source_hash':hashlib.sha256(text.encode('utf-8')).hexdigest(),'text':text})
    return entries

def caption(text):
    return re.sub(r'\(&.\)','',text).replace('&','').replace('...','…').strip('：: ')

def check(root,manifest):
    current=inventory(root);errors=[]
    try:review=json.loads(manifest.read_text(encoding='utf-8'))
    except (OSError,ValueError) as error:return [f'copy review unavailable: {error}']
    stored={}
    for entry in review.get('entries',[]):
        key=(entry.get('file'),entry.get('anchor'))
        if key in stored:errors.append(f'duplicate review: {key}')
        stored[key]=entry
    captions={caption(e['text']) for e in current if e['kind']=='control' or (len(e['text'])<40 and not re.search('[。\n]',e['text']))}
    captions.update({'确定','取消','是','否','重试'})
    live=set()
    for entry in current:
        key=(entry['file'],entry['anchor']);live.add(key);old=stored.get(key)
        if not old:errors.append(f'unreviewed copy: {key}');continue
        if old.get('source_hash')!=entry['source_hash'] or old.get('text')!=entry['text']:errors.append(f'stale copy review: {key}')
        if old.get('decision') not in DECISIONS or not all(old.get(x) for x in ('reason','behavior_test','layout_check')):
            errors.append(f'incomplete review: {key}')
        if old.get('decision')=='business_content':continue
        text=entry['text']
        if '按名称引用' in text or '认人' in text or '不到 1 MB' in text:errors.append(f'inaccurate/colloquial guide: {key}')
        if 'Alt+Q' in text and not re.search('默认|示例',text):errors.append(f'fixed configurable hotkey: {key}')
        for ref in re.findall(r'点击[^。\n「]{0,8}「([^」]+)」',text):
            if caption(ref) not in captions:errors.append(f'unknown clickable control {ref}: {key}')
    for key in stored.keys()-live:errors.append(f'orphaned review: {key}')
    if not errors:print(f'copy review: {len(current)} current product/business literals covered; native semantics/layout remain separate')
    return errors

def main():
    args=argparse.ArgumentParser(description=__doc__)
    args.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[1])
    args.add_argument('--inventory',action='store_true');args.add_argument('--check',action='store_true')
    args.add_argument('--manifest',type=Path)
    opt=args.parse_args();root=opt.root.resolve()
    if opt.inventory:
        print(json.dumps({'schema_version':1,'entries':inventory(root)},ensure_ascii=False,indent=2));return 0
    errors=check(root,opt.manifest or root/'docs/ui-copy-review.json')
    for error in errors:print(error)
    return 1 if errors else 0
if __name__=='__main__':raise SystemExit(main())
