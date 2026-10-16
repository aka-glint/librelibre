"""Execute actual SD save function with synthetic storage failures/readback, no SD evidence."""
from pathlib import Path
import os
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
app=(root/'app.c').read_text()
state=app.split('typedef struct {',1)[1].split('} AppState;',1)[0]
save=app.split('static bool save_capture(',1)[1].split('\n}\n',1)[0]
source=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "capture.h"
#include "integrity.h"
#include "factory.h"
#include "vendor_chain.h"
#include "integrity.h"
#include "factory.h"
#include "lifecycle.h"
#include "transaction.h"
#define CAPTURE_DIR "/ext/apps_data/libre_research"
#define RECORD_STORAGE "storage"
#define FuriWaitForever 0
typedef int FuriMutex; typedef int FuriMessageQueue; typedef int ViewPort;
typedef int Nfc; typedef int NfcPoller; typedef int BitBuffer; typedef int NotificationApp;
typedef enum { PageIdle,PageScan,PageResult } Page;
typedef int Storage; typedef int File;
typedef enum {FSE_OK,FSE_EXIST,FSE_ERROR} FS_Error;
enum {FSAM_WRITE,FSAM_READ,FSOM_CREATE_NEW,FSOM_OPEN_EXISTING};
typedef struct {STATE} AppState;
static unsigned mode,alloc_calls,storage_calls,writes,removes;
static char disk[LC_JSON_CAPACITY];static size_t disk_size;static bool locked;
static char paths[101][128];static unsigned path_count,open_attempts;static FS_Error last_error;
static bool emulate_collisions;
static void furi_mutex_acquire(FuriMutex* m,int w) {(void)m;(void)w;assert(!locked);locked=true;}
static void furi_mutex_release(FuriMutex* m) {(void)m;assert(locked);locked=false;}
static void* test_malloc(size_t n) {alloc_calls++;if((mode==7 && alloc_calls==1)||(mode==8 && alloc_calls==2)) return NULL;return malloc(n);}
#define malloc test_malloc
static uint32_t furi_get_tick(void) {return 123;}
static uint32_t clock_raw=1792108801U;
static uint32_t furi_hal_rtc_get_timestamp(void) {return clock_raw++;}
static Storage* furi_record_open(const char* r) {(void)r;assert(!locked);storage_calls++;return (Storage*)1;}
static void furi_record_close(const char* r) {(void)r;}
static File* storage_file_alloc(Storage* s) {(void)s;return (File*)1;}
static void storage_file_free(File* f) {(void)f;}
static FS_Error storage_common_mkdir(Storage* s,const char* p) {(void)s;(void)p;return mode==1?FSE_ERROR:FSE_OK;}
static bool storage_file_open(File* f,const char* p,int access,int openmode) {
 (void)f;assert(openmode==(access==FSAM_WRITE?FSOM_CREATE_NEW:FSOM_OPEN_EXISTING));
 assert(strlen(p)==67 && strlen(strrchr(p,'/')+1)==37);open_attempts++;
 if(access==FSAM_WRITE && emulate_collisions) {
   for(unsigned i=0;i<path_count;i++) if(!strcmp(p,paths[i])) {last_error=FSE_EXIST;return false;}
   assert(path_count<101);strcpy(paths[path_count++],p);
 }
 last_error=FSE_ERROR;
 return mode!=2 && !(mode==6 && access==FSAM_READ);
}
static FS_Error storage_file_get_error(File* f) {(void)f;return last_error;}
static bool storage_file_close(File* f) {(void)f;return mode!=5;}
static size_t storage_file_write(File* f,const char* p,size_t n) {(void)f;writes++;memcpy(disk,p,n);disk_size=n;return mode==3?n-1:n;}
static bool storage_file_sync(File* f) {(void)f;return mode!=4;}
static size_t storage_file_size(File* f) {(void)f;return disk_size;}
static size_t storage_file_read(File* f,char* p,size_t n) {(void)f;memcpy(p,disk,n);if(mode==9)p[0]^=1;return n;}
static FS_Error storage_common_remove(Storage* s,const char* p) {(void)s;(void)p;removes++;return FSE_OK;}
static bool save_capture(SAVE
}
int main(int argc,char** argv) {
 assert(argc==2);mode=(unsigned)atoi(argv[1]);AppState a={0};a.capture.uid_valid=true;
 a.capture.timestamp=1792108799U;
 (void)furi_hal_rtc_get_timestamp; /* shim can exist before production wiring */
 strcpy(a.status,"Read error 2; partial");
 if(mode==13) {
   assert(!save_capture(&a));assert(!open_attempts && !writes && !removes);
   puts("PASS shortened path capacity fails closed before storage open");return 0;
 }
 if(mode==10 || mode==11) {
   if(mode==10) a.capture.uid_valid=false;else a.gate.cancelled=true;
   assert(!save_capture(&a));assert(!storage_calls && !writes && !removes && !a.saved_name[0]);
   puts("PASS no UID/cancel safely suppresses manual save");return 0;
 }
 if(mode==12) {
   mode=0;emulate_collisions=true;
   const uint8_t first[8]={1,2,3,4,5,6,7,0xe0},second[8]={0xff,0xee,0xdd,0xcc,0xbb,0xaa,7,0xe0};
   memcpy(a.capture.uid,first,8);assert(save_capture(&a));
   assert(!strcmp(a.saved_name,"cap_01020304050607E0_0000007b_00.json"));
   memcpy(a.capture.uid,second,8);assert(save_capture(&a));
   assert(!strcmp(a.saved_name,"cap_FFEEDDCCBBAA07E0_0000007b_00.json"));
   memcpy(a.capture.uid,first,8);
   for(unsigned i=1;i<100;i++) assert(save_capture(&a));
   assert(path_count==101 && strstr(a.saved_name,"_99.json"));
   unsigned before=writes,removed=removes;
   assert(!save_capture(&a));assert(writes==before && removes==removed);
   puts("PASS two synthetic UIDs same tick; repeated collisions 00..99; exhaustion never overwrites");return 0;
 }
 bool saved=save_capture(&a);assert(saved==(mode==0));
 assert(!strcmp(a.status,"Read error 2; partial") && "storage status must not overwrite raw outcome");
 CHECK_STATUS
 if(saved) {
   assert(!strcmp(a.saved_name,"cap_0000000000000000_0000007b_00.json") && "full wire UID must identify saved file");
 } else assert(!a.saved_name[0]);
 if(mode==7 || mode==8)assert(!writes && !storage_calls);
 if(mode==3 || mode==4 || mode==5 || mode==6 || mode==9)assert(removes==1);
 if(mode==0) {
   assert(a.capture.timestamp==1792108799U);
   assert(strstr(disk,"\"scan_local_datetime\":\"2026-10-15T23:59:59\""));
   assert(strstr(disk,"\"save_local_datetime\":\"2026-10-16T00:00:01\""));
   assert(save_capture(&a));
   assert(a.capture.timestamp==1792108799U);
   assert(strstr(disk,"\"scan_local_datetime\":\"2026-10-15T23:59:59\""));
   assert(strstr(disk,"\"save_local_datetime\":\"2026-10-16T00:00:02\""));
 }
 puts("PASS actual save outcome retained + verified storage feedback");return 0;
}
'''.replace('STATE',state).replace('SAVE',save)
# Before new state exists, RED checks the existing status, not a compile error.
field='save_status' if 'save_status' in state else 'status'
source=source.replace('CHECK_STATUS',f'assert(strstr(a.{field},saved?"verified":"retry"));')
with tempfile.TemporaryDirectory(prefix='libre-save-',dir=os.environ.get('TMPDIR')) as tmp:
    c=Path(tmp)/'save.c';exe=Path(tmp)/'save';c.write_text(source)
    sanitizer=['-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'] if os.environ.get('SANITIZE') else []
    subprocess.run(['gcc',*sanitizer,'-std=c11','-Wall','-Wextra','-Werror','-pedantic','-I'+str(root),str(c),str(root/'capture.c'),str(root/'protocol.c'),str(root/'lifecycle.c'),'-o',str(exe)],check=True)
    for mode in range(13):subprocess.run([str(exe),str(mode)],check=True)
    # Exact real path requires 68 bytes including NUL. A 67-byte probe must not open a truncated name.
    c.write_text(source.replace('char path[128];','char path[67];'))
    subprocess.run(['gcc',*sanitizer,'-std=c11','-Wall','-Wextra','-Werror','-pedantic','-I'+str(root),str(c),str(root/'capture.c'),str(root/'protocol.c'),str(root/'lifecycle.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe),'13'],check=True)