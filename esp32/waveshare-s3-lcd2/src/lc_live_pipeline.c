/* lc_live_pipeline.c - LibreLibre ACTUAL-route numerical pipeline.
 * MIT. Constants are recovered program literals. No fitting. */
#include "lc_live_pipeline.h"
#include <math.h>
#include <string.h>

/* Steinhart-Hart cubic-in-log coefficients (recovered rodata doubles). */
static const double C3 = 5.283566e-8L;
static const double C2 = 7.061775e-7L;
static const double C1 = 0.0001964561L;
static const double C0 = 0.0009180023L;
#define GAIN_BASE 1.045L
#define GAIN_REF  32.5L
#define RESIST_SCALE 72500.0f
#define RESIST_OFFSET 1000.0L
#define SIG_K1 65.0f
#define SIG_K2 1000.0f
#define WEIGHT 91.0f
#define DISP_DIV 37.5L
#define DISP_POW 6.0L

static inline float f32d(double x) { return (float)x; }
static inline float fmul(float a, float b) { return (float)(a * b); }

static inline int rec_rejected(const LcLivePackedRec* r) { return (r->flags & 0x800u) != 0; }
static inline int rec_empty(const LcLivePackedRec* r) {
    return r->adc == 0 && r->tcode == 0 && r->flags == 0 && r->denom_q == 0 && r->q2 == 0;
}

/* 0x82d74 / 0x8335f: f32 normalize chain,
 * f32(f32(f32(65.0f*(raw - e182))*1000.0f) / f32(e180 - e182)). */
static float normalize_signal(float raw, uint16_t e180, uint16_t e182)
{
    float x = (float)(raw - (float)e182);
    float y = fmul(SIG_K1, x);
    float z = fmul(y, SIG_K2);
    float d = (float)((int)e180 - (int)e182);
    return (float)(z / d);
}

/* 0x83624: resistance/log/Steinhart-Hart -> degrees C, narrowed to f32. */
static float temperature_celsius(uint16_t tcode, uint16_t denom_q)
{
    if (denom_q == 0) return 0.0f;
    float rr = (float)((RESIST_SCALE * (float)tcode) / (float)denom_q);
    double R = (double)rr - RESIST_OFFSET;
    if (R <= 0.0L) return 0.0f;
    double L = log(R);
    double den = ((C3 * L) * L) * L + ((C2 * L) * L + (C1 * L + C0));
    if (den == 0.0L) return 0.0f;
    return (float)(1.0L / den - 273.15L);
}

/* 0x85534: clipped-window mean of +4 excluding zero entries. */
static float temperature_mean(const float* tvals, int n, int i, int radius)
{
    int lo = i - radius; if (lo < 0) lo = 0;
    int hi = i + radius; if (hi > n - 1) hi = n - 1;
    float acc = 0.0f; int cnt = 0;
    for (int j = lo; j <= hi; ++j)
        if (tvals[j] != 0.0f) { acc = (float)(acc + tvals[j]); cnt++; }
    if (cnt == 0) return 0.0f;
    return (float)(acc / (float)cnt);
}

/* 0x86090: gain in double, result narrowed to f32. */
static float signal_from(float norm_f32, float t_gain)
{
    double p = pow(GAIN_BASE, (double)((float)(GAIN_REF - t_gain)));
    return (float)((double)norm_f32 * p);
}

void lc_live_produce_series(const LcLivePackedRec* recs, int n,
                            const LcLiveSensorConfig* cfg,
                            LcLiveSeriesKind kind, LcLiveWorkRec* out)
{
    if (!recs || !cfg || !out || n <= 0) return;
    int radius = (kind == LC_LIVE_TREND) ? 3 : 1;
    int is_hist = (kind == LC_LIVE_HISTORY);
    float tvals[64];
    int   zeroed[64];
    if (n > 64) n = 64;

    for (int i = 0; i < n; ++i) {
        /* History zeroes rejected/sentinel slots; trend keeps rejected temps. */
        if (is_hist) zeroed[i] = (rec_rejected(&recs[i]) || rec_empty(&recs[i]));
        else         zeroed[i] = 0;
        tvals[i] = zeroed[i] ? 0.0f : temperature_celsius(recs[i].tcode, recs[i].denom_q);
    }
    for (int i = 0; i < n; ++i) {
        out[i].t_raw  = tvals[i];
        out[i].t_ret  = tvals[i];
        out[i].field8 = 0.0f;
    }
    for (int i = 0; i < n; ++i)
        out[i].t_gain = temperature_mean(tvals, n, i, radius);

    for (int i = 0; i < n; ++i) {
        if (zeroed[i]) { out[i].sig = 0.0f; continue; }
        float raw;
        if (kind == LC_LIVE_TREND) {
            int lo = i - 2; if (lo < 0) lo = 0;
            int hi = i + 2; if (hi > n - 1) hi = n - 1;
            double acc = 0.0L; int cnt = 0;
            for (int j = lo; j <= hi; ++j)
                if (!rec_rejected(&recs[j])) { acc += (double)recs[j].adc; cnt++; }
            raw = (cnt == 0) ? 0.0f : (float)(acc / (double)cnt);
        } else {
            raw = (float)recs[i].adc;
        }
        float norm = normalize_signal(raw, cfg->e180, cfg->e182);
        out[i].sig = signal_from(norm, out[i].t_gain);
    }
}

