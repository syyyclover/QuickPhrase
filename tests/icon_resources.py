"""Verify every EXE icon payload, not merely its size list or window icon.

With a setup EXE argument, also verify the embedded application/uninstaller are
byte-identical to the current build. No execution or Windows shell is simulated.
"""
from pathlib import Path
import argparse
import struct
import sys

ROOT=Path(__file__).resolve().parents[1]
SIZES={(n,n) for n in (16,20,24,32,40,48,64,128,256)}

def resources(path):
    data=path.read_bytes(); pe=struct.unpack_from('<I',data,60)[0]
    if data[pe:pe+4]!=b'PE\0\0': raise ValueError('not a PE executable')
    optional=pe+24
    count=struct.unpack_from('<H',data,pe+6)[0]
    optional_size=struct.unpack_from('<H',data,pe+20)[0]
    directory=optional+(112 if struct.unpack_from('<H',data,optional)[0]==0x20B else 96)
    rva=struct.unpack_from('<I',data,directory+16)[0]
    sections=[]
    for i in range(count):
        vsize,va,size,offset=struct.unpack_from('<4I',data,optional+optional_size+i*40+8)
        sections.append((va,max(vsize,size),offset))
    def offset(rva):
        for va,size,at in sections:
            if va<=rva<va+size: return at+rva-va
        raise ValueError('resource points outside PE sections')
    base=offset(rva); found={}
    def walk(at,keys):
        named,ids=struct.unpack_from('<2H',data,base+at+12)
        for i in range(named+ids):
            name,target=struct.unpack_from('<2I',data,base+at+16+i*8)
            if target&0x80000000: walk(target&0x7FFFFFFF,keys+(name,))
            else:
                rva,size=struct.unpack_from('<2I',data,base+target)
                found[keys+(name,)]=data[offset(rva):offset(rva)+size]
    walk(0,()); return found

def ico_frames(path):
    data=path.read_bytes(); reserved,kind,count=struct.unpack_from('<3H',data)
    if reserved or kind!=1: raise ValueError('not an ICO')
    frames={}
    for i in range(count):
        width,height,_,_,_,_,size,offset=struct.unpack_from('<4B2H2I',data,6+i*16)
        frames[(width or 256,height or 256)]=data[offset:offset+size]
    if set(frames)!=SIZES: raise ValueError('missing ICO sizes')
    return frames

def validate(path,embedded_app=None,embedded_uninstall=None):
    setup='setup' in path.name.lower()
    expected=ico_frames(ROOT/('res/setup.ico' if setup or 'uninstall' in path.name.lower() else 'res/app.ico'))
    values=resources(path); groups=0
    for key,group in values.items():
        if key[0]!=14: continue
        sizes=set(); count=struct.unpack_from('<H',group,4)[0]
        for i in range(count):
            width,height,_,_,_,_,size,resource_id=struct.unpack_from('<4B2HIH',group,6+i*14)
            dimensions=(width or 256,height or 256); sizes.add(dimensions)
            payload=values.get((3,resource_id,key[2]))
            if payload is None: payload=next((v for k,v in values.items() if k[0]==3 and k[1]==resource_id),None)
            if payload!=expected.get(dimensions) or payload is None or len(payload)!=size:
                raise ValueError(f'icon group {key[1]}, size {dimensions}: payload differs from new artwork')
        if sizes!=SIZES: raise ValueError('incomplete EXE icon group')
        groups+=1
    if groups!=1: raise ValueError(f'EXE must have one icon group, found {groups}')
    if setup:
        for resource_id,source in ((1000,embedded_app or ROOT/'build/QuickPhrase.exe'),
                                   (1001,embedded_uninstall or ROOT/'build/uninstall.exe')):
            payload=next((v for k,v in values.items() if k[0]==10 and k[1]==resource_id),None)
            if payload!=source.read_bytes(): raise ValueError(f'installer embeds stale {source}')
    print(f'{path.name}: {groups} icon groups, all 9 payloads match new artwork'+('; embedded binaries match build' if setup else ''))

try:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('files',nargs='*',type=Path)
    parser.add_argument('--embedded-app',type=Path)
    parser.add_argument('--embedded-uninstall',type=Path)
    args=parser.parse_args()
    for relative in ('res/app.ico','res/setup.ico'):
        ico_frames(ROOT/relative); print(relative+': all 9 icon frames present')
    for relative in args.files: validate(ROOT/relative,args.embedded_app,args.embedded_uninstall)
except (ValueError,OSError,struct.error) as error:
    print('FAIL: '+str(error),file=sys.stderr); raise SystemExit(1)
