/* Sensor-clock remaining-life and freshness. Pure, host-testable, no Furi/RTC.
 * Grounding: age_minutes and max_life_minutes are the sensor's OWN FRAM
 * minutes (see integrity.c little_word(plain+316) and (plain+326)); the newest
 * trend record's sensor minute is age_minutes and older records are age_minutes
 * minus their newest-first index (DiaBLE Libre.swift:128-145). Nothing here
 * reads the device clock, so every figure is from one capture's sensor clock. */
#include "lifecycle.h"
#include <stdio.h>

/* Display breakpoint: at or above one day (1440 minutes) the days+hours form is
 * more readable for a ~14-day sensor ("Left 12d 6h"); below a day, hours+minutes
 * is more useful ("Left 7h 12m"). The comparison is inclusive at exactly 1440. */
#define LC_MINUTES_PER_DAY 1440U
#define LC_MINUTES_PER_HOUR 60U

void lc_life_compute(uint16_t age_minutes, uint16_t max_life_minutes,
                     uint16_t sample_index, bool fields_valid, LcLife* result) {
    if(!result) return;
    result->status = LcLifeUnknown;
    result->remaining_minutes = 0;
    result->sample_age_minutes = 0;
    result->sample_index = sample_index;
    result->sensor_minute = 0;
    result->life_known = max_life_minutes != 0;
    result->freshness_known = false;
    /* Widen BEFORE subtracting: both inputs are uint16_t, but age can exceed
     * max_life and sample_index can exceed age, so a uint16_t subtraction would
     * wrap. Using uint32_t keeps both differences exact. */
    uint32_t age = age_minutes;
    uint32_t life = max_life_minutes;
    if(life == 0) {
        result->status = LcLifeUnknown;
    } else if(age > life) {
        result->status = LcLifeExceeded;  /* expired: no remaining, no wrap */
        result->remaining_minutes = 0;
    } else {
        result->status = LcLifeRemaining;
        result->remaining_minutes = life - age;
    }
    /* Freshness: the shown sample's age in sensor minutes. Only a capture whose
     * sensor fields parsed (fields_valid) and whose index does not run before
     * sensor start can carry this. sample_index > age would mean a negative
     * sensor minute, i.e. a sample before activation -> fail closed. */
    uint32_t index = sample_index;
    if(fields_valid && index <= age) {
        result->freshness_known = true;
        result->sample_age_minutes = index;
        result->sensor_minute = (uint16_t)(age - index);
    }
}

bool lc_life_format_remaining(const LcLife* life, char* buf, size_t cap) {
    if(!life || !buf || cap == 0) return false;
    switch(life->status) {
    case LcLifeUnknown:
        return snprintf(buf, cap, "Life unknown") > 0;
    case LcLifeExceeded:
        return snprintf(buf, cap, "Life exceeded") > 0;
    case LcLifeRemaining:
    default: {
        uint32_t minutes = life->remaining_minutes;
        int n;
        if(minutes >= LC_MINUTES_PER_DAY) {
            n = snprintf(buf, cap, "Left %ud %uh", (unsigned)(minutes / LC_MINUTES_PER_DAY),
                         (unsigned)((minutes % LC_MINUTES_PER_DAY) / LC_MINUTES_PER_HOUR));
        } else {
            n = snprintf(buf, cap, "Left %uh %um", (unsigned)(minutes / LC_MINUTES_PER_HOUR),
                         (unsigned)(minutes % LC_MINUTES_PER_HOUR));
        }
        return n > 0 && (size_t)n < cap;
    }
    }
}

bool lc_freshness_format(const LcLife* life, char* buf, size_t cap) {
    if(!life || !buf || cap == 0) return false;
    if(!life->freshness_known) return snprintf(buf, cap, "Fresh unknown") > 0;
    /* Compact so it fits the estimate row beside the remaining life on a 128 px
     * screen. "=now" is the newest sample of this capture (sensor minute == age);
     * "=old" is an older trend slot, lagging by that many sensor minutes. */
    const char* state = life->sample_age_minutes == 0 ? "now" : "old";
    int n = snprintf(buf, cap, "Fresh %um=%s", (unsigned)life->sample_age_minutes, state);
    return n > 0 && (size_t)n < cap;
}
