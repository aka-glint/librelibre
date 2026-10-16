#include "protocol.h"
bool lc_timestamp_valid(uint32_t timestamp) { return timestamp>=1704067200U && timestamp<2208988800U; }
bool lc_hex(const uint8_t* data, size_t len, char* out, size_t capacity) {
    static const char digits[]="0123456789ABCDEF";
    if(!data || !out || len > (SIZE_MAX-1)/2 || capacity < len*2+1) return false;
    for(size_t i=0;i<len;i++) { out[i*2]=digits[data[i]>>4]; out[i*2+1]=digits[data[i]&15]; }
    out[len*2]=0; return true;
}
/* DERIVED sensor serial. Transcribed from LibreTools SensorSerialNumber and
 * DiaBLE Libre.swift serialNumber(uid:family:); see SERIAL-IN-FRAM.md. Swift
 * `<<`/`>>` bind tighter than `+`, so the five-bit groups below are the exact
 * ((x<<n) + (y>>m)) shuffles, masked to 0x1F. The alphabet omits B/I/O/S. The
 * leading character is the patch-info family nibble (patch[2] >> 4), NOT the
 * LibreTools patchInfo[0] table which lacks the EU2 0xC5 entry. Byte reversal of
 * the wire-order UID is required: the reversed last-six bytes are the packed
 * source. Fails closed (buffer untouched) on NULL/short capacity. */
bool lc_serial_from_uid(const uint8_t uid[8], const uint8_t patch[6], char* out, size_t capacity) {
    static const char alphabet[]="0123456789ACDEFGHJKLMNPQRTUVWXYZ"; /* 32 symbols */
    if(!uid || !patch || !out || capacity < 12) return false;
    uint8_t b[6];
    /* Reverse the whole wire-order UID, then take its LAST six bytes, i.e. wire
     * UID bytes [5..0] in reverse order. The standard first two reversed bytes
     * (0x07 0xE0) are the TI tag identity and are excluded. For the public
     * worked example wire UID 5E90250000A007E0 this yields A0 00 00 25 90 5E. */
    for(size_t i=0;i<6;i++) b[i] = uid[5-i];
    uint16_t five[10];
    five[0]=(uint16_t)(b[0]>>3);
    five[1]=(uint16_t)((b[0]<<2)+(b[1]>>6));
    five[2]=(uint16_t)(b[1]>>1);
    five[3]=(uint16_t)((b[1]<<4)+(b[2]>>4));
    five[4]=(uint16_t)((b[2]<<1)+(b[3]>>7));
    five[5]=(uint16_t)(b[3]>>2);
    five[6]=(uint16_t)((b[3]<<3)+(b[4]>>5));
    five[7]=(uint16_t)b[4];
    five[8]=(uint16_t)(b[5]>>3);
    five[9]=(uint16_t)(b[5]<<2);
    unsigned family = patch[2] >> 4;
    if(family > 9) return false; /* printed serials carry a single decimal digit */
    out[0] = (char)('0' + family);
    for(size_t i=0;i<10;i++) out[i+1] = alphabet[five[i] & 0x1F];
    out[11]=0;
    return true;
}
bool lc_uid_is_ti(const uint8_t uid[8]) { return uid && uid[7]==0xe0 && uid[6]==0x07; }
bool lc_family_allowed(const uint8_t uid[8], const uint8_t* patch, size_t len) {
    if(!lc_uid_is_ti(uid) || !patch || len != 6 || (patch[0] != 0xc5 && patch[0] != 0x9d)) return false;
    unsigned hi = patch[2] >> 4, lo = patch[2] & 15;
    return (hi == 3 && lo < 9) || (hi == 7 && lo < 4);
}
bool lc_uid_parse(const uint8_t* response, size_t len, uint8_t out[8]) {
    if(!response || !out || len != 10 || response[0] != 0 || response[9] != 0xe0) return false;
    for(size_t i=0;i<8;i++) out[i]=response[i+2];
    return true;
}
bool lc_patch_parse(const uint8_t* response, size_t len, uint8_t out[6]) {
    if(!response || !out || len != 7 || response[0] != 0) return false;
    for(size_t i=0;i<6;i++) out[i]=response[i+1];
    return true;
}
bool lc_command_allowed(const uint8_t* cmd, size_t len) {
    if(!cmd) return false;
    if(len == 4) return cmd[0] == 2 && cmd[1] == 0x23 &&
        ((cmd[2] < 42 && cmd[2] % 3 == 0 && cmd[3] == 2) || (cmd[2] == 42 && cmd[3] == 0));
    if(len != 3) return false;
    return (cmd[0] == 0x26 && cmd[1] == 0x01 && cmd[2] == 0x00) ||
           (cmd[0] == 0x02 && cmd[1] == 0xa1 && cmd[2] == 0x07);
}
bool lc_read_command(size_t index, uint8_t out[4]) {
    if(!out || index >= 15) return false;
    out[0]=2; out[1]=0x23; out[2]=(uint8_t)(index*3); out[3]=index==14?0:2;
    return true;
}
bool lc_read_parse(const uint8_t* response, size_t len, size_t blocks, uint8_t* out) {
    if(!response || !out || (blocks != 1 && blocks != 3) || len != 1+blocks*8 || response[0] != 0) return false;
    for(size_t i=0;i<blocks*8;i++) out[i]=response[i+1];
    return true;
}

