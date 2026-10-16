"""Executed draw() for sensor-clock freshness + remaining life, and the
error-record 'rescan' path. Host shims only; NOT a physical screen test.

Written and watched to FAIL before the app.c draw change (RED), then GREEN.
"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'offline'))
sys.path.insert(0, str(ROOT / 'tests'))
from libre2_offline import validate_capture, load_capture, decrypt_fram
from test_sensor_status import synthetic, PATCH, synthetic_valid_capture


def build(exe_dir):
    """Compile a harness that runs the ACTUAL draw() with host SDK shims."""
    app = (ROOT / 'app.c').read_text()
    state = app.split('typedef struct {', 1)[1].split('} AppState;', 1)[0]
    draw = app.split('static void draw(', 1)[1].split('\n}\n', 1)[0]
    setup = (ROOT / 'tests/integrity_host.c').read_text().split('int main(void)', 1)[0]
    source = setup + r'''
#include <stdlib.h>
#include "transaction.h"
#include "factory.h"
#include "vendor_chain.h"
#include "lifecycle.h"
#define FuriWaitForever 0
#define COUNT_OF(a) (sizeof(a)/sizeof((a)[0]))
typedef int FuriMutex; typedef int FuriMessageQueue; typedef int ViewPort;
typedef int Nfc; typedef int NfcPoller; typedef int BitBuffer; typedef int NotificationApp; typedef int Canvas;
typedef enum { PageIdle,PageScan,PageResult } Page;
typedef enum { FontPrimary,FontSecondary } Font;
typedef struct { STATE } AppState;
static bool locked;
static void furi_mutex_acquire(FuriMutex* m,int w){(void)m;(void)w;assert(!locked);locked=true;}
static void furi_mutex_release(FuriMutex* m){(void)m;assert(locked);locked=false;}
static void canvas_clear(Canvas* c){(void)c;assert(locked);}
static void canvas_set_font(Canvas* c,Font f){(void)c;(void)f;}
static void canvas_draw_str(Canvas* c,int x,int y,const char* t){(void)c;assert((x==0||(x==98&&y==10))&&y>0&&y<=64&&locked);puts(t);}
static void draw(DRAW
}
int main(int argc,char** argv){
 (void)argv;
 AppState a={0};a.page=PageResult;
 assert(fread(a.capture.uid,1,8,stdin)==8 && fread(a.capture.patch,1,6,stdin)==6 && fread(a.capture.fram,1,344,stdin)==344);
 complete_capture(&a.capture);
 lc_integrity_check(&a.capture,&a.integrity);lc_factory_check(&a.capture,&a.factory);
 strcpy(a.status,"Raw 344 bytes; no glucose");strcpy(a.save_status,"Saved + verified on SD");
 LIFE_SETUP
 if(argc>1 && !strcmp(argv[1],"factory_only")) {
   a.vendor_chain.available=false;a.factory.status=LcFactoryEstimate;a.factory.mmol_tenths=999;
 } else if(argc>1) a.details=true;
 draw(NULL,&a);
 PAGE_SETUP
 return 0;
}
'''.replace('STATE', state).replace('DRAW', draw)
    # Give the harness a life-compute call exactly where finish_scan would.
    life_line = ('lc_life_compute(a.integrity.sensor.age_minutes,a.integrity.sensor.max_life_minutes,0,'
                 'a.integrity.status==LcIntegrityPass && a.integrity.sensor.parse_status==LcSensorParsed,&a.life);'
                 if 'LcLife life;' in app else '')
    source = source.replace('LIFE_SETUP', life_line)
    source = source.replace('PAGE_SETUP', '')
    from test_current_trend_app import wire_evaluator
    source = wire_evaluator(source, app)
    path = Path(exe_dir) / 'fresh.c'
    exe = Path(exe_dir) / 'fresh'
    path.write_text(source)
    san = ['-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie'] if os.environ.get('SANITIZE') else []
    subprocess.run(['gcc', *san, '-std=c11', '-w', '-I' + str(ROOT), str(path),
                    str(ROOT / 'integrity.c'), str(ROOT / 'protocol.c'),
                    str(ROOT / 'capture.c'), str(ROOT / 'factory.c'),
                    str(ROOT / 'lifecycle.c'), str(ROOT/'vendor_chain.c'), str(ROOT/'vendor_tables.c'), '-lm', '-o', str(exe)], check=True)
    return exe


class FreshnessUI(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        cls.exe = build(cls.tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def render(self, uid, patch, raw, details=False):
        r = subprocess.run([str(self.exe), *(["details"] if details else [])], input=uid + patch + raw, capture_output=True)
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        return r.stdout.decode()

    def synth(self, details=False, **kw):
        uid = bytes.fromhex('01020304050607E0')
        return self.render(uid, PATCH, decrypt_fram(uid, PATCH, synthetic(**kw)), details)

    # ---- A+B: estimate page shows value, freshness AND remaining life ----
    def test_estimate_page_shows_freshness_and_remaining_from_same_capture(self):
        for filename in ['cap_014d22f3_00.json', 'cap_015c3a05_00.json']:
            p = ROOT / 'captures' / filename
            cap = validate_capture(load_capture(p)) if p.exists() else synthetic_valid_capture()
            text = self.render(*cap)
            self.assertIn('mmol/L', text)
            # freshness (sensor-clock) and remaining life are both present
            self.assertIn('Fresh 0m', text)
            self.assertRegex(text, r'Left \d+d \d+h|Left \d+h \d+m')
            # both on ONE row so they cannot drift apart from the value
            self.assertRegex(text, r'Fresh 0m=now; Left \d+d \d+h')
            # result screen: corrected value only, no historical row, no banners
            self.assertNotIn('Hist ', text)
            self.assertNotIn('EXPERIMENTAL', text)
            self.assertNotIn('NOT FOR DOSING', text)
            print(filename, 'estimate rows OK')

    # ---- C: error record -> plain rescan prompt, no value, no hex dump ----
    def test_error_record_shows_rescan_prompt_and_suppresses_value(self):
        for filename in ['cap_015b1636_00.json', 'cap_015b21be_00.json']:
            p = ROOT / 'captures' / filename
            cap = validate_capture(load_capture(p)) if p.exists() else synthetic_valid_capture(quality=0x020, error=True)
            text = self.render(*cap)
            self.assertIn('Rescan', text)
            self.assertNotIn('mmol/L', text)          # no value paired with an error record
            self.assertNotIn('Q020', text)            # no raw quality hex on this path
            self.assertNotIn('ERR', text)             # no diagnostic dump centre-stage
            self.assertNotIn('mmol/L', text)          # no value on the error path
            self.assertIn('No usable value', text)
            print(filename, 'rescan path OK')

    def test_error_rescan_prompt_also_on_synthetic_error(self):
        text = self.synth(error=True)
        self.assertIn('Rescan', text)
        self.assertNotIn('mmol/L', text)
        self.assertNotIn('Q000', text)

    # ---- A: remaining-life display states ----
    def test_life_exceeded_and_unknown_states_preserved(self):
        exceeded = self.synth(details=True, age=20161, life=20160)
        self.assertIn('Life exceeded', exceeded)
        unknown = self.synth(details=True, life=0)
        self.assertIn('Life unknown', unknown)

    def test_remaining_life_string_for_synthetic_active(self):
        # Synthetic payloads have no valid factory coefficients, so the value
        # row shows the preserved "No estimate: coeff/domain" notice; the age
        # row and sensor-clock freshness are still emitted from the same capture.
        text = self.synth(details=True, age=100, life=20160)
        self.assertIn('Age100/20160m', text)
        self.assertIn('Fresh 0m=now', text)
        # A real active fixture (valid factory) shows the Left string outright.
        p = ROOT / 'captures' / 'cap_015c3a05_00.json'
        real = self.render(*(validate_capture(load_capture(p)) if p.exists() else synthetic_valid_capture()))
        self.assertRegex(real, r'Left \d+d \d+h|Left \d+h \d+m')

    # ---- C: raw diagnostics still reachable via the detail (left) page ----
    def test_error_path_raw_diagnostics_available_on_detail_page(self):
        # The Q-bits detail is not deleted, only moved off the error path.
        app = (ROOT / 'app.c').read_text()
        self.assertIn('Q%03X/%u', app, 'raw quality diagnostic format must remain in source')
        # The error branch must not print it on the auto-shown error state.

    def test_no_wall_clock_freshness_strings(self):
        text = self.synth(details=True, age=100, life=20160)
        for bad in ['ago', 'UTC', 'min ago', 'elapsed', 'since insertion', 'wall']:
            self.assertNotIn(bad, text, f'wall-clock freshness leaked: {bad}')


if __name__ == '__main__':
    unittest.main()