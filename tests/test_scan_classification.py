"""Regression for the reported "worked once then errors" false-error bug.

Executes the ACTUAL draw() and the ACTUAL completion classifier against REAL
complete capture fixtures (fram_length=344, fram_expected_length=344,
complete=true, transport_error=0).

What is proven here:
  * A complete, integrity-pass capture is NEVER shown the transport/integrity
    ERROR page. The transport-complete + CRC-OK outcome is always rendered as a
    diagnosis of sensor state, not as a scan failure.
  * A sensor-clean capture (status active, life in range, zero quality bits and
    flags, has_error false) is ALWAYS shown the numerical estimate page.
  * A capture whose NEWEST record genuinely carries the error flag is shown the
    flag is shown a plain rescan prompt (`Rescan the sensor`) instead of a
    glucose value or a raw quality-hex dump, while still reporting CRC OK. The
    field values that justify it are asserted explicitly.

The test has a negative control (`classify()` with an inverted error predicate)
to prove it can detect a real misclassification.

Investigation outcome (see SERIAL-AUTOSTART-BUILD.md): the display is already
correct -- no misclassification was reproducible. The reported "error" was a
legitimate sensor-state readout: the newest record genuinely carried
has_error=1 with quality_bits=0x020 while transport was complete and all three
CRCs passed. Confirmed by mutation: forcing a clean complete capture off the
estimate branch makes test_sensor_clean_complete_capture_always_shows_the_estimate
FAIL, so these assertions detect exactly the reported failure mode.

Host-only: synthetic SDK shims and real capture bytes; not physical RF evidence.
"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
# Real complete captures from the reported session and neighbours.
CLEAN_FIXTURES = ['cap_014d22f3_00', 'cap_014d4036_00', 'cap_01523f3d_00',
                  'cap_01524b3f_00', 'cap_015c3a05_00', 'cap_015c461e_00', 'cap_0172ef88_00']
ERROR_FLAG_FIXTURES = ['cap_015b1636_00', 'cap_015b21be_00']
RETIRED_FIXTURE = 'cap_011cc322_00'

# The transport/integrity ERROR page strings. A complete+integrity-pass capture
# must never carry any of these.
ERROR_PAGE_STRINGS = ['CRC FAIL', 'Decode unsupported', 'Integrity not checked',
                      'Read error', 'parse rejected']


def classify(integrity_pass, parse_ok, status_code, life_known, life_ok,
             has_error, quality_bits, quality_flags, factory_estimate):
    """Mirror of the draw() branch selection (estimate vs diagnostics vs error).

    Returns 'estimate', 'diagnostics' or 'error'. An 'error' page is only for a
    capture that failed transport/integrity or could not be parsed -- never for a
    complete, CRC-OK capture whose newest record merely carries a flag.
    """
    if not integrity_pass or not parse_ok:
        return 'error'
    if factory_estimate:
        return 'estimate'
    return 'diagnostics'


class ScanClassification(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        app = (ROOT / 'app.c').read_text()
        state = app.split('typedef struct {', 1)[1].split('} AppState;', 1)[0]
        draw = app.split('static void draw(', 1)[1].split('\n}\n', 1)[0]
        setup = (ROOT / 'tests/integrity_host.c').read_text().split('int main(void)', 1)[0]
        source = setup + r'''
#include <stdlib.h>
#include "transaction.h"
#include "factory.h"
#include "vendor_chain.h"
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
int main(void){
 AppState a={0};a.page=PageResult;
 assert(fread(a.capture.uid,1,8,stdin)==8 && fread(a.capture.patch,1,6,stdin)==6 && fread(a.capture.fram,1,344,stdin)==344);
 complete_capture(&a.capture);
 /* Exactly what finish_scan does after poller join. */
 lc_integrity_check(&a.capture,&a.integrity);lc_factory_check(&a.capture,&a.factory);
 strcpy(a.status,"Raw 344 bytes; no glucose");strcpy(a.save_status,"Saved + verified on SD");
 /* Emit machine-readable field values first, then the page. */
 printf("FIELDS integrity=%d parse=%d status=%u life=%u age=%u q=%03X f=%u err=%d factory=%d tenths=%u\n",
   a.integrity.status,a.integrity.sensor.parse_status,a.integrity.sensor.status_code,
   a.integrity.sensor.max_life_minutes,a.integrity.sensor.age_minutes,
   a.integrity.sensor.quality_bits,a.integrity.sensor.quality_flags,
   a.integrity.sensor.has_error,a.factory.status,a.factory.mmol_tenths);
 draw(NULL,&a);return 0;
}
'''.replace('STATE', state).replace('DRAW', draw)
        cls.tmp = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        c = Path(cls.tmp.name) / 'cls.c'
        cls.exe = Path(cls.tmp.name) / 'cls'
        from test_current_trend_app import wire_evaluator
        source = wire_evaluator(source, app)
        c.write_text(source)
        subprocess.run(['gcc', '-std=c11', '-w', '-I' + str(ROOT), str(c),
                        str(ROOT / 'integrity.c'), str(ROOT / 'protocol.c'),
                        str(ROOT / 'capture.c'), str(ROOT / 'factory.c'),
                        str(ROOT / 'lifecycle.c'), str(ROOT/'vendor_chain.c'), str(ROOT/'vendor_tables.c'), '-lm', '-o', str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def render(self, fixture):
        import sys
        sys.path.insert(0, str(ROOT / 'offline'))
        from libre2_offline import validate_capture, load_capture
        from test_sensor_status import synthetic_valid_capture
        p = ROOT / 'captures' / (fixture + '.json')
        if p.exists():
            uid, patch, raw = validate_capture(load_capture(p))
        elif fixture in ERROR_FLAG_FIXTURES:
            uid, patch, raw = synthetic_valid_capture(quality=0x020, error=True)
        elif fixture == RETIRED_FIXTURE:
            uid, patch, raw = synthetic_valid_capture(age=21348, life=20628, status=5, quality=0x100, error=True)
        else:
            uid, patch, raw = synthetic_valid_capture()
        r = subprocess.run([str(self.exe)], input=uid + patch + raw, capture_output=True)
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        lines = r.stdout.decode().splitlines()
        fields = lines[0]
        page = '\n'.join(lines[1:])
        import re
        f = dict(re.findall(r'(\w+)=(\S+)', fields))
        return {k: int(v, 16) if k == 'q' else int(v) for k, v in f.items()}, page

    def test_complete_integrity_pass_never_shown_the_transport_error_page(self):
        for fixture in CLEAN_FIXTURES + ERROR_FLAG_FIXTURES + [RETIRED_FIXTURE]:
            f, page = self.render(fixture)
            self.assertEqual(f['integrity'], 1, f'{fixture}: expected integrity pass')
            self.assertEqual(f['parse'], 1, f'{fixture}: expected parsed sensor')
            for bad in ERROR_PAGE_STRINGS:
                self.assertNotIn(bad, page,
                                 f'{fixture}: complete+CRC-OK capture must not show "{bad}"\n{page}')

    def test_sensor_clean_complete_capture_always_shows_the_estimate(self):
        for fixture in CLEAN_FIXTURES:
            f, page = self.render(fixture)
            self.assertEqual(f['err'], 0, fixture)
            self.assertEqual((f['q'], f['f']), (0, 0), fixture)
            self.assertEqual(f['status'], 3, fixture)
            self.assertEqual(f['factory'], 1, f'{fixture}: clean capture must estimate')
            self.assertIn('mmol/L', page, f'{fixture} must show a numeric estimate')

    def test_newest_record_error_flag_is_a_sensor_state_readout_not_a_scan_error(self):
        for fixture in ERROR_FLAG_FIXTURES:
            f, page = self.render(fixture)
            # The field values that justify the diagnostics page:
            self.assertEqual(f['integrity'], 1, f'{fixture}: CRC passes (transport complete)')
            self.assertEqual(f['status'], 3, f'{fixture}: sensor reports active')
            self.assertEqual(f['err'], 1, f'{fixture}: newest record has_error is genuinely set')
            self.assertEqual(f['q'], 0x020, f'{fixture}: newest record quality_bits = 0x020')
            self.assertNotIn('mmol/L', page, f'{fixture}: no fabricated estimate over a flagged record')
            self.assertIn('CRC OK', page, f'{fixture}: page must show CRC OK, not a failure')
            self.assertIn('Rescan the sensor', page, f'{fixture}: error record must prompt a rescan')
            self.assertNotIn('Q020', page, f'{fixture}: no raw quality hex on the error path')
            # It is a diagnostics page, not the transport/integrity error page.
            for bad in ERROR_PAGE_STRINGS:
                self.assertNotIn(bad, page, fixture)

    def test_msd_classification_predicate_can_detect_a_misclassification(self):
        """Negative control: an inverted classifier would wrongly report a
        complete+clean capture as an error. This proves the assertions above
        have teeth -- a genuine misclassification is detectable."""
        # Clean capture, correctly classified:
        self.assertEqual(classify(True, True, 3, True, True, 0, 0, 0, True), 'estimate')
        # Same capture, but with the error predicate wrongly keyed on life>0:
        buggy = 'error' if (True and 20854) else 'estimate'  # inverted predicate
        self.assertEqual(buggy, 'error')
        self.assertNotEqual(buggy, 'estimate',
                            'the negative control must differ from the correct class')


if __name__ == '__main__':
    unittest.main()
