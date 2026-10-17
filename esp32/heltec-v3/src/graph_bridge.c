/* graph_bridge.c - graph points from the recovered native calibration pipeline.
 * Delegates to lc_vendor_extract_fram_series so graph points are in calibrated
 * mg/dL and mmol/L tenths. MIT. */
#include "graph_bridge.h"
#include "vendor_chain.h"

uint8_t lc_graph_series_from_fram(const uint8_t fram[LC_FRAM_SIZE],
                                  uint16_t age_minutes,
                                  LcVendorGraphSeries* out)
{
    (void)age_minutes;
    if (!fram || !out) return 0u;
    if (!lc_vendor_extract_fram_series(fram, out)) return 0u;
    return out->count;
}
