/* Host-only binary differential harness; input/output never logged. */
#include "integrity.h"
#include "lifecycle.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void complete_capture(LcCapture* c) {
    c->uid_valid = c->patch_valid = true;
    c->fram_length = 344;
    c->transcript_length = 17;
    LcExchange* e = &c->transcript[0];
    const uint8_t inventory[3] = {0x26,1,0}, patch[3] = {2,0xa1,7};
    memcpy(e->command,inventory,3); e->command_length=3; e->response_length=10;
    memcpy(e->response+2,c->uid,8);
    e = &c->transcript[1];
    memcpy(e->command,patch,3); e->command_length=3; e->response_length=7;
    memcpy(e->response+1,c->patch,6);
    for(unsigned i=0;i<15;i++) {
        e=&c->transcript[i+2]; assert(lc_read_command(i,e->command));
        e->command_length=4; e->response_length=1+8*(e->command[3]+1);
        memcpy(e->response+1,c->fram+i*24,e->response_length-1);
    }
}
int main(void) {
    LcCapture c={0}; LcIntegrity result={0}; uint8_t plain[344];
    assert(fread(c.uid,1,8,stdin)==8);
    assert(fread(c.patch,1,6,stdin)==6);
    assert(fread(c.fram,1,344,stdin)==344);
    complete_capture(&c);
    LcCapture before=c;
    lc_integrity_check(&c,&result);
    assert(!memcmp(&c,&before,sizeof(c)) && "capture must remain untouched");
    lc_integrity_transform(c.uid,c.patch,c.fram,plain);
    assert(fwrite(plain,1,344,stdout)==344);
    for(unsigned i=0;i<6;i++) {
        assert(fputc(result.pairs[i]&255,stdout)!=EOF);
        assert(fputc(result.pairs[i]>>8,stdout)!=EOF);
    }
    assert(fputc(result.status,stdout)!=EOF);
    c.fram_length=343;lc_integrity_check(&c,&result);assert(result.status==LcIntegrityNotChecked);
    for(unsigned i=0;i<6;i++) assert(result.pairs[i]==0);
    c=before;c.transcript[2].response[1]^=1;
    lc_integrity_check(&c,&result);assert(result.status==LcIntegrityNotChecked);
    c=before;c.patch[3]=2;c.transcript[1].response[4]=2;
    lc_integrity_check(&c,&result);assert(result.status==LcIntegrityUnsupported);
    c=before;c.patch[0]=0xc6;
    lc_integrity_check(&c,&result);assert(result.status==LcIntegrityNotChecked);
    lc_integrity_check(NULL,&result);assert(result.status==LcIntegrityNotChecked);
    lc_integrity_check(&c,NULL);
    return 0;
}