/* ---- [E] per-position estimate, 0x7637a..0x764f0 -> 0x7d8b2 ---- */
void lc_live_edge_for_i(int i, float* xmm1, float* xmm2, int* al, int* bl)
{
    if (i == 0)      { *xmm1 = 14.0f; *xmm2 = 6.0f; *al = 6; *bl = 15; }
    else if (i == 1) { *xmm1 = 12.0f; *xmm2 = 4.0f; *al = 2; *bl = 7; }
    else             { *xmm1 = 13.0f; *xmm2 = 7.0f; *al = 4; *bl = 12; }
}

float lc_live_estimate_one(const float* V, int n, int i)
{
    if (!V) return 0.0f;
    float xmm1, xmm2; int al, bl;
    lc_live_edge_for_i(i, &xmm1, &xmm2, &al, &bl);
    int ib = n - bl, ia = n - al;
    if (ib < 0 || ia < 0) return 0.0f;
    float c1 = xmm1 / (float)bl;
    float c2 = xmm2 / (float)al;
    float a = (V[n] - V[ib]) * c1;
    float b = (V[n] - V[ia]) * c2;
    return (float)(V[n] + (b + a));
}

/* ---- [B] blend coefficients, helper 0x80d2c state 24 @0x812c6 ---- */
static const int16_t BLEND_TABLE[16] = {
    120, 105, 91, 78, 66, 55, 45, 36, 28, 21, 15, 10, 6, 3, 1, 0
};
#define BLEND_K 120.0f

void lc_live_blend_coeffs(uint16_t selector, LcLiveBlend* out)
{
    if (!out) return;
    if (selector == 0) { out->coef_a = 1.0f; out->coef_b = 0.0f; return; }
    if (selector <= 15) {
        float t   = (float)(int16_t)BLEND_TABLE[selector];
        float den = t + BLEND_K;
        out->coef_b = BLEND_K / den;
        out->coef_a = t / den;
        return;
    }
    out->coef_a = 0.0f; out->coef_b = 1.0f;
}

/* ---- [C] mean + blend + display, closed exactly on 136/136 ---- */
float lc_live_mean_plus_offset(float mean_num, float mean_den, float w84c)
{
    if (mean_den == 0.0f) return w84c;
    float mm = f32d((double)mean_num / (double)mean_den);
    return f32d((double)mm + (double)w84c);
}

float lc_live_blend_apply(const LcLiveBlend* b, float w10, float m)
{
    float t1 = f32d((double)b->coef_a * (double)w10);
    float t2 = f32d((double)b->coef_b * (double)m);
    return f32d((double)t1 + (double)t2);
}

int lc_live_display_transform(float x)
{
    float base = f32d((double)x / DISP_DIV);
    double p   = pow((double)base, DISP_POW);
    float p32  = f32d(p);
    float num  = f32d((double)(1.0f) - (double)p32);
    float den  = f32d((double)p32 + (double)(1.0f));
    float q    = f32d((double)num / (double)den);
    float u    = f32d((double)q + (double)(1.0f));
    float v    = f32d((double)u * (double)(0.49f));
    float w    = f32d((double)v + (double)(1.02f));
    float y    = f32d((double)w * (double)x);
    float din  = f32d((double)y - (double)(4.0f));
    /* Literals are suffixed so -fsingle-precision-constant cannot narrow them. */
    double d = (double)din;
    long long r = (d >= (double)(0.0f)) ? (long long)floor(d + (double)(0.5f))
                                    : (long long)ceil(d - (double)(0.5f));
    return (int)r;
}

int lc_live_downstream_displayed(float mean_num, float mean_den, float w84c,
                                 float w10, float coef_a, float coef_b)
{
    float m = lc_live_mean_plus_offset(mean_num, mean_den, w84c);
    LcLiveBlend b; b.coef_a = coef_a; b.coef_b = coef_b;
    float x = lc_live_blend_apply(&b, w10, m);
    return lc_live_display_transform(x);
}

int lc_live_current_from_estimates(const float* estimates, const uint8_t* admitted,
                                   int count, float w84c, float w10,
                                   uint16_t blend_selector)
{
    if (!estimates || count <= 0) return 0;
    float num = 0.0f, den = 0.0f;
    for (int k = 0; k < count; ++k) {
        if (admitted && !admitted[k]) continue;
        num = (float)(num + (float)(WEIGHT * estimates[k]));
        den = (float)(den + WEIGHT);
    }
    LcLiveBlend b;
    lc_live_blend_coeffs(blend_selector, &b);
    return lc_live_downstream_displayed(num, den, w84c, w10, b.coef_a, b.coef_b);
}
