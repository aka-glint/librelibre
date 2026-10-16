"""Portable historical formula fidelity, not clinical validation. No sensor I/O."""
import ctypes as C
import importlib.util
import math
import os
from pathlib import Path
import random
import subprocess
import tempfile
import unittest
import sys
ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'offline'),str(ROOT/'tests')]
from libre2_offline import load_capture,validate_capture,decrypt_fram
from test_sensor_status import synthetic,seal,PATCH,buffer

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
oracle=module('factory_oracle',ROOT/'glucose-research/factory_reference.py')
parser=module('factory_parser',ROOT/'glucose-research/raw_parser.py')
class Coeff(C.Structure):
    _fields_=[('i1',C.c_int),('i2',C.c_int),('i3',C.c_int),('i4',C.c_int),('i5',C.c_int),('i6',C.c_int)]
class Record(C.Structure):
    _fields_=[('raw_adc',C.c_int),('raw_temperature',C.c_int),('temperature_adjustment',C.c_int),('sensor_minute',C.c_int),('quality_bits',C.c_int),('quality_flags',C.c_int),('has_error',C.c_bool)]
class Estimate(C.Structure):
    _fields_=[('temperature_c',C.c_double),('unrounded_mg_dl',C.c_double),('mg_dl',C.c_uint32),('mmol_tenths',C.c_uint32),('status',C.c_int)]
