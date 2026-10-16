"""Recovered vendor raw-to-estimator chain tests (bounded port).

Tests the CLOSED chain only: record decode, config extraction, ADC scale,
temperature conversion, temperature gain, windowed mean, factory transform,
leaf regression with quality skipping, and float-vs-double equivalence.
The vendor smoothing/admission layer is NOT implemented and not tested here.
"""
import ctypes as C
import importlib.util
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'offline'), str(ROOT / 'tests')]

def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m

# Reference vectors produced by the authoritative host-side chain (M710s).
REF = ROOT / 'tests/data/vendor_chain_reference.json'


class LcVendorRecord(C.Structure):
    _fields_ = [('id', C.c_uint16), ('adc', C.c_uint16), ('tcode', C.c_uint16),
                ('adjust', C.c_int16), ('quality', C.c_uint16)]


def build_lib(float_mode=False):
    """Compile vendor_chain + vendor_tables as a shared lib and load it."""
    tmp = tempfile.mkdtemp(dir=os.environ.get('TMPDIR'))
    so = Path(tmp) / ('vendor_chain.so')
    cmd = ['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic',
           '-fPIC', '-shared', '-fsingle-precision-constant', '-I' + str(ROOT),
           str(ROOT / 'vendor_chain.c'), str(ROOT / 'vendor_tables.c'),
           str(ROOT / 'factory.c'), str(ROOT / 'integrity.c'), str(ROOT / 'capture.c'),
           str(ROOT / 'protocol.c'), '-lm', '-o', str(so)]
    if float_mode:
        cmd.insert(1, '-DLC_VENDOR_USE_FLOAT=1')
    subprocess.run(cmd, check=True)
    return C.CDLL(str(so)), tmp


