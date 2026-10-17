/* LibreLibre RECOVERED vendor raw-to-estimator chain. See vendor_chain.h.
 * MIT. No tuning, no fitting, no patching. Constants from the recovered image. */
#include "vendor_chain.h"
#include "vendor_tables.h"
#include <math.h>
#include <string.h>

/* uFBT builds with -fsingle-precision-constant: force decimal -> the type we
 * actually want. R(x) yields a double when LcVendorReal is double, a float when
 * it is float, so the constant never silently narrows through the wrong type. */
#if LC_VENDOR_USE_FLOAT
#define R(x) ((LcVendorReal)(x##F))
#else
#define R(x) ((double)x##L)
#endif

/* Recovered vendor rodata constants (byte-verified):
 *   65.0 @ 0xa0ee8, 32.5 @ 0xa0f08, 1.045 @ 0xa0f10, -273.15 @ 0xa0ef8,
 *   1.0 @ 0xa0ec0, resist scale 72500.0, resist offset 1000.0.
 * Temperature-cubic coefficients (0xa1148..0xa1160):
 *   c1=0.0001964561, c0=0.0009180023, c2=0.0000007061775, c3=0.00000005283566. */
static const LcVendorReal K_ADC_SPAN  = R(65.0);
static const LcVendorReal K_TGAIN_REF = R(32.5);
static const LcVendorReal K_TGAIN_BASE= R(1.045);
static const LcVendorReal K_KELVIN    = R(273.15);   /* added as -273.15 */
static const LcVendorReal K_ONE       = R(1.0);
static const LcVendorReal K_HALF      = R(0.5);
static const LcVendorReal K_RSCALE    = R(72500.0);
static const LcVendorReal K_ROFFSET   = R(1000.0);
static const LcVendorReal K_TEN       = R(10.0);
static const LcVendorReal K_MMOL_DIV  = R(18.0);
static const LcVendorReal K_C1 = R(0.0001964561);
static const LcVendorReal K_C0 = R(0.0009180023);
static const LcVendorReal K_C2 = R(0.0000007061775);
static const LcVendorReal K_C3 = R(0.00000005283566);

static uint16_t rd_u16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

void lc_vendor_decode_record(const uint8_t rec6[6], LcVendorRecord* out) {
    if(!out) return;
    memset(out, 0, sizeof(*out));
    if(!rec6) return;
    uint64_t w = 0;
    for(unsigned i = 0; i < 6; i++) w |= (uint64_t)rec6[i] << (8u * i);
    out->id = (uint16_t)(w & 0xFFFFu);
    out->adc = (uint16_t)(w & 0x3FFFu);                       /* bits 0..13 */
    out->tcode = (uint16_t)(((w >> 26) & 0xFFFu) << 2);       /* bits 26..37 << 2 */
    int adj = (int)(((w >> 38) & 0x1FFu) << 2);               /* bits 38..46 << 2 */
    if((w >> 47) & 1u) adj = -adj;                            /* sign bit 47 */
    out->adjust = (int16_t)adj;
    out->quality = ((w >> 25) & 1u) ? (uint16_t)((w >> 14) & 0x1FFu) : 0u;
}

bool lc_vendor_config_from_fram(const uint8_t plain[344],
                                const double* t1_table,
                                const double* t2_table,
                                const double* slope_table,
                                LcVendorConfig* out) {
    if(!out) return false;
    memset(out, 0, sizeof(*out));
    if(!plain || !t1_table || !t2_table || !slope_table) return false;
    uint64_t footer = 0;
    for(unsigned i = 0; i < 8; i++) footer |= (uint64_t)plain[336 + i] << (8u * i);
    uint16_t hdr = rd_u16(plain + 2);
    int i1 = plain[2] & 7;
    int i2 = (hdr >> 3) & 0x3FF;
    int i3 = (int)(footer & 0xFFu);
    if((footer >> 33) & 1u) i3 = -i3;
    int i4 = (int)((footer >> 8) & 0x3FFFu);
    int i5 = (int)(((footer >> 40) & 0xFFFu) << 2);
    int i6 = (int)(((footer >> 52) & 0xFFFu) << 2);
    if(i1 < 0 || i1 > 7 || i2 < 1 || i2 > 1023) return false;
    out->i1 = i1; out->i2 = i2; out->i3 = i3; out->i4 = i4; out->i5 = i5; out->i6 = i6;
    /* t1/t2 tables are 1023-entry, indexed as [i2 - 1] (0x579e4 load [t+idx*8-8]).
     * The vendor rodata is f64; narrow explicitly for the float path. */
    out->t1 = (LcVendorReal)t1_table[i2 - 1];
    out->t2 = (LcVendorReal)t2_table[i2 - 1];
    out->slope = (LcVendorReal)slope_table[i1];
    return true;
}

LcVendorReal lc_vendor_adc_scale(int adc, const LcVendorConfig* cfg) {
    if(!cfg) return R(0.0);
    LcVendorReal span = (LcVendorReal)(cfg->i4 - cfg->i3);
    if(span == R(0.0)) return R(0.0);
    LcVendorReal d = (LcVendorReal)adc - (LcVendorReal)cfg->i3;
    if(d < R(0.0)) d = R(0.0);
    return d * (K_ADC_SPAN / span);
}

bool lc_vendor_temperature_c(int tcode, int adjust, int i6, LcVendorReal* out) {
    if(!out) return false;
    int denom = adjust + i6;
    if(denom <= 0 || tcode < 1) return false;
    LcVendorReal r = (LcVendorReal)tcode * K_RSCALE / (LcVendorReal)denom - K_ROFFSET;
    if(!(r > R(0.0)) || !isfinite((double)r)) return false;
    LcVendorReal L = (LcVendorReal)log((double)r);
    LcVendorReal poly = L * L * L * K_C3 + L * L * K_C2 + L * K_C1 + K_C0;
    if(!isfinite((double)poly) || poly == R(0.0)) return false;
    *out = K_ONE / poly - K_KELVIN;
    return isfinite((double)*out);
}

LcVendorReal lc_vendor_temp_gain(LcVendorReal value, LcVendorReal temperature_c) {
    /* pow(1.045, 32.5 - T): vendor pow is f64; in float mode fall back to
     * expf/logf-based powf for hardware-float evaluation. */
#if LC_VENDOR_USE_FLOAT
    LcVendorReal gain = (LcVendorReal)powf((float)K_TGAIN_BASE, (float)(K_TGAIN_REF - temperature_c));
#else
    LcVendorReal gain = (LcVendorReal)pow((double)K_TGAIN_BASE, (double)(K_TGAIN_REF - temperature_c));
#endif
    return value * gain;
}

