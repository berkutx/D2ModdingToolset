"""Compile actual fixed-window stretch transforms against isolated renderer/Win32 state."""
from pathlib import Path
import datetime, hashlib, json, os, subprocess

root = Path(__file__).resolve().parents[2]
run = root / '.diagnostics' / ('windowstretch-tests-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
run.mkdir(parents=True)
(run / 'tmp').mkdir()
bridge = root / 'c4ddraw/features/rendererbridge.c'
source = bridge.read_text(encoding='utf-8-sig')
start = source.index('static volatile LONG g_window_stretch_percent')
end = source.index('static BOOL dd_reload_config', start)
block = source[start:end]
for name in ('DDApplyWindowStretchViewport', 'DDMapWindowStretchMouse'):
    if name not in block:
        raise RuntimeError('Missing production function: ' + name)
plugin = root / 'c4ddraw/features/pluginhost.cpp'
plugin_source = plugin.read_text(encoding='utf-8-sig')
plugin_start = plugin_source.index('int __cdecl host_get_visible_game_rect(')
plugin_end = plugin_source.index('\nC4P_Host g_host', plugin_start)
plugin_block = plugin_source[plugin_start:plugin_end]
(run / 'windowstretch-extracted.h').write_text(block + '\n' + plugin_block, encoding='utf-8')
(run / 'extraction.json').write_text(json.dumps({
    'file': str(bridge), 'block': 'fixed-window crop, corrected viewport and mouse mapping',
    'sha256': hashlib.sha256(block.encode('utf-8')).hexdigest(),
    'plugin_file': str(plugin), 'plugin_function': 'host_get_visible_game_rect',
    'plugin_sha256': hashlib.sha256(plugin_block.encode('utf-8')).hexdigest()
}, indent=2), encoding='utf-8')
env = {key.upper(): value for key, value in os.environ.items()}
env['TEMP'] = env['TMP'] = str(run / 'tmp')
vswhere = Path(env['PROGRAMFILES(X86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
msbuild = subprocess.check_output([
    str(vswhere), '-latest', '-products', '*', '-requires', 'Microsoft.Component.MSBuild',
    '-find', r'MSBuild\**\Bin\MSBuild.exe'
], env=env, universal_newlines=True).splitlines()[0]
subprocess.check_call([
    msbuild, str(root / 'c4ddraw/tests/windowstretch_tests.vcxproj'), '/t:Build',
    '/p:Configuration=Release', '/p:Platform=Win32', '/p:WorkAreaTestDir=' + str(run),
    '/nologo', '/v:minimal'
], env=env)
completed = subprocess.run([str(run / 'bin/windowstretch_tests.exe')], env=env, universal_newlines=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
(run / 'result.log').write_text(completed.stdout, encoding='utf-8')
print(completed.stdout.strip())
print('Evidence:', run)
completed.check_returncode()