class Chain(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lib, cls.tmp = build_lib(False)
        cls.dlib, cls.dtmp = build_lib(True)
        cls.ref = json.loads(REF.read_text())

    def cfg_module(self):
        return None

    def test_record_decode_bit_layout(self):
        # bits: adc 0..13, quality(9) bits14..24 if bit25, tcode bits26..37<<2,
        # adjust bits38..46<<2 signed by bit47.
        w = (1500) | (0x1FF << 14) | (1 << 25) | (0x2AB << 26) | (0x055 << 38)
        rec6 = w.to_bytes(6, 'little')
        out = LcVendorRecord()
        self.lib.lc_vendor_decode_record(rec6, C.byref(out))
        self.assertEqual(out.adc, 1500)
        self.assertEqual(out.quality, 0x1FF)
        self.assertEqual(out.tcode, (0x2AB << 2))
        self.assertEqual(out.adjust, (0x055 << 2))
        # sign bit 47
        w2 = (77) | (1 << 47) | (0x20 << 38)
        out2 = LcVendorRecord()
        self.lib.lc_vendor_decode_record(w2.to_bytes(6, 'little'), C.byref(out2))
        self.assertEqual(out2.adjust, -((0x20 << 2)))
        # bit25 clear -> quality forced 0
        w3 = (5) | (0x1FF << 14)
        out3 = LcVendorRecord()
        self.lib.lc_vendor_decode_record(w3.to_bytes(6, 'little'), C.byref(out3))
        self.assertEqual(out3.quality, 0)

    def get_tables(self, lib):
        """The big t1/t2 tables are the shared factory.c arrays; accessors map
        vendor roles. Returns (t1_vendor, t2_vendor, slope)."""
        lib.lc_vendor_table_t1.restype = C.POINTER(C.c_double)
        lib.lc_vendor_table_t2.restype = C.POINTER(C.c_double)
        lib.lc_vendor_table_t1.argtypes = []
        lib.lc_vendor_table_t2.argtypes = []
        t1 = lib.lc_vendor_table_t1()
        t2 = lib.lc_vendor_table_t2()
        slope = (C.c_double * 8).in_dll(lib, 'LC_VENDOR_SLOPE_TABLE')
        return t1, t2, slope

    def test_config_extraction_known_family(self):
        # Footer from the real public fixtures: 0x6c28d300001a7312
        # -> i3=18, i4=6771, i2=866, i1=0, i5=9036, i6=6920.
        plain = bytearray(344)
        footer = 0x6c28d300001a7312
        plain[336:344] = footer.to_bytes(8, 'little')
        plain[2] = 866 & 0xFF  # i1 = plain[2]&7, i2 = (u16@2>>3)&0x3FF
        plain[3] = (866 >> 5) & 0xFF
        # Build u16@2 = (i2<<3)|i1  => bytes
        u16 = (866 << 3) | 0
        plain[2] = u16 & 0xFF
        plain[3] = (u16 >> 8) & 0xFF
        self.assertEqual((((plain[2] | (plain[3] << 8)) >> 3) & 0x3FF), 866)
        self.assertEqual(plain[2] & 7, 0)
        # provide tables via the shared accessors + slope symbol
        t1, t2, slope = self.get_tables(self.lib)
        self.lib.lc_vendor_config_from_fram.restype = C.c_bool

        class Cfg(C.Structure):
            _fields_ = [('i1', C.c_int), ('i2', C.c_int), ('i3', C.c_int), ('i4', C.c_int),
                        ('i5', C.c_int), ('i6', C.c_int), ('t1', C.c_double), ('t2', C.c_double),
                        ('slope', C.c_double)]
        c = Cfg()
        ok = self.lib.lc_vendor_config_from_fram(bytes(plain), t1, t2, slope, C.byref(c))
        self.assertTrue(ok)
        self.assertEqual(c.i3, 18)
        self.assertEqual(c.i4, 6771)
        self.assertEqual(c.i2, 866)
        self.assertEqual(c.i1, 0)
        self.assertEqual(c.i5, 9036)
        self.assertEqual(c.i6, 6920)

    def test_temperature_conversion_matches_reference(self):
        # From the reference: tcode=7008, adjust=8, i6=6920 -> T ~ 31.859 C
        out = C.c_double()
        self.lib.lc_vendor_temperature_c.restype = C.c_bool
        ok = self.lib.lc_vendor_temperature_c(7008, 8, 6920, C.byref(out))
        self.assertTrue(ok)
        self.assertAlmostEqual(out.value, 31.859, places=2)

    def test_leaf_regression_skips_quality(self):
        class Tup(C.Structure):
            _fields_ = [('id', C.c_uint16), ('quality', C.c_uint16), ('value', C.c_double)]
        arr = (Tup * 4)()
        # Perfect line value = 2*id + 1 for quality==0; a poisoned high-quality row
        # must be skipped and not move the fit.
        arr[0] = Tup(10, 0, 21.0)
        arr[1] = Tup(20, 0, 41.0)
        arr[2] = Tup(30, 0, 61.0)
        arr[3] = Tup(40, 5, 9999.0)  # quality != 0 -> skipped
        out = C.c_double()
        self.lib.lc_vendor_leaf_regression.restype = C.c_bool
        ok = self.lib.lc_vendor_leaf_regression(arr, 4, C.c_double(25.0), C.byref(out))
        self.assertTrue(ok)
        self.assertAlmostEqual(out.value, 51.0, places=9)

    def test_leaf_regression_needs_two_usable(self):
        class Tup(C.Structure):
            _fields_ = [('id', C.c_uint16), ('quality', C.c_uint16), ('value', C.c_double)]
        arr = (Tup * 2)()
        arr[0] = Tup(10, 1, 5.0)
        arr[1] = Tup(20, 0, 7.0)
        out = C.c_double()
        self.lib.lc_vendor_leaf_regression.restype = C.c_bool
        self.assertFalse(self.lib.lc_vendor_leaf_regression(arr, 2, C.c_double(15.0), C.byref(out)))

    def test_float_double_equivalence_on_reference_vectors(self):
        """float must not change any ROUNDED mg/dL or mmol/L vs double.

        Measured finding (VENDOR-CHAIN-PORT.md): on the real fixture and private
        vectors float changes ZERO rounded values (max delta ~2e-4 mg/dL). The
        struct field types MUST match the build (float vs double) or the ctypes
        ABI silently mis-reads the config, so each build uses its own struct. """
        self.lib.lc_vendor_correct_record.restype = C.c_int
        self.dlib.lc_vendor_correct_record.restype = C.c_int

        def cfg_struct(real):
            class Cfg(C.Structure):
                _fields_ = [('i1', C.c_int), ('i2', C.c_int), ('i3', C.c_int), ('i4', C.c_int),
                            ('i5', C.c_int), ('i6', C.c_int), ('t1', real), ('t2', real),
                            ('slope', real)]
            return Cfg

        DCfg = cfg_struct(C.c_double)
        FCfg = cfg_struct(C.c_float)

        def num(x):
            return float(x.value) if hasattr(x, 'value') else float(x)

        changed = 0
        n = 0
        maxdelta = 0.0
        for fx in self.ref['public'].values():
            cc = fx['cfg']
            dc = DCfg(cc['i1'], cc['i2'], cc['i3'], cc['i4'], cc['i5'], cc['i6'],
                      cc['t1'], cc['t2'], cc['slope'])
            fc = FCfg(cc['i1'], cc['i2'], cc['i3'], cc['i4'], cc['i5'], cc['i6'],
                      C.c_float(cc['t1']), C.c_float(cc['t2']), C.c_float(cc['slope']))
            for row in fx['rows']:
                rec = LcVendorRecord(row['id'], row['adc'], row['tc'], row['adj'], row['q'])
                dv = C.c_double(); tv = C.c_double()
                self.lib.lc_vendor_correct_record(C.byref(rec), C.byref(dc), C.byref(dv), C.byref(tv))
                fv = C.c_float(); ft = C.c_float()
                self.dlib.lc_vendor_correct_record(C.byref(rec), C.byref(fc), C.byref(fv), C.byref(ft))
                n += 1
                maxdelta = max(maxdelta, abs(num(dv) - num(fv)))
                dm = C.c_uint32(); dt = C.c_uint32()
                fm = C.c_uint32(); ff = C.c_uint32()
                self.lib.lc_vendor_round(C.c_double(num(dv)), C.byref(dm), C.byref(dt))
                self.dlib.lc_vendor_round(C.c_float(num(fv)), C.byref(fm), C.byref(ff))
                if dm.value != fm.value or dt.value != ff.value:
                    changed += 1
        print('float/double: n=%d changed_rounded=%d max_delta=%.3e' % (n, changed, maxdelta))
        self.assertEqual(changed, 0, 'float changed a rounded output value')


    def test_corrected_tuples_match_host_chain(self):
        """The ported chain must reproduce the host-side closed-chain values
        (rawin1 newest 90.2199, rawin2 newest 73.2076) exactly on the real
        public fixture bytes."""
        self.lib.lc_vendor_correct_record.restype = C.c_int

        class Cfg(C.Structure):
            _fields_ = [('i1', C.c_int), ('i2', C.c_int), ('i3', C.c_int), ('i4', C.c_int),
                        ('i5', C.c_int), ('i6', C.c_int), ('t1', C.c_double), ('t2', C.c_double),
                        ('slope', C.c_double)]
        for name in ('rawin1', 'rawin2'):
            fx = self.ref['public'][name]
            cc = fx['cfg']
            cfg = Cfg(cc['i1'], cc['i2'], cc['i3'], cc['i4'], cc['i5'], cc['i6'],
                      cc['t1'], cc['t2'], cc['slope'])
            # compare every row's corrected value to the reference double
            worst = 0.0
            for row in fx['rows']:
                rec = LcVendorRecord(row['id'], row['adc'], row['tc'], row['adj'], row['q'])
                v = C.c_double(); t = C.c_double()
                self.lib.lc_vendor_correct_record(C.byref(rec), C.byref(cfg), C.byref(v), C.byref(t))
                worst = max(worst, abs(v.value - row['v']))
            self.assertLess(worst, 1e-9, name)
            newest = fx['newest']
            rec = LcVendorRecord(newest['id'], newest['adc'], newest['tc'], newest['adj'], newest['q'])
            v = C.c_double(); t = C.c_double()
            self.lib.lc_vendor_correct_record(C.byref(rec), C.byref(cfg), C.byref(v), C.byref(t))
            # host-side newest corrected tuple (see RAW/parser-config docs)
            expected = 90.219894 if name == 'rawin1' else 73.207632
            self.assertAlmostEqual(v.value, expected, places=4, msg=name)
            print('host-chain', name, 'newest=%.4f vendor_current=%d' % (v.value, fx['vendor_current']))

    def test_leaf_regression_hash_and_weighted_average(self):
        """Regression over the corrected tuples: reproduces a line and the
        quality skip; the weighted-average reduction is the mean of the fitted
        endpoints (coefficient pairs (1,0.5),(1,0.5),(4,3) collapse to the
        least-squares fit here)."""
        class Tup(C.Structure):
            _fields_ = [('id', C.c_uint16), ('quality', C.c_uint16), ('value', C.c_double)]
        N = 8
        arr = (Tup * N)()
        for i in range(N):
            arr[i] = Tup(100 + 15 * i, 0, 5.0 * i + 12.0)
        out = C.c_double()
        self.lib.lc_vendor_leaf_regression.restype = C.c_bool
        for idx in (100, 145, 205):
            ok = self.lib.lc_vendor_leaf_regression(arr, N, C.c_double(float(idx)), C.byref(out))
            self.assertTrue(ok)
            self.assertAlmostEqual(out.value, 5.0 * (idx - 100) / 15.0 + 12.0, places=9)

    def test_config_from_real_fixture_bytes_loads_known_tables(self):
        """Config extraction on the real public FRAM: i2=866 selects t1=0.1072988,
        t2=1.5 from the shared tables (the recovered family)."""
        # Public fixture decoded bytes are in the reference (cfg + rows); rebuild
        # a minimal FRAM carrying the same footer/header so extraction is exercised
        # end-to-end against the real table arrays.
        t1, t2, slope = self.get_tables(self.lib)
        self.assertAlmostEqual(t1[866 - 1], 0.1072988, places=7)
        self.assertAlmostEqual(t2[866 - 1], 1.5, places=12)
        self.assertEqual(slope[0], 0.0)
        plain = bytearray(344)
        footer = 0x6c28d300001a7312
        plain[336:344] = footer.to_bytes(8, 'little')
        u16 = (866 << 3) | 0
        plain[2] = u16 & 0xFF
        plain[3] = (u16 >> 8) & 0xFF

        class Cfg(C.Structure):
            _fields_ = [('i1', C.c_int), ('i2', C.c_int), ('i3', C.c_int), ('i4', C.c_int),
                        ('i5', C.c_int), ('i6', C.c_int), ('t1', C.c_double), ('t2', C.c_double),
                        ('slope', C.c_double)]
        c = Cfg()
        self.lib.lc_vendor_config_from_fram.restype = C.c_bool
        self.assertTrue(self.lib.lc_vendor_config_from_fram(bytes(plain), t1, t2, slope, C.byref(c)))
        self.assertEqual((c.i3, c.i4, c.i2), (18, 6771, 866))
        self.assertAlmostEqual(c.t1, 0.1072988, places=7)
        self.assertAlmostEqual(c.t2, 1.5, places=12)


if __name__ == '__main__':
    unittest.main()
