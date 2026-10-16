"""Synthetic engineering tests only; no sensor capture or clinical validation."""
import importlib.util
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def decoder():
    spec = importlib.util.spec_from_file_location('libre2_offline', ROOT / 'libre2_offline.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class CRC(unittest.TestCase):
    def test_reference_known_vector(self):
        self.assertTrue((ROOT / 'libre2_offline.py').exists(), 'CRC implementation missing')
        self.assertEqual(decoder().crc16_libre(b'123456789'), 0x89F6)


    def test_three_section_integrity_detects_each_corruption(self):
        import random
        m = decoder()
        self.assertTrue(hasattr(m, 'section_crcs'), 'section_crcs missing')
        plain = bytearray(random.Random(399).randbytes(344))
        for offset, end in ((0, 24), (24, 320), (320, 344)):
            plain[offset:offset+2] = m.crc16_libre(plain[offset+2:end]).to_bytes(2, 'little')
        checks = m.section_crcs(plain)
        self.assertEqual([c['name'] for c in checks], ['header', 'body', 'footer'])
        self.assertTrue(all(c['passed'] for c in checks))
        for index in (0, 2, 23, 24, 26, 319, 320, 322, 343):
            broken = plain.copy()
            broken[index] ^= 1
            results = m.section_crcs(broken)
            self.assertEqual(sum(not c['passed'] for c in results), 1)
        with self.assertRaises(ValueError):
            m.section_crcs(plain[:-1])


class Crypto(unittest.TestCase):
    def test_prepare_variables_wraps_words_without_reversing_uid(self):
        m = decoder()
        self.assertTrue(hasattr(m, 'prepare_variables'), 'prepare_variables missing')
        self.assertEqual(m.prepare_variables(bytes.fromhex('FFFFFFFFFFFF07E0'), 0xFFFF, 0xFFFF),
                         [0xFFFD, 0xFFFF, 0xFFFD, 0x30DC])
        self.assertEqual(m.prepare_variables(bytes.fromhex('00112233445507E0'), 1, 2),
                         [0x5547, 0x3322, 0x1102, 0x30DC])


    def test_fram_xor_roundtrip_matches_separate_c_reference(self):
        import random, subprocess, tempfile, os
        m = decoder()
        self.assertTrue(hasattr(m, 'decrypt_fram'), 'decrypt_fram missing')
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as tmp:
            exe = pathlib.Path(tmp) / 'reference-c'
            subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', str(ROOT / 'tests/reference.c'), '-o', str(exe)], check=True)
            rng = random.Random(2813)
            for _ in range(100):
                uid = rng.randbytes(6) + bytes.fromhex('07E0')
                patch = bytes.fromhex('C5093001') + rng.randbytes(2)
                raw = rng.randbytes(344)
                expected = subprocess.run([str(exe)], input=uid+patch+raw, capture_output=True, check=True).stdout
                plain = m.decrypt_fram(uid, patch, raw)
                self.assertEqual(plain, expected)
                self.assertEqual(m.decrypt_fram(uid, patch, plain), raw)
        for uid, patch, raw in [(b'', b'123456', bytes(344)), (bytes(8), b'', bytes(344)), (bytes(8), bytes(6), bytes(343))]:
            with self.assertRaises(ValueError):
                m.decrypt_fram(uid, patch, raw)


def synthetic_capture():
    uid = bytes.fromhex('00112233445507E0')
    patch = bytes.fromhex('C50930010000')
    raw = bytes((i * 17) % 256 for i in range(344))
    transcript = [{'command_hex': '260100', 'response_hex': (b'\x00\x00' + uid).hex(), 'response_length': 10, 'error': 0},
                  {'command_hex': '02A107', 'response_hex': (b'\x00' + patch).hex(), 'response_length': 7, 'error': 0}]
    for block in range(0, 43, 3):
        count = min(3, 43-block)
        response = b'\x00' + raw[block*8:(block+count)*8]
        transcript.append({'command_hex': bytes([2, 0x23, block, count-1]).hex(),
                           'response_hex': response.hex(), 'response_length': len(response), 'error': 0})
    return {'schema': 'libre-nfc-research-capture', 'schema_version': 2, 'capture_kind': 'conservative_raw_fram',
            'uid_order': 'ISO15693 inventory wire LSB-first', 'uid_wire_hex': uid.hex(),
            'patch_info_hex': patch.hex(), 'patch_info_length': 6, 'fram_hex': raw.hex(),
            'fram_length': 344, 'fram_expected_length': 344, 'complete': True, 'transport_error': 0,
            'family_gate_passed': True, 'glucose_available': False, 'transcript': transcript}


class Capture(unittest.TestCase):
    def test_schema2_and_transcript_validation_rejects_inconsistent_inputs(self):
        import copy
        m = decoder()
        self.assertTrue(hasattr(m, 'validate_capture'), 'validate_capture missing')
        capture = synthetic_capture()
        uid, patch, raw = m.validate_capture(capture)
        self.assertEqual((len(uid), len(patch), len(raw)), (8, 6, 344))
        mutations = [lambda d: d.update(schema_version=1), lambda d: d.update(schema_version=True),
                     lambda d: d.update(complete=False), lambda d: d.update(complete=1),
                     lambda d: d.update(transport_error=1), lambda d: d.update(fram_length=343),
                     lambda d: d.update(fram_expected_length=352), lambda d: d.update(patch_info_length=7),
                     lambda d: d.update(uid_order='MSB-first'), lambda d: d.update(uid_wire_hex='zz'*8),
                     lambda d: d.update(fram_hex=' '+d['fram_hex']), lambda d: d.update(glucose_available=True),
                     lambda d: d['transcript'].pop(), lambda d: d['transcript'].append(d['transcript'][2]),
                     lambda d: d['transcript'][0].update(response_hex='00'*10),
                     lambda d: d['transcript'][1].update(response_hex='00'*7),
                     lambda d: d['transcript'][2].update(response_length=1),
                     lambda d: d['transcript'][2].update(error=1),
                     lambda d: d['transcript'][2].update(response_hex='01'+d['transcript'][2]['response_hex'][2:]),
                     lambda d: d['transcript'][2].update(response_hex='00'*25),
                     lambda d: d['transcript'][2].update(command_hex='021E0002'),
                     lambda d: d.pop('transcript')]
        for mutation in mutations:
            bad = copy.deepcopy(capture)
            mutation(bad)
            with self.assertRaises(ValueError):
                m.validate_capture(bad)


    def test_conservative_gate_recomputes_family_not_capture_claim(self):
        m = decoder()
        self.assertTrue(hasattr(m, 'family_gate'), 'family_gate missing')
        uid = bytes.fromhex('00112233445507E0')
        for marker in (0xC5, 0x9D):
            for generation in (0x30, 0x38, 0x70, 0x73):
                m.family_gate(uid, bytes([marker, 9, generation, 1, 0, 0]))
        for marker in (0xC6, 0x7F, 0x2C, 0x2B, 0xE5, 0):
            with self.assertRaises(ValueError):
                m.family_gate(uid, bytes([marker, 9, 0x30, 1, 0, 0]))
        for generation in (0x39, 0x3F, 0x74, 0x40):
            with self.assertRaises(ValueError):
                m.family_gate(uid, bytes([0xC5, 9, generation, 1, 0, 0]))
        with self.assertRaises(ValueError):
            m.family_gate(uid[::-1], bytes.fromhex('C50930010000'))
        with self.assertRaises(ValueError):
            m.family_gate(uid, bytes.fromhex('C50930020000'))


class CLI(unittest.TestCase):
    def test_cli_private_outputs_crc_exit_status_and_no_uid_leak(self):
        import json, os, subprocess, sys, tempfile, hashlib
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temp:
            work = pathlib.Path(temp)
            capture = synthetic_capture()
            source = work / 'capture.json'
            source.write_text(json.dumps(capture))
            before = source.read_bytes()
            out = work / 'result'
            result = subprocess.run([sys.executable, str(ROOT/'libre2_offline.py'), str(source), '--output-dir', str(out)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 3, result.stderr + result.stdout)
            report = json.loads((out/'report.json').read_text())
            self.assertTrue(report['experimental'])
            self.assertFalse(report['glucose_available'])
            self.assertFalse(report['clinical_validity_claimed'])
            self.assertEqual(report['integrity_status'], 'fail')
            self.assertEqual((out/'raw.bin').read_bytes(), bytes.fromhex(capture['fram_hex']))
            self.assertEqual(len((out/'decrypted.bin').read_bytes()), 344)
            self.assertEqual(source.read_bytes(), before)
            self.assertEqual(out.stat().st_mode & 0o777, 0o700)
            for file in out.iterdir():
                self.assertEqual(file.stat().st_mode & 0o777, 0o600)
            self.assertNotIn(capture['uid_wire_hex'].lower(), (result.stdout+result.stderr+json.dumps(report)).lower())
            rerun = subprocess.run([sys.executable, str(ROOT/'libre2_offline.py'), str(source), '--output-dir', str(out)], capture_output=True)
            self.assertEqual(rerun.returncode, 2)
            invalid = work/'invalid.json'
            invalid.write_text('{"schema_version":2,"schema_version":1}')
            denied = work/'denied'
            res = subprocess.run([sys.executable, str(ROOT/'libre2_offline.py'), str(invalid), '--output-dir', str(denied)], capture_output=True)
            self.assertEqual(res.returncode, 2)
            self.assertFalse(denied.exists())
            m = decoder()
            uid, patch, _ = m.validate_capture(capture)
            plain = bytearray(344)
            for start, end in [(0,24), (24,320), (320,344)]:
                plain[start:start+2] = m.crc16_libre(plain[start+2:end]).to_bytes(2,'little')
            encrypted = m.decrypt_fram(uid,patch,plain)
            capture['fram_hex'] = encrypted.hex()
            for index, item in enumerate(capture['transcript'][2:]):
                block = index*3
                item['response_hex'] = (b'\x00'+encrypted[block*8:min(block+3,43)*8]).hex()
            source.write_text(json.dumps(capture))
            success = work/'success'
            res = subprocess.run([sys.executable, str(ROOT/'libre2_offline.py'), str(source), '--output-dir', str(success)], capture_output=True)
            self.assertEqual(res.returncode, 0, res.stderr)
            self.assertEqual(json.loads((success/'report.json').read_text())['integrity_status'], 'pass')


class ReferenceVerification(unittest.TestCase):
    def test_crc_matches_pinned_msb_first_bit_reference(self):
        import random
        rng = random.Random(7934)
        for size in range(350):
            data = rng.randbytes(size)
            crc = 0xFFFF
            for byte in data:
                for i in range(8):
                    feedback = ((crc >> 15) ^ (byte >> i)) & 1
                    crc = ((crc << 1) ^ (0x1021 if feedback else 0)) & 0xFFFF
            self.assertEqual(decoder().crc16_libre(data), crc)

    def test_available_private_capture_integrity_and_c_reference(self):
        import json, subprocess, hashlib
        capture_path = ROOT.parent/'captures/cap_011b01aa_00.json'
        if not capture_path.exists():
            self.skipTest('private capture not distributed')
        before = hashlib.sha256(capture_path.read_bytes()).digest()
        m = decoder()
        uid, patch, raw = m.validate_capture(m.load_capture(capture_path))
        m.family_gate(uid, patch)
        plain = m.decrypt_fram(uid, patch, raw)
        self.assertTrue(all(c['passed'] for c in m.section_crcs(plain)))
        self.assertFalse(any(c['passed'] for c in m.section_crcs(raw)))
        exe = ROOT/'evidence/reference-c'
        subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', str(ROOT/'tests/reference.c'), '-o', str(exe)], check=True)
        separate = subprocess.run([str(exe)], input=uid+patch+raw, capture_output=True, check=True).stdout
        self.assertTrue(plain == separate, 'actual private byte comparison failed; content redacted')
        self.assertEqual(hashlib.sha256(capture_path.read_bytes()).digest(), before)


if __name__ == '__main__':
    unittest.main()
