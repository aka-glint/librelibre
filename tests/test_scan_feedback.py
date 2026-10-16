"""Execute extracted real main-thread completion/stop/start functions with SDK shims.
Synthetic effects only: no physical LED, sound, SD, NFC timing evidence.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
app = (root / 'app.c').read_text()
state = app.split('typedef struct {', 1)[1].split('} AppState;', 1)[0]
emit_fn = 'static void emit_uart_bridge_frame(' + app.split('static void emit_uart_bridge_frame(', 1)[1].split('\n}\n', 1)[0] + '\n}\n'
def body(name, kind='void'):
    return app.split(f'static {kind} {name}(', 1)[1].split('\n}\n', 1)[0]
# RED uses the pre-feature main transition, rather than a missing-symbol error.
if 'static void finish_scan(' in app:
    finish = emit_fn + 'static void finish_scan(' + body('finish_scan') + '\n}\n'
else:
    old = app.split('            if(finished) {', 1)[1].split('\n            }', 1)[0]
    finish = 'static void finish_scan(AppState* app) { uint32_t elapsed=100;'+old+'\n}\n'
# Execute both real completion and evaluator; unavailable current must not chime.
from test_current_trend_app import evaluator
vendor_stub = '#include "vendor_chain.h"\n' + evaluator(app)
notifications = ''
if 'static const NotificationSequence scan_complete_sequence' in app:
    notifications = app.split('static const NotificationSequence scan_complete_sequence', 1)[1].split('typedef enum', 1)[0]
    notifications = 'static const NotificationSequence scan_complete_sequence' + notifications
source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "capture.h"
#include "integrity.h"
#include "factory.h"
#include "lifecycle.h"
#include "transaction.h"
#define FuriWaitForever 0
#define NfcProtocolIso15693_3 0
#define COUNT_OF(a) (sizeof(a)/sizeof((a)[0]))
typedef int FuriMutex; typedef int FuriMessageQueue; typedef int ViewPort;
typedef int Nfc; typedef int NfcPoller; typedef int BitBuffer; typedef int NotificationApp;
VENDORSTUB
typedef int Expansion; typedef int FuriHalSerialHandle;
#define RECORD_EXPANSION "expansion"
#define FuriHalSerialIdUsart 0
static unsigned uart_tx_bytes;
static void expansion_disable(Expansion* e) {(void)e;}
static void expansion_enable(Expansion* e) {(void)e;}
static FuriHalSerialHandle* furi_hal_serial_control_acquire(int id) {(void)id; return (FuriHalSerialHandle*)1;}
static void furi_hal_serial_control_release(FuriHalSerialHandle* h) {(void)h;}
static void furi_hal_serial_init(FuriHalSerialHandle* h, uint32_t b) {(void)h;(void)b;}
static void furi_hal_serial_deinit(FuriHalSerialHandle* h) {(void)h;}
static void furi_hal_serial_tx(FuriHalSerialHandle* h, const uint8_t* d, size_t n) {(void)h;(void)d;uart_tx_bytes=(unsigned)n;}
static void furi_hal_serial_tx_wait_complete(FuriHalSerialHandle* h) {(void)h;}
static Expansion* furi_record_open(const char* r) {(void)r; return (Expansion*)1;}
static void furi_record_close(const char* r) {(void)r;}

typedef enum { PageIdle, PageScan, PageResult } Page;
typedef int NotificationMessage;
typedef const NotificationMessage* NotificationSequence[];
static const NotificationMessage message_green_255=1, message_red_255=2, message_blue_255=9,
 message_note_c5=3,message_note_g5=4,message_note_c4=5,message_delay_100=6,
 message_delay_50=7,message_sound_off=8;
static const NotificationSequence sequence_blink_start_blue={NULL},sequence_blink_stop={NULL},sequence_reset_rgb={NULL};
NOTIFICATIONS
typedef struct { STATE } AppState;
static unsigned saves, joins, frees, success_alerts, error_alerts, starts, blink_stops, resets;
static bool locked, save_ok=true, blinking;
static unsigned clock_tick=100;
static void furi_mutex_acquire(FuriMutex* m,int t) {(void)m;(void)t;assert(!locked);locked=true;}
static void furi_mutex_release(FuriMutex* m) {(void)m;assert(locked);locked=false;}
static uint32_t furi_get_tick(void) {return clock_tick;}
static uint32_t furi_kernel_get_tick_frequency(void) {return 1000;}
static void view_port_update(ViewPort* v) {(void)v;}
static void notification_message_block(NotificationApp* n,const NotificationSequence* s) {
 (void)n;assert(!locked);
 if(s==&sequence_blink_start_blue) {assert(!blinking);blinking=true;starts++;}
 else if(s==&sequence_blink_stop) {blinking=false;blink_stops++;}
 else if(s==&sequence_reset_rgb) {assert(!blinking);resets++;}
 else {
  assert(!blinking && joins && saves && "alerts only after stop and save attempt");
  bool green=false,red=false,blue=false,sound_off=false;
  for(unsigned i=0;(*s)[i];i++) {
   green|=(*s)[i]==&message_green_255;red|=(*s)[i]==&message_red_255;
   blue|=(*s)[i]==&message_blue_255;
   sound_off|=(*s)[i]==&message_sound_off;
  }
  assert(sound_off && "finite alert must stop its sound");
  assert(!green && "unknown freshness must never show healthy-success green");
  if(blue && !red) success_alerts++; else {assert(red);error_alerts++;}
 }
}
static void nfc_poller_stop(NfcPoller* p) {(void)p;assert(!locked);assert(!blinking);joins++;clock_tick=250;}
static void nfc_poller_free(NfcPoller* p) {(void)p;assert(joins);frees++;}
static NfcPoller* nfc_poller_alloc(Nfc* n,int p) {(void)n;(void)p;return (NfcPoller*)1;}
static int poller_callback;
static void nfc_poller_start_ex(NfcPoller* p,int cb,AppState* a) {(void)p;(void)cb;assert(a->started && blinking);}
STOP
static bool save_capture(AppState* a) {
 assert(!locked && !a->started && a->poller==NULL && joins);
 assert(a->capture.elapsed_ms==250 && "elapsed finalized after poller join");
 saves++;return save_ok;
}
FINISH
START
int main(int argc,char** argv) {
 assert(argc==2); AppState a={.started=true,.finished=true,.poller=(NfcPoller*)1,.page=PageScan};
 a.capture.uid_valid=true;blinking=true;
 const char* test=argv[1];
 if(!strcmp(test,"partial_once") || !strcmp(test,"uid_only")) {
  if(!strcmp(test,"partial_once")) {a.capture.fram_length=24;a.capture.error=2;}
  finish_scan(&a);assert(saves==1 && joins==1 && frees==1 && a.page==PageResult);
  assert(!blinking && blink_stops && resets);
  assert(error_alerts==1 && success_alerts==0 && "partial never sounds successful");
  finish_scan(&a);assert(saves==1 && joins==1 && "completion is exactly once");
 }
 else if(!strcmp(test,"full_chime") || !strcmp(test,"savefail") || !strcmp(test,"integrity_fail") || !strcmp(test,"integrity_unsupported") || !strncmp(test,"sensor_",7) || !strncmp(test,"real_",5)) {
  const uint8_t uid[8]={1,2,3,4,5,6,7,0xe0},patch[6]={0xc5,9,0x30,1,0,0};
  memcpy(a.capture.uid,uid,8);memcpy(a.capture.patch,patch,6);a.capture.patch_valid=true;
  a.capture.transcript_length=17;a.capture.fram_length=344;
  for(unsigned i=0;i<17;i++) {
   LcExchange* e=&a.capture.transcript[i];
   if(i==0) {uint8_t c[3]={0x26,1,0};memcpy(e->command,c,3);e->command_length=3;e->response_length=10;memcpy(e->response+2,uid,8);}
   else if(i==1) {uint8_t c[3]={2,0xa1,7};memcpy(e->command,c,3);e->command_length=3;e->response_length=7;memcpy(e->response+1,patch,6);}
   else {assert(lc_read_command(i-2,e->command));e->command_length=4;e->response_length=1+8*(e->command[3]+1);}
  }
  uint8_t plain[344]={0};uint16_t pairs[6];
  plain[4]=3;plain[316]=100;plain[322]=0x30;plain[323]=1;plain[326]=200;
  plain[2]=0xa1;plain[3]=0x0f; /* i1=1,i2=500 */
  uint64_t factory_footer=100ULL|(1000ULL<<8)|(1ULL<<33)|(250ULL<<40)|(2000ULL<<52);
  for(unsigned i=0;i<8;i++) plain[336+i]=(uint8_t)(factory_footer>>(8*i));
  uint64_t factory_word=1500ULL|(2000ULL<<26);
  for(unsigned i=0;i<6;i++) plain[28+i]=(uint8_t)(factory_word>>(8*i));
  if(!strcmp(test,"sensor_shutdown")) {plain[4]=5;plain[316]=201;plain[31]=3;}
  if(!strcmp(test,"sensor_error")) plain[31]|=2;
  if(!strcmp(test,"sensor_coeff")) {plain[2]=1;plain[3]=0;}
  if(!strcmp(test,"sensor_quality")) plain[30]=0x40;
  if(!strcmp(test,"sensor_flags")) plain[30]=0x80;
  /* Newest slot for trend_index=0 is 15, not slot zero. */
  memcpy(plain+28+15*6,plain+28,6);memset(plain+28,0,6);
  if(!strcmp(test,"sensor_warmup")) plain[4]=2;
  if(!strcmp(test,"sensor_expired")) plain[4]=4;
  if(!strcmp(test,"sensor_unknown")) plain[4]=255;
  if(!strcmp(test,"sensor_life")) plain[316]=201;
  if(!strcmp(test,"sensor_life_unknown")) plain[326]=0;
  if(!strcmp(test,"sensor_index")) plain[26]=16;
  if(!strcmp(test,"sensor_profile")) plain[322]=0;
  lc_integrity_sections(plain,pairs);
  for(unsigned s=0;s<3;s++) {
   unsigned o=s==0?0:s==1?24:320;
   plain[o]=(uint8_t)pairs[s*2+1];plain[o+1]=(uint8_t)(pairs[s*2+1]>>8);
  }
  lc_integrity_transform(uid,patch,plain,a.capture.fram);
  if(!strcmp(test,"integrity_fail")) a.capture.fram[26]^=1;
  if(!strcmp(test,"integrity_unsupported")) {a.capture.patch[3]=2;a.capture.transcript[1].response[4]=2;}
  if(!strncmp(test,"real_",5)) {
   assert(fread(a.capture.uid,1,8,stdin)==8 && fread(a.capture.patch,1,6,stdin)==6 && fread(a.capture.fram,1,344,stdin)==344);
   memcpy(a.capture.transcript[0].response+2,a.capture.uid,8);
   memcpy(a.capture.transcript[1].response+1,a.capture.patch,6);
  }
  for(unsigned i=0;i<15;i++) memcpy(a.capture.transcript[i+2].response+1,a.capture.fram+i*24,a.capture.transcript[i+2].response_length-1);
  assert(lc_capture_complete(&a.capture));
  LcCapture before=a.capture;
  save_ok=strcmp(test,"savefail")!=0;
  bool expected_success=save_ok && strcmp(test,"integrity_fail") && strcmp(test,"integrity_unsupported") && strncmp(test,"sensor_",7) && strcmp(test,"real_retired") && strcmp(test,"real_error");
  finish_scan(&a);assert(saves==1 && success_alerts==(expected_success?1U:0U) && error_alerts==(expected_success?0U:1U));
  assert(a.factory.status==(expected_success || !save_ok ? LcFactoryEstimate : LcFactoryRejected));
  before.elapsed_ms=a.capture.elapsed_ms;
  assert(!memcmp(&before,&a.capture,sizeof(before)) && "decryption must not change raw/transcript");
  assert(blink_stops==2 && resets==2 && "explicit cleanup after completion alert");
  finish_scan(&a);assert(saves==1);
  if(!save_ok) {save_ok=true;assert(save_capture(&a));assert(saves==2 && lc_capture_complete(&a.capture));}
 }
 else if(!strcmp(test,"cancel")) {
  a.gate.cancelled=true;finish_scan(&a);
  assert(saves==0 && success_alerts==0 && error_alerts==0 && !blinking);
 } else if(!strcmp(test,"empty")) {
  a.capture.uid_valid=false;finish_scan(&a);
  assert(saves==0 && success_alerts==0 && error_alerts==0 && !blinking);
 }
 else if(!strcmp(test,"start_cancel_restart")) {
  a.started=false;a.poller=NULL;blinking=false;a.factory.status=LcFactoryEstimate;a.factory.mmol_tenths=1234;
  memset(a.capture.uid,0xaa,8);a.capture.uid_valid=true;a.uid_details=true;strcpy(a.saved_name,"old");
  start_scan(&a);assert(starts==1 && a.page==PageScan && !a.finished && !a.gate.cancelled);
  assert(a.factory.status==LcFactoryNotChecked && !a.factory.mmol_tenths && !a.details && !a.uid_details);
  const uint8_t zero[8]={0};assert(!a.capture.uid_valid && !memcmp(a.capture.uid,zero,8) && !a.saved_name[0]);
  a.gate.cancelled=true;stop_scan(&a);assert(!blinking && saves==0);
  start_scan(&a);assert(starts==2 && !a.gate.cancelled && !a.capture.uid_valid);
  stop_scan(&a);assert(!blinking && saves==0);
 }
 CASES
 puts("PASS extracted scan feedback transition");return 0;
}
'''.replace('STATE',state).replace('NOTIFICATIONS',notifications).replace('VENDORSTUB',vendor_stub).replace('STOP','static void stop_scan('+body('stop_scan')+'\n}\n').replace('FINISH',finish).replace('START','static void start_scan('+body('start_scan')+'\n}\n').replace('CASES','')
with tempfile.TemporaryDirectory(prefix='libre-feedback-', dir=os.environ.get('TMPDIR')) as tmp:
    c = Path(tmp)/'feedback.c'; exe=Path(tmp)/'feedback';c.write_text(source)
    sanitizer=['-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'] if os.environ.get('SANITIZE') else []
    subprocess.run(['gcc',*sanitizer,'-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-const-variable','-Wno-unused-variable','-pedantic','-I'+str(root),str(c),str(root/'capture.c'),str(root/'protocol.c'),str(root/'integrity.c'),str(root/'factory.c'),str(root/'lifecycle.c'),str(root/'vendor_chain.c'),str(root/'vendor_tables.c'),'-lm','-o',str(exe)],check=True)
    for case in ['partial_once', 'uid_only', 'full_chime', 'cancel', 'empty', 'savefail', 'start_cancel_restart', 'integrity_fail', 'integrity_unsupported', 'sensor_shutdown','sensor_error','sensor_quality','sensor_flags','sensor_warmup','sensor_expired','sensor_unknown','sensor_life','sensor_life_unknown','sensor_index','sensor_profile','sensor_coeff']:
        subprocess.run([str(exe),case],check=True)
    import sys
    sys.path.insert(0,str(root/'offline'))
    sys.path.insert(0,str(root/'tests'))
    from libre2_offline import validate_capture, load_capture
    from test_sensor_status import synthetic_valid_capture
    for filename, case in [('cap_011cc322_00.json','real_retired'),('cap_014d22f3_00.json','real_active'),('cap_014d4036_00.json','real_active'),('cap_015b1636_00.json','real_error'),('cap_015b21be_00.json','real_error'),('cap_015c3a05_00.json','real_active'),('cap_015c461e_00.json','real_active')]:
        p = root/'captures'/filename
        if p.exists():
            uid,patch,raw = validate_capture(load_capture(p))
        elif case == 'real_retired':
            uid,patch,raw = synthetic_valid_capture(age=21348, life=20628, status=5, quality=0x100, error=True)
        elif case == 'real_error':
            uid,patch,raw = synthetic_valid_capture(quality=0x020, error=True)
        else:
            uid,patch,raw = synthetic_valid_capture()
        subprocess.run([str(exe),case],input=uid+patch+raw,check=True)
    print('Real completion fixtures: shutdown uses red/error; active uses neutral blue, never healthy green')
