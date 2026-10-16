"""Compile the actual app RF boundary with host SDK shims; no NFC simulation claim."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
app = (root / "app.c").read_text()
state = app.split("typedef struct {", 1)[1].split("} AppState;", 1)[0]
send = app.split("static Iso15693_3Error send_read(", 1)[1].split("\n}\n", 1)[0]
gate_header = '#include "transaction.h"\n'
cancel = "lc_transaction_cancel(&app.gate);"
source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "capture.h"
#include "integrity.h"
#include "factory.h"
#include "integrity.h"
#include "factory.h"
#include "vendor_chain.h"
#include "lifecycle.h"
GATE_HEADER
#define FuriWaitForever 0
#define FRAME_WAIT_FC 270000U
typedef int FuriMutex;
typedef int FuriMessageQueue;
typedef int ViewPort;
typedef int Nfc;
typedef int NfcPoller;
typedef int NotificationApp;
typedef int BitBuffer;
typedef int Iso15693_3Poller;
typedef enum { PageIdle, PageScan, PageResult } Page;
typedef enum { Iso15693_3ErrorNone, Iso15693_3ErrorNotSupported, Iso15693_3ErrorTimeout } Iso15693_3Error;
typedef struct { STATE } AppState;
static unsigned frames;
static bool locked;
static AppState* cancel_during_io;
static void furi_mutex_acquire(FuriMutex* m, int wait) { (void)m; (void)wait; assert(!locked); locked=true; }
static void furi_mutex_release(FuriMutex* m) { (void)m; assert(locked); locked=false; }
static void bit_buffer_reset(BitBuffer* b) { (void)b; }
static void bit_buffer_append_bytes(BitBuffer* b, const uint8_t* c, size_t n) { (void)b; assert(lc_command_allowed(c,n)); }
static Iso15693_3Error iso15693_3_poller_send_frame(Iso15693_3Poller* p, BitBuffer* tx, BitBuffer* rx, unsigned wait) {
    (void)p; (void)tx; (void)rx; (void)wait;
    assert(!locked); /* Never hold GUI/cancel mutex during SDK I/O. */
    frames++;
    if(cancel_during_io) {
        furi_mutex_acquire(cancel_during_io->mutex,FuriWaitForever);
        lc_transaction_cancel(&cancel_during_io->gate);
        furi_mutex_release(cancel_during_io->mutex);
    }
    return Iso15693_3ErrorNone;
}
static Iso15693_3Error send_read(SEND
}
int main(void) {
    AppState app = {0};
    const uint8_t patch[3] = {0x02,0xa1,0x07};
    /* Callback previously observed cancel=false; cancel wins before RF commit. */
    furi_mutex_acquire(app.mutex,FuriWaitForever);
    CANCEL
    furi_mutex_release(app.mutex);
    (void)send_read(&app,NULL,patch,3);
    assert(frames == 0 && "cancel before commit must suppress actual app RF send");
    puts("PASS actual app send_read: cancel between snapshot and RF commitment suppresses frame");
    app.gate = (LcTransactionGate){0};
    cancel_during_io = &app;
    assert(send_read(&app,NULL,patch,3) == Iso15693_3ErrorNone);
    assert(frames == 1 && app.gate.cancelled);
    assert(send_read(&app,NULL,patch,3) == Iso15693_3ErrorTimeout);
    assert(frames == 1);
    puts("PASS commit first: in-flight completion allowed; cancellation suppresses next frame");
    app.gate = (LcTransactionGate){0};
    cancel_during_io = NULL;
    const uint8_t read[4] = {2,0x23,42,0};
    assert(send_read(&app,NULL,read,4) == Iso15693_3ErrorNone);
    assert(frames == 2);
    const uint8_t forbidden[4] = {2,0xa1,7,0};
    assert(send_read(&app,NULL,forbidden,4) == Iso15693_3ErrorNotSupported);
    assert(frames == 2);
    app.finished = true;
    assert(send_read(&app,NULL,patch,3) == Iso15693_3ErrorTimeout);
    assert(frames == 2);
    puts("PASS finished scan cannot commit a frame");
    return 0;
}
'''.replace('GATE_HEADER', gate_header).replace('STATE', state).replace('SEND', send).replace('CANCEL', cancel)
with tempfile.TemporaryDirectory(prefix='libre-boundary-', dir=os.environ.get('TMPDIR', None)) as tmp:
    c = Path(tmp) / 'boundary.c'
    exe = Path(tmp) / 'boundary'
    c.write_text(source)
    sources = [str(root / 'protocol.c'), str(root / 'lifecycle.c')]
    if gate_header:
        sources.append(str(root / 'transaction.c'))
    sanitizer = ['-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if os.environ.get('SANITIZE') else []
    subprocess.run(['gcc', *sanitizer, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-pedantic', '-I'+str(root), str(c), *sources, '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
