#include "scan_history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ll_history_init(LlHistoryStore* store) {
    if(!store) return;
    memset(store, 0, sizeof(*store));
}

bool ll_history_add(
    LlHistoryStore* store,
    const char* serial,
    uint16_t age_minutes,
    uint16_t mmol_tenths,
    uint16_t mg_dl,
    uint32_t boot_sec) {
    if(!store || !serial || !serial[0] || mmol_tenths == 0U) return false;

    /* If the newest entry for this serial has the exact same sensor minute age, update it in place */
    for(size_t i = store->count; i > 0U; i--) {
        LlHistoryPoint* p = &store->points[i - 1U];
        if(strcmp(p->serial, serial) == 0 && p->age_minutes == age_minutes) {
            p->mmol_tenths = mmol_tenths;
            p->mg_dl = mg_dl;
            p->boot_sec = boot_sec;
            return false; /* updated in place, no new CSV row needed */
        }
    }

    if(store->count >= LL_HISTORY_MAX_POINTS) {
        memmove(&store->points[0], &store->points[1], (LL_HISTORY_MAX_POINTS - 1U) * sizeof(LlHistoryPoint));
        store->count = LL_HISTORY_MAX_POINTS - 1U;
    }

    LlHistoryPoint* dst = &store->points[store->count++];
    memset(dst, 0, sizeof(*dst));
    snprintf(dst->serial, sizeof(dst->serial), "%s", serial);
    dst->age_minutes = age_minutes;
    dst->mmol_tenths = mmol_tenths;
    dst->mg_dl = mg_dl;
    dst->boot_sec = boot_sec;
    return true;
}

size_t ll_history_format_csv_line(
    const LlHistoryPoint* pt,
    char* out_buf,
    size_t out_cap) {
    if(!pt || !out_buf || out_cap == 0U) return 0U;
    int n = snprintf(
        out_buf,
        out_cap,
        "%s,%u,%u,%u,%lu\n",
        pt->serial,
        (unsigned)pt->age_minutes,
        (unsigned)pt->mmol_tenths,
        (unsigned)pt->mg_dl,
        (unsigned long)pt->boot_sec);
    if(n < 0 || (size_t)n >= out_cap) return 0U;
    return (size_t)n;
}

bool ll_history_parse_csv_line(
    const char* line,
    LlHistoryPoint* pt_out) {
    if(!line || !pt_out) return false;
    while(*line == ' ' || *line == '\t') line++;
    if(*line == '#' || *line == '\0' || *line == '\n' || *line == '\r') return false;
    if(strncmp(line, "serial,", 7) == 0) return false;

    char buf[128];
    snprintf(buf, sizeof(buf), "%s", line);
    char* tok_ser = strtok(buf, ",\r\n");
    char* tok_age = strtok(NULL, ",\r\n");
    char* tok_mmol = strtok(NULL, ",\r\n");
    char* tok_mg = strtok(NULL, ",\r\n");
    char* tok_boot = strtok(NULL, ",\r\n");
    if(!tok_ser || !tok_age || !tok_mmol || !tok_mg) return false;

    unsigned long age = strtoul(tok_age, NULL, 10);
    unsigned long mmol = strtoul(tok_mmol, NULL, 10);
    unsigned long mg = strtoul(tok_mg, NULL, 10);
    unsigned long boot = tok_boot ? strtoul(tok_boot, NULL, 10) : 0UL;
    if(mmol == 0UL || mmol > 500UL || age > 65535UL) return false;

    memset(pt_out, 0, sizeof(*pt_out));
    snprintf(pt_out->serial, sizeof(pt_out->serial), "%s", tok_ser);
    pt_out->age_minutes = (uint16_t)age;
    pt_out->mmol_tenths = (uint16_t)mmol;
    pt_out->mg_dl = (uint16_t)mg;
    pt_out->boot_sec = (uint32_t)boot;
    return true;
}

size_t ll_history_query_window(
    const LlHistoryStore* store,
    const char* serial,
    uint32_t window_minutes,
    const LlHistoryPoint** out_ptrs,
    size_t out_cap,
    uint16_t* min_age_out,
    uint16_t* max_age_out,
    uint16_t* min_mmol_out,
    uint16_t* max_mmol_out) {
    if(!store || store->count == 0U || !out_ptrs || out_cap == 0U) return 0U;

    const char* target_serial = (serial && serial[0]) ? serial : store->points[store->count - 1U].serial;

    /* Find latest age_minutes for target_serial */
    uint16_t latest_age = 0U;
    bool found_any = false;
    for(size_t i = 0; i < store->count; i++) {
        if(strcmp(store->points[i].serial, target_serial) == 0) {
            if(!found_any || store->points[i].age_minutes >= latest_age) {
                latest_age = store->points[i].age_minutes;
                found_any = true;
            }
        }
    }
    if(!found_any) return 0U;

    uint16_t cutoff_age = 0U;
    if(window_minutes > 0U && latest_age > window_minutes) {
        cutoff_age = (uint16_t)(latest_age - window_minutes);
    }

    size_t n = 0U;
    uint16_t min_age = 0xFFFFU, max_age = 0U;
    uint16_t min_mmol = 0xFFFFU, max_mmol = 0U;
    for(size_t i = 0; i < store->count && n < out_cap; i++) {
        const LlHistoryPoint* p = &store->points[i];
        if(strcmp(p->serial, target_serial) != 0) continue;
        if(window_minutes > 0U && p->age_minutes < cutoff_age) continue;
        out_ptrs[n++] = p;
        if(p->age_minutes < min_age) min_age = p->age_minutes;
        if(p->age_minutes > max_age) max_age = p->age_minutes;
        if(p->mmol_tenths < min_mmol) min_mmol = p->mmol_tenths;
        if(p->mmol_tenths > max_mmol) max_mmol = p->mmol_tenths;
    }

    if(min_age_out) *min_age_out = (n > 0U) ? min_age : 0U;
    if(max_age_out) *max_age_out = (n > 0U) ? max_age : 0U;
    if(min_mmol_out) *min_mmol_out = (n > 0U) ? min_mmol : 0U;
    if(max_mmol_out) *max_mmol_out = (n > 0U) ? max_mmol : 0U;
    return n;
}
