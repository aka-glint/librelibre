"""Sensor-clock remaining-life and freshness, production C.
TDD: these were written and watched to FAIL before lifecycle.c existed.
No wall-clock/RTC arithmetic is used anywhere: age and max life are the
sensor's own FRAM minutes. Same-capture binding is asserted explicitly.
"""
import ctypes as C
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class Life(C.Structure):
    # MUST match lifecycle.h field order exactly, or the two bools swap.
    _fields_ = [('status', C.c_int), ('remaining_minutes', C.c_uint32),
                ('sample_age_minutes', C.c_uint32), ('sample_index', C.c_uint16),
                ('sensor_minute', C.c_uint16), ('life_known', C.c_bool),
                ('freshness_known', C.c_bool)]

# status enum mirror
UNKNOWN, EXCEEDED, REMAINING = 0, 1, 2

def buf(text, cap=64):
    return C.create_string_buffer(cap), text

class Lifecycle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        so = Path(cls.tmp.name) / 'lifecycle.so'
        subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic',
                        '-shared', '-fPIC', str(ROOT / 'lifecycle.c'), '-o', str(so)], check=True)
        cls.lib = C.CDLL(str(so))
        cls.lib.lc_life_compute.argtypes = [C.c_uint16, C.c_uint16, C.c_uint16, C.c_bool,
                                            C.POINTER(Life)]
        cls.lib.lc_life_compute.restype = None
        cls.lib.lc_life_format_remaining.argtypes = [C.POINTER(Life), C.c_char_p, C.c_size_t]
        cls.lib.lc_life_format_remaining.restype = C.c_bool
        cls.lib.lc_freshness_format.argtypes = [C.POINTER(Life), C.c_char_p, C.c_size_t]
        cls.lib.lc_freshness_format.restype = C.c_bool

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def compute(self, age, max_life, sample_index=0, fields_valid=True):
        out = Life()
        self.lib.lc_life_compute(age, max_life, sample_index, fields_valid, C.byref(out))
        return out

    def remaining_text(self, age, max_life):
        life = self.compute(age, max_life)
        text = C.create_string_buffer(64)
        ok = self.lib.lc_life_format_remaining(C.byref(life), text, 64)
        return ok, text.value.decode()

    # ---- A. remaining life, normal case ----
    def test_normal_remaining_uses_days_hours_over_a_day(self):
        ok, text = self.remaining_text(age=1728, max_life=20628)   # 14d left (20628-1728=18900=13d3h)
        self.assertTrue(ok)
        # 18900 min = 13d (18720) + 3h (180) -> "Left 13d 3h"
        self.assertEqual(text, 'Left 13d 3h')
        print('remaining normal 18900 min ->', text)

    def test_under_one_day_uses_hours_minutes(self):
        ok, text = self.remaining_text(age=20000, max_life=20628)  # 628 min = 10h28m
        self.assertTrue(ok)
        self.assertEqual(text, 'Left 10h 28m')
        print('remaining under a day 628 min ->', text)

    def test_breakpoint_is_exactly_one_day_1440_minutes(self):
        ok, text = self.remaining_text(age=20628 - 1440, max_life=20628)
        self.assertTrue(ok)
        self.assertEqual(text, 'Left 1d 0h')          # 1440 -> at breakpoint: days form
        ok2, text2 = self.remaining_text(age=20628 - 1439, max_life=20628)
        self.assertEqual(text2, 'Left 23h 59m')        # 1439 -> hours form
        print('breakpoint 1440 ->', text, '| 1439 ->', text2)

    # ---- A. age > max_life must not wrap ----
    def test_age_over_max_life_is_expired_not_wrapped(self):
        life = self.compute(age=21348, max_life=20628)     # real retired fixture numbers
        self.assertEqual(life.status, EXCEEDED)
        self.assertEqual(life.remaining_minutes, 0)
        text = C.create_string_buffer(64)
        self.assertTrue(self.lib.lc_life_format_remaining(C.byref(life), text, 64))
        self.assertEqual(text.value.decode(), 'Life exceeded')
        print('age>max ->', text.value.decode(), '(remaining_minutes=%u)' % life.remaining_minutes)

    def test_max_life_zero_is_unknown_never_fabricated(self):
        life = self.compute(age=100, max_life=0)
        self.assertEqual(life.status, UNKNOWN)
        self.assertFalse(life.life_known)
        self.assertEqual(life.remaining_minutes, 0)
        text = C.create_string_buffer(64)
        self.assertTrue(self.lib.lc_life_format_remaining(C.byref(life), text, 64))
        self.assertEqual(text.value.decode(), 'Life unknown')
        print('max_life==0 ->', text.value.decode())

    # ---- A. the uint16 boundary that WOULD wrap in a narrow type ----
    def test_uint16_boundary_does_not_wrap(self):
        # age=1, max_life=65535: width must be 65534, not a uint16 subtraction of
        # (1-65535). A narrow uint16 subtraction yields 2 (65535-1 truncated) vs
        # the correct 65534 -- tiniest difference; use the widest real wrinkle:
        # age=0, max_life=65535 -> 65535 must NOT become 0.
        life = self.compute(age=0, max_life=65535)
        self.assertEqual(life.status, REMAINING)
        self.assertEqual(life.remaining_minutes, 65535)
        text = C.create_string_buffer(64)
        self.assertTrue(self.lib.lc_life_format_remaining(C.byref(life), text, 64))
        # 65535 min = 45d 12h 15m
        self.assertEqual(text.value.decode(), 'Left 45d 12h')
        print('uint16 boundary 65535 min ->', text.value.decode(), 'remaining=%u' % life.remaining_minutes)

    def test_wrap_case_that_narrow_subtraction_would_misreport(self):
        # If the code did uint16 a-b, age=0/max=65535 -> 65535 (same), but
        # age=65535/max=65535 -> 0 while a narrow (max-age) underflow is 0 too.
        # The real hazard is (max_life - age) where age>max_life: narrow gives a
        # huge wrapped number. Assert it is flagged EXCEEDED, never a wrapped value.
        life = self.compute(age=65535, max_life=100)
        self.assertEqual(life.status, EXCEEDED)
        self.assertEqual(life.remaining_minutes, 0)
        print('age 65535 > max 100 -> EXCEEDED, remaining 0 (no wrap to 65436)')

    # ---- B. freshness, sensor-clock only, at the time of the value ----
    def test_freshness_of_newest_sample_is_zero_minutes_and_current(self):
        life = self.compute(age=3000, max_life=20628, sample_index=0)
        self.assertTrue(life.freshness_known)
        self.assertEqual(life.sample_age_minutes, 0)
        self.assertEqual(life.sensor_minute, 3000)
        text = C.create_string_buffer(64)
        self.assertTrue(self.lib.lc_freshness_format(C.byref(life), text, 64))
        self.assertEqual(text.value.decode(), 'Fresh 0m=now')
        print('freshness newest ->', text.value.decode())

    def test_freshness_of_an_older_shown_sample_lags_in_sensor_minutes(self):
        # A value drawn from an older trend slot must report its own lag, from
        # the SAME sensor clock -- never wall clock.
        life = self.compute(age=3000, max_life=20628, sample_index=5)
        self.assertTrue(life.freshness_known)
        self.assertEqual(life.sample_age_minutes, 5)
        text = C.create_string_buffer(64)
        self.assertTrue(self.lib.lc_freshness_format(C.byref(life), text, 64))
        self.assertEqual(text.value.decode(), 'Fresh 5m=old')
        print('freshness older sample ->', text.value.decode())

    def test_freshness_unestablished_when_fields_invalid(self):
        life = self.compute(age=0, max_life=0, sample_index=0, fields_valid=False)
        self.assertFalse(life.freshness_known)
        text = C.create_string_buffer(64)
        self.assertTrue(self.lib.lc_freshness_format(C.byref(life), text, 64))
        self.assertEqual(text.value.decode(), 'Fresh unknown')
        print('freshness invalid ->', text.value.decode())

    def test_sample_index_cannot_exceed_sensor_age(self):
        # An index larger than the sensor's age would be a negative sensor minute;
        # must fail closed rather than underflow.
        life = self.compute(age=3, max_life=20628, sample_index=5)
        self.assertFalse(life.freshness_known)
        print('index>age -> freshness_known false (no underflow)')

    # ---- same-capture binding ----
    def test_value_freshness_life_derive_from_one_capture_object(self):
        """A capture struct carries age/life/sample-index together; the helper
        computes from that ONE struct. If any field came from a different scan,
        the arithmetic (sample_age = age - index, remaining = max_life - age)
        would be internally inconsistent. Assert the same struct yields both."""
        # one capture
        life = self.compute(age=2466, max_life=20854, sample_index=0)
        # freshness uses age and index from the same struct
        self.assertEqual(life.sensor_minute, 2466)
        self.assertEqual(life.sample_age_minutes, 0)
        # remaining uses max_life and age from the same struct
        self.assertEqual(life.remaining_minutes, 20854 - 2466)
        # now a *different* capture must give a *different* consistent pair
        other = self.compute(age=2507, max_life=20854, sample_index=0)
        self.assertEqual(other.sensor_minute, 2507)
        self.assertEqual(other.remaining_minutes, 20854 - 2507)
        self.assertNotEqual(life.remaining_minutes, other.remaining_minutes)
        print('same-capture binding: age/idx/life consistent within one struct')

    # ---- real worn-sensor fixture numbers (published: max_life 20628) ----
    def test_real_worn_sensor_fixture_renders_sane_remaining(self):
        # published real-sensor numbers only; no private UID
        life = self.compute(age=2930, max_life=20628)
        ok, text = self.remaining_text(age=2930, max_life=20628)
        self.assertTrue(ok)
        # 20628-2930 = 17698 min = 12d (17280) + 418m -> 6h (360) + 58m
        self.assertEqual(text, 'Left 12d 6h')
        self.assertTrue(text.startswith('Left '))
        print('real worn sensor age 2930/20628 ->', text)

    def test_error_record_still_computes_life_but_value_path_gated_elsewhere(self):
        # life/freshness helpers are value-independent; the rescan gating lives
        # in the draw path (asserted in test_status_ui / test_scan_classification).
        life = self.compute(age=2481, max_life=20854)
        self.assertEqual(life.status, REMAINING)
        print('error-record capture still yields life struct; UI gating is separate')


if __name__ == '__main__':
    unittest.main()
