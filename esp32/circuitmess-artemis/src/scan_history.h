#ifndef LIBRE_SCAN_HISTORY_H
#define LIBRE_SCAN_HISTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LL_HISTORY_MAX_POINTS 512U

typedef struct {
    char serial[16];          /* e.g. "3M00009DHCR" */
    uint16_t age_minutes;     /* Sensor minute clock at scan (0..20854+) */
    uint16_t mmol_tenths;     /* e.g. 85 -> 8.5 mmol/L */
    uint16_t mg_dl;           /* e.g. 153 mg/dL */
    uint32_t boot_sec;        /* Device monotonic uptime seconds at scan */
} LlHistoryPoint;

typedef struct {
    LlHistoryPoint points[LL_HISTORY_MAX_POINTS];
    size_t count;
} LlHistoryStore;

void ll_history_init(LlHistoryStore* store);

/* Appends or updates a scan point (dedupes if same serial + same age_minutes,
 * keeping points sorted ascending by age_minutes within the active serial). */
bool ll_history_add(
    LlHistoryStore* store,
    const char* serial,
    uint16_t age_minutes,
    uint16_t mmol_tenths,
    uint16_t mg_dl,
    uint32_t boot_sec);

/* Formats a CSV line: "<serial>,<age_minutes>,<mmol_tenths>,<mg_dl>,<boot_sec>\n" */
size_t ll_history_format_csv_line(
    const LlHistoryPoint* pt,
    char* out_buf,
    size_t out_cap);

/* Parses a CSV line written by ll_history_format_csv_line */
bool ll_history_parse_csv_line(
    const char* line,
    LlHistoryPoint* pt_out);

/* Filters points for `serial` (or latest serial in store if `serial` is NULL/empty)
 * that fall within `window_minutes` of that sensor's latest scan age_minutes.
 * If `window_minutes == 0`, returns all scans for that sensor across its full life.
 * Also computes min/max mmol_tenths and min/max age_minutes across the matched slice. */
size_t ll_history_query_window(
    const LlHistoryStore* store,
    const char* serial,
    uint32_t window_minutes,
    const LlHistoryPoint** out_ptrs,
    size_t out_cap,
    uint16_t* min_age_out,
    uint16_t* max_age_out,
    uint16_t* min_mmol_out,
    uint16_t* max_mmol_out);

#ifdef __cplusplus
}
#endif

#endif
