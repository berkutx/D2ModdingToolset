"""Build Win32 tests of the maintained RGB565 helper.

Default CI uses an independent per-channel model and needs only Python/MSBuild.
Optional --native-exe verifies/extracts known self-contained native kernels into a
local diagnostic directory; pefile and capstone are required only for that mode.
No game bytes are included in the repository or needed by CI.
"""
from pathlib import Path
import argparse,datetime,hashlib,json,os,subprocess

parser=argparse.ArgumentParser()
parser.add_argument('--native-exe',type=Path,help='Local supported Discipl2.exe for exact native comparison')
args=parser.parse_args()
root=Path(__file__).resolve().parents[2]
run=root/'.diagnostics'/('blend565-tests-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
run.mkdir(parents=True);(run/'tmp').mkdir()
helper=root/'c4ddraw/features/blend565.h'
evidence={'helper':str(helper),'helper_sha256':hashlib.sha256(helper.read_bytes()).hexdigest(),'oracle':'independent scalar model','native_bytes_in_repository':False}
if args.native_exe:
    import pefile,capstone
    from capstone.x86 import X86_OP_IMM,X86_OP_MEM
    exe=args.native_exe.resolve();pe=pefile.PE(str(exe))
    if pe.FILE_HEADER.Machine!=0x14c or pe.OPTIONAL_HEADER.Magic!=0x10b or pe.OPTIONAL_HEADER.ImageBase!=0x400000:
        raise RuntimeError('Native oracle requires the supported PE32/x86 image layout')
    functions=[
        ('alpha',0x67A41B,0x67A557,36,'4611529407add8e52d4f60a4480936a88542795feaa0aa58342db5c607c70c8a'),
        ('add',0x67A557,0x67A654,28,'e2264e53899e25682936a27eb48d4ddd5b455003fdcb75406190bbddc40928bd'),
        ('sub',0x67A654,0x67A735,28,'54440934811e4d892cd47fd0546cba04cb6be1226f376f4bfc1a954a417f3ec6')]
    md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32);md.detail=True
    chunks=[];records=[]
    for name,start,end,args_bytes,expected_sha in functions:
        blob=pe.get_data(start-pe.OPTIONAL_HEADER.ImageBase,end-start)
        digest=hashlib.sha256(blob).hexdigest()
        if digest!=expected_sha:raise RuntimeError('Unsupported native kernel bytes: '+name)
        instructions=list(md.disasm(blob,start))
        if sum(i.size for i in instructions)!=len(blob):raise RuntimeError('Native decode gap: '+name)
        for instruction in instructions:
            if instruction.group(capstone.CS_GRP_CALL):raise RuntimeError('Native external call: '+name)
            if instruction.group(capstone.CS_GRP_JUMP):
                operands=instruction.operands
                if not operands or operands[0].type!=X86_OP_IMM or not start<=operands[0].imm<end:raise RuntimeError('Native external/indirect jump: '+name)
            for operand in instruction.operands:
                if operand.type==X86_OP_MEM and not operand.mem.base and not operand.mem.index:raise RuntimeError('Native absolute/global access: '+name)
        if instructions[-1].mnemonic!='ret' or instructions[-1].operands[0].imm!=args_bytes:raise RuntimeError('Native calling convention mismatch: '+name)
        chunks.append('static const unsigned char native_%s_bytes[]={%s};'%(name,','.join('0x%02x'%value for value in blob)))
        records.append({'name':name,'start':hex(start),'end_exclusive':hex(end),'sha256':digest,'instructions':len(instructions),'stack_bytes':args_bytes})
    header=exe.read_bytes()[:pe.OPTIONAL_HEADER.SizeOfHeaders]
    type_entry=next(entry for entry in pe.DIRECTORY_ENTRY_RESOURCE.entries if getattr(entry,'id',None)==16)
    name_entry=next(entry for entry in type_entry.directory.entries if getattr(entry,'id',None)==1)
    version_data=name_entry.directory.entries[0].data.struct
    version=pe.get_data(version_data.OffsetToData,version_data.Size)
    for name,blob in [('pe_header',header),('version',version)]:
        chunks.append('static const unsigned char native_%s_bytes[]={%s};'%(name,','.join('0x%02x'%value for value in blob)))
    evidence['native_header_sha256']=hashlib.sha256(header).hexdigest()
    evidence['native_version_resource_sha256']=hashlib.sha256(version).hexdigest()
    (run/'blend565-native-bytes.h').write_text('\n'.join(chunks),encoding='ascii')
    evidence.update(oracle='independent scalar model and verified original native machine code',native_exe=str(exe),native_exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),native_functions=records)
install_helper=root/'c4ddraw/features/blend565_install.h'
evidence['install_helper_sha256']=hashlib.sha256(install_helper.read_bytes()).hexdigest()
(run/'extraction.json').write_text(json.dumps(evidence,indent=2),encoding='utf-8')
env={key.upper():value for key,value in os.environ.items()};env['TEMP']=env['TMP']=str(run/'tmp')
vswhere=Path(env['PROGRAMFILES(X86)'])/'Microsoft Visual Studio/Installer/vswhere.exe'
msbuild=subprocess.check_output([str(vswhere),'-latest','-products','*','-requires','Microsoft.Component.MSBuild','-find',r'MSBuild\**\Bin\MSBuild.exe'],env=env,universal_newlines=True).splitlines()[0]
command=[msbuild,str(root/'c4ddraw/tests/blend565_tests.vcxproj'),'/t:Build','/p:Configuration=Release','/p:Platform=Win32','/p:WorkAreaTestDir='+str(run),'/nologo','/v:minimal']
if args.native_exe:command.append('/p:NativeOracle=true')
subprocess.check_call(command,env=env)
completed=subprocess.run([str(run/'bin/blend565_tests.exe')],env=env,universal_newlines=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
(run/'results.txt').write_text(completed.stdout,encoding='utf-8');print(completed.stdout);print('Evidence:',run)
completed.check_returncode()