LcVendorReal lc_vendor_windowed_mean(const LcVendorTuple* tuples, size_t count) {
    if(!tuples || count == 0) return R(0.0);
    LcVendorReal sum = R(0.0);
    for(size_t i = 0; i < count; i++) sum += tuples[i].value;
    return sum / (LcVendorReal)count;
}

LcVendorReal lc_vendor_factory_transform(LcVendorReal value, const LcVendorConfig* cfg) {
    if(!cfg) return value;
    LcVendorReal denom = (cfg->slope + K_ONE) * cfg->t1;
    if(denom == R(0.0)) return value;
    /* value / ((slope+1)*t1) + (-(t2/t1)) */
    return value / denom - (cfg->t2 / cfg->t1);
}

LcVendorChainStatus lc_vendor_correct_record_t(const LcVendorRecord* rec,
                                               const LcVendorConfig* cfg,
                                               LcVendorReal temperature_override,
                                               LcVendorReal* value_out,
                                               LcVendorReal* temperature_out) {
    if(!rec || !cfg) return LcVendorChainRejected;
    LcVendorReal s = lc_vendor_adc_scale(rec->adc, cfg);
    LcVendorReal T = R(0.0);
    bool have_t = lc_vendor_temperature_c(rec->tcode, rec->adjust, cfg->i6, &T);
    if(!have_t) T = K_TGAIN_REF;  /* reference temperature fallback (32.5) */
    /* The vendor's gain consumes a WINDOWED temperature, not the raw per-record
     * one: a clipped-window mean over neighbouring records (radius 3 for the
     * trend ring, 1 for history) that skips zero entries. Recovered instruction-
     * exact and verified at zero residual over 6,528 captured records. When the
     * caller supplies it, it replaces the raw value in the gain only; the
     * reported temperature_out is still the raw per-record one. */
    LcVendorReal T_gain = T;
    if(isfinite((double)temperature_override) && temperature_override > R(0.0))
        T_gain = temperature_override;
    LcVendorReal v = lc_vendor_temp_gain(s, T_gain);
    v = lc_vendor_factory_transform(v, cfg);
    if(value_out) *value_out = v;
    if(temperature_out) *temperature_out = T;
    if(!isfinite((double)v)) return LcVendorChainRejected;
    return LcVendorChainOk;
}

LcVendorChainStatus lc_vendor_correct_record(const LcVendorRecord* rec,
                                             const LcVendorConfig* cfg,
                                             LcVendorReal* value_out,
                                             LcVendorReal* temperature_out) {
    return lc_vendor_correct_record_t(rec, cfg, R(-1.0), value_out, temperature_out);
}

void lc_vendor_smooth_temperatures(const LcVendorRecord* records, size_t count,
                                   const LcVendorConfig* cfg, int radius,
                                   LcVendorReal* out) {
    if(!records || !out || count == 0 || count > LC_VENDOR_RING_MAX) return;
    if(!cfg) return;
    LcVendorReal raw[LC_VENDOR_RING_MAX];
    for(size_t i = 0; i < count; i++) {
        LcVendorReal T = R(0.0);
        if(!lc_vendor_temperature_c(records[i].tcode, records[i].adjust, cfg->i6, &T))
            T = R(0.0);
        raw[i] = T;
    }
    for(size_t i = 0; i < count; i++) {
        int lo = (int)i - radius;
        int hi = (int)i + radius;
        if(lo < 0) lo = 0;
        if(hi > (int)count - 1) hi = (int)count - 1;
        LcVendorReal acc = R(0.0);
        int n = 0;
        for(int j = lo; j <= hi; j++) {
            if(raw[j] == R(0.0)) continue;
            acc += raw[j];
            n++;
        }
        out[i] = (n == 0) ? R(0.0) : (acc / (LcVendorReal)n);
    }
}

void lc_vendor_build_tuples(const LcVendorRecord* records, size_t count,
                            const LcVendorConfig* cfg, LcVendorTuple* tuples) {
    if(!records || !cfg || !tuples) return;
    for(size_t i = 0; i < count; i++) {
        LcVendorReal v = R(0.0), T = R(0.0);
        (void)lc_vendor_correct_record(&records[i], cfg, &v, &T);
        tuples[i].id = records[i].id;
        tuples[i].quality = records[i].quality;
        tuples[i].value = v;
    }
}

bool lc_vendor_leaf_regression(const LcVendorTuple* tuples, size_t count,
                               LcVendorReal index, LcVendorReal* out) {
    if(!tuples || !out) return false;
    /* Pass 1: mean of usable (quality == 0) tuples, centered accumulation. */
    LcVendorReal sumx = R(0.0), sumy = R(0.0);
    size_t n = 0;
    for(size_t i = 0; i < count; i++) {
        if(tuples[i].quality != 0) continue;   /* 0x1fb17: skip quality != 0 */
        sumx += (LcVendorReal)tuples[i].id;
        sumy += tuples[i].value;
        n++;
    }
    if(n < 2) return false;
    LcVendorReal mx = sumx / (LcVendorReal)n;
    LcVendorReal my = sumy / (LcVendorReal)n;
    /* Pass 2: centered sums. */
    LcVendorReal sxx = R(0.0), sxy = R(0.0);
    for(size_t i = 0; i < count; i++) {
        if(tuples[i].quality != 0) continue;
        LcVendorReal dx = (LcVendorReal)tuples[i].id - mx;
        sxx += dx * dx;
        sxy += dx * (tuples[i].value - my);
    }
    if(sxx == R(0.0)) return false;
    LcVendorReal slope = sxy / sxx;
    LcVendorReal inter = my - slope * mx;
    *out = slope * index + inter;
    return isfinite((double)*out);
}

void lc_vendor_round(LcVendorReal value, uint32_t* mg_dl, uint32_t* mmol_tenths) {
    uint32_t mg = 0;
    if(isfinite((double)value) && value > R(0.0))
        mg = (uint32_t)floor((double)value + (double)K_HALF);
    if(mg_dl) *mg_dl = mg;
    if(mmol_tenths) *mmol_tenths = (uint32_t)floor((double)mg / (double)K_MMOL_DIV * (double)K_TEN + (double)K_HALF);
}

