#include "capture.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

bool lc_capture_complete(const LcCapture* c) {
    if(!c || !c->uid_valid || !c->patch_valid || c->error ||
       !lc_family_allowed(c->uid,c->patch,6) || c->fram_length != LC_FRAM_SIZE ||
       c->transcript_length != LC_TRANSCRIPT_MAX) return false;
    const uint8_t inventory[3]={0x26,1,0}, patch_command[3]={2,0xa1,7};
    const LcExchange* inventory_exchange=&c->transcript[0];
    const LcExchange* patch_exchange=&c->transcript[1];
    uint8_t uid[8], patch[6];
    if(inventory_exchange->error || inventory_exchange->command_length!=3 ||
       memcmp(inventory_exchange->command,inventory,3) ||
       !lc_uid_parse(inventory_exchange->response,inventory_exchange->response_length,uid) || memcmp(uid,c->uid,8) ||
       patch_exchange->error || patch_exchange->command_length!=3 ||
       memcmp(patch_exchange->command,patch_command,3) ||
       !lc_patch_parse(patch_exchange->response,patch_exchange->response_length,patch) || memcmp(patch,c->patch,6)) return false;
    size_t offset=0;
    for(size_t i=0;i<15;i++) {
        uint8_t command[4], payload[24];
        const LcExchange* e=&c->transcript[i+2];
        lc_read_command(i,command);
        size_t bytes=(command[3]+1U)*8U;
        if(e->error || e->command_length!=4 || memcmp(e->command,command,4) ||
           !lc_read_parse(e->response,e->response_length,command[3]+1U,payload) ||
           memcmp(c->fram+offset,payload,bytes)) return false;
        offset+=bytes;
    }
    return true;
}
static bool append(char* out, size_t capacity, size_t* used, const char* format, ...) {
    va_list args;
    va_start(args,format);
    int n=vsnprintf(out+*used,capacity-*used,format,args);
    va_end(args);
    if(n<0 || (size_t)n>=capacity-*used) return false;
    *used+=(size_t)n;
    return true;
}
/* RTC firmware encodes calendar fields as seconds since 1970 without a TZ.
 * Decode those same fields, NEVER apply host timezone or invent UTC. */
