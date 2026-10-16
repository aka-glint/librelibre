#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "capture.h"
#include "integrity.h"
#include "factory.h"
#include "vendor_chain.h"
#include "lifecycle.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum line length for a bridge frame:
 * "LL2|" (4) + 16-hex UID + "|" (1) + 12-hex patch + "|" (1) + 688-hex FRAM + "|" (1) + 4-hex CRC16 + "\r\n\0"
 * = 728 bytes. We allow up to 768 bytes bounded. */
#define LL_BRIDGE_LINE_MAX 768U

typedef enum {
    LlBridgeWait = 0,
    LlBridgeOk = 1,
    LlBridgeBadPrefix = 2,
    LlBridgeBadFormat = 3,
    LlBridgeBadCrc = 4,
} LlBridgeParseStatus;

typedef struct {
    bool has_scan;
    uint8_t uid[8];
    uint8_t patch[6];
    uint8_t fram[LC_FRAM_SIZE];
    LcCapture capture;
    LcIntegrity integrity;
    LcFactory factory;
    LcVendorChainResult vendor_chain;
    LcLife life;
    char serial[13];
    bool serial_valid;
    char primary_line[32];
    char fresh_life_line[64];
    char serial_line[24];
    char diag_line1[48];
    char diag_line2[48];
    char diag_line3[48];
    char uid_hex[17];
} LlCompanionState;

/* Format a complete 344-byte capture into an ASCII bridge frame:
 * LL2|<16-hex-uid>|<12-hex-patch>|<688-hex-fram>|<4-hex-crc16>\n
 * where crc16 is lc_integrity_crc over the 358 raw bytes (uid[8] + patch[6] + fram[344]).
 * Returns number of bytes written (excluding NUL), or 0 on error/short buffer. */
size_t ll_bridge_format_frame(
    const uint8_t uid[8],
    const uint8_t patch[6],
    const uint8_t fram[LC_FRAM_SIZE],
    char* out,
    size_t capacity);

/* Parse and verify a single line (without trailing CR/LF).
 * Validates exact lengths, strict hex, and the 16-bit frame CRC before writing out. */
LlBridgeParseStatus ll_bridge_parse_line(
    const char* line,
    uint8_t uid_out[8],
    uint8_t patch_out[6],
    uint8_t fram_out[LC_FRAM_SIZE]);

/* Evaluate a verified (uid, patch, fram) tuple using the exact LibreLibre
 * integrity, sensor status, lifecycle, and vendor_chain pipeline.
 * Wipes transient plaintext FRAM before returning. */
void ll_companion_evaluate(
    const uint8_t uid[8],
    const uint8_t patch[6],
    const uint8_t fram[LC_FRAM_SIZE],
    LlCompanionState* state);

#ifdef __cplusplus
}
#endif
