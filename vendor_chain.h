#pragma once
/* LibreLibre RECOVERED vendor raw-to-estimator chain (bounded port).
 *
 * Implements the arithmetic that is CLOSED and instruction-verified in
 * libre-nfc-research (PARSER-CONFIG-RECOVERY.md, SYMBOLIC-DISPATCH-EXECUTION.md,
 * RAW-CORRECTION-COMPOSITION.md, RAW-TO-ESTIMATOR.md):
 *
 *   FRAM 6-byte record decode        (0x52e3f)
 *   -> 16-byte double tuple          (0x9064 accessor adapters 0x9056/0x9998/0x99ad)
 *   -> 0x88a0 ADC scale
 *   -> 0xdb4c temperature cubic-in-log  (degrees C)
 *   -> 0xe6f4 temperature gain
 *   -> 0xf0e4 windowed mean
 *   -> 0xc36c factory reciprocal-linear
 *   -> leaf regression 0x1f7e8 with weighted average 0x1f2e8
 *
 * DELIBERATELY NOT IMPLEMENTED (documented residual):
 *   the vendor smoothing / admission layer  0x18e90..0x1b25a
 *   and the 0x119dc state adjustment.
 * The corrected tuple is the LINEARISED raw estimate; the vendor's displayed
 * current value is not implemented. The current unsmoothed mismatch remains
 * UNRESOLVED; omitted smoothing is not a proven explanation. No fitting.
 *
 * PRECISION: every vendor stage is scalar f64. This module is built with a
 * double path and an optional float path (LC_VENDOR_USE_FLOAT) so the
 * float-sufficiency question can be measured on the target. See
 * VENDOR-CHAIN-PORT.md for the measured float-vs-double delta.
 *
 * MIT. No tuning, no fitting, no binary patching. Constants come from the
 * recovered image / payload only.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Compile-time precision toggle: 0 = double (vendor-exact), 1 = hardware float. */
#ifndef LC_VENDOR_USE_FLOAT
#define LC_VENDOR_USE_FLOAT 0
#endif

#if LC_VENDOR_USE_FLOAT
typedef float LcVendorReal;
#else
typedef double LcVendorReal;
#endif

/* Factory config object (recovered offsets; cross-checked i2..i6 family). */
typedef struct {
    int i1;   /* +0x0c byte, 3-bit header field -> slope table 0xa1d20 index */
    int i2;   /* 10-bit header field -> t1/t2 table index */
    int i3;   /* +0x00 signed i16, footer & 0xFF (sign bit 33) */
    int i4;   /* +0x02 unsigned u16, (footer>>8)&0x3FFF */
    int i5;   /* ((footer>>40)&0xFFF)<<2 (unused by the closed chain; carried) */
    int i6;   /* ((footer>>52)&0xFFF)<<2 ; thermistor adjust offset */
    LcVendorReal t1;     /* t1[i2] (f64 table 0xa7850) */
    LcVendorReal t2;     /* t2[i2] (f64 table 0xa9850) */
    LcVendorReal slope;  /* slope[i1] (f64 table 0xa1d20) */
} LcVendorConfig;

/* One decoded 6-byte FRAM record -> 10-byte integer record fields. */
typedef struct {
    uint16_t id;    /* u16 @0 */
    uint16_t adc;   /* u16 @2, 14-bit sensor-domain code */
    uint16_t tcode; /* u16 @4, temperature code (bits 26..37 << 2) */
    int16_t adjust; /* i16 @6, adjustment code (bits 38..46 << 2, sign bit 47) */
    uint16_t quality; /* u16 @8, low 9 bits of bits 14..24 when bit 25 set, else 0 */
} LcVendorRecord;

/* 16-byte double tuple {u16 id; u16 quality; pad4; f64 value} (stride 16). */
typedef struct {
    uint16_t id;
    uint16_t quality;
    LcVendorReal value;
} LcVendorTuple;