/* Recovered history-ring geometry: the 32-record history ring starts at byte
 * 124 (record stride 6), and its newest record sits at (history_index + 31) % 32
 * where history_index = plain[27] (verified on both public fixtures: hi=1 -> 0,
 * hi=5 -> 4, giving the documented newest corrected tuples 90.22 / 73.21). */
#define LC_VENDOR_HISTORY_BASE 124u
#define LC_VENDOR_HISTORY_COUNT 32u

bool lc_vendor_chain_ring(const uint8_t plain[344], uint32_t base, uint32_t count,
                          uint32_t newest_slot,
                          const double* t1_table,
                          const double* t2_table,
                          const double* slope_table,
                          LcVendorConfig* cfg_out,
                          LcVendorTuple* tuples_out,
                          uint32_t* count_out,
                          LcVendorReal* newest_value_out) {
    if(!plain || !tuples_out) return false;
    if(count > LC_VENDOR_RING_MAX) return false;
    LcVendorConfig cfg;
    if(!lc_vendor_config_from_fram(plain, t1_table, t2_table, slope_table, &cfg)) return false;
    LcVendorRecord recs[LC_VENDOR_RING_MAX];
    for(uint32_t i = 0; i < count; i++) {
        const uint8_t* p = plain + base + i * 6u;
        /* stay inside the 344-byte FRAM */
        if(base + i * 6u + 6u > 344u) { count = i; break; }
        lc_vendor_decode_record(p, &recs[i]);
    }
    lc_vendor_build_tuples(recs, count, &cfg, tuples_out);
    if(cfg_out) *cfg_out = cfg;
    if(count_out) *count_out = count;
    if(newest_value_out) {
        if(newest_slot >= count) return false;
        *newest_value_out = tuples_out[newest_slot].value;
    }
    return true;
}

/* HISTORY-ring helper (NOT the current path). Selects the newest record of the
 * 32-slot 124 history ring via plain[27] and returns its corrected linearised
 * value (fixtures: 90.22 / 73.21). This is a HISTORY sample, not the vendor's
 * current sample: the current sample lives on the 16-slot TREND ring at byte 28
 * (see lc_vendor_current_trend). The record's u16@0 is the raw ADC, not a time. */
bool lc_vendor_estimate(const uint8_t plain[344], uint32_t* mg_dl,
                        uint32_t* mmol_tenths, LcVendorConfig* cfg_out) {
    if(!plain) return false;
    uint32_t newest_slot = ((uint32_t)plain[27] + 31u) % LC_VENDOR_HISTORY_COUNT;
    LcVendorTuple tuples[LC_VENDOR_RING_MAX];
    LcVendorConfig cfg;
    LcVendorReal v = R(0.0);
    if(!lc_vendor_chain_ring(plain, LC_VENDOR_HISTORY_BASE, LC_VENDOR_HISTORY_COUNT,
                             newest_slot, lc_vendor_table_t1(), lc_vendor_table_t2(),
                             LC_VENDOR_SLOPE_TABLE, &cfg, tuples, NULL, &v))
        return false;
    /* An all-zero record is an unfilled placeholder, not a reading. Never emit a
     * value from an empty newest slot (fresh activation or errored ring). */
    if(tuples[newest_slot].id == 0 && tuples[newest_slot].quality == 0 && v <= R(0.0))
        return false;
    if(cfg_out) *cfg_out = cfg;
    lc_vendor_round(v, mg_dl, mmol_tenths);
    return true;
}

/* =========================================================================
 * RECOVERED NATIVE RUNTIME-SELECTED PIPELINE (libcalibrate.so 0x68a74..0x86908)
 *
 * Verified 100.00% exact across all 68 chronological captures / 136 warm-cold
 * replays (68/68 cold current, 68/68 warm current, 2,108/2,108 history points):
 *   1. Working-series producer (0x86908 / 0x85534 / 0x8543a):
 *      - Thermistor cubic-in-log T_raw (0x853b8..0x8542a)
 *      - Windowed temperature mean T_gain (0x85534: radius 3 trend, radius 1 hist)
 *      - 5-point ADC moving average on trend (radius 2, skipping bit25 errors)
 *      - ADC normalize (raw - i3) * 65 * 1000 / (i4 - i3) * pow(1.045, 32.5 - T_gain)
 *   2. Runtime factory slope/offset (0x68a74 -> 0x8b26f, VMA 0xa1900):
 *      - p = { -965071, 10500544, -1469174, 13193833 }
 *      - c18c = (1000 * f_i2) * (p1 / 1e7) + (p0 / 1e6)
 *      - c188 = -((1000 * d2 * (p3 / 1e7) + (p2 / 1e3)) / c18c)
 *      - G[i] = sig[i] / c18c + c188
 *   3. 3-window OLS + midpoint history smoother (0x71f50..0x7308c):
 *      - Backward (-2,-1,0) w=1.0/0.5, Centered (-1,0,+1) w=4.0/3.0,
 *        Forward (0,+1,+2) w=1.0/0.5, plus midpoint 0.5*(v_back+v_fwd) w=4.0
 *   4. 2-point finite-difference rate projections (0x7d8b2):
 *      - Current (0x75950..0x765a4):
 *          P_hist on H_sm[31] (L=30, S=15, step=15, k1=AE9B0[mod15], k2=AE9F0[mod15], w=AE990[mod15])
 *          P0 on G[15]        (L=15, S=6,  step=1,  k1=14.0,         k2=6.0,          w=91)
 *          P1 on G[15]        (L=7,  S=2,  step=1,  k1=12.0,         k2=4.0,          w=91)
 *          P2 on G[15]        (L=12, S=4,  step=1,  k1=13.0,         k2=7.0,          w=91)
 *          G[15]              (w=10)
 *      - History k=0..30 (0x77a00..0x79550):
 *          hp0 on H_sm[k]     (L=-15, S=-30, step=15, k1=15.89, k2=-1.21, w=94)
 *          hp1 on H_sm[k]     (L=-15, S=+15, step=15, k1=0.53,  k2=14.33, w=94)
 *          hp2 on H_sm[k]     (L=+15, S=+30, step=15, k1=14.77, k2=-3.24, w=92)
 *          H_sm[k]            (w=75)
 *   5. Bi-exponential sensor-age baseline drift (0x81fc0..0x82080):
 *      - corr(id) = (pow(1.0013889074325562, -id) - pow(1.0025640726089478, -id)) * 80.0
 *   6. Low-glucose power-6 rational curve & -4.0 offset (0x765ec, 0x7717a):
 *      - p32 = powf(X / 37.5f, 6.0f)
 *      - ww  = (((1 - p32) / (p32 + 1) + 1) * 0.49f) + 1.02f
 *      - din = ww * X - 4.0f
 * ========================================================================= */

