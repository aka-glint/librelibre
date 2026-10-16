"""Run actual app evaluator and draw, not a mirror; host SDK only."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
import sys
ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT/'tests'), str(ROOT/'offline')]
from libre2_offline import validate_capture, load_capture, decrypt_fram
from test_sensor_status import seal, synthetic_valid_capture


def evaluator(app):
    return 'static void vendor_chain_evaluate(' + app.split('static void vendor_chain_evaluate(', 1)[1].split('\n}\n', 1)[0] + '\n}\n'


def wire_evaluator(source, app):
    # Add before draw (or main for non-draw harnesses), then evaluate after factory.
    marker = 'static void draw(' if 'static void draw(' in source else 'int main('
    source = source.replace(marker, evaluator(app) + marker, 1)
    return source.replace('lc_factory_check(&a.capture,&a.factory);',
                          'lc_factory_check(&a.capture,&a.factory);vendor_chain_evaluate(&a.capture,&a.vendor_chain);')


class CurrentTrendApp(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Reuse only SDK shims, replacing the actual harness setup below.
        from test_freshness_ui import build
        cls.tmp = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        cls.exe = build(cls.tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def fixture(self):
        p = ROOT/'captures/cap_015c3a05_00.json'
        return validate_capture(load_capture(p)) if p.exists() else synthetic_valid_capture()

    def render(self, uid, patch, raw):
        r = subprocess.run([str(self.exe)], input=uid+patch+raw, capture_output=True)
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        return r.stdout.decode()

    def test_evaluator_is_current_not_history_and_draw_has_no_factory_fallback(self):
        app = (ROOT/'app.c').read_text()
        body = evaluator(app)
        self.assertNotIn('lc_vendor_estimate(', body)
        self.assertIn('lc_vendor_current_trend(', body)
        draw = app.split('static void draw(',1)[1].split('\n}\n',1)[0]
        self.assertNotIn('app->factory.mmol_tenths', draw)
        self.assertNotIn('app->factory.mg_dl', draw)

    def test_reject_newest_empty_error_quality_and_invalid_ring_without_fallback(self):
        uid, patch, raw = self.fixture()
        plain = bytearray(decrypt_fram(uid,patch,raw))
        slot = (plain[26]+15)%16
        offset = 28+slot*6
        for mode in ['empty','error','quality','flags','index']:
            p = plain.copy()
            if mode == 'empty': p[offset:offset+6] = bytes(6)
            elif mode == 'error': p[offset+3] |= 2
            elif mode == 'quality': p[offset+1] |= 0x40
            elif mode == 'flags': p[offset+2] |= 0x80
            else: p[26] = 16
            seal(p)
            text = self.render(uid,patch,decrypt_fram(uid,patch,p))
            self.assertNotIn('mmol/L',text, mode)
            self.assertIn('Rescan',text, mode)

    def test_full_integrity_active_lifetime_gates(self):
        uid,patch,raw = self.fixture()
        plain = bytearray(decrypt_fram(uid,patch,raw))
        for mode in ['crc','profile','warmup','shutdown','unknown_life','expired','at_end']:
            p = plain.copy()
            if mode == 'profile': p[322] = 0
            elif mode == 'warmup': p[4] = 2
            elif mode == 'shutdown': p[4] = 5
            elif mode == 'unknown_life': p[326:328] = bytes(2)
            elif mode in ['expired','at_end']:
                age = int.from_bytes(p[316:318],'little')
                p[326:328] = (age-(mode=='expired')).to_bytes(2,'little')
            seal(p)
            if mode == 'crc': p[40] ^= 1
            text = self.render(uid,patch,decrypt_fram(uid,patch,p))
            self.assertNotIn('mmol/L',text,mode)
            self.assertIn('Rescan',text,mode)

    def test_history_mutation_cannot_change_current_value_or_freshness(self):
        uid,patch,raw = self.fixture()
        initial = self.render(uid,patch,raw)
        p = bytearray(decrypt_fram(uid,patch,raw))
        p[124:316] = bytes(192)
        seal(p)
        self.assertEqual(initial,self.render(uid,patch,decrypt_fram(uid,patch,p)))
        self.assertIn('mmol/L',initial)
        self.assertIn('Fresh 0m=now; Left',initial)

    def test_factory_available_cannot_supply_fallback_number(self):
        uid,patch,raw = self.fixture()
        r = subprocess.run([str(self.exe),'factory_only'],input=uid+patch+raw,capture_output=True,check=True)
        self.assertNotIn('mmol/L',r.stdout.decode())
        self.assertIn('Rescan',r.stdout.decode())

    def test_vendor_reference_temperature_not_blocked_by_historical_math(self):
        uid,patch,raw = self.fixture()
        p = bytearray(decrypt_fram(uid,patch,raw))
        offset = 28+((p[26]+15)%16)*6
        word = int.from_bytes(p[offset:offset+6],'little')
        word &= ~(0xfff << 26)  # no temperature: vendor explicitly uses 32.5C
        p[offset:offset+6] = word.to_bytes(6,'little')
        seal(p)
        self.assertIn('mmol/L',self.render(uid,patch,decrypt_fram(uid,patch,p)))

    def test_newest_sensor_age_and_remaining_are_bound_to_this_capture(self):
        uid,patch,raw = self.fixture()
        p = bytearray(decrypt_fram(uid,patch,raw))
        p[316:318] = (100).to_bytes(2,'little')
        p[326:328] = (700).to_bytes(2,'little')
        seal(p)
        text = self.render(uid,patch,decrypt_fram(uid,patch,p))
        self.assertIn('mmol/L',text)
        self.assertIn('Fresh 0m=now; Left 10h 0m',text)

    def test_current_value_changes_with_newest_not_history(self):
        uid,patch,raw = self.fixture()
        p = bytearray(decrypt_fram(uid,patch,raw))
        first = self.render(uid,patch,raw)
        offset = 28+((p[26]+15)%16)*6
        word = int.from_bytes(p[offset:offset+6],'little')
        word = (word & ~0x3fff) | 10000
        p[offset:offset+6] = word.to_bytes(6,'little')
        seal(p)
        second = self.render(uid,patch,decrypt_fram(uid,patch,p))
        value = lambda t: next(l for l in t.splitlines() if 'mmol/L' in l)
        self.assertNotEqual(value(first),value(second))

if __name__ == '__main__': unittest.main()
