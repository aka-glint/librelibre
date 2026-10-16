"""CRC-gated production C metadata, differential against pinned research parser.
Private capture contents never printed. Same-source fidelity, not clinical validation.
"""
import ctypes as C
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('status_oracle', ROOT/'glucose-research/raw_parser.py')
oracle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(oracle)
from libre2_offline import crc16_libre, load_capture, validate_capture, decrypt_fram

class Sensor(C.Structure):
    _fields_ = [('age_minutes', C.c_uint16), ('max_life_minutes', C.c_uint16),
                ('quality_bits', C.c_uint16), ('parse_status', C.c_uint8),
                ('status_code', C.c_uint8), ('trend_index', C.c_uint8),
                ('history_index', C.c_uint8), ('newest_slot', C.c_uint8),
                ('quality_flags', C.c_uint8), ('has_error', C.c_bool)]

def buffer(data):
    return (C.c_uint8*len(data)).from_buffer_copy(data)

def seal(data):
    for start, end in [(0,24),(24,320),(320,344)]:
        data[start:start+2] = crc16_libre(data[start+2:end]).to_bytes(2,'little')
    return data

PATCH = bytes.fromhex('C50930010000')
def synthetic(status=3, age=100, life=20160, trend=0, history=0, quality=0, flags=0, error=False):
    data = bytearray(344)
    data[4], data[26], data[27] = status, trend, history
    data[316:318] = age.to_bytes(2,'little')
    data[322:324] = PATCH[2:4]
    data[326:328] = life.to_bytes(2,'little')
    slot = (trend-1)%16
    data[28+6*slot:34+6*slot] = ((quality<<14)|(flags<<23)|(int(error)<<25)).to_bytes(6,'little')
    return seal(data)


def synthetic_valid_capture(age=2466, life=20854, status=3, adc=1500, tcode=7008, adj=8, quality=0, flags=0, error=False):
    uid = bytes.fromhex('5E90250000A007E0')
    patch = bytes.fromhex('C50930010000')
    data = bytearray(344)
    u16 = (866 << 3) | 0
    data[2] = u16 & 0xFF
    data[3] = (u16 >> 8) & 0xFF
    data[4] = status
    data[26] = 1
    data[27] = 1
    data[316:318] = age.to_bytes(2, 'little')
    data[322:324] = patch[2:4]
    data[326:328] = life.to_bytes(2, 'little')
    footer = 0x6c28d300001a7312
    data[336:344] = footer.to_bytes(8, 'little')
    slot = 0
    word = (adc & 0x3FFF) | ((quality & 0x1FF) << 14) | ((flags & 0x3) << 23) | (int(bool(error or quality)) << 25) | (((tcode >> 2) & 0xFFF) << 26) | (((abs(adj) >> 2) & 0x1FF) << 38) | (int(adj < 0) << 47)
    data[28+6*slot:34+6*slot] = word.to_bytes(6, 'little')
    seal(data)
    from libre2_offline import decrypt_fram
    return uid, patch, decrypt_fram(uid, patch, data)

