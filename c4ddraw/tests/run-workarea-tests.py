"""Compile real work-area/viewport/mode functions with isolated Win32/render stubs."""
from pathlib import Path
import argparse, datetime, hashlib, json, os, re, subprocess
p=argparse.ArgumentParser()
p.add_argument("build_directory", type=Path)
a=p.parse_args()
root=Path(__file__).resolve().parents[2]
run=root/".diagnostics"/("workarea-tests-"+datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
run.mkdir(parents=True)
(run/"tmp").mkdir()
bridge=root/"c4ddraw/features/rendererbridge.c"
dd=a.build_directory/"src/dd.c"
parts=[]; evidence=[]
for source,names in [(bridge,["DDGetDisplayMode","DDToggleWorkAreaWindow","DDLeaveWorkAreaForManualMove","DDPrepareDisplayModeChange","DDToggleWindowedMode"]),(dd,["dd_CalcViewport"])]:
    text=source.read_text(encoding="utf-8-sig")
    for name in names:
        match=re.search(r"^(?:int|void|BOOL)\s+"+name+r"\([^;]*?\)\s*\{",text,re.M)
        if not match: raise RuntimeError("Missing definition: "+name)
        start=match.start(); pos=match.end(); depth=1
        while depth:
            if text[pos]=="{": depth+=1
            elif text[pos]=="}": depth-=1
            pos+=1
        body=text[start:pos]
        parts.append(body)
        evidence.append({"file":str(source),"function":name,"sha256":hashlib.sha256(body.encode()).hexdigest()})
(run/"workarea-extracted.h").write_text("\n\n".join(parts),encoding="utf-8")
(run/"extraction.json").write_text(json.dumps(evidence,indent=2),encoding="utf-8")
env={k.upper():v for k,v in os.environ.items()}
env["TEMP"]=env["TMP"]=str(run/"tmp")
vswhere=Path(env["PROGRAMFILES(X86)"])/"Microsoft Visual Studio/Installer/vswhere.exe"
msbuild=subprocess.check_output([str(vswhere),"-latest","-products","*","-requires","Microsoft.Component.MSBuild","-find",r"MSBuild\**\Bin\MSBuild.exe"],env=env,universal_newlines=True).splitlines()[0]
args=[msbuild,str(root/"c4ddraw/tests/workarea_tests.vcxproj"),"/t:Build","/p:Configuration=Release","/p:Platform=Win32","/p:WorkAreaTestDir="+str(run),"/nologo","/v:minimal"]
subprocess.check_call(args,env=env)
result=subprocess.check_output([str(run/"bin/workarea_tests.exe")],env=env,universal_newlines=True)
(run/"result.log").write_text(result,encoding="utf-8")
print(result.strip())
print("Evidence:",run)
