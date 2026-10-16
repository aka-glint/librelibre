#pragma once
#include "capture.h"
/* Experimental byte integrity only, no physiology or clinical validity.
 * Wire-order UID; conservative EU2/gen1 gate. See INTEGRITY-LICENSE.md. */
typedef enum {
    LcIntegrityNotChecked, LcIntegrityPass, LcIntegrityFail, LcIntegrityUnsupported
} LcIntegrityStatus;
/* Historical raw diagnostics only. Never establishes usable/current glucose. */
typedef enum {
    LcSensorNotChecked, LcSensorParsed, LcSensorCrcFail,
    LcSensorUnsupported, LcSensorProfileMismatch, LcSensorInvalidIndex
} LcSensorParseStatus;
typedef struct {
    uint16_t age_minutes;
    uint16_t max_life_minutes;
    uint16_t quality_bits;
    uint8_t parse_status;
    uint8_t status_code;
    uint8_t trend_index;
    uint8_t history_index;
    uint8_t newest_slot;
    uint8_t quality_flags;
    bool has_error;
} LcSensor;
void lc_sensor_parse(const uint8_t plain[344], const uint8_t patch[6], LcSensor* result);
typedef struct {
    LcIntegrityStatus status;
    uint16_t pairs[6];
    LcSensor sensor;
} LcIntegrity;
/* Only complete, transcript-bound and strictly gated captures are decoded. */
void lc_integrity_check(const LcCapture* capture, LcIntegrity* result);
/* pairs: stored/computed for header, body, footer. */
uint16_t lc_integrity_crc(const uint8_t* bytes, size_t length);
bool lc_integrity_sections(const uint8_t plain[344], uint16_t pairs[6]);
void lc_integrity_transform(const uint8_t uid[8], const uint8_t patch[6], const uint8_t raw[344], uint8_t out[344]);
void lc_integrity_prepare(const uint8_t uid[8], uint16_t x, uint16_t y, uint16_t words[4]);
