"""RED-first regression for auto-start scanning on app open.

Executes the ACTUAL application entry-point initialization with SDK shims and
asserts that scanning begins with NO input event after open. Also asserts the
completion / restart / cancel policies are unchanged (scan-until-tag-or-Back,
no attempt-count or time cutoff, Right restarts after a completed read, Back
exits outside scanning).

Host-only: synthetic SDK shims, not physical RF evidence.
"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AutoStart(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        app = (ROOT / 'app.c').read_text()
        # The entry point from its first line to end of file.
        cls.app = app
        entry = app.split('int32_t libre_capture_app(void* argument) {', 1)[1]
        state = app.split('typedef struct {', 1)[1].split('} AppState;', 1)[0]
        # Extract the main-thread helpers we need to exercise unchanged policy.
        def body(name, kind='void'):
            return app.split(f'static {kind} {name}(', 1)[1].split('\n}\n', 1)[0]
        cls.entry = entry

    def test_source_auto_starts_before_the_input_loop(self):
        """The entry point must call start_scan() during initialization, before
        the event loop reads the first key, so no OK press is required."""
        entry = self.app.split('int32_t libre_capture_app(void* argument) {', 1)[1]
        loop_at = entry.index('while(running)')
        start_at = entry.find('start_scan(app);')
        self.assertNotEqual(start_at, -1, 'entry point must auto-start a scan on open')
        self.assertLess(start_at, loop_at, 'auto-start must occur before the input loop')

    def test_no_extra_ok_press_required_and_back_still_exits(self):
        """Idle page must not be the entry state, and Back outside a scan exits."""
        entry = self.app.split('int32_t libre_capture_app(void* argument) {', 1)[1]
        # Back on the idle page is how the user leaves when nothing is scanning.
        self.assertIn('if(app->page == PageIdle) start_scan(app);', self.app)
        self.assertRegex(
            self.app,
            r'else running = false;',
            'Back outside scanning must still exit the app')

    def test_restart_and_cancel_policies_unchanged(self):
        app = self.app
        # Right (>) restarts a scan only from a completed result page.
        self.assertIn("key.key == InputKeyRight && app->page == PageResult", app)
        # Back during scanning cancels and returns to idle, no auto-save.
        self.assertIn('lc_transaction_cancel(&app->gate);', app)
        # No attempt-count or wall-clock cutoff may exist.
        self.assertNotIn('app->attempts >=', app)
        self.assertNotIn('elapsed >= furi_ms_to_ticks', app)
        self.assertIn('char* json = malloc(LC_JSON_CAPACITY);', app)

    def test_start_scan_resets_state_and_is_reentrant(self):
        """Compile the real start_scan with shims and assert it zeroes prior
        capture state, matching the existing feedback-test guarantee."""
        src = self._build_start_harness()
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as tmp:
            c = Path(tmp) / 'start.c'
            exe = Path(tmp) / 'start'
            c.write_text(src)
            subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic',
                            '-Wno-unused-function', '-Wno-unused-variable',
                            '-I' + str(ROOT), str(c), str(ROOT / 'protocol.c'),
                            str(ROOT / 'capture.c'), str(ROOT / 'transaction.c'),
                            str(ROOT / 'lifecycle.c'),
                            '-o', str(exe)], check=True)
            r = subprocess.run([str(exe)], capture_output=True)
            self.assertEqual(r.returncode, 0, r.stderr.decode())

    def _build_start_harness(self):
        app = self.app
        state = app.split('typedef struct {', 1)[1].split('} AppState;', 1)[0]

        def body(name, kind='void'):
            return app.split(f'static {kind} {name}(', 1)[1].split('\n}\n', 1)[0]

        return r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "capture.h"
#include "integrity.h"
#include "factory.h"
#include "vendor_chain.h"
#include "lifecycle.h"
#include "transaction.h"
#define FuriWaitForever 0
#define NfcProtocolIso15693_3 0
#define COUNT_OF(a) (sizeof(a)/sizeof((a)[0]))
typedef int FuriMutex; typedef int FuriMessageQueue; typedef int ViewPort;
typedef int Nfc; typedef int NfcPoller; typedef int BitBuffer; typedef int NotificationApp;
typedef enum { PageIdle, PageScan, PageResult } Page;
typedef int NotificationMessage;
typedef const NotificationMessage* NotificationSequence[];
static const NotificationMessage message_blue_255=9;
static const NotificationSequence sequence_blink_start_blue={NULL},sequence_blink_stop={NULL},sequence_reset_rgb={NULL};
typedef struct { STATE } AppState;
static unsigned starts; static bool locked, blinking;
static void furi_mutex_acquire(FuriMutex* m,int t){(void)m;(void)t;assert(!locked);locked=true;}
static void furi_mutex_release(FuriMutex* m){(void)m;assert(locked);locked=false;}
static uint32_t furi_get_tick(void){return 100;}
static void notification_message_block(NotificationApp* n,const NotificationSequence* s){
 (void)n;assert(!locked);
 if(s==&sequence_blink_start_blue){assert(!blinking);blinking=true;starts++;return;}
 if(s==&sequence_blink_stop){blinking=false;return;}
 /* sequence_reset_rgb */ }
static void nfc_poller_stop(NfcPoller* p){(void)p;assert(!locked);assert(!blinking);}
static void nfc_poller_free(NfcPoller* p){(void)p;}
static NfcPoller* nfc_poller_alloc(Nfc* n,int p){(void)n;(void)p;return (NfcPoller*)1;}
static void nfc_poller_start_ex(NfcPoller* p,int cb,AppState* a){(void)p;(void)cb;assert(a->started && blinking);}
static int poller_callback; /* stub name referenced by the real start_scan */
static void stop_scan(STOP
static void start_scan(START
int main(void){
 AppState a={0};
 a.started=false;a.poller=NULL;blinking=false;
 a.factory.status=LcFactoryEstimate;a.factory.mmol_tenths=1234;
 memset(a.capture.uid,0xaa,8);a.capture.uid_valid=true;a.uid_details=true;strcpy(a.saved_name,"old");
 start_scan(&a);
 assert(starts==1 && a.page==PageScan && a.started && !a.finished && !a.gate.cancelled);
 assert(a.factory.status==LcFactoryNotChecked && !a.factory.mmol_tenths && !a.details && !a.uid_details);
 const uint8_t zero[8]={0};assert(!a.capture.uid_valid && !memcmp(a.capture.uid,zero,8) && !a.saved_name[0]);
 stop_scan(&a);assert(!blinking);
 start_scan(&a);assert(starts==2 && !a.gate.cancelled && !a.capture.uid_valid);
 puts("PASS start_scan reset + reentrancy");return 0;
}
'''.replace('STOP', body('stop_scan') + '\n}').replace('START', body('start_scan') + '\n}').replace('STATE', state)


if __name__ == '__main__':
    unittest.main()
