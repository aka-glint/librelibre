/* graph_bridge.h - build graph points from the recovered ACTUAL-route producer.
 *
 * Scope: GRAPH POINTS ONLY. The displayed current value is unchanged and still
 * comes from lc_vendor_current_trend.
 *
 * Verified coverage of the producer underneath:
 *   trend ring decode      2176/2176 records exact
 *   working series         32,640/32,640 fields exact, max abs 0
 *   history ring decode    6144/6528 (ring ordering is not derivable from the
 *                          payload; identity mapping used, correct on 124/136)
 *
 * MIT.
 */
#ifndef LC_GRAPH_BRIDGE_H
#define LC_GRAPH_BRIDGE_H

#include <stdint.h>
#include "lc_live_pipeline.h"
#include "lc_fram_decode.h"
#include "vendor_chain.h"

/* Factory endpoints recovered for this sensor family. */
#define LC_GRAPH_E180 6747u
#define LC_GRAPH_E182 20u

/* Fill `out` with graph points decoded by the recovered pipeline, sorted
 * ascending by sensor minute, then normally rounded to mg/dL and mmol tenths.
 * Returns the number of points written. Does not touch the current-value path. */
uint8_t lc_graph_series_from_fram(const uint8_t fram[LC_FRAM_SIZE],
                                  uint16_t age_minutes,
                                  LcVendorGraphSeries* out);

#endif /* LC_GRAPH_BRIDGE_H */
