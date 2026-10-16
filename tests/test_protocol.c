#include "protocol.h"
#include "capture.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    const uint8_t inventory[] = {0x26,0x01,0x00};
    const uint8_t patch[] = {0x02,0xa1,0x07};
    assert(lc_command_allowed(inventory, sizeof inventory));
    assert(lc_command_allowed(patch, sizeof patch));
    puts("PASS exact allowlisted read commands");
    assert(!lc_command_allowed(NULL,3));
    for(size_t n=0;n<10;n++) {
        uint8_t c[10]={0x02,0xa1,0x07,1,2,3,4,5,6,7};
        if(n!=3) assert(!lc_command_allowed(c,n));
    }
    for(unsigned a=0;a<256;a++) for(unsigned b=0;b<256;b++) {
        uint8_t c[3]={(uint8_t)a,(uint8_t)b,7};
        assert(lc_command_allowed(c,3)==(a==2 && b==0xa1));
    }
    for(unsigned a=0;a<256;a++) for(unsigned b=0;b<256;b++) for(unsigned c=0;c<256;c++) {
        uint8_t frame[3]={(uint8_t)a,(uint8_t)b,(uint8_t)c};
        bool expected=(a==0x26 && b==1 && c==0) || (a==2 && b==0xa1 && c==7);
        assert(lc_command_allowed(frame,3)==expected);
    }
    puts("PASS all 16777216 three-byte commands: exactly two allowlisted");
    puts("PASS payload extension / write / reset / arbitrary command rejected");
    const uint8_t response[7]={0,1,2,3,4,5,6}; uint8_t out[6]={0};
    assert(lc_patch_parse(response,7,out));
    for(size_t n=0;n<6;n++) assert(out[n]==n+1);
    puts("PASS exact six-byte patch extraction");
    for(size_t n=0;n<10;n++) {
        uint8_t r[10]={0}; if(n!=7) assert(!lc_patch_parse(r,n,out));
    }
    for(unsigned f=1;f<256;f++) {
        uint8_t r[7]={(uint8_t)f}; out[0]=99;
        assert(!lc_patch_parse(r,7,out)); assert(out[0]==99);
    }
    assert(!lc_patch_parse(NULL,7,out)); assert(!lc_patch_parse(response,7,NULL));
    puts("PASS malformed / error / extended patch fails closed without output mutation");
    const uint8_t inv[10]={0,0,1,2,3,4,5,6,7,0xe0}; uint8_t uid[8]={0};
    assert(lc_uid_parse(inv,10,uid));
    for(size_t n=0;n<8;n++) assert(uid[n]==inv[n+2]);
    puts("PASS UID retained in inventory wire order");
    for(size_t n=0;n<12;n++) {
        uint8_t r[12]={0}; if(n!=10) assert(!lc_uid_parse(r,n,uid));
    }
    uint8_t badinv[10]={1,0,1,2,3,4,5,6,7,0xe0};
    assert(!lc_uid_parse(badinv,10,uid)); badinv[0]=0;badinv[9]=0;
    assert(!lc_uid_parse(badinv,10,uid));
    assert(!lc_uid_parse(NULL,10,uid));assert(!lc_uid_parse(inv,10,NULL));
    assert(lc_uid_is_ti(uid)); uid[6]=2; assert(!lc_uid_is_ti(uid));
    puts("PASS only TI wire-prefix permits patch query");
    assert(!lc_uid_is_ti(NULL));
    uint8_t hexbytes[3]={0,0xa1,0xff}; char hex[7]={0};
    assert(lc_hex(hexbytes,3,hex,sizeof hex)); assert(strcmp(hex,"00A1FF")==0);
    puts("PASS bounded hex encoding");
    assert(!lc_hex(hexbytes,3,hex,6));assert(!lc_hex(hexbytes,SIZE_MAX,hex,7));
    assert(!lc_hex(NULL,0,hex,7));assert(!lc_hex(hexbytes,0,NULL,7));
    assert(lc_hex(hexbytes,0,hex,1));assert(hex[0]==0);
    /* The at-a-glance identifier is now the DERIVED sensor SERIAL (see
     * tests/test_serial.c for the full transform tests). Here we assert the
     * canonical 16-hex wire UID still encodes and that a distinct sensor
     * produces a distinct derived serial, replacing the retired 4-hex hint. */
    const uint8_t ex[8]={0x5e,0x90,0x25,0x00,0x00,0xa0,0x07,0xe0};
    char full[17];assert(lc_hex(ex,8,full,sizeof full));
    assert(strcmp(full,"5E90250000A007E0")==0);
    const uint8_t eu2patch[6]={0xc5,0x09,0x30,0x01,0x61,0x12};
    char serial[13],serial2[13];assert(lc_serial_from_uid(ex,eu2patch,serial,sizeof serial));
    assert(strcmp(serial,"3M00009DHCR")==0);
    const uint8_t other[8]={0x11,0x22,0x33,0x44,0x55,0x66,0x07,0xe0};
    assert(lc_serial_from_uid(other,eu2patch,serial2,sizeof serial2));
    assert(strcmp(serial,serial2)!=0);
    puts("PASS derived sensor serial replaces the retired 4-hex hint");
    assert(lc_timestamp_valid(1704067200));assert(lc_timestamp_valid(2208988799U));
    assert(!lc_timestamp_valid(0));assert(!lc_timestamp_valid(1704067199));
    assert(!lc_timestamp_valid(2208988800U));
    puts("PASS wall timestamp plausibility bounds (not clock authentication)");
    LcCapture cap={.uid={1,2,3,4,5,6,7,0xe0},.patch={1,2,3,4,5,6},
        .uid_valid=true,.patch_valid=true,.timestamp=1704067200};
    char json[LC_JSON_CAPACITY];size_t jlen=lc_capture_json(&cap,json,sizeof json);
    assert(jlen>0 && jlen==strlen(json));
    assert(strstr(json,"\"schema_version\":2"));
    assert(strstr(json,"\"uid_wire_hex\":\"01020304050607E0\""));
    assert(strstr(json,"\"uid_canonical_convention\":\"uppercase 16-hex ISO15693 inventory wire LSB-first; no byte reversal\""));
    assert(strstr(json,"\"uid_serial_mapping\":\"not established; not Juggluco Sensorid\""));
    /* The derived serial is recorded alongside the UID fields, clearly labelled
     * as DERIVED (not read from the sensor/FRAM), and carries the leading-char
     * caveat. It must reproduce the same transform the UI uses. */
    {
        char want[13];assert(lc_serial_from_uid(cap.uid,cap.patch,want,sizeof want));
        char needle[64];snprintf(needle,sizeof needle,"\"serial_derived\":\"%s\"",want);
        assert(strstr(json,needle));
    }
    assert(strstr(json,"\"serial_derived_source\":\"DERIVED from uid_wire_hex + patch-info family nibble; NOT read from FRAM\""));
    assert(strstr(json,"\"serial_derived_leading_char_caveat\":\"leading char is patch_info[2]>>4; the LibreTools patchInfo[0] table is not used\""));
    assert(strstr(json,"\"fram_length\":0"));
    assert(strstr(json,"\"glucose_available\":false"));
    assert(strstr(json,"\"patch_info_hex\":\"010203040506\""));
    assert(lc_capture_json(&cap,json,jlen+1)==jlen);
    assert(!lc_capture_json(&cap,json,jlen) && !json[0]);
    assert(lc_capture_json(&cap,json,sizeof json)==jlen);
    puts("PASS capture schema includes provenance and no fabricated FRAM/glucose; exact JSON capacity boundary");
    cap.patch_valid=false;cap.timestamp=0;
    assert(lc_capture_json(&cap,json,sizeof json)>0);
    assert(strstr(json,"\"timestamp_unix\":null"));
    assert(strstr(json,"\"patch_info_length\":0"));
    assert(strstr(json,"\"patch_info_hex\":\"\""));
    assert(!lc_capture_json(&cap,json,8));assert(json[0]==0);
    cap.uid_valid=false;assert(!lc_capture_json(&cap,json,sizeof json));
    assert(!lc_capture_json(NULL,json,sizeof json));
    cap.uid_valid=true;cap.fram_length=LC_FRAM_SIZE;cap.transcript_length=LC_TRANSCRIPT_MAX;
    for(size_t i=0;i<LC_TRANSCRIPT_MAX;i++) {
        cap.transcript[i].command_length=4;
        cap.transcript[i].response_length=LC_RESPONSE_MAX;
        cap.transcript[i].error=UINT32_MAX;
    }
    jlen=lc_capture_json(&cap,json,sizeof json);assert(jlen>0);
    assert(lc_capture_json(&cap,json,jlen+1)==jlen);
    assert(!lc_capture_json(&cap,json,jlen) && !json[0]);
    puts("PASS maximal bounded transcript JSON capacity and one-byte-short rejection");
    puts("PASS partial capture and invalid RTC honestly represented; truncation rejected");
    uint8_t gate_uid[8]={0,0,0,0,0,0,7,0xe0};
    for(unsigned first=0;first<256;first++) for(unsigned gen=0;gen<256;gen++) {
        uint8_t p[6]={(uint8_t)first,0,(uint8_t)gen,0,0,0};
        bool expected=(first==0xc5 || first==0x9d) &&
            (((gen>>4)==3 && (gen&15)<9) || ((gen>>4)==7 && (gen&15)<4));
        assert(lc_family_allowed(gate_uid,p,6)==expected);
    }
    for(size_t i=0;i<15;i++) {
        uint8_t command[4], response[27]={0}, payload[24];
        assert(lc_read_command(i,command));assert(lc_command_allowed(command,4));
        size_t blocks=command[3]+1U;
        for(size_t n=0;n<27;n++) assert(lc_read_parse(response,n,blocks,payload)==(n==1+8*blocks));
        for(unsigned flags=1;flags<256;flags++) {
            response[0]=(uint8_t)flags;assert(!lc_read_parse(response,1+8*blocks,blocks,payload));
        }
    }
    for(unsigned start=0;start<256;start++) for(unsigned count=0;count<256;count++) {
        uint8_t frame[4]={2,0x23,(uint8_t)start,(uint8_t)count};
        assert(lc_command_allowed(frame,4)==((start<42 && start%3==0 && count==2) || (start==42 && count==0)));
    }
    puts("PASS exhaustive EU2/security gate + 65536 read-multiple start/count combinations; exact response lengths/flags");
    return 0;
}
