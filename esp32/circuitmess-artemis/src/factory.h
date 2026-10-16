#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "capture.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Historical LibreLink-2.3-derived estimate, NOT current/clinical glucose.
 * Double coefficients copied without tuning from pinned MIT DiaBLE source.
 * Source/license/provenance: GLUCOSE-BUILD.md and INTEGRITY-LICENSE.md. */
typedef struct { int i1, i2, i3, i4, i5, i6; } LcFactoryCoefficients;
typedef struct {
    int raw_adc, raw_temperature, temperature_adjustment, sensor_minute;
    int quality_bits, quality_flags;
    bool has_error;
} LcFactoryRecord;
typedef enum {
    LcFactoryNotChecked, LcFactoryEstimate, LcFactoryRejected
} LcFactoryStatus;
typedef struct {
    double temperature_c;
    double unrounded_mg_dl;
    uint32_t mg_dl;
    uint32_t mmol_tenths;
    LcFactoryStatus status;
} LcFactory;
/* Shared physical calibration tables (factory.c). These are the pinned DiaBLE
 * arrays, named by the vendor ROLE in the recovery docs. The recovered chain
 * (vendor_chain.c) uses these single copies via vendor_tables.h accessors.
 * NOTE: the vendor's config+0x10 "t1" (0.1073... family) is DiaBLE `t2`
 * (LC_FACTORY_DIABLE_T2), and config+0x18 "t2" (1.5 family) is DiaBLE `t1`. */
extern const double LC_FACTORY_DIABLE_T1[1023];
extern const double LC_FACTORY_DIABLE_T2[1023];
void lc_factory_parse(const uint8_t plain[344], const uint8_t patch[6], LcFactory* result);
void lc_factory_check(const LcCapture* capture, LcFactory* result);
void lc_factory_calculate(const LcFactoryRecord* record, const LcFactoryCoefficients* coefficients, LcFactory* result);

#ifdef __cplusplus
}
#endif