typedef enum {
    LcVendorChainOk = 0,
    LcVendorChainRejected
} LcVendorChainStatus;

/* Decoded 6-byte FRAM record at `rec6` (little-endian). Mirrors 0x52e3f. */
void lc_vendor_decode_record(const uint8_t rec6[6], LcVendorRecord* out);

/* Extract the factory config object from the decoded 344-byte FRAM.
 * t1/t2/slope are the recovered f64 calibration tables (vendor_tables.c); they
 * are ALWAYS f64 (the vendor rodata is f64) and are narrowed to LcVendorReal
 * only when the float path is selected. Returns false if the config indices are
 * out of range. */
bool lc_vendor_config_from_fram(const uint8_t plain[344],
                                const double* t1_table,
                                const double* t2_table,
                                const double* slope_table,
                                LcVendorConfig* out);

/* 0x88a0: max(0, ADC - i3) * (65.0 / (i4 - i3)). */
LcVendorReal lc_vendor_adc_scale(int adc, const LcVendorConfig* cfg);

/* 0xdb4c: temperature cubic-in-log -> degrees C. Returns false on non-physical
 * input (ratio <= 0). */
bool lc_vendor_temperature_c(int tcode, int adjust, int i6, LcVendorReal* out);

/* 0xe6f4: gain = pow(1.045, 32.5 - T); value *= gain. */
LcVendorReal lc_vendor_temp_gain(LcVendorReal value, LcVendorReal temperature_c);

/* 0xf0e4: windowed mean (sum values / count) over the first `count` tuples. */
LcVendorReal lc_vendor_windowed_mean(const LcVendorTuple* tuples, size_t count);

/* 0xc36c: value = value / ((slope[i1] + 1.0) * t1) + (-(t2 / t1)). */
LcVendorReal lc_vendor_factory_transform(LcVendorReal value, const LcVendorConfig* cfg);

/* Full per-record corrected value: 0x88a0 -> 0xdb4c -> 0xe6f4 -> 0xc36c.
 * Follows the resolved executed order: the ADC accessor overwrites the
 * temperature array, so the live W+0x108 is ADC-domain. */
LcVendorChainStatus lc_vendor_correct_record(const LcVendorRecord* rec,
                                             const LcVendorConfig* cfg,
                                             LcVendorReal* value_out,
                                             LcVendorReal* temperature_out);

/* Build the corrected 16-byte tuple array from a record array (order preserved). */
void lc_vendor_build_tuples(const LcVendorRecord* records, size_t count,
                            const LcVendorConfig* cfg, LcVendorTuple* tuples);

/* Leaf estimator 0x1f7e8: least-squares linear regression over tuples, skipping
 * tuples whose quality@+2 is non-zero, evaluated at `index`; weighted average
 * 0x1f2e8. Uses the centered (mean-subtracted) accumulation form. Returns false
 * if fewer than two usable tuples or degenerate x-spread. */
bool lc_vendor_leaf_regression(const LcVendorTuple* tuples, size_t count,
                               LcVendorReal index, LcVendorReal* out);

/* UI-facing result of the bounded closed chain. `available` is false when no
 * non-empty newest record could be corrected. corrected mg/dL / mmol tenths are
 * the LINEARISED raw estimate; the vendor smoothing/admission layer (0x18e90..
 * 0x1b25a) is deliberately NOT applied, so this is not the vendor's displayed
 * current value. cfg_* carry the recovered factory config for the info page. */
typedef struct {
    bool available;
    uint32_t mg_dl;
    uint32_t mmol_tenths;
    int cfg_i2, cfg_i3, cfg_i4, cfg_i6;
} LcVendorChainResult;

/* Convenience: corrected + rounded mg/dL and mmol/L tenths from a corrected
 * value. Rounds with floor(v+0.5); mmol uses the 18.0 conversion (historical
 * app convention) for display parity. */
void lc_vendor_round(LcVendorReal value, uint32_t* mg_dl, uint32_t* mmol_tenths);