#include <string.h>
static uint16_t lc_proto_u16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint16_t lc_proto_op(uint16_t value) {
    return (uint16_t)((value >> 2) ^ ((value & 1U) ? 0x6860U : 0U) ^ ((value & 2U) ? 0xa0c5U : 0U));
}
void lc_integrity_ble_enable_cmd(const uint8_t uid[8], const uint8_t patch[6], uint32_t unlock_code, uint8_t cmd_out[12]) {
    uint8_t params[4] = {
        (uint8_t)(unlock_code & 0xffU),
        (uint8_t)((unlock_code >> 8) & 0xffU),
        (uint8_t)((unlock_code >> 16) & 0xffU),
        (uint8_t)((unlock_code >> 24) & 0xffU),
    };
    uint16_t secret = (uint16_t)(lc_proto_u16(patch + 4) ^ lc_proto_u16(params));
    uint16_t w0 = (uint16_t)((uint32_t)lc_proto_u16(uid + 4) + 0x001eU + secret);
    uint16_t w1 = lc_proto_u16(uid + 2);
    uint16_t w2 = (uint16_t)((uint32_t)lc_proto_u16(uid) + 2U * 0x001eU);
    uint16_t w3 = 0x241aU ^ 0x14c6U;
    uint16_t r0 = lc_proto_op(w0) ^ w3;
    uint16_t r1 = lc_proto_op(r0) ^ w2;
    uint16_t r2 = lc_proto_op(r1) ^ w1;
    uint16_t r3 = lc_proto_op(r2) ^ w0;
    uint16_t r4 = lc_proto_op(r3);
    uint16_t r5 = lc_proto_op(r4 ^ r0);
    uint16_t r6 = lc_proto_op(r5 ^ r1);
    uint16_t r7 = lc_proto_op(r6 ^ r2);
    uint16_t k0 = (uint16_t)((r3 ^ r7) ^ 0x4163U);
    uint16_t k1 = (uint16_t)((r2 ^ r6) ^ 0x4344U);
    cmd_out[0] = 0x02U;
    cmd_out[1] = 0xa1U;
    cmd_out[2] = 0x07U;
    cmd_out[3] = 0x1eU;
    memcpy(cmd_out + 4, params, 4U);
    cmd_out[8]  = (uint8_t)(k0 & 0xffU);
    cmd_out[9]  = (uint8_t)((k0 >> 8) & 0xffU);
    cmd_out[10] = (uint8_t)(k1 & 0xffU);
    cmd_out[11] = (uint8_t)((k1 >> 8) & 0xffU);
}
bool lc_integrity_parse_ble_mac(const uint8_t* response, size_t len, uint8_t mac_be_out[6]) {
    if(!response || !mac_be_out || len != 7U || response[0] != 0U) return false;
    for(size_t i = 0; i < 6U; i++) mac_be_out[i] = response[6U - i];
    return true;
}
