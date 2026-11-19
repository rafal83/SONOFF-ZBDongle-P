"""Build/test independent core algorithms; never build/flash a coordinator.

Host models and an isolated Cortex-M4F test ELF have a separate evidence file.
The production 20261117 image and qualification manifest are never modified.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from build import compiler

ROOT=Path(__file__).resolve().parents[1]
SOURCES=['firmware/src/core_'+n+'.c' for n in ['index','indirect','broadcast','security']]

def execute(command):
    try:
        return subprocess.check_output([str(x) for x in command],cwd=ROOT,text=True,stderr=subprocess.STDOUT).strip()
    except subprocess.CalledProcessError as exc:
        print(exc.output,flush=True)
        raise

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--arm',action='store_true')
    ap.add_argument('--publish-report',type=Path)
    args=ap.parse_args()
    build=ROOT/'build/core-engines'; build.mkdir(parents=True,exist_ok=True)
    os.environ.setdefault('ZIG_GLOBAL_CACHE_DIR',str(ROOT/'.cache/zig'))
    cc=compiler(); results={}
    for arch in ['host','x86'] if os.name=='nt' else ['host']:
        for name in ['index','indirect','broadcast']:
            output=build/('test-'+name+'-'+arch+('.exe' if os.name=='nt' else ''))
            files=['firmware/src/core_index.c']
            if name!='index': files.append('firmware/src/core_'+name+'.c')
            flags=['-target','x86-windows-gnu'] if arch=='x86' else []
            execute(cc+flags+['-std=c11','-O2','-g','-UNDEBUG','-Wall','-Wextra','-Werror',
                '-Ifirmware/src','tests/test_core_'+name+'.c']+files+['-o',output])
            result=execute([output]); results[name+'_'+arch]=result; print(result,flush=True)
    shared=build/('security.dll' if os.name=='nt' else 'security.so')
    execute(cc+['-std=c11','-O2','-g','-Wall','-Wextra','-Werror','-shared','-fPIC',
        '-Ifirmware/src','tests/core_security_ffi.c','firmware/src/core_security.c','firmware/src/core_index.c','-o',shared])
    from test_core_security import run
    results['security']=run(shared)
    if args.arm:
        compiler_dir=ROOT/'.tooling/compiler/ti-cgt-armllvm_4.0.2.LTS/bin'
        ti=compiler_dir/'tiarmclang.exe'
        flags=['-mcpu=cortex-m4','-mthumb','-mfloat-abi=hard','-mfpu=fpv4-sp-d16',
            '-std=c11','-Oz','-g','-ffreestanding','-fno-builtin','-fno-common','-ffunction-sections','-fdata-sections',
            '-fstack-usage','-Wall','-Wextra','-Werror','-Ifirmware/src']
        objects=[]
        for source in SOURCES+['tests/core_arm_runtime.c']:
            obj=build/(Path(source).stem+'.obj')
            execute([ti]+flags+['-c',source,'-o',obj]); objects.append(obj)
        elf=build/'core-test-only.elf'
        response=build/'linker.rsp'
        # TI linker command files require quoted filenames with spaces.
        runtime=compiler_dir.parent/'lib/armv7em-ti-none-eabihf/libclang_rt.builtins.a'
        response.write_text('\n'.join('"'+str(p)+'"' for p in objects+[runtime,ROOT/'tests/core_arm_test.cmd'])+
            '\n--entry_point=core_test_entry\n--unused_section_elimination=off\n--disable_auto_rts\n'+
            '--output_file="'+str(elf)+'"\n',encoding='utf-8')
        execute([ti,'-mcpu=cortex-m4','-mfloat-abi=hard','-mfpu=fpv4-sp-d16','-nostdlib','-Wl,'+str(response)])
        from test_arm_core_engines import run as arm_run
        results['arm']=arm_run(elf)
    inputs=SOURCES+['firmware/src/core_'+n+'.h' for n in ['index','indirect','broadcast','security']]
    inputs += ['tests/test_core_'+n+'.c' for n in ['index','indirect','broadcast']]
    inputs += ['tests/core_security_ffi.c','tests/core_arm_runtime.c','tests/core_arm_test.cmd',
        'tools/test_core_engines.py','tools/test_core_security.py','tools/test_arm_core_engines.py',
        'tools/core-tests-requirements.txt']
    report=dict(passed=True,production_integration=False,hardware_tested=False,
        final_objectives_completed=False,tests=results,
        input_sha256={n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in inputs})
    (build/'report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    if args.publish_report:
        destination=args.publish_report.resolve()
        if destination.parent!=ROOT/'docs' or destination.suffix!='.json': raise ValueError('Public report must be in docs/')
        destination.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report['tests']['security']),flush=True)

if __name__=='__main__': main()
