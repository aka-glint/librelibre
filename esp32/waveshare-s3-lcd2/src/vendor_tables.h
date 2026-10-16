/* LibreLibre shared vendor calibration tables. The big 1023-entry f64 t1/t2
 * tables live ONCE in factory.c (verified bit-exact). This header exposes the
 * 8-entry slope table and accessors that map the vendor roles onto factory.c's
 * arrays (no duplicate rodata). Indexing into the big tables is [i2-1]. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif


extern const double LC_VENDOR_SLOPE_TABLE[8];

/* vendor config+0x10 ("t1", 0.1073... family) == factory.c DiaBLE `t2`
 * vendor config+0x18 ("t2", 1.5 family)       == factory.c DiaBLE `t1` */
const double* lc_vendor_table_t1(void);
const double* lc_vendor_table_t2(void);

#ifdef __cplusplus
}
#endif
