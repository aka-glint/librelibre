/* lc_live_pipeline.h - LibreLibre ACTUAL-route numerical pipeline.
 *
 * Recovered from the live runtime-selected route of EU Libre 2 libcalibrate:
 *   processScan 0x350f4 -> 0x21828 -> 0x48fd4 -> 0x5fb28 -> 0x60f6c -> 0x6d294 -> 0x703f4
 * The historic 0x6208/0x10d8c chain never executed in any replay and is NOT used.
 *
 * Segments and their validation status:
 *   [S] packed records -> working series   VALIDATED EXACT (6528/6528 signal bits)
 *   [B] blend coefficients                 CLOSED EXACT (136/136, 8.0e-10 worst)
 *   [C] mean/blend/display -> lround       CLOSED EXACT (136/136, residual 0)
 *   [E] per-position estimates E[i]        FORMULA EXACT, live operands pending
 *
 * MIT. No fitting, no hardcoded per-capture values, no proprietary binary needed.
 */
#ifndef LC_LIVE_PIPELINE_H
#define LC_LIVE_PIPELINE_H

#include <stdint.h>

/* 10-byte packed record as produced by parser 0x8a598 and copied C+0x194/C+0x234
 * into the working object. */
typedef struct {
    uint16_t adc;      /* +0  sensor-domain code */
    uint16_t tcode;    /* +2  temperature code (no x4) */
    uint16_t flags;    /* +4  0x800 marks a rejected record */
    uint16_t denom_q;  /* +6  precombined temperature denominator (quarter units) */
    uint16_t q2;       /* +8  additional quality */
} LcLivePackedRec;

/* 20-byte working float record (stride 20). */
typedef struct {
    float sig;      /* +0  scaled signal consumed by the estimator */
    float t_raw;    /* +4  unsmoothed temperature, degrees C */
    float field8;   /* +8  zero in every observed capture */
    float t_gain;   /* +c  smoothed temperature used by the gain */
    float t_ret;    /* +10 retained temperature */
} LcLiveWorkRec;

typedef enum { LC_LIVE_TREND = 0, LC_LIVE_HISTORY = 1 } LcLiveSeriesKind;

typedef struct {
    uint16_t e180;  /* factory endpoint, C+0x180 = 6747 observed */
    uint16_t e182;  /* factory endpoint, C+0x182 = 20   observed */
} LcLiveSensorConfig;

/* ---- [S] packed -> working series (VALIDATED EXACT) ----
 * Trend: radius-2 clipped mean of ADC over non-rejected neighbours, then
 *        normalized and gained. History: own ADC normalized and gained.
 * Temperature: Steinhart-Hart on the packed ratio; the gain consumes a
 * clipped-window mean (radius 3 trend / 1 history) excluding zero entries.
 * History zeroes rejected/sentinel slots; trend does not.
 * The gain multiply is DOUBLE (cvtss2sd/mulsd/cvtsd2ss), not float. */
void lc_live_produce_series(const LcLivePackedRec* recs, int n,
                            const LcLiveSensorConfig* cfg,
                            LcLiveSeriesKind kind, LcLiveWorkRec* out);

/* ---- [E] per-position estimate (FORMULA EXACT, operands pending) ----
 *   E[i] = (V[n]-V[n-bl])*(xmm1/bl) + (V[n]-V[n-al])*(xmm2/al)
 * V is the window array, n the window bound. The live V/n at the write site
 * are not yet observable; callers must supply them or bypass this stage. */
void lc_live_edge_for_i(int i, float* xmm1, float* xmm2, int* al, int* bl);
float lc_live_estimate_one(const float* V, int n, int i);

/* ---- [B] blend coefficients (CLOSED EXACT) ---- */
typedef struct { float coef_a, coef_b; } LcLiveBlend;
void lc_live_blend_coeffs(uint16_t selector, LcLiveBlend* out);

/* ---- [C] mean + blend + display -> displayed mg/dL (CLOSED EXACT) ----
 *   M   = f32(mean_num/mean_den + w84c)
 *   X   = f32(coef_a*W10 + coef_b*M)
 *   base= f32(X/37.5); p = pow((double)base, 6.0); p32 = f32(p)
 *   din = f32(f32(f32(f32(f32((1-p32)/(1+p32)+1)*0.49+1.02)*X) + (-4.0))
 *   current_mgdl = lround(din)                                            */
float lc_live_mean_plus_offset(float mean_num, float mean_den, float w84c);
float lc_live_blend_apply(const LcLiveBlend* b, float w10, float m);
int   lc_live_display_transform(float x);
int   lc_live_downstream_displayed(float mean_num, float mean_den, float w84c,
                                   float w10, float coef_a, float coef_b);

/* Weighted-mean accumulator: admitted positions contribute weight 91.0.
 * Returns the rounded displayed current produced from the supplied estimates. */
int lc_live_current_from_estimates(const float* estimates, const uint8_t* admitted,
                                   int count, float w84c, float w10,
                                   uint16_t blend_selector);

#endif /* LC_LIVE_PIPELINE_H */
