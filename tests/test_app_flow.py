"""Compile real callback/send boundary with SDK shims, synthetic RF responses.
This exercises application control flow, not physical NFC or SDK reception.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
app = (root/'app.c').read_text()
state = app.split('typedef struct {',1)[1].split('} AppState;',1)[0]
send = app.split('static Iso15693_3Error send_read(',1)[1].split('\n}\n',1)[0]
callback = app.split('static NfcCommand poller_callback(',1)[1].split('\n}\n',1)[0]
source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "capture.h"
#include "integrity.h"
#include "factory.h"
#include "vendor_chain.h"
#include "integrity.h"
#include "factory.h"
#include "lifecycle.h"
#include "transaction.h"
#define FuriWaitForever 0
#define FRAME_WAIT_FC 270000U
#define COUNT_OF(a) (sizeof(a)/sizeof((a)[0]))
typedef int FuriMutex;
typedef int FuriMessageQueue;
typedef int ViewPort;
typedef int Nfc;
typedef int NfcPoller;
typedef int NotificationApp;
typedef int Iso15693_3Poller;
typedef struct { uint8_t bytes[256]; size_t length; } BitBuffer;
typedef enum { PageIdle, PageScan, PageResult } Page;
typedef enum { Iso15693_3ErrorNone, Iso15693_3ErrorNotSupported, Iso15693_3ErrorTimeout, Iso15693_3ErrorFormat } Iso15693_3Error;
typedef enum { NfcCommandContinue, NfcCommandStop } NfcCommand;
typedef enum { NfcEventTypePollerReady } NfcEventType;
typedef struct { NfcEventType type; } NfcEvent;
typedef struct { const void* parent_event_data; Iso15693_3Poller* poller; } NfcGenericEventEx;
typedef struct { STATE } AppState;
static unsigned frames, reads, mode, invalid_inventory;
static bool locked;
static void furi_mutex_acquire(FuriMutex* m, int w) { (void)m;(void)w;assert(!locked);locked=true; }
static void furi_mutex_release(FuriMutex* m) { (void)m;assert(locked);locked=false; }
static unsigned rtc_reads;
static uint32_t furi_hal_rtc_get_timestamp(void) { rtc_reads++; return 1704067200; }
static void bit_buffer_reset(BitBuffer* b) { b->length=0; }
static void bit_buffer_append_bytes(BitBuffer* b,const uint8_t* c,size_t n) { assert(n<=256);memcpy(b->bytes,c,n);b->length=n; }
static size_t bit_buffer_get_size_bytes(const BitBuffer* b) { return b->length; }
static const uint8_t* bit_buffer_get_data(const BitBuffer* b) { return b->bytes; }
static Iso15693_3Error iso15693_3_poller_send_frame(Iso15693_3Poller* p,BitBuffer* tx,BitBuffer* rx,unsigned w) {
    (void)p;(void)w;assert(!locked);assert(lc_command_allowed(tx->bytes,tx->length));frames++;
    if(tx->bytes[1]==1) {
        if(invalid_inventory) { invalid_inventory--; return Iso15693_3ErrorTimeout; }
        uint8_t r[]={0,0,1,2,3,4,5,6,7,0xe0};
        if(mode==9) r[8]=2;
        memcpy(rx->bytes,r,10);rx->length=10;
    } else if(tx->bytes[1]==0xa1) {
        uint8_t r[]={0,0xc5,9,0x30,1,0,0};
        if(mode==1) r[1]=0xc6;
        if(mode==2) r[3]=0x39;
        if(mode==8) r[0]=1;
        memcpy(rx->bytes,r,7);rx->length=7;
    } else {
        assert(tx->length==4 && tx->bytes[1]==0x23);
        assert(tx->bytes[2]==reads*3 && tx->bytes[3]==(reads==14?0:2));
        reads++;rx->length=1+8*(tx->bytes[3]+1);rx->bytes[0]=0;
        for(size_t i=1;i<rx->length;i++) rx->bytes[i]=(uint8_t)(tx->bytes[2]*8+i-1);
        if(reads==4) {
            if(mode==3) rx->length--;
            if(mode==4) rx->length++;
            if(mode==5) rx->bytes[0]=1;
            if(mode==6) return Iso15693_3ErrorTimeout;
        }
    }
    return Iso15693_3ErrorNone;
}
static Iso15693_3Error send_read(SEND
}
static NfcCommand poller_callback(CALLBACK
}
int main(void) {
    for(mode=0;mode<10;mode++) {
        BitBuffer tx={0},rx={0};AppState a={.tx=&tx,.rx=&rx};
        NfcEvent parent={NfcEventTypePollerReady};NfcGenericEventEx e={&parent,NULL};
        frames=reads=rtc_reads=0;invalid_inventory=70;
        for(unsigned i=0;i<70;i++) {
            assert(poller_callback(e,&a)==NfcCommandContinue);
            assert(!a.finished && !a.capture.uid_valid);
        }
        for(unsigned i=0;i<20;i++) if(poller_callback(e,&a)==NfcCommandStop) break;
        assert(a.finished);
        assert(rtc_reads==1 && a.capture.timestamp==1704067200U);
        if(mode==0 || mode==7) {
            assert(reads==15 && "accepted EU2/gen1 must read exactly 43 blocks in 15 frames");
            assert(frames==87);
        } else if(mode>=3 && mode<=6) { assert(reads==4); }
        else { assert(reads==0); assert(frames==(mode==9?71U:72U)); }
        char json[16384];assert(lc_capture_json(&a.capture,json,sizeof(json)));
        assert(strstr(json,"\"glucose_available\":false"));
        if(mode==0 || mode==7) {
            assert(strstr(json,"\"fram_length\":344"));
            assert(strstr(json,"\"complete\":true"));
            assert(strstr(json,"\"command_hex\":\"02232A00\""));
            assert(strstr(json,"\"response_hex\":\"005051525354555657\""));
        } else {
            assert(strstr(json,"\"complete\":false"));
            if(mode>=3 && mode<=6) assert(strstr(json,"\"fram_length\":72"));
        }
        if(mode==0) {
            const char* output=getenv("LC_SYNTHETIC_JSON");
            if(output) { FILE* f=fopen(output,"w");assert(f);assert(fputs(json,f)>=0);assert(fclose(f)==0); }
            assert(lc_capture_complete(&a.capture));
            a.capture.transcript[0].response[2]^=1;
            assert(!lc_capture_complete(&a.capture) && "completeness must bind metadata transcript to stored UID");
            a.capture.transcript[0].response[2]^=1;
            a.capture.transcript[1].response[1]^=1;
            assert(!lc_capture_complete(&a.capture));
            a.capture.transcript[1].response[1]^=1;
            a.capture.transcript[14].response[1]^=1;
            assert(!lc_capture_complete(&a.capture));
            a.capture.transcript[14].response[1]^=1;
            assert(lc_capture_complete(&a.capture));
            assert(!lc_capture_json(&a.capture,json,20) && json[0]==0);
        }
        unsigned before=frames;
        assert(poller_callback(e,&a)==NfcCommandStop); assert(frames==before);
    }
    puts("PASS actual app callback: persistent inventory, family gate, 15 exact reads, fail-closed partial capture + transcript, no retry after error");
    return 0;
}
'''.replace('STATE',state).replace('SEND',send).replace('CALLBACK',callback)
with tempfile.TemporaryDirectory(prefix='libre-flow-',dir=os.environ.get('TMPDIR')) as tmp:
    c=Path(tmp)/'flow.c'; exe=Path(tmp)/'flow';c.write_text(source)
    sanitizer=['-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'] if os.environ.get('SANITIZE') else []
    subprocess.run(['gcc',*sanitizer,'-std=c11','-Wall','-Wextra','-Werror','-pedantic','-I'+str(root),str(c),str(root/'protocol.c'),str(root/'capture.c'),str(root/'transaction.c'),str(root/'lifecycle.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)