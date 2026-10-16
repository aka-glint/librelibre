"""Inspect actual ARM object/table bytes, imports and compiler stack estimates."""
from pathlib import Path
import hashlib
import json
import os
import re
import shlex
import struct
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
# The global ufbt toolchain path (~/.ufbt) is broken (I/O error); use
# the project-local toolchain that the build actually uses. Override with
# ARM_TOOLCHAIN_PREFIX if needed.
_default=ROOT/'.uid-toolchain/toolchain/x86_64-linux/bin/arm-none-eabi-'
PREFIX=os.environ.get('ARM_TOOLCHAIN_PREFIX', str(_default))
SOURCE_SHA='ed44d885bf2af099c1d1fc3cc6baa60d51e1b1c4dd9d9d4e1ea584bf00387e94'
source=(ROOT/'glucose-research/reference/diable-Glucose.swift').read_bytes()
assert hashlib.sha256(source).hexdigest()==SOURCE_SHA
report={'source_sha256':SOURCE_SHA,'clinical_validation':False,'device_execution':False,'tables':{},'stack_bytes':{}}
with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR', None)) as tmp:
    tmp=Path(tmp)
    for name in ['t1','t2']:
        out=tmp/name
        # factory.c now exposes the tables under LC_FACTORY_DIABLE_* so the
        # recovered chain shares the single copy; the section is named after the
        # symbol.
        section='.rodata.LC_FACTORY_DIABLE_'+name.upper()
        subprocess.run([PREFIX+'objcopy','--dump-section',f'{section}={out}',str(ROOT/'.ufbt-unleashed/build/libre_capture/factory.o')],check=True)
        expected=struct.pack('<1023d',*[float(v) for v in re.search(r'let '+name+r' = \[(.*?)\]',source.decode(),re.S)[1].split(',') if v.strip()])
        actual=out.read_bytes();assert actual==expected
        report['tables'][name]={'entries':1023,'bytes':len(actual),'bit_exact_ieee_double':True,'sha256':hashlib.sha256(actual).hexdigest()}
    commands=json.loads((ROOT/'.vscode/compile_commands.json').read_text())
    for entry in commands:
        if Path(entry['file']).name not in ['app.c','factory.c','integrity.c','capture.c','protocol.c']:continue
        args=shlex.split(entry['command']);assert '-mcpu=cortex-m4' in args
        out=tmp/(Path(entry['file']).stem+'.o');args[args.index('-o')+1]=str(out)
        if Path(entry['file']).name=='app.c':
            probe=tmp/'app_probe.c'
            probe.write_text((ROOT/'app.c').read_text()+'\nconst unsigned lc_build_resource_sizes[]={sizeof(AppState),sizeof(LcCapture),sizeof(LcFactory),LC_JSON_CAPACITY};\n')
            args[args.index(entry['file'])]=str(probe)
        subprocess.run(args+['-fstack-usage'],cwd=entry['directory'],check=True)
        if Path(entry['file']).name=='app.c':
            sizes=tmp/'sizes.bin'
            subprocess.run([PREFIX+'objcopy','--dump-section',f'.rodata.lc_build_resource_sizes={sizes}',str(out)],check=True)
            app,capture,factory,json_capacity=struct.unpack('<4I',sizes.read_bytes())
            report['arm_sizes']={'AppState_heap_bytes':app,'LcCapture_bytes':capture,'LcFactory_bytes':factory,'each_json_buffer_heap_bytes':json_capacity,'json_buffers_count':2,'manifest_app_stack_bytes':4096,'runtime_heap_available':'not measured; no device access'}
        for line in out.with_suffix('.su').read_text().splitlines():
            location,size,kind=line.split('\t');name=location.rsplit(':',1)[1]
            report['stack_bytes'][name]={'bytes':int(size),'classification':kind}
    undefined=subprocess.check_output([PREFIX+'nm','-u',str(ROOT/'dist/debug/libre_capture_d.elf')],text=True)
    imports=[line.split()[-1] for line in undefined.splitlines()]
    symbols=subprocess.check_output([PREFIX+'nm','-S',str(ROOT/'dist/debug/libre_capture_d.elf')],text=True)
    # Double libm routines and aeabi helpers must be present, not unresolved.
    for name in ['log','pow','floor','__aeabi_ddiv','__aeabi_dmul']:
        assert re.search(r'\b[Tt] '+re.escape(name)+r'$',symbols,re.M),name
        assert name not in imports
    report['math']={'double_libm_statically_linked':['log','pow','floor'],'double_aeabi_helpers_linked':True,'float_only_fallback':False}
    sdk=list((ROOT/'.ufbt-unleashed/current').rglob('api_symbols.csv'))
    assert len(sdk)==1
    csv=sdk[0].read_text();assert all(re.search(r',\+,.*\b'+re.escape(name)+r'\b',csv) for name in imports if name!='uxTopUsedPriority')
    report['sdk_api_sha256']=hashlib.sha256(sdk[0].read_bytes()).hexdigest()
    report['imports_checked_against_sdk']=imports
    report['elf_sections']=subprocess.check_output([PREFIX+'size','-A',str(ROOT/'dist/debug/libre_capture_d.elf')],text=True)
    fap=(ROOT/'dist/libre_capture.fap').read_bytes()
    report['fap']={'bytes':len(fap),'sha256':hashlib.sha256(fap).hexdigest(),'md5':hashlib.md5(fap).hexdigest()}
print(json.dumps(report,indent=2))
