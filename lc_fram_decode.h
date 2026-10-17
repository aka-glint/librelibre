/* lc_fram_decode.h - raw 344-byte FRAM -> packed trend/history records.
 *
 * Closed against the 136 captured replays: the physical ring mapping and the
 * per-record bit fields reproduce the parser's packed output exactly.
 *
 *   trend  ring base   : FRAM byte 28, 16 slots, 6-byte stride
 *   history ring base  : FRAM byte 124, 32 slots, 6-byte stride
 *   trend  stored index: FRAM byte 26
 *   history stored index: FRAM byte 27
 *   sensor age         : u16 LE at FRAM byte 316
 *
 * The stored index is the NEXT write position; slot (index-1) holds the newest
 * committed record. Position i of the chronological output series maps to
 * physical slot (index + i) % count.
 *
 * MIT. Recovered bit intervals; no fitting.
 */
#ifndef LC_FRAM_DECODE_H
#define LC_FRAM_DECODE_H

#include <stdint.h>
#include "lc_live_pipeline.h"

#ifndef LC_FRAM_SIZE
#define LC_FRAM_SIZE        344
#endif
#define LC_TREND_BASE       28
#define LC_TREND_COUNT      16
#define LC_TREND_INDEX_OFF  26
#define LC_HIST_BASE        124
#define LC_HIST_COUNT       32
#define LC_HIST_INDEX_OFF   27
#define LC_AGE_OFF          316

/* Decode one 6-byte little-endian record. Bit intervals recovered from the
 * parser: ADC bits 0..13, error bit 25, quality bits 14..24 (gated by bit 25),
 * temperature code bits 26..37, adjustment bits 38..46 with sign bit 47. */
void lc_fram_decode_rec(const uint8_t rec6[6], LcLivePackedRec* out);

/* Decode the whole FRAM into chronological packed series.
 * trend[0..15]  oldest -> newest
 * history[0..31] oldest -> newest
 * Returns 0 on success, -1 on a bad argument or an out-of-range ring index. */
int lc_fram_to_packed(const uint8_t fram[LC_FRAM_SIZE],
                      LcLivePackedRec trend[LC_TREND_COUNT],
                      LcLivePackedRec history[LC_HIST_COUNT]);

/* Sensor age in minutes. */
uint16_t lc_fram_age(const uint8_t fram[LC_FRAM_SIZE]);

#endif /* LC_FRAM_DECODE_H */
