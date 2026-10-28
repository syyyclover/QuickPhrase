"""Validate the XML Windows loads, including manifests embedded in PE files.

Resource scripts can compile successfully while embedding malformed XML. Check
both source manifests and RT_MANIFEST payloads rather than just VERSIONINFO.
"""
from pathlib import Path
import re
import struct
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
ASM = '{urn:schemas-microsoft-com:asm.v1}'
version = re.search(r'^#define QP_FILE_VERSION_STR "([^"]+)"',
                    (ROOT / 'src/platform/version.h').read_text(), re.M).group(1)

def embedded_manifests(path):
    data = path.read_bytes()
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('not a PE executable')
    sections, optional_size = struct.unpack_from('<H', data, pe + 6)[0], struct.unpack_from('<H', data, pe + 20)[0]
    optional = pe + 24
    magic = struct.unpack_from('<H', data, optional)[0]
    directory = optional + (112 if magic == 0x20B else 96)
    resource_rva = struct.unpack_from('<I', data, directory + 16)[0]
    ranges = []
    for i in range(sections):
        at = optional + optional_size + i * 40
        vsize, va, size, offset = struct.unpack_from('<4I', data, at + 8)
        ranges.append((va, max(vsize, size), offset))
    def file_offset(rva):
        for va, size, offset in ranges:
            if va <= rva < va + size:
                return offset + rva - va
        raise ValueError('resource RVA outside image sections')
    base = file_offset(resource_rva)
    def entries(relative):
        at = base + relative
        named, ids = struct.unpack_from('<2H', data, at + 12)
        for i in range(named + ids):
            yield struct.unpack_from('<2I', data, at + 16 + i * 8)
    def payloads(target):
        if target & 0x80000000:
            for _, child in entries(target & 0x7FFFFFFF):
                yield from payloads(child)
        else:
            rva, size = struct.unpack_from('<2I', data, base + target)
            offset = file_offset(rva)
            yield data[offset:offset + size]
    for name, target in entries(0):
        if name == 24:  # RT_MANIFEST
            yield from payloads(target)

def validate(xml, name):
    root = ET.fromstring(xml)
    identity = root.find(ASM + 'assemblyIdentity')
    if root.tag != ASM + 'assembly' or identity is None:
        raise ValueError('missing assembly identity')
    actual = identity.get('version', '')
    if not re.fullmatch(r'\d+\.\d+\.\d+\.\d+', actual) or actual != version:
        raise ValueError(f'invalid/stale assembly version {actual!r}, expected {version!r}')
    print(f'{name}: valid XML, assembly version {actual}')

failures = []
for relative in ('res/app.manifest', 'installer/setup.manifest'):
    try:
        validate((ROOT / relative).read_bytes(), relative)
    except (ET.ParseError, ValueError) as error:
        failures.append(f'{relative}: {error}')

for relative in sys.argv[1:]:
    try:
        payloads = list(embedded_manifests(ROOT / relative))
        if not payloads:
            raise ValueError('executable has no embedded RT_MANIFEST')
        for i, payload in enumerate(payloads):
            validate(payload, f'{relative} RT_MANIFEST[{i}]')
    except (ET.ParseError, ValueError, OSError, struct.error) as error:
        failures.append(f'{relative}: {error}')

for error in failures:
    print('FAIL: ' + error, file=sys.stderr)
raise SystemExit(1 if failures else 0)
