"""Execute actual draw callback, no physical display or RF claims."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
import sys
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'offline'))
sys.path.insert(0,str(ROOT/'tests'))
from libre2_offline import validate_capture,load_capture,decrypt_fram
from test_sensor_status import synthetic,PATCH,seal,synthetic_valid_capture

class StatusUI(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        app = (ROOT/'app.c').read_text()
        state = app.split('typedef struct {',1)[1].split('} AppState;',1)[0]
        draw = app.split('static void draw(',1)[1].split('\n}\n',1)[0]
        setup = (ROOT/'tests/integrity_host.c').read_text().split('int main(void)',1)[0]
        source = setup + r'''
#include <stdlib.h>
#include "transaction.h"
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
static void render_flipper_graph(Canvas* c, const AppState* a, const char* s){(void)c;(void)a;(void)s;}
static void draw(DRAW puts(PAGE_SEP);
}
static void result_left(AppState* app) {
LEFT_BODY
}
int main(int argc,char** argv) {
    (void)argc;(void)argv;
    AppState a={0};a.page=PageResult;
    FACTORY_SETUP
    assert(fread(a.capture.uid,1,8,stdin)==8 && fread(a.capture.patch,1,6,stdin)==6 && fread(a.capture.fram,1,344,stdin)==344);
    complete_capture(&a.capture);lc_integrity_check(&a.capture,&a.integrity);
    /* Mirror finish_scan: sensor-clock life+freshness from the SAME capture. */
    lc_life_compute(a.integrity.sensor.age_minutes,a.integrity.sensor.max_life_minutes,0,
        a.integrity.status==LcIntegrityPass && a.integrity.sensor.parse_status==LcSensorParsed,&a.life);
    strcpy(a.status,"Raw 344 bytes; no glucose");strcpy(a.save_status,"Saved + verified on SD");
    if(argc>2) {a.vendor_chain.mmol_tenths=(uint32_t)strtoul(argv[2],NULL,10);}
    if(argc>3) a.capture.timestamp=1792092225U;
    if(argc>1 && !strcmp(argv[1],"identity")) { IDENTITY_SETUP }
    if(argc>1 && !strcmp(argv[1],"no_uid")) { IDENTITY_SETUP a.capture.uid_valid=false; }
    if(argc>1 && !strcmp(argv[1],"cancel_uid")) { IDENTITY_SETUP a.gate.cancelled=true; }
    if(argc>1 && !strcmp(argv[1],"repeat")) {
        a.uid_details=true;draw(NULL,&a);
        memset(&a.capture,0,sizeof(a.capture));draw(NULL,&a);
        a.capture.uid_valid=true;
        const uint8_t next[8]={0xff,0xee,0xdd,0xcc,0xbb,0xaa,7,0xe0};memcpy(a.capture.uid,next,8);
        a.integrity.status=LcIntegrityUnsupported;a.factory.status=LcFactoryRejected;draw(NULL,&a);
        a.integrity.status=LcIntegrityFail;draw(NULL,&a);
        a.gate.cancelled=true;draw(NULL,&a);return 0;
    }
    if(argc>1 && !strcmp(argv[1],"navigate")) {
        a.details=false;a.uid_details=false;a.vendor_chain.available=true;
        result_left(&a);assert(a.details && !a.uid_details);
        result_left(&a);assert(a.uid_details);draw(NULL,&a);
        result_left(&a);assert(!a.uid_details && !a.details);
        a.vendor_chain.available=false;result_left(&a);assert(a.details);
        result_left(&a);assert(a.uid_details);
    }
    draw(NULL,&a);assert(!locked);return 0;
}
'''.replace('STATE',state).replace('DRAW',draw).replace('FACTORY_SETUP','a.details=argc==1;' if 'bool details;' in app else '')
        left=app.split('key.key == InputKeyLeft && app->page == PageResult) {',1)[1].split('\n            }',1)[0]
        source=source.replace('LEFT_BODY',left)
        source=source.replace('IDENTITY_SETUP','a.uid_details=true;' if 'bool uid_details;' in app else 'a.details=true;')
        source=source.replace('#include <stdlib.h>','#include <stdlib.h>\n#include "factory.h"\n#include "vendor_chain.h"')
        if 'LcFactory factory;' in app:
            source=source.replace('strcpy(a.status,','lc_factory_check(&a.capture,&a.factory);\n    strcpy(a.status,')
        cls.tmp=tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        from test_current_trend_app import wire_evaluator
        source = wire_evaluator(source, app)
        c=Path(cls.tmp.name)/'ui.c';cls.exe=Path(cls.tmp.name)/'ui';c.write_text(source)
        san=['-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie'] if os.environ.get('SANITIZE') else []
        subprocess.run(['gcc',*san,'-std=c11','-Wall','-Wextra','-Werror','-pedantic','-I'+str(ROOT),str(c),str(ROOT/'integrity.c'),str(ROOT/'protocol.c'),str(ROOT/'capture.c'),str(ROOT/'factory.c'),str(ROOT/'lifecycle.c'),str(ROOT/'vendor_chain.c'),str(ROOT/'vendor_tables.c'),'-lm','-o',str(cls.exe)],check=True)
    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()
    def render(self,uid,patch,raw):
        result=subprocess.run([str(self.exe)],input=uid+patch+raw,capture_output=True)
        self.assertEqual(result.returncode,0,result.stderr.decode())
        return result.stdout.decode()
    def synthetic_render(self,data):
        uid=bytes.fromhex('01020304050607E0')
        return self.render(uid,PATCH,decrypt_fram(uid,PATCH,data))
    def test_repeated_draw_identity_clears_and_binds_errors_to_current_sensor(self):
        uid=bytes.fromhex('01020304050607E0')
        raw=decrypt_fram(uid,PATCH,synthetic())
        text=subprocess.run([str(self.exe),'repeat'],input=uid+PATCH+raw,capture_output=True,check=True).stdout.decode()
        pages=[pg for pg in text.split('\x1ePAGE\x1e') if pg.strip()]
        self.assertEqual(len(pages),5)
        self.assertIn(uid.hex().upper(),pages[0])
        for i in [1,4]:
            self.assertIn('UID unavailable',pages[i])
            self.assertNotIn(uid.hex().upper(),pages[i])
        for i in [2,3]:
            self.assertIn('FFEEDDCCBBAA07E0',pages[i])
            self.assertNotIn(uid.hex().upper(),pages[i])
    def test_full_wire_uid_on_accessible_result_detail(self):
        for uid in [bytes.fromhex('01020304050607E0'),bytes.fromhex('FFEEDDCCBBAA07E0')]:
            raw=decrypt_fram(uid,PATCH,synthetic())
            run=subprocess.run([str(self.exe),'identity'],input=uid+PATCH+raw,capture_output=True,check=True)
            text=run.stdout.decode()
            self.assertIn(uid.hex().upper(),text)
            for label in ['UID wire LSB-first','--:--']:
                self.assertIn(label,text)
            navigated=subprocess.run([str(self.exe),'navigate'],input=uid+PATCH+raw,capture_output=True,check=True).stdout.decode()
            self.assertIn(uid.hex().upper(),navigated)
            for mode in ['no_uid','cancel_uid']:
                absent=subprocess.run([str(self.exe),mode],input=uid+PATCH+raw,capture_output=True,check=True).stdout.decode()
                self.assertNotIn(uid.hex().upper(),absent)
                self.assertIn('UID unavailable',absent)
    def test_acquisition_clock_in_header_preserves_safety_labels(self):
        uid,patch,raw=(validate_capture(load_capture(ROOT/'captures/cap_014d22f3_00.json')) if (ROOT/'captures/cap_014d22f3_00.json').exists() else synthetic_valid_capture())
        run=subprocess.run([str(self.exe),'estimate','65','time'],input=uid+patch+raw,capture_output=True,check=True)
        text=run.stdout.decode()
        self.assertIn('19:23',text)
        unknown=subprocess.run([str(self.exe),'estimate'],input=uid+patch+raw,capture_output=True,check=True)
        self.assertIn('--:--',unknown.stdout.decode())
        for label in ['Fresh 0m=now']:
            self.assertIn(label,text)
        self.assertNotIn('Hist ',text)
        self.assertNotIn('EXPERIMENTAL',text)
        self.assertRegex(text,r'Left \d+d \d+h')
    def test_experimental_estimate_default_real_active_unknown_age_no_dosing(self):
        for filename,wanted in [('cap_014d22f3_00.json',''),('cap_01523f3d_00.json','')]:
            uid,patch,raw=(validate_capture(load_capture(ROOT/'captures'/filename)) if (ROOT/'captures'/filename).exists() else (synthetic_valid_capture(quality=0x020, error=True) if '015b' in filename else synthetic_valid_capture()))
            run=subprocess.run([str(self.exe),'estimate'],input=uid+patch+raw,capture_output=True)
            self.assertEqual(run.returncode,0,run.stderr.decode());text=run.stdout.decode()
            for required in ['mmol/L','Fresh 0m=now','Saved + verified']:
                self.assertIn(required,text)
    def test_actual_error_then_clean_recovery_preserves_unknown_currentness(self):
        from test_factory import parser,oracle
        sequence=[('cap_01523f3d_00.json',True),('cap_015b1636_00.json',False),('cap_015b21be_00.json',False),('cap_015c3a05_00.json',True),('cap_015c461e_00.json',True)]
        for filename,available in sequence:
            uid,patch,raw=(validate_capture(load_capture(ROOT/'captures'/filename)) if (ROOT/'captures'/filename).exists() else (synthetic_valid_capture(quality=0x020, error=True) if '015b' in filename else synthetic_valid_capture()))
            run=subprocess.run([str(self.exe),'estimate'],input=uid+patch+raw,capture_output=True)
            self.assertEqual(run.returncode,0,run.stderr.decode());text=run.stdout.decode()
            if available:
                meta=parser.parse(decrypt_fram(uid,patch,raw),patch);candidate=oracle.candidate(meta['trend'][0],meta['factory_coefficients'])
                import math
                tenths=math.floor(candidate['candidate_mg_dl']/18*10+0.5)
                self.assertIn('mmol/L',text)
                self.assertIn('Fresh 0m=now',text)
            else:
                self.assertNotIn('mmol/L',text);self.assertIn('Rescan the sensor',text);self.assertNotIn('Q020',text)
            self.assertNotIn('EXPERIMENTAL',text)
    def test_actual_draw_keeps_one_decimal_at_100_plus_rendering_limit(self):
        uid,patch,raw=(validate_capture(load_capture(ROOT/'captures/cap_014d22f3_00.json')) if (ROOT/'captures/cap_014d22f3_00.json').exists() else synthetic_valid_capture())
        for tenths in [0,1,999,1000,1234,5555]:
            run=subprocess.run([str(self.exe),'estimate',str(tenths)],input=uid+patch+raw,capture_output=True)
            self.assertEqual(run.returncode,0,run.stderr.decode())
            self.assertIn(f'{tenths//10}.{tenths%10} mmol/L',run.stdout.decode())
    def test_invalid_factory_coefficient_explicit_rejection(self):
        data=synthetic();data[2]=1;data[3]=0;seal(data)
        text=self.synthetic_render(data)
        self.assertIn('No estimate: coeff/domain',text)
        self.assertIn('Fresh 0m=now',text)
        self.assertNotIn('mmol/L',text)
    def test_blocked_default_has_no_numeric_estimate(self):
        for filename in ['cap_011cc322_00.json','cap_015b1636_00.json','cap_015b21be_00.json']:
            uid,patch,raw=(validate_capture(load_capture(ROOT/'captures'/filename)) if (ROOT/'captures'/filename).exists() else (synthetic_valid_capture(age=21348, life=20628, status=5, quality=0x100, error=True) if '011cc' in filename else synthetic_valid_capture(quality=0x020, error=True)))
            run=subprocess.run([str(self.exe),'estimate'],input=uid+patch+raw,capture_output=True)
            self.assertEqual(run.returncode,0,run.stderr.decode());text=run.stdout.decode()
            self.assertIn('shutdown' if '011cc' in filename else 'Rescan',text);self.assertNotIn('mmol/L',text)
    def test_actual_active_shows_sensor_clock_freshness_and_no_glucose(self):
        for filename in ['cap_014d22f3_00.json','cap_014d4036_00.json']:
            text=self.render(*(validate_capture(load_capture(ROOT/'captures'/filename)) if (ROOT/'captures'/filename).exists() else (synthetic_valid_capture(quality=0x020, error=True) if '015b' in filename else synthetic_valid_capture())))
            for required in ['CRC OK; active','Age2466/20854m Q000/0','Fresh 0m=now','Left 12d 18h; record clear']:
                self.assertIn(required,text)
            self.assertNotIn('healthy',text.lower())
    def test_synthetic_warmup_expired_error_quality_and_unknown(self):
        for status,label in [(0,'unknown'),(1,'not activated'),(2,'warming up'),(3,'active'),(4,'expired'),(5,'shutdown'),(6,'failure'),(255,'unknown(255)')]:
            self.assertIn('CRC OK; '+label,self.synthetic_render(synthetic(status=status)))
        # Auto-shown error path is the rescan prompt (test_freshness_ui); the
        # detail view keeps the raw diagnostics and never a paired value.
        err=self.synthetic_render(synthetic(error=True))
        self.assertIn('Age100/20160m Q000/0',err);self.assertNotIn('mmol/L',err)
        self.assertIn('quality',self.synthetic_render(synthetic(quality=1)))
        self.assertIn('Q000/3',self.synthetic_render(synthetic(flags=3)))
        self.assertIn('Life unknown',self.synthetic_render(synthetic(life=0)))
        self.assertIn('Life exceeded',self.synthetic_render(synthetic(age=20161,life=20160)))
        self.assertIn('Age20160/20160m',self.synthetic_render(synthetic(age=20160,life=20160)))
    def test_invalid_index_footer_or_crc_never_displays_active_metadata(self):
        for offset,value,required,reseal in [(26,16,'Invalid ring index',True),(27,32,'Invalid ring index',True),(322,0,'Footer profile mismatch',True),(4,5,'CRC FAIL',False)]:
            data=synthetic();data[offset]=value
            if reseal:seal(data)
            text=self.synthetic_render(data)
            self.assertIn(required,text)
            self.assertNotIn('Age',text)
            self.assertNotIn('CRC OK; active',text)
            self.assertIn('Fresh unknown',text)
    def test_retired_capture_shows_shutdown_expired_life_record_error(self):
        text=self.render(*(validate_capture(load_capture(ROOT/'captures/cap_011cc322_00.json')) if (ROOT/'captures/cap_011cc322_00.json').exists() else synthetic_valid_capture(age=21348, life=20628, status=5, quality=0x100, error=True)))
        for required in ['CRC OK','shutdown','Life exceeded','record error','21348/20628','Q100','Freshness unknown','Saved + verified']:
            self.assertIn(required,text)
        self.assertNotIn('Raw 344',text)

if __name__ == '__main__':
    unittest.main()