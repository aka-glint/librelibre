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

LcVendorChainStatus lc_vendor_correct_record(const LcVendorRecord* rec,
                                             const LcVendorConfig* cfg,
                                             LcVendorReal* value_out,
                                             LcVendorReal* temperature_out) {
    if(!rec || !cfg) return LcVendorChainRejected;
    LcVendorReal s = lc_vendor_adc_scale(rec->adc, cfg);
    LcVendorReal T = R(0.0);
    bool have_t = lc_vendor_temperature_c(rec->tcode, rec->adjust, cfg->i6, &T);
    if(!have_t) T = K_TGAIN_REF;  /* reference temperature fallback (32.5) */
    LcVendorReal v = lc_vendor_temp_gain(s, T);
    v = lc_vendor_factory_transform(v, cfg);
    if(value_out) *value_out = v;
    if(temperature_out) *temperature_out = T;
    if(!isfinite((double)v)) return LcVendorChainRejected;
    return LcVendorChainOk;
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

/* Source-backed CURRENT/TREND path.
 *
 * The vendor UI's current sample and its freshness both live on the 16-slot TREND
 * ring at byte 28 (plain[26] index), NOT the 32-slot 124 history ring. This
 * selector is deliberately strict:
 *   - bounded: next-write index must be < LC_VENDOR_TREND_COUNT (zero wraps),
 *     an out-of-range index is REJECTED, never masked with a modulo;
 *   - fail-closed: an empty (all-zero) newest record, an error-flagged newest
 *     (bit 25), or a non-zero quality field on the newest record is REJECTED;
 *   - no fallback: it never walks back to an older trend slot or over to the
 *     history ring to produce a value.
 * The returned value is the corrected LINEARISED estimate (no vendor smoothing);
 * it is an approximation of the current sample, not a smoothed vendor current. */
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
    uint16_t quality = ((w >> 14) & 0x1FFu);
    LcVendorRecord rec;
    lc_vendor_decode_record(p, &rec);
    /* Fail closed on an unfilled / errored / low-quality newest trend record. */
    if(w == 0) return LcVendorChainRejected;
    if(has_error) return LcVendorChainRejected;
    if(quality != 0) return LcVendorChainRejected;
    LcVendorReal v = R(0.0), T = R(0.0);
    LcVendorChainStatus st = lc_vendor_correct_record(&rec, &cfg, &v, &T);
    /* Representation bound before converting double to uint32_t, not a fitted
     * physiological gate. Matches the screen's existing numeric capacity. */
    if(st != LcVendorChainOk || !isfinite((double)v) || v <= R(0.0) || v > R(9999.0))
        return LcVendorChainRejected;
    if(out) {
        out->rec = rec;
        out->adc = rec.adc;
        /* newest sample's sensor minute is the age counter itself; older slots
         * step back by 1 (pinned DiaBLE: sensor_minute = age - i, i=0 newest). */
        out->sensor_minute = (int32_t)age;
        out->has_error = false;      /* rejected if set */
        out->has_quality = false;    /* rejected if set */
        out->value = v;
    }
    if(cfg_out) *cfg_out = cfg;
    lc_vendor_round(v, mg_dl, mmol_tenths);
    return LcVendorChainOk;
}