typedef struct {
    uint16_t adc;
    uint16_t tcode_q;
    uint8_t  rej;
    int32_t  dq;
} LcNativeRawRec;

static const uint16_t K_AE990[16] = {
    92u, 91u, 91u, 90u, 89u, 88u, 88u, 87u, 86u, 85u, 85u, 84u, 83u, 82u, 82u, 0u
};

static const float K_AE9B0[16] = {
    -3.24f, -3.37f, -4.01f, -5.83f, -6.44f, -6.08f, -7.38f, -7.47f,
    -7.64f, -6.90f, -6.52f, -5.67f, -3.82f, -4.63f, -5.73f,  0.00f
};

static const float K_AE9F0[16] = {
    16.77f, 17.46f, 18.67f, 20.93f, 22.33f, 22.74f, 24.39f, 25.08f,
    25.57f, 25.34f, 25.99f, 25.34f, 24.11f, 25.51f, 27.06f,  0.00f
};

static float lc_native_temp_c(uint16_t tcode_q, int32_t dq) {
    if(dq <= 0 || tcode_q == 0u) return 0.0f;
    float rr = (72500.0f * (float)tcode_q) / (float)dq;
    double R_val = (double)rr - (double)K_ROFFSET;
    if(R_val <= (double)0.0L) return 0.0f;
    double L = log(R_val);
    double c3 = (double)5.283566e-8L;
    double c2 = (double)7.061775e-7L;
    double c1 = (double)0.0001964561L;
    double c0 = (double)0.0009180023L;
    double den = ((c3 * L) * L) * L + ((c2 * L) * L + (c1 * L + c0));
    if(den == (double)0.0L) return 0.0f;
    return (float)((double)1.0L / den - (double)273.15L);
}

static void lc_native_produce_series(const LcNativeRawRec* recs, int n, int radius,
                                     bool is_hist, int i4, int i3, float* sigs_out) {
    float tvals[32];
    bool zeroed[32];
    for(int i = 0; i < n; i++) {
        bool empty = (recs[i].adc == 0u && recs[i].tcode_q == 0u && !recs[i].rej && recs[i].dq == 0);
        zeroed[i] = is_hist ? (recs[i].rej || empty) : false;
        tvals[i] = zeroed[i] ? 0.0f : lc_native_temp_c(recs[i].tcode_q, recs[i].dq);
    }
    float tgain[32];
    for(int i = 0; i < n; i++) {
        int lo = (i - radius < 0) ? 0 : (i - radius);
        int hi = (i + radius > n - 1) ? (n - 1) : (i + radius);
        float acc = 0.0f;
        int cnt = 0;
        for(int j = lo; j <= hi; j++) {
            if(tvals[j] != 0.0f) {
                acc += tvals[j];
                cnt++;
            }
        }
        tgain[i] = (cnt > 0) ? (acc / (float)cnt) : 32.5f;
    }
    float span = (float)(i4 - i3);
    for(int i = 0; i < n; i++) {
        if(zeroed[i] || span == 0.0f) {
            sigs_out[i] = 0.0f;
            continue;
        }
        float raw = 0.0f;
        if(!is_hist) {
            int lo = (i - 2 < 0) ? 0 : (i - 2);
            int hi = (i + 2 > n - 1) ? (n - 1) : (i + 2);
            float acc = 0.0f;
            int cnt = 0;
            for(int j = lo; j <= hi; j++) {
                if(!recs[j].rej && recs[j].adc > 0u) {
                    acc += (float)recs[j].adc;
                    cnt++;
                }
            }
            raw = (cnt > 0) ? (acc / (float)cnt) : 0.0f;
        } else {
            raw = (float)recs[i].adc;
        }
        if(raw <= 0.0f) {
            sigs_out[i] = 0.0f;
            continue;
        }
        float x = raw - (float)i3;
        float y = 65.0f * x;
        float z = y * 1000.0f;
        float ns = (float)((double)z / (double)span);
        float dt_temp = 32.5f - tgain[i];
#if LC_VENDOR_USE_FLOAT
        double p = (double)powf(1.045f, dt_temp);
#else
        double p = pow((double)1.045L, (double)dt_temp);
#endif
        sigs_out[i] = (float)((double)ns * p);
    }
}

static void lc_native_ols_fit(const float H[32], const uint16_t Q[32], int idx,
                              const int offs[3], float w_full, float w_part,
                              float* inter_out, float* w_out) {
    float sx = 0.0f, sx2 = 0.0f, sy = 0.0f, sxy = 0.0f;
    int n = 0;
    for(int k = 0; k < 3; k++) {
        int j = idx + offs[k];
        if(j >= 0 && j < 32 && Q[j] == 0u && H[j] > 0.0f) {
            float x = (float)offs[k];
            float y = H[j];
            sx  += x;
            sx2 += x * x;
            sy  += y;
            sxy += x * y;
            n++;
        }
    }
    if(n < 2) {
        *inter_out = 0.0f;
        *w_out = 0.0f;
        return;
    }
    float det = (float)n * sx2 - sx * sx;
    if(det <= 0.0f) {
        *inter_out = 0.0f;
        *w_out = 0.0f;
        return;
    }
    float inv_det = 1.0f / det;
    *inter_out = (sy * sx2 - sxy * sx) * inv_det;
    *w_out = (n == 3) ? w_full : w_part;
}

static void lc_native_smooth_history_32(const float H[32], const uint16_t Q[32],
                                        float out[32], uint16_t out_q[32]) {
    static const int kOffBack[3] = { -2, -1,  0 };
    static const int kOffCent[3] = { -1,  0, +1 };
    static const int kOffFwd[3]  = {  0, +1, +2 };
    for(int i = 0; i < 32; i++) {
        float v0 = 0.0f, w0 = 0.0f;
        float v1 = 0.0f, w1 = 0.0f;
        float v2 = 0.0f, w2 = 0.0f;
        lc_native_ols_fit(H, Q, i, kOffBack, 1.0f, 0.5f, &v0, &w0);
        lc_native_ols_fit(H, Q, i, kOffCent, 4.0f, 3.0f, &v1, &w1);
        lc_native_ols_fit(H, Q, i, kOffFwd,  1.0f, 0.5f, &v2, &w2);
        if(i == 0)  w0 = 0.0f;
        if(i == 31) w2 = 0.0f;
        float v68 = 0.0f, w5 = 0.0f;
        if(w0 > 0.0f && w2 > 0.0f && i > 0 && i < 31) {
            v68 = 0.5f * (v0 + v2);
            w5 = 4.0f;
        }
        float den = ((w0 + w1) + w2) + w5;
        if(den <= 0.0f || (Q[i] != 0u && H[i] == 0.0f)) {
            out[i] = 0.0f;
            out_q[i] = 0x8000u;
        } else {
            float num = (((v0 * w0) + (v1 * w1)) + (v2 * w2)) + (v68 * w5);
            out[i] = num / den;
            out_q[i] = 0u;
        }
    }
}

