/* Adapted from gui-dos/DiaBLE, MIT, Copyright (c) 2026 Guido Soranzio.
 * Pinned e6a909c88faeada49f461d30834174cd95db4042; Libre2.swift 64-105,
 * 126-171; Libre.swift 247-261; Extensions.swift 36-43.
 * EU2 transform, byte CRC and raw diagnostics; no glucose. See INTEGRITY-LICENSE.md. */
#include "integrity.h"
#include <string.h>

void lc_integrity_check(const LcCapture* capture, LcIntegrity* result) {
    if(!result) return;
    memset(result, 0, sizeof(*result));
    if(!lc_capture_complete(capture)) return;
    /* Match Python's stricter patch[3] gate without changing ANY RF policy. */
    if(capture->patch[3] != 1) {
        result->status = LcIntegrityUnsupported;
        return;
    }
    uint8_t plain[LC_FRAM_SIZE];
    lc_integrity_transform(capture->uid, capture->patch, capture->fram, plain);
    result->status = lc_integrity_sections(plain, result->pairs) ? LcIntegrityPass : LcIntegrityFail;
    lc_sensor_parse(plain, capture->patch, &result->sensor);
    /* No decrypted FRAM retained or serialized. */
    volatile uint8_t* wipe = plain;
    for(size_t i = 0; i < sizeof(plain); i++) wipe[i] = 0;
}

static uint16_t little_word(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
/* DiaBLE Libre.swift:116-145,179-181; Sensor.swift:77-98, same pinned MIT source.
 * Only the newest trend slot is inspected; never search older slots for a value. */
void lc_sensor_parse(const uint8_t plain[344], const uint8_t patch[6], LcSensor* result) {
    if(!result) return;
    memset(result, 0, sizeof(*result));
    if(!plain || !patch) return;
    uint16_t pairs[6];
    if(!lc_integrity_sections(plain, pairs)) {
        result->parse_status = LcSensorCrcFail;
        return;
    }
    if((patch[0] != 0xc5 && patch[0] != 0x9d) || patch[2] != 0x30 || patch[3] != 1) {
        result->parse_status = LcSensorUnsupported;
        return;
    }
    if(plain[322] != patch[2] || plain[323] != patch[3]) {
        result->parse_status = LcSensorProfileMismatch;
        return;
    }
    if(plain[26] >= 16 || plain[27] >= 32) {
        result->parse_status = LcSensorInvalidIndex;
        return;
    }
    result->status_code = plain[4];
    result->age_minutes = little_word(plain+316);
    result->max_life_minutes = little_word(plain+326);
    result->trend_index = plain[26];
    result->history_index = plain[27];
    result->newest_slot = (plain[26]+15U)%16U;
    const uint8_t* record = plain+28+result->newest_slot*6U;
    uint32_t bits = (uint32_t)record[1] | ((uint32_t)record[2]<<8) | ((uint32_t)record[3]<<16);
    result->quality_bits = (uint16_t)((bits>>6)&0x1ffU);
    result->quality_flags = (uint8_t)((bits>>15)&3U);
    result->has_error = ((bits>>17)&1U) != 0;
    result->parse_status = LcSensorParsed;
}
uint16_t lc_integrity_crc(const uint8_t* bytes, size_t length) {
    uint16_t crc = 0xffffU;
    for(size_t i = 0; i < length; i++) {
        crc ^= bytes[i];
        for(unsigned bit = 0; bit < 8; bit++)
            crc = (uint16_t)((crc >> 1) ^ ((crc & 1U) ? 0x8408U : 0U));
    }
    uint16_t reversed = 0;
    for(unsigned bit = 0; bit < 16; bit++) {
        reversed = (uint16_t)((reversed << 1) | (crc & 1U));
        crc >>= 1;
    }
    return reversed;
}
bool lc_integrity_sections(const uint8_t plain[344], uint16_t pairs[6]) {
    static const unsigned bounds[4] = {0, 24, 320, 344};
    bool passed = true;
    for(unsigned section = 0; section < 3; section++) {
        unsigned start = bounds[section];
        pairs[section*2] = little_word(plain+start);
        pairs[section*2+1] = lc_integrity_crc(plain+start+2, bounds[section+1]-start-2);
        if(pairs[section*2] != pairs[section*2+1]) passed = false;
    }
    return passed;
}
static uint16_t crypto_op(uint16_t value) {
    return (uint16_t)((value >> 2) ^ ((value & 1U) ? 0x6860U : 0U) ^
                      ((value & 2U) ? 0xa0c5U : 0U));
}
static void process_crypto(const uint16_t words[4], uint16_t key[4]) {
    uint16_t r0 = crypto_op(words[0]) ^ words[3];
    uint16_t r1 = crypto_op(r0) ^ words[2];
    uint16_t r2 = crypto_op(r1) ^ words[1];
    uint16_t r3 = crypto_op(r2) ^ words[0];
    uint16_t r4 = crypto_op(r3);
    uint16_t r5 = crypto_op(r4 ^ r0);
    uint16_t r6 = crypto_op(r5 ^ r1);
    uint16_t r7 = crypto_op(r6 ^ r2);
    key[0] = r3 ^ r7; key[1] = r2 ^ r6;
    key[2] = r1 ^ r5; key[3] = r0 ^ r4;
}
void lc_integrity_transform(const uint8_t uid[8], const uint8_t patch[6], const uint8_t raw[344], uint8_t out[344]) {
    uint16_t y = little_word(patch+4) ^ 0x44U;
    for(uint16_t block = 0; block < 43; block++) {
        uint16_t words[4], key[4];
        lc_integrity_prepare(uid, block, y, words);
        process_crypto(words, key);
        for(unsigned j = 0; j < 8; j++) {
            unsigned offset = block * 8U + j;
            out[offset] = raw[offset] ^ (uint8_t)(key[j/2] >> ((j%2)*8));
        }
    }
}
void lc_integrity_prepare(const uint8_t uid[8], uint16_t x, uint16_t y, uint16_t words[4]) {
    words[0] = (uint16_t)((uint32_t)little_word(uid+4) + x + y);
    words[1] = little_word(uid+2);
    words[2] = (uint16_t)((uint32_t)little_word(uid) + 2U * x);
    words[3] = 0x241aU ^ 0x14c6U;
}