bool lc_capture_local_datetime(uint32_t raw, char* out, size_t capacity) {
    if(!out || capacity < 20) return false;
    out[0] = 0;
    if(!lc_timestamp_valid(raw)) return false;
    uint32_t days = raw / 86400U, seconds = raw % 86400U;
    unsigned year = 1970, month = 1;
    for(;; year++) {
        unsigned length = (year%4 == 0 && (year%100 != 0 || year%400 == 0)) ? 366 : 365;
        if(days < length) break;
        days -= length;
    }
    const unsigned months[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    for(; month <= 12; month++) {
        unsigned length = months[month-1];
        if(month == 2 && year%4 == 0 && (year%100 != 0 || year%400 == 0)) length++;
        if(days < length) break;
        days -= length;
    }
    snprintf(out, capacity, "%04u-%02u-%02uT%02u:%02u:%02u", year, month,
             (unsigned)days+1, (unsigned)(seconds/3600U),
             (unsigned)(seconds/60U%60U), (unsigned)(seconds%60U));
    return true;
}
size_t lc_capture_json(const LcCapture* c, char* out, size_t capacity) {
    if(!c || !out || !capacity || !c->uid_valid) return 0;
    out[0]=0;
    if(c->fram_length>LC_FRAM_SIZE || c->transcript_length>LC_TRANSCRIPT_MAX) return 0;
    char uid[17],patch[13],fram[689],timestamp[24],local[20],local_json[24];
    if(lc_capture_local_datetime(c->timestamp, local, sizeof local))
        snprintf(local_json, sizeof local_json, "\"%s\"", local);
    else snprintf(local_json, sizeof local_json, "null");
    lc_hex(c->uid,8,uid,sizeof uid);
    lc_hex(c->patch,c->patch_valid?6:0,patch,sizeof patch);
    lc_hex(c->fram,c->fram_length,fram,sizeof fram);
    if(lc_timestamp_valid(c->timestamp)) snprintf(timestamp,sizeof timestamp,"%lu",(unsigned long)c->timestamp);
    else snprintf(timestamp,sizeof timestamp,"null");
    char save_local[20],save_json[24],save_raw[24];
    if(c->save_time_recorded) {
        snprintf(save_raw,sizeof save_raw,"%lu",(unsigned long)c->save_timestamp);
        if(lc_capture_local_datetime(c->save_timestamp,save_local,sizeof save_local))
            snprintf(save_json,sizeof save_json,"\"%s\"",save_local);
        else snprintf(save_json,sizeof save_json,"null");
    } else {
        snprintf(save_raw,sizeof save_raw,"null");
        snprintf(save_json,sizeof save_json,"null");
    }
    bool family=c->patch_valid && lc_family_allowed(c->uid,c->patch,6);
    /* Derived sensor serial, recorded only when it can actually be computed from
     * this capture's UID and patch. Clearly labelled DERIVED (never read from
     * FRAM/the sensor); empty string otherwise so no fabricated/stale value. */
    char serial[13]="";
    if(c->uid_valid && c->patch_valid)
        lc_serial_from_uid(c->uid,c->patch,serial,sizeof serial);
    size_t used=0;
    if(!append(out,capacity,&used,
        "{\"schema\":\"libre-nfc-research-capture\",\"schema_version\":2,"
        "\"app_version\":\"0.2.0\",\"sdk_target\":\"f7\","
        "\"capture_kind\":\"conservative_raw_fram\",\"uid_order\":\"ISO15693 inventory wire LSB-first\","
        "\"uid_canonical_convention\":\"uppercase 16-hex ISO15693 inventory wire LSB-first; no byte reversal\","
        "\"uid_serial_mapping\":\"not established; not Juggluco Sensorid\","
        "\"serial_derived\":\"%s\","
        "\"serial_derived_source\":\"DERIVED from uid_wire_hex + patch-info family nibble; NOT read from FRAM\","
        "\"serial_derived_leading_char_caveat\":\"leading char is patch_info[2]>>4; the LibreTools patchInfo[0] table is not used\","
        "\"uid_wire_hex\":\"%s\",\"patch_info_hex\":\"%s\",\"patch_info_length\":%u,"
        "\"timestamp_unix\":%s,\"timestamp_source\":\"device RTC; plausibility only\","
        "\"timestamp_unix_semantics\":\"legacy local-wall epoch encoding; not UTC\","
        "\"scan_rtc_raw\":%lu,\"scan_local_datetime\":%s,"
        "\"scan_timezone_status\":\"unknown\",\"scan_utc_offset_minutes\":null,\"scan_utc_unix\":null,"
        "\"scan_time_event\":\"UID accepted\","
        "\"save_rtc_raw\":%s,\"save_local_datetime\":%s,"
        "\"save_timezone_status\":\"unknown\",\"save_utc_unix\":null,"
        "\"save_time_event\":\"save attempt started; not completion\","
        "\"elapsed_ms\":%lu,\"transport_error\":%u,\"inventory_attempts\":%u,"
        "\"family\":\"%s\",\"family_verified\":false,\"family_gate_passed\":%s,"
        "\"fram_length\":%u,\"fram_expected_length\":344,\"fram_hex\":\"%s\","
        "\"complete\":%s,\"integrity\":\"not checked; raw may be encrypted\","
        "\"snapshot_stability\":\"not checked; sequential reads\",\"glucose_available\":false,"
        "\"transcript_boundary\":\"SDK returned bytes; link CRC removed; original bit alignment unavailable\","
        "\"inventory_transcript_policy\":\"successful UID response only; prior attempts counted\",\"transcript\":[",
        serial,uid,patch,c->patch_valid?6U:0U,timestamp,(unsigned long)c->timestamp,local_json,save_raw,save_json,
        (unsigned long)c->elapsed_ms,c->error,c->inventory_attempts,
        family?"historical standard EU2/gen1 candidate":"unsupported or unverified",
        family?"true":"false",(unsigned)c->fram_length,fram,lc_capture_complete(c)?"true":"false")) goto fail;
    for(size_t i=0;i<c->transcript_length;i++) {
        const LcExchange* e=&c->transcript[i];
        char command[9],response[513];
        if(e->command_length>4 || e->response_length>LC_RESPONSE_MAX ||
           !lc_hex(e->command,e->command_length,command,sizeof command) ||
           !lc_hex(e->response,e->response_length,response,sizeof response)) goto fail;
        if(!append(out,capacity,&used,"%s{\"command_hex\":\"%s\",\"response_hex\":\"%s\",\"response_length\":%u,\"error\":%u}",
            i?",":"",command,response,(unsigned)e->response_length,e->error)) goto fail;
    }
    if(c->ble_mac_valid) {
        if(!append(out,capacity,&used,"],\"ble_paired_mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"ble_unlock_code\":42}\n",
                   c->ble_mac[0],c->ble_mac[1],c->ble_mac[2],c->ble_mac[3],c->ble_mac[4],c->ble_mac[5])) goto fail;
    } else {
        if(!append(out,capacity,&used,"]}\n")) goto fail;
    }
    return used;
fail:
    out[0]=0;
    return 0;
}