/* High-level: run the closed chain over a decoded 344-byte FRAM at a ring base
 * (records of 6 bytes, `count` slots) and return the corrected value of the
 * newest record (ring slot `newest_slot`), plus every corrected tuple.
 * Returns false if the config/tables cannot be resolved. This is the whole
 * recovery-closed path; the smoothing/admission layer is NOT applied, so the
 * returned value is the linearised raw estimate, not the vendor's displayed
 * current value. */
#define LC_VENDOR_RING_MAX 32u

/* Recovered TREND-ring geometry (pinned DiaBLE Libre.swift / raw_parser.py):
 *   trend ring base 28, count 16, newest index plain[26], slot = (index-1-i) mod 16.
 * The UI's freshness is (age_minutes - i) on THIS ring; the source-backed
 * approximation of the current sample is the trend ring, not the 124 history ring.
 * Also matches the recovered ring-builder call sites (0x1c/16, 0x7c/32). */
#define LC_VENDOR_TREND_BASE 28u
#define LC_VENDOR_TREND_COUNT 16u
#define LC_VENDOR_TREND_MAX 16u

/* One decoded trend record with its reconstructed sensor minute. u16@0 is the raw
 * ADC (NOT a sample time); the sample minute is age_minutes - slot_offset. */
typedef struct {
    LcVendorRecord rec;
    uint16_t     adc;
    int32_t      sensor_minute;  /* age_minutes - slot_offset; -1 if unavailable */
    bool         has_error;      /* bit 25 */
    bool         has_quality;    /* quality bits non-zero */
    LcVendorReal value;          /* corrected linearised estimate (no smoothing) */
} LcVendorTrendSample;

/* Source-backed CURRENT/TREND selector. Selects the newest TREND-ring record at
 * base 28 / count 16 using index byte plain[26], with a BOUNDED selector that
 * REJECTS an out-of-range index (>= count) rather than masking it with a modulo,
 * and FAILS CLOSED when the newest record is empty (all-zero), marked error
 * (bit 25), or has non-zero quality bits. It does NOT silently fall back to an
 * older sample or to the history ring. On rejection no value is written. */
LcVendorChainStatus lc_vendor_current_trend(const uint8_t plain[344],
                                            LcVendorConfig* cfg_out,
                                            LcVendorTrendSample* out,
                                            uint32_t* mg_dl,
                                            uint32_t* mmol_tenths);

bool lc_vendor_chain_ring(const uint8_t plain[344], uint32_t base, uint32_t count,
                          uint32_t newest_slot,
                          const double* t1_table,
                          const double* t2_table,
                          const double* slope_table,
                          LcVendorConfig* cfg_out,
                          LcVendorTuple* tuples_out,
                          uint32_t* count_out,
                          LcVendorReal* newest_value_out);

/* Historical numerical seam only, NOT the app display path. Takes decoded FRAM
 * and returns the newest corrected mg/dL + mmol/L tenths. `cfg_out` (optional)
 * receives the extracted factory config. Honest caveat: no smoothing layer. */
bool lc_vendor_estimate(const uint8_t plain[344], uint32_t* mg_dl,
                        uint32_t* mmol_tenths, LcVendorConfig* cfg_out);

#define LC_VENDOR_SERIES_MAX 48u

typedef struct {
    uint16_t sensor_minute;
    uint16_t mg_dl;
    uint16_t mmol_tenths;
    bool is_trend;
} LcVendorGraphPoint;

typedef struct {
    LcVendorGraphPoint points[LC_VENDOR_SERIES_MAX];
    uint8_t count;
} LcVendorGraphSeries;

/* Extracts all valid, unflagged historical (32 x 15-min = 8h) and trend (16 x 1-min)
 * samples from a decoded 344-byte FRAM, calibrates each with lc_vendor_correct_record,
 * and sorts them ascending by sensor_minute into `out`. */
bool lc_vendor_extract_fram_series(const uint8_t plain[344], LcVendorGraphSeries* out);
