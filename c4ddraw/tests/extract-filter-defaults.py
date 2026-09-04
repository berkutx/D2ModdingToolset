"""Extract actual INI lookup functions and patched cfg_load filter expressions for a small C test."""
import argparse
import hashlib
import json
import pathlib
import re

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('workspace', type=pathlib.Path)
p.add_argument('run', type=pathlib.Path)
a = p.parse_args()
upstream = a.workspace / 'c4ddraw/upstream/cnc-ddraw'
patched = a.run / 'src/config.c'
evidence, chunks = [], []

def extract(path, name):
    raw = path.read_bytes()
    text = raw.decode('utf-8-sig')
    pattern = re.compile(r'(?ms)^(?:static )?(?:void|BOOL|DWORD|int|unsigned long)\s+' +
                         re.escape(name) + r'\([^;{}]*\)\s*\{.*?^\}')
    matches = list(pattern.finditer(text))
    if len(matches) != 1:
        raise SystemExit('Expected exactly one definition: ' + str(path) + ':' + name)
    match = matches[0]
    evidence.append(dict(file=str(path.resolve()), function=name,
                         line=text.count('\n', 0, match.start()) + 1,
                         source_sha256=hashlib.sha256(raw).hexdigest(),
                         function_sha256=hashlib.sha256(match.group().encode()).hexdigest()))
    return match.group()

chunks.append((upstream / 'inc/ini.h').read_text())
chunks.append(extract(upstream / 'src/crc32.c', 'Crc32_ComputeBuf'))
chunks.append('#define BUF_SIZE (8192)')
for name in ('ini_create', 'ini_get_string', 'ini_free'):
    chunks.append(extract(upstream / 'src/ini.c', name))
chunks.append('static struct { INIFILE ini; char game_section[MAX_PATH]; '
              'char shader[MAX_PATH]; int d3d9_filter; } g_config;')
for name in ('cfg_get_string', 'cfg_get_int'):
    chunks.append(extract(patched, name))
config = patched.read_text()
shader = re.findall(r'^\s*GET_STRING\("shader",[^\n]+', config, re.M)
portable = re.findall(r'^\s*GET_INT\(g_config\.d3d9_filter,[^\n]+', config, re.M)
if len(shader) != 1 or len(portable) != 1:
    raise SystemExit('Filter cfg_load expressions are ambiguous')
if 'lanczos2-sharp.glsl' not in shader[0] or 'FILTER_LANCZOS' not in portable[0]:
    raise SystemExit('Patched runtime defaults are not Lanczos')
menu = (a.workspace / 'c4ddraw/features/featuremenu.cpp').read_text(encoding='utf-8-sig')
for expected in ('int g_d3dFilter = 3;', 'readDdrawStr("shader", kShaders[0].value, sh,',
                 'g_d3dFilter = readDdrawInt("d3d9_filter", 3);'):
    if expected not in menu:
        raise SystemExit('Menu fallback mismatch: ' + expected)
release = (a.workspace / 'c4ddraw/release/ddraw.ini').read_text(encoding='utf-8-sig')
for expected in ('shader=Shaders\\interpolation\\lanczos2-sharp.glsl', 'd3d9_filter=3'):
    if expected not in release:
        raise SystemExit('Packaged defaults mismatch: ' + expected)
chunks += ['#define FILTER_LANCZOS 3',
           '#define GET_STRING(a,b,c,d) cfg_get_string(a,b,c,d)',
           '#define GET_INT(a,b,c) a = cfg_get_int(b,c)',
           'static void load_filter_defaults(void) {\n' + shader[0] + '\n' + portable[0] + '\n}']
(a.run / 'filter-defaults-extracted.h').write_text('\n\n'.join(chunks) + '\n', encoding='utf-8')
(a.run / 'extraction.json').write_text(json.dumps(evidence, indent=2), encoding='utf-8')
print('PASS: patched cfg_load, packaged defaults and menu fallbacks agree; actual parser extracted')
