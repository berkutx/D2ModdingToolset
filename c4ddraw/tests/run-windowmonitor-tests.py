"""Compile actual monitor, Auto, maximize and recommendation code against isolated Win32 stubs."""
from pathlib import Path
import datetime,hashlib,json,os,re,subprocess
root=Path(__file__).resolve().parents[2]
run=root/'.diagnostics'/('windowmonitor-tests-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
run.mkdir(parents=True); (run/'tmp').mkdir()
parts=[]; evidence=[]
def function(path,name):
    text=path.read_text(encoding='utf-8-sig')
    match=re.search(r'^(?:static )?(?:int|void|BOOL|bool|double)\s+'+name+r'\([^;]*?\)\s*\{',text,re.M)
    if not match: raise RuntimeError('Missing '+name)
    start=match.start(); end=match.end(); depth=1
    while depth:
        if text[end]=='{':depth+=1
        elif text[end]=='}':depth-=1
        end+=1
    body=text[start:end]
    evidence.append(dict(file=str(path),function=name,sha256=hashlib.sha256(body.encode()).hexdigest()))
    return body
bridge=root/'c4ddraw/features/rendererbridge.c'; text=bridge.read_text(encoding='utf-8-sig')
start=text.index('typedef struct DDMonitorSearch')
end=text.index('/* The caption maximize button',start)
parts.append(text[start:end])
evidence.append(dict(file=str(bridge),function='monitor/native-maximize block',sha256=hashlib.sha256(parts[-1].encode()).hexdigest()))
parts.append(function(bridge,'DDCalcWindowStretchCrop'))
hor=root/'c4ddraw/features/horplus.cpp'; text=hor.read_text(encoding='utf-8-sig')
start=text.index('struct CanvasPreset'); end=text.index('struct RequestedCanvas',start)
parts.append(text[start:end])
parts.append(function(hor,'adaptiveCanvasForOutput'))
menu=root/'c4ddraw/features/featuremenu.cpp'
for name in ['fitScale','predictViewport','adaptiveResolutionNeedsRestart','resolutionReducesVisibleScaling']:
    parts.append(function(menu,name))
(run/'windowmonitor-extracted.h').write_text('\n\n'.join(parts),encoding='utf-8')
(run/'extraction.json').write_text(json.dumps(evidence,indent=2),encoding='utf-8')
env={k.upper():v for k,v in os.environ.items()};env['TEMP']=env['TMP']=str(run/'tmp')
vswhere=Path(env['PROGRAMFILES(X86)'])/'Microsoft Visual Studio/Installer/vswhere.exe'
ms=subprocess.check_output([str(vswhere),'-latest','-products','*','-requires','Microsoft.Component.MSBuild','-find',r'MSBuild\**\Bin\MSBuild.exe'],env=env,universal_newlines=True).splitlines()[0]
subprocess.check_call([ms,str(root/'c4ddraw/tests/windowmonitor_tests.vcxproj'),'/t:Build','/p:Configuration=Release','/p:Platform=Win32','/p:WorkAreaTestDir='+str(run),'/nologo','/v:minimal'],env=env)
result=subprocess.check_output([str(run/'bin/windowmonitor_tests.exe')],env=env,universal_newlines=True)
(run/'result.log').write_text(result,encoding='utf-8')
print(result.strip()); print('Evidence:',run)