class Status(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        so = Path(cls.tmp.name)/'status.so'
        subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-pedantic','-shared','-fPIC',
                        str(ROOT/'integrity.c'),str(ROOT/'capture.c'),str(ROOT/'protocol.c'),'-o',str(so)],check=True)
        cls.lib = C.CDLL(str(so))
    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()
    def parse(self, plain, patch=PATCH):
        self.assertTrue(hasattr(self.lib,'lc_sensor_parse'), 'missing production CRC-gated sensor diagnostic parser')
        fn = self.lib.lc_sensor_parse
        fn.argtypes = [C.c_void_p,C.c_void_p,C.POINTER(Sensor)]
        fn.restype = None
        result = Sensor()
        fn(buffer(plain), buffer(patch), C.byref(result))
        return result
    def test_encrypted_capture_integration_preserves_raw_and_clears_stale_metadata(self):
        # Existing production entry point must actually carry parsed diagnostics.
        setup = (ROOT/'tests/integrity_host.c').read_text().split('int main(void)',1)[0]
        sensor = 'r.sensor'
        source = setup + r'''
int main(void) {
    LcCapture c={0}; LcIntegrity r={0};
    assert(fread(c.uid,1,8,stdin)==8 && fread(c.patch,1,6,stdin)==6 && fread(c.fram,1,344,stdin)==344);
    complete_capture(&c); LcCapture before=c;
    lc_integrity_check(&c,&r); LcSensor s=SENSOR;
    assert(!memcmp(&c,&before,sizeof(c)));
    printf("%u %u %u %u %u %u %u %u %u %u\n",s.parse_status,s.status_code,s.age_minutes,s.max_life_minutes,
           s.trend_index,s.history_index,s.newest_slot,s.quality_bits,s.quality_flags,s.has_error);
    c.fram_length=343; lc_integrity_check(&c,&r); s=SENSOR;
    assert(s.parse_status==LcSensorNotChecked && s.age_minutes==0);
    lc_sensor_parse(NULL,c.patch,&s);assert(s.parse_status==LcSensorNotChecked);
    lc_sensor_parse(c.fram,NULL,&s);assert(s.parse_status==LcSensorNotChecked);
    lc_sensor_parse(c.fram,c.patch,NULL);
    return 0;
}
'''.replace('SENSOR',sensor)
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as tmp:
            c = Path(tmp)/'status.c'; exe = Path(tmp)/'status'
            c.write_text(source)
            san = ['-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie'] if os.environ.get('SANITIZE') else []
            subprocess.run(['gcc',*san,'-std=c11','-Wall','-Wextra','-Werror','-pedantic','-I'+str(ROOT),str(c),
                            str(ROOT/'integrity.c'),str(ROOT/'protocol.c'),str(ROOT/'capture.c'),'-o',str(exe)],check=True)
            cases = []
            for path in sorted((ROOT/'captures').glob('*.json')):
                capture = load_capture(path)
                if capture.get('complete') is True:
                    cases.append(validate_capture(capture))
            uid = bytes.fromhex('01020304050607E0')
            for status in list(range(7))+[7,255]:
                for trend in range(16):
                    data = synthetic(status=status,trend=trend,history=31,quality=(trend*31)&511,flags=trend%4,error=bool(trend%2))
                    cases.append((uid,PATCH,decrypt_fram(uid,PATCH,data)))
            for trend,history in [(16,0),(255,0),(0,32),(0,255)]:
                data=synthetic();data[26],data[27]=trend,history
                cases.append((uid,PATCH,decrypt_fram(uid,PATCH,seal(data))))
            for age,life in [(0,0),(0,1),(20160,20160),(20161,20160),(65535,65535)]:
                cases.append((uid,PATCH,decrypt_fram(uid,PATCH,synthetic(age=age,life=life))))
            data=synthetic();data[322]^=1
            cases.append((uid,PATCH,decrypt_fram(uid,PATCH,seal(data))))
            for offset in [0,4,23,24,26,100,316,319,320,326,343]:
                data=synthetic();data[offset]^=1
                cases.append((uid,PATCH,decrypt_fram(uid,PATCH,data)))
            for uid,patch,raw in cases:
                expected = self.parse(decrypt_fram(uid,patch,raw),patch)
                wanted = [expected.parse_status,expected.status_code,expected.age_minutes,expected.max_life_minutes,
                          expected.trend_index,expected.history_index,expected.newest_slot,expected.quality_bits,
                          expected.quality_flags,int(expected.has_error)]
                run = subprocess.run([str(exe)],input=uid+patch+raw,capture_output=True)
                self.assertEqual(run.returncode,0,run.stderr.decode())
                self.assertEqual(list(map(int,run.stdout.split())),wanted)
            print('Encrypted capture metadata integration:',len(cases),'host cases; sanitized='+str(bool(san)))

    def test_synthetic_status_age_life_and_newest_fields_match_python(self):
        for status in list(range(7))+[7,255]:
            for trend in range(16):
                for age,life in [(0,0),(0,1),(20160,20160),(20161,20160),(65535,65535)]:
                    data = synthetic(status=status,age=age,life=life,trend=trend,history=trend+16,
                                     quality=511 if trend%2 else 0,flags=trend%4,error=bool(trend%2))
                    expected=oracle.parse(data,PATCH)
                    got=self.parse(data)
                    self.assertEqual(got.parse_status,1)
                    self.assertEqual(got.status_code,expected['status_code'])
                    self.assertEqual(got.age_minutes,expected['age_minutes'])
                    self.assertEqual(got.max_life_minutes,expected['max_life_minutes'])
                    self.assertEqual(got.trend_index,expected['trend_index'])
                    self.assertEqual(got.history_index,expected['history_index'])
                    newest=expected['trend'][0]
                    for field,key in [('newest_slot','slot'),('quality_bits','quality_bits'),('quality_flags','quality_flags'),('has_error','has_error')]:
                        self.assertEqual(getattr(got,field),newest[key])
        # Alternate supported family is not silently narrowed to C5.
        self.assertEqual(self.parse(synthetic(),bytes.fromhex('9D0930010000')).parse_status,1)

    def test_profile_and_ring_indexes_fail_closed(self):
        for patch in [bytes.fromhex('C60930010000'),bytes.fromhex('C50931010000'),bytes.fromhex('C50930020000')]:
            self.assertEqual(self.parse(synthetic(),patch).parse_status,3)
        data = synthetic()
        data[322] ^= 1
        self.assertEqual(self.parse(seal(data)).parse_status,4)
        for trend,history in [(16,0),(255,0),(0,32),(0,255)]:
            data = synthetic()
            data[26],data[27] = trend,history
            got = self.parse(seal(data))
            self.assertEqual(got.parse_status,5)
            self.assertEqual(got.age_minutes,0)
            self.assertEqual(got.quality_bits,0)

    def test_corrupt_crc_rejected_without_metadata(self):
        for offset in [0,4,23,24,26,100,316,319,320,326,343]:
            data = synthetic()
            data[offset] ^= 1
            got = self.parse(data)
            self.assertEqual(got.parse_status,2, 'CRC failure must block parsing')
            self.assertEqual(got.status_code,0)
            self.assertEqual(got.age_minutes,0)
            self.assertFalse(got.has_error)

    def test_actual_capture_shutdown_not_usable(self):
        if not list((ROOT/'captures').glob('*.json')):
            self.skipTest('private captures/ directory not present in clean checkout')
        found = 0
        for path in sorted((ROOT/'captures').glob('*.json')):
            before = path.read_bytes()
            capture = load_capture(path)
            if capture.get('complete') is not True:
                continue
            uid, patch, raw = validate_capture(capture)
            plain = decrypt_fram(uid, patch, raw)
            expected = oracle.parse(plain, patch)
            got = self.parse(plain, patch)
            self.assertEqual(got.parse_status,1)
            self.assertEqual(got.status_code,expected['status_code'])
            self.assertEqual(got.age_minutes,expected['age_minutes'])
            self.assertEqual(got.max_life_minutes,expected['max_life_minutes'])
            self.assertEqual(got.trend_index,expected['trend_index'])
            self.assertEqual(got.history_index,expected['history_index'])
            newest = expected['trend'][0]
            for field, key in [('newest_slot','slot'),('quality_bits','quality_bits'),('quality_flags','quality_flags'),('has_error','has_error')]:
                self.assertEqual(getattr(got,field),newest[key])
            if path.name.startswith('cap_011'):
                self.assertEqual(got.status_code,5)
                self.assertGreater(got.age_minutes,got.max_life_minutes)
                self.assertTrue(got.has_error)
                self.assertEqual(got.quality_bits,0x100)
            elif path.name in ('cap_014d22f3_00.json','cap_014d4036_00.json'):
                self.assertEqual(got.status_code,3)
                self.assertEqual(got.age_minutes,2466)
                self.assertEqual(got.max_life_minutes,20854)
                self.assertFalse(got.has_error)
                self.assertEqual(got.quality_bits,0)
            self.assertEqual(before,path.read_bytes())
            found += 1
        self.assertGreaterEqual(found,6)
        print('Actual diagnostics differential:',found,'read-only capture files; no freshness claim or UID disclosed')

if __name__ == '__main__':
    unittest.main()
