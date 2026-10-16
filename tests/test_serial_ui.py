"""RED-first tests for the DERIVED sensor serial on the result screens.

Executes the ACTUAL draw callback compiled from app.c (same harness pattern as
test_uid_hint.py / test_status_ui.py) and measures the REAL pinned firmware
fonts. Host-only: this is not physical display, RF or clinical evidence.

The displayed identifier is the DERIVED sensor SERIAL (11 chars), computed from
the wire-order UID plus the patch-info family nibble per
glucose-research/SERIAL-IN-FRAM.md. It replaces the old at-a-glance 4-hex UID
hint. It is DERIVED from the UID, never read from FRAM.

NEUTRAL: when the serial cannot be derived (no UID, cancelled, unsupported tag
with no inventory) the row shows a neutral placeholder and NEVER a stale or
fabricated serial, and never a stale one after a sensor change.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FONT_REF = ROOT / 'tests/font-reference'

# Synthetic identities. Serial is derived from (uid, patch) with the leading
# character taken from the patch-info FAMILY NIBBLE (patch[2] >> 4).
UID_A = bytes.fromhex('11223344556607E0')   # -> 3DTAM8DT224 (family nibble 3)
UID_B = bytes.fromhex('5E90250000A007E0')   # public LibreTools UID with family nibble 3 -> 3M00009DHCR
SERIAL_A = '3DTAM8DT224'
SERIAL_B = '3M00009DHCR'
PATCH = bytes.fromhex('C50930010000')  # patch[2]=0x30 -> family nibble 3
NEUTRAL = ['Serial unavailable', 'Serial -----------', 'Serial ....']

ALPHABET = '0123456789ACDEFGHJKLMNPQRTUVWXYZ'


def derive_serial(uid_wire: bytes, patch: bytes) -> str:
    """Reference transform, transposed from SERIAL-IN-FRAM.md (test oracle)."""
    b = list(reversed(uid_wire))[-6:]
    five = [b[0] >> 3, (b[0] << 2) + (b[1] >> 6), b[1] >> 1, (b[1] << 4) + (b[2] >> 4),
            (b[2] << 1) + (b[3] >> 7), b[3] >> 2, (b[3] << 3) + (b[4] >> 5), b[4],
            b[5] >> 3, b[5] << 2]
    return str(patch[2] >> 4) + ''.join(ALPHABET[v & 0x1F] for v in five)


class SerialUI(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert derive_serial(bytes.fromhex('5E90250000A007E0'), bytes.fromhex('C50900010000')) == '0M00009DHCR'
        assert derive_serial(UID_B, PATCH) == SERIAL_B, derive_serial(UID_B, PATCH)
        assert derive_serial(UID_A, PATCH) == SERIAL_A, derive_serial(UID_A, PATCH)
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
#define PAGE_SEP "\x1ePAGE\x1e"
typedef int FuriMutex; typedef int FuriMessageQueue; typedef int ViewPort;
typedef int Nfc; typedef int NfcPoller; typedef int BitBuffer; typedef int NotificationApp; typedef int Canvas;
typedef enum { PageIdle,PageScan,PageResult } Page;
typedef enum { FontPrimary,FontSecondary } Font;
typedef struct { STATE } AppState;
static bool locked;
static void furi_mutex_acquire(FuriMutex* m,int w) {(void)m;(void)w;assert(!locked);locked=true;}
static void furi_mutex_release(FuriMutex* m) {(void)m;assert(locked);locked=false;}
static void canvas_clear(Canvas* c) {(void)c;assert(locked);}
static void canvas_set_font(Canvas* c,Font f) {(void)c;(void)f;}
static void canvas_draw_str(Canvas* c,int x,int y,const char* text) {
    (void)c;assert((x==0 || (x==98 && y==10)) && y>0 && y<=64 && locked);puts(text);
}
static void draw(DRAW puts(PAGE_SEP);
}
int main(int argc,char** argv) {
    (void)argc;(void)argv;
    AppState a={0};a.page=PageResult;
    assert(fread(a.capture.uid,1,8,stdin)==8);
    assert(fread(a.capture.patch,1,6,stdin)==6);
    assert(fread(a.capture.fram,1,344,stdin)==344);
    complete_capture(&a.capture);lc_integrity_check(&a.capture,&a.integrity);
    lc_factory_check(&a.capture,&a.factory);
    strcpy(a.status,"Raw 344 bytes; no glucose");strcpy(a.save_status,"Saved + verified on SD");
    const char* mode = argc>1 ? argv[1] : "estimate";
    if(!strcmp(mode,"estimate") || !strcmp(mode,"diagnostics")) {
        a.integrity.status=LcIntegrityPass; a.integrity.sensor.parse_status=LcSensorParsed;
        a.integrity.sensor.status_code=3; a.integrity.sensor.max_life_minutes=20854;
        a.integrity.sensor.age_minutes=2466;
        lc_life_compute(a.integrity.sensor.age_minutes,a.integrity.sensor.max_life_minutes,0,true,&a.life);
        if(!strcmp(mode,"estimate")) a.factory.status=LcFactoryEstimate;
        else {a.factory.status=LcFactoryRejected;a.details=true;}
        draw(NULL,&a);
    } else if(!strcmp(mode,"error")) {
        a.integrity.status=LcIntegrityFail;a.factory.status=LcFactoryRejected;
        strcpy(a.status,"Read error 7; partial");draw(NULL,&a);
    } else if(!strcmp(mode,"no_uid")) {
        a.capture.uid_valid=false;a.factory.status=LcFactoryRejected;
        a.integrity.status=LcIntegrityNotChecked;draw(NULL,&a);
    } else if(!strcmp(mode,"cancel_uid")) {
        a.gate.cancelled=true;a.integrity.status=LcIntegrityNotChecked;draw(NULL,&a);
    } else if(!strcmp(mode,"unsupported")) {
        a.capture.uid_valid=false;a.integrity.status=LcIntegrityUnsupported;
        strcpy(a.status,"Unsupported family/gen; stop");draw(NULL,&a);
    } else if(!strcmp(mode,"restart")) {
        a.integrity.status=LcIntegrityFail;a.factory.status=LcFactoryRejected;
        draw(NULL,&a);
        memset(&a.capture,0,sizeof(a.capture));
        memset(&a.integrity,0,sizeof(a.integrity));memset(&a.factory,0,sizeof(a.factory));
        a.integrity.status=LcIntegrityNotChecked;a.factory.status=LcFactoryRejected;
        draw(NULL,&a);
        a.capture.uid_valid=true;a.capture.patch_valid=true;
        const uint8_t next[8]={0xde,0xad,0xbe,0xef,0x00,0x00,0x77,0x88};
        memcpy(a.capture.uid,next,8);
        const uint8_t npat[6]={0xc5,0x09,0x30,0x00,0x00,0x00};memcpy(a.capture.patch,npat,6);
        a.integrity.status=LcIntegrityFail;
        a.factory.status=LcFactoryRejected;draw(NULL,&a);
        a.integrity.status=LcIntegrityPass;a.integrity.sensor.parse_status=LcSensorParsed;
        a.integrity.sensor.status_code=3;a.integrity.sensor.max_life_minutes=20854;
        a.integrity.sensor.age_minutes=2466;a.factory.status=LcFactoryEstimate;
        draw(NULL,&a);
        a.gate.cancelled=true;draw(NULL,&a);
    }
    assert(!locked);return 0;
}
'''.replace('STATE', state).replace('DRAW', draw)
        cls.tmp = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        c = Path(cls.tmp.name) / 'serialui.c'
        cls.exe = Path(cls.tmp.name) / 'serialui'
        c.write_text(source)
        san = ['-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie'] if os.environ.get('SANITIZE') else []
        subprocess.run(['gcc', *san, '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic', '-I' + str(ROOT), str(c),
                        str(ROOT / 'integrity.c'), str(ROOT / 'protocol.c'), str(ROOT / 'capture.c'),
                        str(ROOT / 'factory.c'), str(ROOT / 'lifecycle.c'), '-lm', '-o', str(cls.exe)], check=True)
        cls.w = Path(cls.tmp.name) / 'width'
        subprocess.run(['gcc', '-w', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        '-I' + str(FONT_REF),
                        *[str(FONT_REF / n) for n in ['width.c', 'fonts.c', 'u8g2_font.c', 'u8x8_8x8.c']],
                        '-o', str(cls.w)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_draw(self, mode, uid):
        from test_sensor_status import synthetic
        import sys
        sys.path.insert(0, str(ROOT / 'offline'))
        from libre2_offline import decrypt_fram
        raw = decrypt_fram(uid, PATCH, synthetic())
        r = subprocess.run([str(self.exe), mode], input=uid + PATCH + raw, capture_output=True)
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        return r.stdout.decode()

    def width(self, font, text):
        return int(subprocess.check_output([str(self.w), str(font), text], text=True).split(' ', 1)[0])

    def serial_line(self, text):
        return [l for l in text.splitlines() if l.startswith('Serial ')]

    def test_serial_appears_and_tracks_two_different_synthetic_wire_uids(self):
        pages = {SERIAL_A: self.run_draw('estimate', UID_A), SERIAL_B: self.run_draw('estimate', UID_B)}
        for serial, text in pages.items():
            self.assertIn(serial, text, 'derived serial must appear on the numeric result page')
        lines_a = self.serial_line(pages[SERIAL_A])
        lines_b = self.serial_line(pages[SERIAL_B])
        self.assertTrue(lines_a and lines_b, 'missing serial line')
        self.assertNotEqual(lines_a, lines_b, 'serial must differ for different sensors')

    def test_serial_leading_char_and_not_the_old_hint(self):
        text = self.run_draw('estimate', UID_B)
        joined = ' '.join(self.serial_line(text))
        self.assertIn(SERIAL_B, joined, 'derived serial 3M00009DHCR must appear')
        self.assertTrue(SERIAL_B[0] == '3' and SERIAL_B.startswith('3M0'))
        self.assertNotIn('0M00009DHCR', text, 'LibreTools patchInfo[0] table must NOT be used')
        # The old 4-hex prefix hint must be gone from the result rows.
        self.assertNotIn('ID ..5E90', text, 'old 4-hex hint must be replaced')
        self.assertNotIn('5E90250000A007E0', text, 'full 16-hex UID must not be on the result page')

    def test_serial_absent_or_neutral_when_uid_unavailable(self):
        for mode in ['no_uid', 'cancel_uid', 'unsupported']:
            text = self.run_draw(mode, UID_A)
            self.assertNotIn(SERIAL_A, text, f'{mode} must not show a stale serial')
            self.assertNotIn(SERIAL_B, text)
            self.assertTrue(any(n in text for n in NEUTRAL),
                            f'{mode} must be neutral or absent, got:\n{text}')

    def test_no_stale_serial_after_restart_or_new_sensor(self):
        text = self.run_draw('restart', UID_A)
        pages = [pg for pg in text.split('\x1ePAGE\x1e') if pg.strip()]
        self.assertEqual(len(pages), 5, 'restart scenario must render five pages')
        self.assertIn(SERIAL_A, pages[0])
        self.assertNotIn(SERIAL_A, pages[1])
        self.assertNotIn(SERIAL_B, pages[1])
        new_serial = derive_serial(bytes.fromhex('DEADBEEF00007788'), PATCH)
        self.assertIn(new_serial, pages[3])
        self.assertNotIn(SERIAL_A, pages[3], 'new sensor must not show previous serial')
        self.assertNotIn(new_serial, pages[4], 'cancel must not leave the new serial')

    def test_serial_does_not_remove_safety_labels_or_clock(self):
        for mode in ['estimate', 'diagnostics', 'error']:
            text = self.run_draw(mode, UID_B)
            self.assertNotIn('EXPERIMENTAL', text)   # banners removed by owner request
            if mode == 'estimate':
                for label in ['Freshness unknown', 'Serial ']:
                    self.assertIn(label, text)
                self.assertNotIn('Hist ', text)
            elif mode == 'diagnostics':
                # sensor-clock freshness from the same hand-set capture
                self.assertIn('Fresh 0m=now', text)
            else:
                # 'error' fails integrity, so no life struct is computed here
                self.assertIn('Freshness unknown', text)
            self.assertIn('--:--', text)

    def test_serial_width_fits_128px_and_avoids_the_clock(self):
        # The serial shares a dedicated row and may carry a nav suffix on the page.
        worst = 'Serial ' + '0' + 'W' * 10  # widest possible serial (digit + 10 widest glyphs)
        self.assertLessEqual(self.width(0, worst), 128)
        for row in ['Serial ' + SERIAL_B, 'Serial ' + SERIAL_A, 'Serial unavailable', worst]:
            self.assertLessEqual(self.width(0, row), 128, f'{row!r} overflows 128 px')
        # Combined safety rows actually used on the two pages must still fit.
        for combined in ['<:info >:scan', 'Serial unavailable  <:UID >:scan',
                         'Freshness unknown']:
            self.assertLessEqual(self.width(0, combined), 128, f'{combined!r} overflows')
        self.assertLessEqual(98 + self.width(0, '23:59'), 128)
        print('Measured pinned-font width of serial row, worst case:', self.width(0, worst), 'px')


if __name__ == '__main__':
    unittest.main()