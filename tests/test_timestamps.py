"""Synthetic clock vectors through real C serializer; no private captures or device I/O."""
from pathlib import Path
import ctypes as C
import calendar
import datetime as dt
import json
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

def raw(local):
    return calendar.timegm(dt.datetime.fromisoformat(local).timetuple())

class TimestampTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        shim = Path(cls.tmp.name)/'shim.c'
        shim.write_text('''#include "capture.h"
size_t serialize(uint32_t scan, char* out, size_t n) {
    LcCapture c = {0}; c.uid_valid = true; c.timestamp = scan;
    return lc_capture_json(&c, out, n);
}
''')
        lib = Path(cls.tmp.name)/'time.so'
        subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-pedantic','-shared','-fPIC','-I'+str(ROOT),str(shim),str(ROOT/'capture.c'),str(ROOT/'protocol.c'),'-o',str(lib)],check=True)
        cls.lib=C.CDLL(str(lib))
        cls.lib.serialize.argtypes=[C.c_uint32,C.c_void_p,C.c_size_t]
        cls.lib.serialize.restype=C.c_size_t
        cls.lib.lc_capture_local_datetime.argtypes=[C.c_uint32,C.c_void_p,C.c_size_t]
        cls.lib.lc_capture_local_datetime.restype=C.c_bool
        # Execute the pinned firmware's calendar conversion, with furi_check shim.
        sdk=ROOT/'tests/datetime-reference'
        furi=Path(cls.tmp.name)/'furi.h'
        furi.write_text('#include <assert.h>\n#define furi_check(x) assert(x)\n')
        firmware=Path(cls.tmp.name)/'firmware.so'
        subprocess.run(['gcc','-std=c11','-shared','-fPIC','-I'+cls.tmp.name,'-I'+str(sdk),str(sdk/'datetime.c'),'-o',str(firmware)],check=True)
        cls.firmware=C.CDLL(str(firmware))
        class DateTime(C.Structure):
            _fields_=[('hour',C.c_uint8),('minute',C.c_uint8),('second',C.c_uint8),('day',C.c_uint8),('month',C.c_uint8),('year',C.c_uint16),('weekday',C.c_uint8)]
        cls.DateTime=DateTime
        cls.firmware.datetime_timestamp_to_datetime.argtypes=[C.c_uint32,C.POINTER(DateTime)]
        cls.firmware.datetime_datetime_to_timestamp.argtypes=[C.POINTER(DateTime)]
        cls.firmware.datetime_datetime_to_timestamp.restype=C.c_uint32
    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()
    def capture(self,value):
        out=C.create_string_buffer(16384)
        self.assertGreater(self.lib.serialize(value,out,len(out)),0)
        return json.loads(out.value)
    def test_local_clock_not_falsely_utc_or_shifted_one_hour(self):
        local='2026-10-15T19:23:45'
        value=raw(local)
        data=self.capture(value)
        self.assertEqual(data.get('scan_local_datetime'),local)
        self.assertEqual(data['timestamp_unix'],value)
        self.assertEqual(data.get('scan_rtc_raw'),value)
        self.assertEqual(data.get('scan_timezone_status'),'unknown')
        self.assertIsNone(data.get('scan_utc_unix'))
        self.assertIsNone(data.get('scan_utc_offset_minutes'))
        self.assertEqual(data.get('timestamp_unix_semantics'),'legacy local-wall epoch encoding; not UTC')

    def test_matches_pinned_firmware_each_day_with_no_timezone_adjustment(self):
        for value in range(1704067200,2208988800,86400):
            value+=13*3600+37*60+29
            firmware=self.DateTime()
            self.firmware.datetime_timestamp_to_datetime(value,C.byref(firmware))
            local=f'{firmware.year:04}-{firmware.month:02}-{firmware.day:02}T{firmware.hour:02}:{firmware.minute:02}:{firmware.second:02}'
            self.assertEqual(self.capture(value)['scan_local_datetime'],local)
            self.assertEqual(self.firmware.datetime_datetime_to_timestamp(C.byref(firmware)),value)
    def test_null_and_short_format_buffers_fail_closed(self):
        value=raw('2026-10-15T19:23:45')
        self.assertFalse(self.lib.lc_capture_local_datetime(value,None,20))
        for size in range(20):
            out=C.create_string_buffer(b'x'*20,21)
            self.assertFalse(self.lib.lc_capture_local_datetime(value,out,size))
            self.assertEqual(out.value,b'x'*20)
    def test_local_date_rollover_and_leap_day(self):
        for local in ['2026-10-15T23:59:59','2026-10-16T00:00:00','2024-02-29T23:59:59','2024-03-01T00:00:00','2026-10-25T01:30:00']:
            self.assertEqual(self.capture(raw(local)).get('scan_local_datetime'),local)
        # A repeated DST wall hour has no inferable UTC identity.
        self.assertIsNone(self.capture(raw('2026-10-25T01:30:00')).get('scan_utc_unix'))
    def test_invalid_raw_preserved_but_calendar_and_legacy_null(self):
        for value in [0,1,1704067199,2208988801,4294967295]:
            data=self.capture(value)
            self.assertEqual(data.get('scan_rtc_raw'),value)
            self.assertIsNone(data.get('scan_local_datetime'))
            self.assertIsNone(data['timestamp_unix'])
            self.assertIsNone(data.get('scan_utc_unix'))
    def test_save_event_is_separate_from_acquisition(self):
        data=self.capture(raw('2026-10-15T23:59:59'))
        self.assertIn('save_local_datetime',data)
        self.assertIsNone(data['save_local_datetime'])
        self.assertEqual(data.get('save_time_event'),'save attempt started; not completion')

if __name__=='__main__':
    unittest.main()
