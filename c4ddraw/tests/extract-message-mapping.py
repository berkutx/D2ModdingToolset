"""Extract unchanged mapping functions from a selected generated build for tests."""
import argparse
import hashlib
import json
import pathlib
import re

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('build_source', type=pathlib.Path)
p.add_argument('output', type=pathlib.Path)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
chunks = []
evidence = []
for filename, names in (
        ('winapi_hooks.c', ('HandleMessage', 'fake_PeekMessageA')),
        ('rendererbridge.c', ('DDMessageBatchPeekRaw', 'DDMessageBatchMapRemoved'))):
    path = a.build_source / filename
    raw = path.read_bytes()
    source = raw.decode('utf-8-sig')
    for name in names:
        # Generated functions use a top-level closing brace at column zero.
        # Do not silently fall back to an upstream or hand-copied function.
        pattern = re.compile(r'(?ms)^(?:void|BOOL(?: WINAPI)?)\s+' + re.escape(name) +
                             r'\([^;{}]*\)\s*\{.*?^\}')
        matches = list(pattern.finditer(source))
        if len(matches) != 1:
            raise SystemExit('Expected one complete definition: ' + str(path) + ':' + name)
        match = matches[0]
        body = match.group()
        evidence.append(dict(file=str(path.resolve()), source_sha256=hashlib.sha256(raw).hexdigest(),
                             function=name, line=source.count('\n', 0, match.start())+1,
                             function_sha256=hashlib.sha256(body.encode('utf-8')).hexdigest()))
        chunks.append(body)
header = a.output / 'message-mapping-extracted.h'
header.write_bytes(('// Generated mechanically; do not edit. See extraction.json.\n' +
                    '\n\n'.join(chunks) + '\n').encode('utf-8'))
(a.output / 'extraction.json').write_text(json.dumps(evidence, indent=2), encoding='utf-8')
print('Extracted', len(chunks), 'unchanged functions into', header)