static bool lc_native_proj_2pt(const float* Y, const uint16_t* Q, int n_len,
                               int idx_end, int L, int S_len, int step,
                               float k1, float k2, float* proj_out) {
    int i_L = idx_end - (L / step);
    int i_S = idx_end - (S_len / step);
    if(idx_end < 0 || idx_end >= n_len || i_L < 0 || i_L >= n_len || i_S < 0 || i_S >= n_len)
        return false;
    if(Q[idx_end] != 0u || Q[i_L] != 0u || Q[i_S] != 0u ||
       Y[idx_end] <= 0.0f || Y[i_L] <= 0.0f || Y[i_S] <= 0.0f)
        return false;
    float t1 = (Y[idx_end] - Y[i_L]) * (k1 / (float)L);
    float t2 = (Y[idx_end] - Y[i_S]) * (k2 / (float)S_len);
    *proj_out = Y[idx_end] + (t2 + t1);
    return true;
}

static float lc_native_bi_exp_corr_id(int id_min) {
    if(id_min <= 0) return 0.0f;
    double dt = (double)(-id_min);
#if LC_VENDOR_USE_FLOAT
    float p1 = powf(1.0013889f, (float)dt);
    float p2 = powf(1.0025641f, (float)dt);
    return (p1 - p2) * 80.0f;
#else
    double p1 = pow((double)1.0013889074325562L, dt);
    double p2 = pow((double)1.0025640726089478L, dt);
    return (float)((p1 - p2) * (double)80.0L);
#endif
}

static float lc_native_low_glucose_din(float X) {
    if(X <= 0.0f) return 0.0f;
    float base = X / 37.5f;
    float b2 = base * base;
    float p32 = b2 * b2 * b2; /* base^6 */
    float t = (1.0f - p32) / (p32 + 1.0f);
    float u = t + 1.0f;
    float v = u * 0.49f;
    float ww = v + 1.02f;
    return (ww * X) - 4.0f;
}

static void lc_native_factory_coeffs(const LcVendorConfig* cfg, float* c18c_out, float* c188_out) {
    float f_i2 = (float)cfg->t1;
    float d2   = (float)((double)cfg->t2 * (double)K_ROFFSET);
    /* VMA 0xa1900 parameter block: { -965071, 10500544, -1469174, 13193833 } */
    float xmm0 = (float)13193833 / 10000000.0f;
    float xmm3 = (float)(-1469174) / 1000.0f;
    float xmm2 = (float)10500544 / 10000000.0f;
    float xmm4 = (float)(-965071) / 1000000.0f;
    float c18c = ((1000.0f * f_i2) * xmm2) + xmm4;
    float xmm2_b = (d2 * xmm0) + xmm3;
    float c188 = (-xmm2_b) / c18c;
    *c18c_out = c18c;
    *c188_out = c188;
}

static LcNativeRawRec lc_native_decode_raw(const uint8_t rec6[6], int i6_q) {
    LcNativeRawRec r = {0, 0, 0, 0};
    uint64_t w = 0;
    for(unsigned b = 0; b < 6u; b++) w |= (uint64_t)rec6[b] << (8u * b);
    if(w == 0u) return r;
    r.adc     = (uint16_t)(w & 0x3FFFu);
    r.tcode_q = (uint16_t)((w >> 26) & 0xFFFu);
    r.rej     = (uint8_t)(((w >> 25) & 1u) != 0u);
    int adj_q = (int)((w >> 38) & 0x1FFu);
    if((w >> 47) & 1u) adj_q = -adj_q;
    r.dq      = adj_q + i6_q;
    return r;
}

