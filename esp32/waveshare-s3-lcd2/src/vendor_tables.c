/* LibreLibre shared vendor calibration tables. The big t1/t2 tables live ONCE
 * in factory.c (verified bit-exact by verify_factory_build.py). This file adds
 * only the 8-entry slope table and thin, name-mapping accessors so the recovered
 * chain shares one physical copy (no duplicate 16 KB of rodata).
 * See VENDOR-CHAIN-PORT.md. */
#include "vendor_tables.h"
#include "factory.h"

/* slope table 0xa1d20: slope[i] = -i * (1/15000), recovered from the image.
 * uFBT uses -fsingle-precision-constant: the L suffix forces decimal -> double. */
const double LC_VENDOR_SLOPE_TABLE[8] = {
    0.0L, -6.666666666666667e-05L, -0.00013333333333333334L, -0.0002L,
    -0.0002666666666666667L, -0.0003333333333333333L, -0.0004L,
    -0.00046666666666666666L,
};

/* vendor config+0x10 ("t1", 0.1073... family) == factory.c's DiaBLE `t2`.
 * vendor config+0x18 ("t2", 1.5 family)       == factory.c's DiaBLE `t1`. */
const double* lc_vendor_table_t1(void) { return LC_FACTORY_DIABLE_T2; }
const double* lc_vendor_table_t2(void) { return LC_FACTORY_DIABLE_T1; }
