#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sensor-clock remaining-life and freshness. NO wall-clock/RTC arithmetic:
 * age_minutes and max_life_minutes are the sensor's OWN FRAM minutes
 * (integrity.c:316 and :326). See FRESHNESS-LIFE-BUILD.md. */
typedef enum {
    LcLifeUnknown,   /* max_life_minutes == 0: no stored life to compare against. */
    LcLifeExceeded,  /* age_minutes > max_life_minutes: expired; never a wrapped value. */
    LcLifeRemaining, /* remaining = max_life - age, computed in a wider type. */
} LcLifeStatus;
/* All inputs are the SAME capture's sensor fields. sample_index is the trend
 * record's newest-first index: 0 = the newest record (the one the UI shows),
 * i = age_minutes-i sensor minute (DiaBLE Libre.swift:128-145). */
typedef struct {
    LcLifeStatus status;
    uint32_t remaining_minutes;  /* valid only when status == LcLifeRemaining */
    uint32_t sample_age_minutes; /* sensor minutes since the shown sample */
    uint16_t sample_index;
    uint16_t sensor_minute;      /* the shown sample's sensor-clock minute, or 0 */
    bool life_known;             /* max_life_minutes != 0 */
    bool freshness_known;        /* sample age establishable from this capture */
} LcLife;
/* Compute from one capture. age/max_life are uint16_t; the subtraction is done
 * in uint32_t so no value can wrap, and sample_index must not exceed age. */
void lc_life_compute(uint16_t age_minutes, uint16_t max_life_minutes,
                     uint16_t sample_index, bool fields_valid, LcLife* result);
/* Formats into buf. Returns false if buf too small / arguments null. */
bool lc_life_format_remaining(const LcLife* life, char* buf, size_t cap);
bool lc_freshness_format(const LcLife* life, char* buf, size_t cap);

#ifdef __cplusplus
}
#endif