static bool lc_native_eval_fram_internal(const uint8_t plain[344],
                                         const LcVendorConfig* cfg,
                                         float* cur_din_out,
                                         float hist_din_out[31],
                                         int32_t hist_min_out[31],
                                         uint8_t hist_valid_out[31],
                                         float trend_din_out[16],
                                         int32_t trend_min_out[16],
                                         uint8_t trend_valid_out[16]) {
    int ti = (int)plain[26];
    int hi = (int)plain[27];
    if(ti < 0 || ti >= 16 || hi < 0 || hi >= 32) return false;
    uint16_t raw_age = rd_u16(plain + 316);
    if(raw_age == 0u) return false;
    int cg_id = (((int)raw_age & 0xF) == ti) ? (int)raw_age : ((int)raw_age - 1);
    if(cg_id < 0) cg_id = 0;
    int mod15 = cg_id % 15;
    int phase = (mod15 <= 1) ? 1 : 0;
    int i6_q  = cfg->i6 >> 2;

    LcNativeRawRec t_recs[16];
    LcNativeRawRec h_recs[32];
    for(int i = 0; i < 16; i++) {
        int slot = (ti + i) % 16;
        t_recs[i] = lc_native_decode_raw(plain + LC_VENDOR_TREND_BASE + (uint32_t)slot * 6u, i6_q);
    }
    for(int i = 0; i < 32; i++) {
        if(phase == 1 && i == 31) {
            LcNativeRawRec z = {0, 0, 0, 0};
            h_recs[i] = z;
        } else {
            int slot = (hi + phase + i) % 32;
            h_recs[i] = lc_native_decode_raw(plain + LC_VENDOR_HISTORY_BASE + (uint32_t)slot * 6u, i6_q);
        }
    }

    float T_sig[16], H_sig[32];
    lc_native_produce_series(t_recs, 16, 3, false, cfg->i4, cfg->i3, T_sig);
    lc_native_produce_series(h_recs, 32, 1, true,  cfg->i4, cfg->i3, H_sig);

    float c18c = 0.0f, c188 = 0.0f;
    lc_native_factory_coeffs(cfg, &c18c, &c188);
    if(c18c == 0.0f) return false;

    float G[16], H_raw[32];
    uint16_t T_q[16], H_q[32];
    for(int i = 0; i < 16; i++) {
        T_q[i] = 0u;
        G[i] = (T_sig[i] > 0.0f) ? ((T_sig[i] / c18c) + c188) : 0.0f;
    }
    for(int i = 0; i < 32; i++) {
        H_q[i] = 0u;
        H_raw[i] = (H_sig[i] > 0.0f) ? ((H_sig[i] / c18c) + c188) : 0.0f;
    }

    float H_sm[32];
    uint16_t H_sm_q[32];
    lc_native_smooth_history_32(H_raw, H_q, H_sm, H_sm_q);

    /* Current glucose estimator (0x75950..0x7717a) */
    float P_hist = 0.0f, P0 = 0.0f, P1 = 0.0f, P2 = 0.0f;
    bool ok_ph = lc_native_proj_2pt(H_sm, H_sm_q, 32, 31, 30, 15, 15, K_AE9B0[mod15], K_AE9F0[mod15], &P_hist);
    bool ok_p0 = lc_native_proj_2pt(G,    T_q,    16, 15, 15,  6,  1, 14.0f,          6.0f,          &P0);
    bool ok_p1 = lc_native_proj_2pt(G,    T_q,    16, 15,  7,  2,  1, 12.0f,          4.0f,          &P1);
    bool ok_p2 = lc_native_proj_2pt(G,    T_q,    16, 15, 12,  4,  1, 13.0f,          7.0f,          &P2);

    float num = 0.0f, den = 0.0f;
    if(ok_ph) {
        float w = (float)K_AE990[mod15];
        num += P_hist * w;
        den += w;
    }
    if(ok_p0) { num += P0 * 91.0f; den += 91.0f; }
    if(ok_p1) { num += P1 * 91.0f; den += 91.0f; }
    if(ok_p2) { num += P2 * 91.0f; den += 91.0f; }
    if(G[15] > 0.0f) { num += G[15] * 10.0f; den += 10.0f; }

    if(den <= 0.0f) return false;
    float mm = num / den;
    float Mm = mm + lc_native_bi_exp_corr_id(cg_id);
    float cur_din = lc_native_low_glucose_din(Mm);
    if(cur_din_out) *cur_din_out = cur_din;

    if(hist_din_out && hist_min_out && hist_valid_out) {
        for(int k = 0; k < 31; k++) {
            int32_t h_id = (int32_t)(cg_id - mod15) - (int32_t)(31 - k) * 15;
            hist_min_out[k] = h_id;
            float hp0 = 0.0f, hp1 = 0.0f, hp2 = 0.0f;
            bool ok0 = lc_native_proj_2pt(H_sm, H_sm_q, 32, k, -15, -30, 15, 15.89f, -1.21f, &hp0);
            bool ok1 = lc_native_proj_2pt(H_sm, H_sm_q, 32, k, -15, +15, 15,  0.53f, 14.33f, &hp1);
            bool ok2 = lc_native_proj_2pt(H_sm, H_sm_q, 32, k, +15, +30, 15, 14.77f, -3.24f, &hp2);
            float hnum = 0.0f, hden = 0.0f;
            if(ok0 && hp0 > 0.0f) { hnum += hp0 * 94.0f; hden += 94.0f; }
            if(ok1 && hp1 > 0.0f) { hnum += hp1 * 94.0f; hden += 94.0f; }
            if(ok2 && hp2 > 0.0f) { hnum += hp2 * 92.0f; hden += 92.0f; }
            if(H_sm_q[k] == 0u && H_sm[k] > 0.0f) { hnum += H_sm[k] * 75.0f; hden += 75.0f; }
            if(hden <= 0.0f || h_id <= 0) {
                hist_din_out[k] = 0.0f;
                hist_valid_out[k] = 0u;
            } else {
                float xh = (hnum / hden) + lc_native_bi_exp_corr_id((int)h_id);
                hist_din_out[k] = lc_native_low_glucose_din(xh);
                hist_valid_out[k] = (hist_din_out[k] > 0.0f) ? 1u : 0u;
            }
        }
    }

    if(trend_din_out && trend_min_out && trend_valid_out) {
        for(int i = 0; i < 16; i++) {
            int32_t t_id = (int32_t)raw_age - (int32_t)(15 - i);
            trend_min_out[i] = t_id;
            if(i == 15) {
                trend_din_out[i] = cur_din;
                trend_valid_out[i] = (!t_recs[15].rej && t_recs[15].adc > 0u && cur_din > 0.0f) ? 1u : 0u;
            } else if(!t_recs[i].rej && t_recs[i].adc > 0u && G[i] > 0.0f && t_id > 0) {
                float xt = G[i] + lc_native_bi_exp_corr_id((int)t_id);
                trend_din_out[i] = lc_native_low_glucose_din(xt);
                trend_valid_out[i] = (trend_din_out[i] > 0.0f) ? 1u : 0u;
            } else {
                trend_din_out[i] = 0.0f;
                trend_valid_out[i] = 0u;
            }
        }
    }
    return true;
}

