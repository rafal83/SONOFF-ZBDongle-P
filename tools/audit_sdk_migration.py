"""Compare pinned original TI SDK objects; no flashing or SDK modification."""
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile

from download_ti_toolchain import PACKAGES

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'.tooling/arm-tests'))
from elftools.elf.elffile import ELFFile

def sha(data): return hashlib.sha256(data).hexdigest()

def object_runtime(path):
    elf=ELFFile(io.BytesIO(path.read_bytes()))
    result={}
    for section in elf.iter_sections():
        if section['sh_flags']&2:
            result[section.name]=dict(size=section['sh_size'],alignment=section['sh_addralign'],
                sha256=None if section['sh_type']=='SHT_NOBITS' else sha(section.data()),relocations=[])
    for section in elf.iter_sections():
        if section['sh_type'] not in ('SHT_REL','SHT_RELA'): continue
        target=elf.get_section(section['sh_info']).name
        if target not in result: continue
        linked=elf.get_section(section['sh_link'])
        for relocation in section.iter_relocations():
            symbol=linked.get_symbol(relocation['r_info_sym'])
            name=symbol.name
            if not name and isinstance(symbol['st_shndx'],int): name=elf.get_section(symbol['st_shndx']).name
            result[target]['relocations'].append([relocation['r_offset'],relocation['r_info_type'],name,
                relocation['r_addend'] if relocation.is_RELA() else None])
    return result

def main():
    build=ROOT/'build/sdk-migration-analysis'; build.mkdir(parents=True,exist_ok=True)
    if not build.resolve().is_relative_to(ROOT.resolve()): raise ValueError('Temporary analysis must stay inside the workspace')
    ar=ROOT/'.tooling/compiler/ti-cgt-armllvm_4.0.2.LTS/bin/tiarmar.exe'
    versions=[('8.30.01.01','sdk'),('8.33.00.16','sdk833')]
    snapshots=[]; provenance=[]
    for version,package in versions:
        sdk=ROOT/('vendor-sdk/simplelink_cc13xx_cc26xx_sdk_'+version.replace('.','_'))
        verified=json.loads((sdk/'.extraction-verified.json').read_text())
        if verified['archive_sha256']!=PACKAGES[package]['sha256']: raise ValueError('Wrong SDK provenance')
        library=sdk/'source/ti/zstack/lib/ticlang/m4f/libZStack_nwk_all.a'
        with tempfile.TemporaryDirectory(dir=build) as directory:
            subprocess.run([str(ar),'x',str(library)],cwd=directory,check=True,capture_output=True)
            snapshots.append({p.name:object_runtime(p) for p in Path(directory).glob('*.o')})
        provenance.append(dict(version=version,official_archive_sha256=verified['archive_sha256'],nwk_archive_sha256=sha(library.read_bytes())))
    old,new=snapshots
    changes={}
    for name in sorted(set(old)|set(new)):
        if old.get(name)==new.get(name): continue
        a,b=old.get(name,{}),new.get(name,{})
        changes[name]=[s for s in sorted(set(a)|set(b)) if a.get(s)!=b.get(s)]
    source_changes={}
    paths=['source/ti/zstack/npi/npi_frame_mt.c','source/ti/zstack/npi/npi_task.c',
        'source/ti/zstack/npi/npi_tl.c','source/ti/zstack/npi/npi_tl_uart.c',
        'source/ti/zstack/zmac/f8w/zmac.c','source/ti/zstack/stack/nwk/nwk.h',
        'source/ti/zstack/stack/nwk/nwk_globals.c','source/ti/zstack/stack/zdo/zd_sec_mgr.c',
        'source/ti/common/nv/nvocmp.c','source/ti/drivers/uart2/UART2CC26X2.c','source/ti/drivers/dpl/ClockP.h']
    archives=[ROOT/'.tooling/downloads'/PACKAGES[p]['url'].rsplit('/',1)[1] for _,p in versions]
    with zipfile.ZipFile(archives[0]) as a,zipfile.ZipFile(archives[1]) as b:
        roots=['simplelink_cc13xx_cc26xx_sdk_'+v.replace('.','_') for v,_ in versions]
        for relative in paths:
            before,after=a.read(roots[0]+'/'+relative),b.read(roots[1]+'/'+relative)
            source_changes[relative]=dict(identical=before==after,old_sha256=sha(before),new_sha256=sha(after))
    report=dict(comparison_completed=True,provenance=provenance,objects_old=len(old),objects_new=len(new),
        runtime_object_sections_and_relocation_targets_identical=not changes,changed_objects=changes,
        original_source_comparison=source_changes,audit_script_sha256=sha(Path(__file__).read_bytes()),
        semantics='Allocated section bytes, sizes, alignments and relocation symbol names; debug excluded. Does not prove hardware compatibility.',
        hardware_tested=False,production_deployed=False)
    (ROOT/'build/sdk-migration-comparison.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report))

if __name__=='__main__': main()
