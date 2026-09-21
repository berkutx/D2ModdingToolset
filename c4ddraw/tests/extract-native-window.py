"""Apply the maintained patch chain and extract the actual native resize policy."""
from pathlib import Path
import re
import shutil
import subprocess
import sys

root, run = map(Path, sys.argv[1:])
fixture = run / 'patched'
shutil.copytree(root / 'c4ddraw/upstream/cnc-ddraw', fixture,
                ignore=shutil.ignore_patterns('.git'))
subprocess.run(['git', '-C', str(fixture), 'init', '-q'], check=True)
build = (root / 'c4ddraw/build.ps1').read_text(encoding='utf-8-sig')
paths = dict(re.findall(r'\$(patch\w*) = Join-Path \$root "([^"]+)"', build))
order = re.findall(r'& git -c core\.autocrlf=false apply[^\n]+"\$(patch\w*)"', build)
assert order[-1] == 'patchNativeWindow'
for key in order:
    subprocess.run(['git', '-C', str(fixture), '-c', 'core.autocrlf=false',
                    'apply'] + (['--recount'] if key == 'patchEventTrace' else []) + ['--ignore-whitespace',
                    str(root / 'c4ddraw' / Path(paths[key].replace('\\', '/')))], check=True)
source = (fixture / 'src/dd.c').read_text()
window = (fixture / 'src/wndproc.c').read_text()

def function(text, name):
    start = text.index(name)
    brace = text.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

resize = function(source, 'void dd_ResizeWindowOutput(')
mode = function(source, 'HRESULT dd_SetDisplayMode(')
predicate = mode[mode.index('    RECT native_client'):mode.index('    InterlockedExchange')]
assert 'BOOL keep_native_window' in predicate
assert 'if (!keep_native_window && (!zooming || g_config.fullscreen))' in mode
assert '~(keep_native_window ? 0 : WS_MAXIMIZE)' in mode
assert re.search(r'if \(!keep_native_window\)\s*\{\s*real_SetWindowPos', mode)
assert 'util_toggle_maximize();' not in window
assert 'hack: disable aero snap' not in window
assert window.count('dd_ResizeWindowOutput(') == 2
assert '(wParam & 0xFFF0) == SC_RESTORE' in window
(run / 'native_window_extracted.h').write_text(resize + '\n\n' +
    'BOOL extracted_keep_native(DWORD dwFlags)\n{\n' + predicate +
    '    return keep_native_window;\n}\n')
print('PASS: complete maintained patch chain; native routing, geometry guard and canvas clamp gates')