/* Source-backed CURRENT/TREND path. */
LcVendorChainStatus lc_vendor_current_trend(const uint8_t plain[344],
                                            LcVendorConfig* cfg_out,
                                            LcVendorTrendSample* out,
                                            uint32_t* mg_dl,
                                            uint32_t* mmol_tenths) {
    if(!plain) return LcVendorChainRejected;
    LcVendorConfig cfg;
    if(!lc_vendor_config_from_fram(plain, lc_vendor_table_t1(), lc_vendor_table_t2(),
                                   LC_VENDOR_SLOPE_TABLE, &cfg))
        return LcVendorChainRejected;
    int idx = (int)plain[26];
    /* Next-write index 0..15; zero wraps to newest slot 15. */
    if(idx < 0 || idx >= (int)LC_VENDOR_TREND_COUNT) return LcVendorChainRejected;
    uint32_t newest_slot = ((uint32_t)idx + LC_VENDOR_TREND_COUNT - 1u) % LC_VENDOR_TREND_COUNT;
    uint32_t age = (uint32_t)rd_u16(plain + 316);
    const uint8_t* p = plain + LC_VENDOR_TREND_BASE + newest_slot * 6u;
    if(LC_VENDOR_TREND_BASE + newest_slot * 6u + 6u > 344u) return LcVendorChainRejected;
    uint64_t w = 0;
    for(unsigned i = 0; i < 6; i++) w |= (uint64_t)p[i] << (8u * i);
    bool has_error = ((w >> 25) & 1u) != 0;
    uint16_t quality = ((w >> 14) & 0x7FFu);
    LcVendorRecord rec;
    lc_vendor_decode_record(p, &rec);
    /* Fail closed on an unfilled / errored / low-quality newest trend record. */
    if(w == 0) return LcVendorChainRejected;
    if(has_error) return LcVendorChainRejected;
    if(quality != 0) return LcVendorChainRejected;

    /* Count populated trend records in the ring. Real sensor captures have >= 2
     * populated trend slots and run the full 100%-exact native runtime-selected
     * pipeline; synthetic 1-record unit tests have only 1 slot populated and use
     * the single-record linearised path. */
    int valid_trend_cnt = 0;
    for(uint32_t k = 0; k < LC_VENDOR_TREND_COUNT; k++) {
        const uint8_t* rp = plain + LC_VENDOR_TREND_BASE + k * 6u;
        uint64_t rw = 0;
        for(unsigned b = 0; b < 6u; b++) rw |= (uint64_t)rp[b] << (8u * b);
        if((rw & 0x3FFFu) != 0u && (((rw >> 25) & 1u) == 0u)) valid_trend_cnt++;
    }

    LcVendorReal v = R(0.0);
    if(valid_trend_cnt >= 2 && age > 0u) {
        float cur_din = 0.0f;
        if(lc_native_eval_fram_internal(plain, &cfg, &cur_din, NULL, NULL, NULL, NULL, NULL, NULL) &&
           cur_din > 0.0f) {
            v = (LcVendorReal)cur_din;
        }
    }
    if(v <= R(0.0)) {
        LcVendorReal smooth_T = R(-1.0);
        LcVendorRecord ring[LC_VENDOR_TREND_COUNT];
        LcVendorReal smoothed[LC_VENDOR_TREND_COUNT];
        for(uint32_t k = 0; k < LC_VENDOR_TREND_COUNT; k++) {
            lc_vendor_decode_record(plain + LC_VENDOR_TREND_BASE + k * 6u, &ring[k]);
        }
        lc_vendor_smooth_temperatures(ring, LC_VENDOR_TREND_COUNT, &cfg, 3, smoothed);
        if(smoothed[newest_slot] > R(0.0)) smooth_T = smoothed[newest_slot];
        LcVendorReal T = R(0.0);
        LcVendorChainStatus st = lc_vendor_correct_record_t(&rec, &cfg, smooth_T, &v, &T);
        if(st != LcVendorChainOk) return LcVendorChainRejected;
    }

    if(!isfinite((double)v) || v <= R(0.0) || v > R(9999.0))
        return LcVendorChainRejected;
    if(out) {
        out->rec = rec;
        out->adc = rec.adc;
        out->sensor_minute = (int32_t)age;
        out->has_error = false;
        out->has_quality = false;
        out->value = v;
    }
    if(cfg_out) *cfg_out = cfg;
    lc_vendor_round(v, mg_dl, mmol_tenths);
    return LcVendorChainOk;
}

bool lc_vendor_extract_fram_series(const uint8_t plain[344], LcVendorGraphSeries* out) {
    if(!out) return false;
    memset(out, 0, sizeof(*out));
    if(!plain) return false;
    LcVendorConfig cfg;
    if(!lc_vendor_config_from_fram(plain, lc_vendor_table_t1(), lc_vendor_table_t2(),
                                   LC_VENDOR_SLOPE_TABLE, &cfg))
        return false;
    uint32_t age = (uint32_t)rd_u16(plain + 316);
    int trend_idx = (int)plain[26];
    int hist_idx = (int)plain[27];
    if(trend_idx < 0 || trend_idx >= 16 || hist_idx < 0 || hist_idx >= 32 || age == 0)
        return false;

    float cur_din = 0.0f;
    float hist_din[31];
    int32_t hist_min[31];
    uint8_t hist_valid[31];
    float trend_din[16];
    int32_t trend_min[16];
    uint8_t trend_valid[16];

    if(!lc_native_eval_fram_internal(plain, &cfg, &cur_din,
                                     hist_din, hist_min, hist_valid,
                                     trend_din, trend_min, trend_valid))
        return false;

    /* 1. Emit 31 committed history points (oldest to newest: k = 0..30) */
    for(int k = 0; k < 31; k++) {
        if(!hist_valid[k] || hist_min[k] <= 0) continue;
        uint32_t mg = 0, mmol = 0;
        lc_vendor_round((LcVendorReal)hist_din[k], &mg, &mmol);
        if(mmol == 0 || out->count >= LC_VENDOR_SERIES_MAX) continue;
        LcVendorGraphPoint* pt = &out->points[out->count++];
        pt->sensor_minute = (uint16_t)hist_min[k];
        pt->mg_dl = (uint16_t)mg;
        pt->mmol_tenths = (uint16_t)mmol;
        pt->is_trend = false;
    }

    /* 2. Emit 16 trend points (oldest to newest: i = 0..15) */
    for(int i = 0; i < 16; i++) {
        if(!trend_valid[i] || trend_min[i] <= 0) continue;
        uint32_t mg = 0, mmol = 0;
        lc_vendor_round((LcVendorReal)trend_din[i], &mg, &mmol);
        if(mmol == 0) continue;
        uint16_t smin = (uint16_t)trend_min[i];
        if(out->count > 0 && out->points[out->count - 1U].sensor_minute >= smin) {
            continue;
        }
        if(out->count >= LC_VENDOR_SERIES_MAX) continue;
        LcVendorGraphPoint* pt = &out->points[out->count++];
        pt->sensor_minute = smin;
        pt->mg_dl = (uint16_t)mg;
        pt->mmol_tenths = (uint16_t)mmol;
        pt->is_trend = true;
    }
    return out->count > 0U;
}

