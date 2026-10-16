#pragma once
#include "protocol.h"
#define LC_FRAM_SIZE 344U
#define LC_TRANSCRIPT_MAX 17U
#define LC_RESPONSE_MAX 256U
#define LC_JSON_CAPACITY 16384U
typedef struct {
    uint8_t command[4];
    size_t command_length;
    uint8_t response[LC_RESPONSE_MAX];
    size_t response_length;
    unsigned error;
} LcExchange;
typedef struct {
    uint8_t uid[8];
    uint8_t patch[6];
    bool uid_valid;
    bool patch_valid;
    /* Exact naive local-wall RTC encoding at UID acceptance; not UTC. */
    uint32_t timestamp;
    uint32_t save_timestamp;
    bool save_time_recorded;
    uint32_t elapsed_ms;
    unsigned error;
    uint8_t fram[LC_FRAM_SIZE];
    size_t fram_length;
    LcExchange transcript[LC_TRANSCRIPT_MAX];
    size_t transcript_length;
    unsigned inventory_attempts;
} LcCapture;
/* Format naive RTC calendar fields; no UTC/offset inference. Needs 20 bytes. */
bool lc_capture_local_datetime(uint32_t raw, char* out, size_t capacity);
bool lc_capture_complete(const LcCapture* capture);
size_t lc_capture_json(const LcCapture* capture, char* out, size_t capacity);
