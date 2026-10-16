"""Production C versus pinned Python; private fixtures read-only and never printed."""
from pathlib import Path
import ctypes as C
import importlib.util
import os
import random
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('oracle', ROOT/'offline/libre2_offline.py')
oracle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(oracle)
U8 = C.c_uint8
U16 = C.c_uint16

def buf(data):
    return (U8*len(data)).from_buffer_copy(data)

class Integrity(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='libre-integrity-', dir=os.environ.get('TMPDIR'))
        so = Path(cls.tmp.name)/'integrity.so'
        sources = [ROOT/'protocol.c', ROOT/'capture.c']
        if (ROOT/'integrity.c').exists():
            sources.append(ROOT/'integrity.c')
        subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-pedantic','-shared','-fPIC',*[str(p) for p in sources],'-o',str(so)],check=True)
        cls.lib = C.CDLL(str(so))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def function(self, name, arguments, result):
        self.assertTrue(hasattr(self.lib, name), 'missing production C behavior: '+name)
        fn = getattr(self.lib,name)
        fn.argtypes, fn.restype = arguments, result
        return fn

    def test_prepare_wrapping_wire_order(self):
        prepare = self.function('lc_integrity_prepare',[C.c_void_p,U16,U16,C.c_void_p],None)
        rng = random.Random(9941)
        cases = [(bytes.fromhex('FFFFFFFFFFFF07E0'),65535,65535),
                 (bytes.fromhex('00112233445507E0'),1,2)]
        cases += [(rng.randbytes(8),rng.randrange(65536),rng.randrange(65536)) for _ in range(1000)]
        for uid,x,y in cases:
            out = (U16*4)()
            prepare(buf(uid),x,y,out)
            self.assertTrue(list(out)==oracle.prepare_variables(uid,x,y), 'UInt16 wrapping/order mismatch; input redacted')

    def test_transform_matches_python_real_and_synthetic(self):
        transform = self.function('lc_integrity_transform',[C.c_void_p,C.c_void_p,C.c_void_p,C.c_void_p],None)
        rng = random.Random(3341)
        cases = [(rng.randbytes(6)+b'\x07\xe0', b'\xc5\x09\x30\x01'+rng.randbytes(2), rng.randbytes(344)) for _ in range(500)]
        private = sorted((ROOT/'captures').glob('*.json'))
        found = 0
        for path in private:
            before = path.read_bytes()
            data = oracle.load_capture(path)
            if data.get('complete') is not True:
                continue
            uid,patch,raw = oracle.validate_capture(data)
            oracle.family_gate(uid,patch)
            cases.append((uid,patch,raw))
            self.assertTrue(path.read_bytes()==before, 'private input changed')
            found += 1
        print('Note: no private captures in captures/, verified 500 synthetic cases' if not found else f'Verified {found} private captures')
        for uid,patch,raw in cases:
            out = (U8*344)()
            transform(buf(uid),buf(patch),buf(raw),out)
            self.assertTrue(bytes(out)==oracle.decrypt_fram(uid,patch,raw), 'differential mismatch; private input redacted')
            roundtrip = (U8*344)()
            transform(buf(uid),buf(patch),out,roundtrip)
            self.assertTrue(bytes(roundtrip)==raw, 'XOR roundtrip mismatch')
        print('Differential: 500 synthetic plus',found,'read-only real captures; wire UID not disclosed')

    def test_three_crc_sections_match_python_and_detect_corruption(self):
        crc = self.function('lc_integrity_crc',[C.c_void_p,C.c_size_t],U16)
        sections = self.function('lc_integrity_sections',[C.c_void_p,C.c_void_p],C.c_bool)
        self.assertEqual(crc(buf(b'123456789'),9),0x89f6)
        rng = random.Random(2093)
        for length in range(350):
            data = rng.randbytes(length)
            self.assertEqual(crc(buf(data),length),oracle.crc16_libre(data))
        plain = bytearray(rng.randbytes(344))
        for start,end in [(0,24),(24,320),(320,344)]:
            plain[start:start+2] = oracle.crc16_libre(plain[start+2:end]).to_bytes(2,'little')
        for offset in [None,0,2,23,24,26,319,320,322,343]:
            sample = plain.copy()
            if offset is not None:
                sample[offset] ^= 1
            results = (U16*6)()
            passed = sections(buf(sample),results)
            expected = oracle.section_crcs(sample)
            self.assertEqual(passed,offset is None)
            self.assertEqual(list(results),[int(s[k],16) for s in expected for k in ['stored_hex','computed_hex']])

    def test_capture_check_host_sanitizers_real_and_corruption(self):
        self.assertTrue(hasattr(self.lib,'lc_integrity_check'),'missing capture integrity integration')
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as tmp:
            exe = Path(tmp)/'host'
            sanitizers = ['-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie'] if os.environ.get('SANITIZE') else []
            subprocess.run(['gcc',*sanitizers,'-std=c11','-Wall','-Wextra','-Werror','-pedantic','-I'+str(ROOT),str(ROOT/'tests/integrity_host.c'),str(ROOT/'integrity.c'),str(ROOT/'protocol.c'),str(ROOT/'capture.c'),'-o',str(exe)],check=True)
            rng = random.Random(3221)
            cases = []
            uid = bytes.fromhex('FFFFFFFFFFFF07E0')
            patch = bytes.fromhex('C5093001FFFF')
            plain = bytearray(rng.randbytes(344))
            for start,end in [(0,24),(24,320),(320,344)]:
                plain[start:start+2] = oracle.crc16_libre(plain[start+2:end]).to_bytes(2,'little')
            for offset in [None,0,2,23,24,26,319,320,322,343]:
                changed=plain.copy()
                if offset is not None: changed[offset]^=1
                cases.append((uid,patch,oracle.decrypt_fram(uid,patch,changed)))
            cases += [(rng.randbytes(6)+b'\x07\xe0',b'\xc5\x09\x30\x01'+rng.randbytes(2),rng.randbytes(344)) for _ in range(100)]
            for path in sorted((ROOT/'captures').glob('*.json')):
                data = oracle.load_capture(path)
                if data.get('complete') is True: cases.append(oracle.validate_capture(data))
            for uid,patch,raw in cases:
                result=subprocess.run([str(exe)],input=uid+patch+raw,capture_output=True)
                self.assertTrue(result.returncode==0,'production C host/sanitizer failed; binary input/output redacted; '+result.stderr.decode(errors='replace'))
                expected=oracle.decrypt_fram(uid,patch,raw)
                checks=oracle.section_crcs(expected)
                pairs=b''.join(int(s[k],16).to_bytes(2,'little') for s in checks for k in ['stored_hex','computed_hex'])
                status=1 if all(s['passed'] for s in checks) else 2
                self.assertTrue(result.stdout==expected+pairs+bytes([status]),'host differential/status mismatch; data redacted')
            print('Host binary differential:',len(cases),'cases; sanitized='+str(bool(sanitizers)))

if __name__ == '__main__':
    unittest.main()