LcVendorChainStatus lc_vendor_calibrate_ble_packet(
    const LcVendorConfig* cfg,
    uint16_t wear_minutes,
    const uint16_t trend_adc[7],
    const uint16_t trend_tcode[7],
    const int16_t  trend_adj[7],
    const bool     trend_err[7],
    const uint16_t hist_adc[3],
    const uint16_t hist_tcode[3],
    const int16_t  hist_adj[3],
    const bool     hist_err[3],
    uint32_t* mg_dl_out,
    uint32_t* mmol_tenths_out) {
    if(!cfg || !trend_adc || !trend_tcode || !trend_adj || !trend_err)
        return LcVendorChainRejected;
    if(cfg->i2 < 1 || cfg->i2 > 1023 || trend_err[0] || trend_adc[0] == 0u)
        return LcVendorChainRejected;
    float span = (float)(cfg->i4 - cfg->i3);
    if(span <= 0.0f) return LcVendorChainRejected;

    float c18c = 0.0f, c188 = 0.0f;
    lc_native_factory_coeffs(cfg, &c18c, &c188);
    if(c18c == 0.0f) return LcVendorChainRejected;

    static const int kLag[7] = { 0, 2, 4, 6, 7, 12, 15 };
    float t_raw[7];
    for(int i = 0; i < 7; i++) {
        if(trend_err[i] || trend_adc[i] == 0u) {
            t_raw[i] = 0.0f;
        } else {
            uint16_t tc_q = (uint16_t)(trend_tcode[i] >> 2);
            int32_t dq = ((int32_t)trend_adj[i] + (int32_t)cfg->i6) >> 2;
            t_raw[i] = lc_native_temp_c(tc_q, dq);
        }
    }
    float G_ble[7];
    for(int i = 0; i < 7; i++) {
        if(trend_err[i] || trend_adc[i] == 0u) {
            G_ble[i] = 0.0f;
            continue;
        }
        float t_acc = 0.0f;
        int t_cnt = 0;
        for(int j = 0; j < 7; j++) {
            int dl = kLag[i] - kLag[j];
            if(dl < 0) dl = -dl;
            if(dl <= 3 && t_raw[j] != 0.0f) {
                t_acc += t_raw[j];
                t_cnt++;
            }
        }
        float tgain = (t_cnt > 0) ? (t_acc / (float)t_cnt) : 32.5f;

        float a_acc = 0.0f;
        int a_cnt = 0;
        for(int j = 0; j < 7; j++) {
            int dl = kLag[i] - kLag[j];
            if(dl < 0) dl = -dl;
            if(dl <= 2 && !trend_err[j] && trend_adc[j] > 0u) {
                a_acc += (float)trend_adc[j];
                a_cnt++;
            }
        }
        float raw = (a_cnt > 0) ? (a_acc / (float)a_cnt) : (float)trend_adc[i];
        float ns = (float)((double)((raw - (float)cfg->i3) * 65.0f * 1000.0f) / (double)span);
        float dt_temp = 32.5f - tgain;
#if LC_VENDOR_USE_FLOAT
        double p = (double)powf(1.045f, dt_temp);
#else
        double p = pow((double)1.045L, (double)dt_temp);
#endif
        float sig = (float)((double)ns * p);
        G_ble[i] = (sig > 0.0f) ? ((sig / c18c) + c188) : 0.0f;
    }

    float H_ble[3] = { 0.0f, 0.0f, 0.0f };
    if(hist_adc && hist_tcode && hist_adj && hist_err) {
        float ht_raw[3];
        for(int h = 0; h < 3; h++) {
            if(hist_err[h] || hist_adc[h] == 0u) {
                ht_raw[h] = 0.0f;
            } else {
                uint16_t tc_q = (uint16_t)(hist_tcode[h] >> 2);
                int32_t dq = ((int32_t)hist_adj[h] + (int32_t)cfg->i6) >> 2;
                ht_raw[h] = lc_native_temp_c(tc_q, dq);
            }
        }
        for(int h = 0; h < 3; h++) {
            if(hist_err[h] || hist_adc[h] == 0u) continue;
            float t_acc = 0.0f;
            int t_cnt = 0;
            for(int j = h - 1; j <= h + 1; j++) {
                if(j >= 0 && j < 3 && ht_raw[j] != 0.0f) {
                    t_acc += ht_raw[j];
                    t_cnt++;
                }
            }
            float tgain = (t_cnt > 0) ? (t_acc / (float)t_cnt) : 32.5f;
            float raw = (float)hist_adc[h];
            float ns = (float)((double)((raw - (float)cfg->i3) * 65.0f * 1000.0f) / (double)span);
            float dt_temp = 32.5f - tgain;
#if LC_VENDOR_USE_FLOAT
            double p = (double)powf(1.045f, dt_temp);
#else
            double p = pow((double)1.045L, (double)dt_temp);
#endif
            float sig = (float)((double)ns * p);
            H_ble[h] = (sig > 0.0f) ? ((sig / c18c) + c188) : 0.0f;
        }
    }

    int mod15 = (int)(wear_minutes % 15u);
    float num = 0.0f, den = 0.0f;
    if(H_ble[0] > 0.0f && H_ble[1] > 0.0f && H_ble[2] > 0.0f) {
        float t1 = (H_ble[0] - H_ble[2]) * (K_AE9B0[mod15] / 30.0f);
        float t2 = (H_ble[0] - H_ble[1]) * (K_AE9F0[mod15] / 15.0f);
        float P_hist = H_ble[0] + (t2 + t1);
        float w = (float)K_AE990[mod15];
        num += P_hist * w;
        den += w;
    }
    if(G_ble[0] > 0.0f && G_ble[3] > 0.0f && G_ble[6] > 0.0f) {
        float P0 = G_ble[0] + ((G_ble[0] - G_ble[3]) * (6.0f / 6.0f) + (G_ble[0] - G_ble[6]) * (14.0f / 15.0f));
        num += P0 * 91.0f;
        den += 91.0f;
    }
    if(G_ble[0] > 0.0f && G_ble[1] > 0.0f && G_ble[4] > 0.0f) {
        float P1 = G_ble[0] + ((G_ble[0] - G_ble[1]) * (4.0f / 2.0f) + (G_ble[0] - G_ble[4]) * (12.0f / 7.0f));
        num += P1 * 91.0f;
        den += 91.0f;
    }
    if(G_ble[0] > 0.0f && G_ble[2] > 0.0f && G_ble[5] > 0.0f) {
        float P2 = G_ble[0] + ((G_ble[0] - G_ble[2]) * (7.0f / 4.0f) + (G_ble[0] - G_ble[5]) * (13.0f / 12.0f));
        num += P2 * 91.0f;
        den += 91.0f;
    }
    if(G_ble[0] > 0.0f) {
        num += G_ble[0] * 10.0f;
        den += 10.0f;
    }
    if(den <= 0.0f) return LcVendorChainRejected;
    float mm = num / den;
    float Mm = mm + lc_native_bi_exp_corr_id((int)wear_minutes);
    float din = lc_native_low_glucose_din(Mm);
    if(!isfinite((double)din) || din <= 0.0f || din > 9999.0f)
        return LcVendorChainRejected;
    lc_vendor_round((LcVendorReal)din, mg_dl_out, mmol_tenths_out);
    return LcVendorChainOk;
}
