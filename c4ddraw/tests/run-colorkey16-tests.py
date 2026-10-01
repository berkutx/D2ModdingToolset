"""Differential Win32 tests for the maintained SIMD helper; optional synthetic/captured microbenchmarks.
Captured game assets remain external: pass --sprite and --queue for a local GRID sample.
"""
from pathlib import Path
import argparse,datetime,hashlib,json,os,struct,subprocess
parser=argparse.ArgumentParser()
parser.add_argument('--benchmark',action='store_true')
parser.add_argument('--sprite',type=Path)
parser.add_argument('--queue',type=Path)
parser.add_argument('--handle',type=lambda text:int(text,0),default=0x114590c0)
args=parser.parse_args()
if bool(args.sprite)!=bool(args.queue):parser.error('--sprite and --queue must be provided together')
root=Path(__file__).resolve().parents[2]
run=root/'.diagnostics'/('colorkey16-tests-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
run.mkdir(parents=True);(run/'tmp').mkdir()
upstream=root/'c4ddraw/upstream/cnc-ddraw/src/blt.c'
source=upstream.read_text(encoding='utf-8-sig');start=source.index('void blt_colorkey(');end=source.index('\nvoid blt_colorkey_mirror_stretch(',start);block=source[start:end]
(run/'scalar-extracted.h').write_text(block.replace('void blt_colorkey(', '__declspec(noinline) void blt_colorkey_scalar(',1),encoding='utf-8')
helper=root/'c4ddraw/features/colorkey16.h'
evidence={'source':str(upstream),'function':'blt_colorkey','scalar_sha256':hashlib.sha256(block.encode('utf-8')).hexdigest(),'helper':str(helper),'helper_sha256':hashlib.sha256(helper.read_bytes()).hexdigest(),'scalar_change':'Only function name and noinline marker added; body verbatim.'}
rectfile=None
if args.sprite:
    sprite=args.sprite.resolve();queue=args.queue.resolve();blob=queue.read_bytes()
    if len(sprite.read_bytes())!=4096 or len(blob)%64:raise RuntimeError('Expected captured62x32/128byte-pitch GRID sprite and64byte queue records')
    records=[];matched=0
    for pos in range(0,len(blob),64):
        fields=struct.unpack_from('<16i',blob,pos)
        if fields[3]&0xffffffff!=args.handle:continue
        matched+=1
        sx,sy=fields[5:7];dx,dy=fields[7:9];w,h=fields[9:11];cl,ct,cr,cb=fields[11:15]
        left=max(dx,cl,0);top=max(dy,ct,0);right=min(dx+w,cr,1600);bottom=min(dy+h,cb,900)
        if right<=left or bottom<=top:continue
        sx+=left-dx;sy+=top-dy;w=right-left;h=bottom-top
        if sx<0 or sy<0 or sx+w>62 or sy+h>32:raise RuntimeError('Captured rectangle exceeds sprite')
        records.append((left,top,w,h,sx,sy))
    rectfile=run/'capture-rects.txt';rectfile.write_text(''.join(' '.join(str(v) for v in row)+'\n' for row in records),encoding='ascii')
    evidence['capture']={'sprite':str(sprite),'sprite_sha256':hashlib.sha256(sprite.read_bytes()).hexdigest(),'queue':str(queue),'queue_sha256':hashlib.sha256(blob).hexdigest(),'handle':hex(args.handle),'matched_records':matched,'visible_records':len(records),'note':'Replays only colorkey pixel placement; does not emulate native alpha blend or all queue work.'}
(run/'extraction.json').write_text(json.dumps(evidence,indent=2),encoding='utf-8')
env={key.upper():value for key,value in os.environ.items()};env['TEMP']=env['TMP']=str(run/'tmp')
vswhere=Path(env['PROGRAMFILES(X86)'])/'Microsoft Visual Studio/Installer/vswhere.exe'
msbuild=subprocess.check_output([str(vswhere),'-latest','-products','*','-requires','Microsoft.Component.MSBuild','-find',r'MSBuild\**\Bin\MSBuild.exe'],env=env,universal_newlines=True).splitlines()[0]
subprocess.check_call([msbuild,str(root/'c4ddraw/tests/colorkey16_tests.vcxproj'),'/t:Build','/p:Configuration=Release','/p:Platform=Win32','/p:WorkAreaTestDir='+str(run),'/nologo','/v:minimal'],env=env)
command=[str(run/'bin/colorkey16_tests.exe')]
if args.benchmark:command.append('--benchmark')
if args.sprite:
    if not args.benchmark:command.append('--capture-check')
    command.extend([str(sprite),str(rectfile)])
completed=subprocess.run(command,env=env,universal_newlines=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
(run/'results.txt').write_text(completed.stdout,encoding='utf-8');print(completed.stdout);print('Evidence:',run)
completed.check_returncode()