COEFF=dict(i1=1,i2=500,i3=-100,i4=1000,i5=1000,i6=8000)
RECORD=dict(raw_adc=1500,raw_temperature=8000,temperature_adjustment=0,sensor_minute=100,quality_bits=0,quality_flags=0,has_error=False)
class Factory(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        source=ROOT/'factory.c'
        if not source.exists():
            cls.lib=None;return
        so=Path(cls.tmp.name)/'factory.so'
        subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-pedantic','-fPIC','-shared','-fsingle-precision-constant',str(source),str(ROOT/'integrity.c'),str(ROOT/'capture.c'),str(ROOT/'protocol.c'),'-lm','-o',str(so)],check=True)
        cls.lib=C.CDLL(str(so))
        cls.lib.lc_factory_calculate.argtypes=[C.POINTER(Record),C.POINTER(Coeff),C.POINTER(Estimate)]
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def calculate(self,record=RECORD,coeff=COEFF):
        self.assertIsNotNone(self.lib,'missing portable historical factory implementation')
        result=Estimate();self.lib.lc_factory_calculate(C.byref(Record(**record)),C.byref(Coeff(**coeff)),C.byref(result));return result
    def test_all_1023_indexes_exact_historical_formula(self):
        for i in range(1,1024):
            coeff=dict(COEFF,i2=i);wanted=oracle.candidate(RECORD,coeff);got=self.calculate(RECORD,coeff)
            self.assertEqual(got.status,1)
            self.assertAlmostEqual(got.temperature_c,wanted['temperature_c'],places=10)
            self.assertAlmostEqual(got.unrounded_mg_dl,wanted['unrounded_candidate_mg_dl'],delta=abs(wanted['unrounded_candidate_mg_dl'])*1e-12)
            self.assertEqual(got.mg_dl,wanted['candidate_mg_dl'])
            self.assertEqual(got.mmol_tenths,math.floor(wanted['candidate_mg_dl']/18*10+0.5))
    def test_invalid_coefficients_record_and_domain_fail_closed(self):
        for key,values in {'i1':[-1,8],'i2':[0,1024],'i3':[-256,256],'i4':[-1,16384,-100],'i5':[-1,16381],'i6':[0,16381]}.items():
            for value in values:
                got=self.calculate(RECORD,dict(COEFF,**{key:value}))
                self.assertEqual(got.status,2,(key,value));self.assertEqual(got.mmol_tenths,0)
        for key,values in {'raw_adc':[0,16384],'raw_temperature':[0,16381], 'temperature_adjustment':[-2045,2045],'sensor_minute':[-1,65536], 'quality_bits':[1,511],'quality_flags':[1,3],'has_error':[True]}.items():
            for value in values:
                got=self.calculate(dict(RECORD,**{key:value}))
                self.assertEqual(got.status,2,(key,value));self.assertEqual(got.mmol_tenths,0)
        for record,coeff in [(dict(RECORD,raw_temperature=1),COEFF),
                             (dict(RECORD,temperature_adjustment=-100),dict(COEFF,i6=100)),
                             (dict(RECORD,raw_adc=1),dict(COEFF,i3=255)),
                             (dict(RECORD,raw_adc=16383),dict(COEFF,i3=0,i4=1))]:
            got=self.calculate(record,coeff)
            self.assertEqual(got.status,2);self.assertEqual(got.mmol_tenths,0)
    def test_plain_parser_newest_only_status_crc_life_profile_gates(self):
        self.assertTrue(hasattr(self.lib,'lc_factory_parse'),'missing gated FRAM-to-estimate parser')
        fn=self.lib.lc_factory_parse;fn.argtypes=[C.c_void_p,C.c_void_p,C.POINTER(Estimate)]
        data=synthetic()
        data[2:4]=((COEFF['i2']<<3)|COEFF['i1']).to_bytes(2,'little')
        footer=abs(COEFF['i3'])|(COEFF['i4']<<8)|(1<<33)|((COEFF['i5']//4)<<40)|((COEFF['i6']//4)<<52)
        data[336:344]=footer.to_bytes(8,'little')
        word=RECORD['raw_adc']|((RECORD['raw_temperature']//4)<<26)
        data[118:124]=word.to_bytes(6,'little');seal(data)
        def parse(d,patch=PATCH):
            out=Estimate();fn(buffer(d),buffer(patch),C.byref(out));return out
        self.assertEqual(parse(data).status,1)
        for offset,value in [(4,0),(4,1),(4,2),(4,4),(4,5),(4,6),(4,255),(26,16),(27,32),(322,0),(326,0),(3,0),(119,0)]:
            changed=data.copy();changed[offset]=value
            if offset==326:changed[327]=0
            if offset==3:changed[2]=COEFF['i1'] # zero table index
            if offset==119:changed[118]=0 # zero ADC
            got=parse(seal(changed));self.assertEqual(got.status,2,(offset,value));self.assertEqual(got.mmol_tenths,0)
        for bit in [14,23,24,25]:
            changed=data.copy();changed[118:124]=(word|(1<<bit)).to_bytes(6,'little')
            self.assertEqual(parse(seal(changed)).status,2)
        for offset in [0,24,320,343]:
            changed=data.copy();changed[offset]^=1;self.assertEqual(parse(changed).status,2)
        self.assertEqual(parse(data,bytes.fromhex('C50930020000')).status,2)
        # No searching an older apparently clear record when newest is invalid.
        changed=data.copy();changed[118:124]=bytes(6);changed[112:118]=word.to_bytes(6,'little')
        self.assertEqual(parse(seal(changed)).status,2)
        changed=data.copy();changed[316:318]=(20161).to_bytes(2,'little');self.assertEqual(parse(seal(changed)).status,2)
    def test_actual_complete_captures_match_python_or_are_blocked(self):
        if not list((ROOT/'captures').glob('*.json')):
            self.skipTest('private captures/ directory not present in clean checkout')
        self.assertTrue(hasattr(self.lib,'lc_factory_parse'),'missing gated FRAM-to-estimate parser')
        fn=self.lib.lc_factory_parse;fn.argtypes=[C.c_void_p,C.c_void_p,C.POINTER(Estimate)]
        active=blocked=0
        for path in sorted((ROOT/'captures').glob('*.json')):
            before=path.read_bytes();capture=load_capture(path)
            if capture.get('complete') is not True:continue
            uid,patch,raw=validate_capture(capture);plain=decrypt_fram(uid,patch,raw)
            meta=parser.parse(plain,patch);out=Estimate();fn(buffer(plain),buffer(patch),C.byref(out))
            if meta['status_code']!=3 or any(x.startswith(('sensor_life','newest_record')) for x in meta['blockers']):
                self.assertEqual(out.status,2);self.assertEqual(out.mmol_tenths,0);blocked+=1
            else:
                expected=oracle.candidate(meta['trend'][0],meta['factory_coefficients'])
                self.assertEqual(out.status,1);self.assertEqual(out.mg_dl,expected['candidate_mg_dl'])
                self.assertAlmostEqual(out.unrounded_mg_dl,expected['unrounded_candidate_mg_dl'],places=9);active+=1
            self.assertEqual(before,path.read_bytes())
        self.assertGreater(active,0);self.assertGreater(blocked,0)
        print('Read-only actual formula differential:',active,'active;',blocked,'blocked')
    def test_real_capture_entrypoint_and_sanitized_synthetic_differential(self):
        self.assertTrue(hasattr(self.lib,'lc_factory_check'),'missing transcript-bound factory entrypoint')
        setup=(ROOT/'tests/integrity_host.c').read_text().split('int main(void)',1)[0]
        source=setup+r'''
#include "factory.h"
int main(void) {
 LcCapture c={0};LcFactory out;
 while(fread(c.uid,1,8,stdin)==8) {
  assert(fread(c.patch,1,6,stdin)==6 && fread(c.fram,1,344,stdin)==344);
  complete_capture(&c);LcCapture before=c;out.status=LcFactoryEstimate;out.mmol_tenths=1234;lc_factory_check(&c,&out);
  assert(!memcmp(&c,&before,sizeof(c)));
  printf("%d %.17g %.17g %u %u\n",out.status,out.temperature_c,out.unrounded_mg_dl,out.mg_dl,out.mmol_tenths);
  c.fram_length=343;lc_factory_check(&c,&out);assert(out.status==LcFactoryNotChecked && !out.mmol_tenths);
  lc_factory_check(NULL,&out);assert(out.status==LcFactoryNotChecked);
  lc_factory_check(&c,NULL);lc_factory_parse(NULL,c.patch,&out);assert(out.status==LcFactoryRejected);
  lc_factory_parse(c.fram,NULL,&out);assert(out.status==LcFactoryRejected);lc_factory_parse(NULL,NULL,NULL);
  lc_factory_calculate(NULL,NULL,&out);assert(out.status==LcFactoryNotChecked);lc_factory_calculate(NULL,NULL,NULL);
 }
 return 0;
}
'''
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as tmp:
            src=Path(tmp)/'factory_host.c';exe=Path(tmp)/'factory_host';src.write_text(source)
            san=['-g','-fsanitize=address,undefined,float-cast-overflow','-fno-omit-frame-pointer','-no-pie'] if os.environ.get('SANITIZE') else []
            subprocess.run(['gcc',*san,'-fsingle-precision-constant','-std=c11','-Wall','-Wextra','-Werror','-pedantic','-I'+str(ROOT),str(src),*[str(ROOT/n) for n in ['factory.c','integrity.c','capture.c','protocol.c']],'-lm','-o',str(exe)],check=True)
            cases=[];expected=[];rng=random.Random(7342)
            for index in range(1,1024):
                d=synthetic(trend=index%16)
                coeff=dict(COEFF,i1=index%8,i2=index,i3=rng.randrange(-255,256),i4=rng.randrange(500,16000),i5=rng.randrange(4096)*4,i6=rng.randrange(1200,4096)*4)
                record=dict(RECORD,raw_adc=rng.randrange(1,16384),raw_temperature=rng.randrange(1000,4096)*4,temperature_adjustment=rng.randrange(-511,512)*4)
                d[2:4]=((index<<3)|coeff['i1']).to_bytes(2,'little')
                footer=abs(coeff['i3'])|(coeff['i4']<<8)|(int(coeff['i3']<0)<<33)|((coeff['i5']//4)<<40)|((coeff['i6']//4)<<52)
                d[336:344]=footer.to_bytes(8,'little')
                word=record['raw_adc']|((record['raw_temperature']//4)<<26)|((abs(record['temperature_adjustment'])//4)<<38)|(int(record['temperature_adjustment']<0)<<47)
                offset=28+((index%16-1)%16)*6;d[offset:offset+6]=word.to_bytes(6,'little');seal(d)
                uid=bytes.fromhex('01020304050607E0');cases.append(uid+PATCH+decrypt_fram(uid,PATCH,d))
                try:
                    value=oracle.candidate(record,coeff)
                    expected.append(value if value['unrounded_candidate_mg_dl']<=9999 else None)
                except (ValueError,OverflowError):expected.append(None)
            for path in sorted((ROOT/'captures').glob('*.json')):
                capture=load_capture(path)
                if capture.get('complete') is not True:continue
                uid,patch,raw=validate_capture(capture);cases.append(uid+patch+raw)
                meta=parser.parse(decrypt_fram(uid,patch,raw),patch)
                expected.append(oracle.candidate(meta['trend'][0],meta['factory_coefficients']) if meta['status_code']==3 and not any(x.startswith(('sensor_life','newest_record')) for x in meta['blockers']) else None)
            run=subprocess.run([str(exe)],input=b''.join(cases),capture_output=True)
            self.assertEqual(run.returncode,0,run.stderr.decode());lines=run.stdout.decode().splitlines();self.assertEqual(len(lines),len(expected))
            for line,want in zip(lines,expected):
                status,temp,value,mgdl,mmol=line.split()
                if want is None:self.assertEqual(int(status),2);self.assertEqual(int(mmol),0)
                else:
                    self.assertEqual(int(status),1);self.assertEqual(int(mgdl),want['candidate_mg_dl'])
                    self.assertEqual(int(mmol),math.floor(want['candidate_mg_dl']/18*10+0.5))
                    self.assertAlmostEqual(float(value),want['unrounded_candidate_mg_dl'],delta=max(1e-9,abs(float(value))*1e-12))
            print('Factory encrypted differential:',len(cases),'cases; sanitized='+str(bool(san)))
if __name__=='__main__':unittest.main()
