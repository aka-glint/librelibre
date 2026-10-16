#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "capture.h"
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Abstract ISO15693 RF transceiver ops.
 * Whether the physical 13.56 MHz front-end is a Flipper Zero acting as a dumb
 * UART/USB passthrough or a direct SPI NFC chip (PN5180 / ST25R3916 / TRF7970A)
 * wired to the ESP32, the ESP32 owns the entire 17-step state machine and calls
 * transceive() for each raw ISO15693 frame.
 *
 * tx_buf / tx_len: raw ISO15693 command payload (without SOF/EOF/CRC16; hardware
 *                  or passthrough appends/strips link CRC16).
 * rx_buf / rx_cap: buffer for raw ISO15693 response (flags byte + payload).
 * rx_len_out:      actual bytes received.
 * Returns 0 on success, nonzero error code on timeout/framing error. */
typedef int (*LlIso15693TransceiveFn)(
    void* ctx,
    const uint8_t* tx_buf,
    size_t tx_len,
    uint8_t* rx_buf,
    size_t rx_cap,
    size_t* rx_len_out);

typedef struct {
    LlIso15693TransceiveFn transceive;
    void* ctx;
} LlNfcHal;

typedef enum {
    LlReaderOk = 0,
    LlReaderNoTag = 1,
    LlReaderNonTi = 2,
    LlReaderUnsupportedFamily = 3,
    LlReaderPatchError = 4,
    LlReaderFramError = 5,
    LlReaderForbiddenCommand = 6,
} LlReaderStatus;

/* Sole RF send boundary on the ESP32.
 * Enforces lc_command_allowed(cmd, cmd_len) BEFORE touching the HAL. */
int ll_nfc_send_allowed(
    const LlNfcHal* hal,
    const uint8_t* cmd,
    size_t cmd_len,
    uint8_t* rx_buf,
    size_t rx_cap,
    size_t* rx_len_out);

/* Master 17-command acquisition state machine driven by the ESP32:
 *   Phase 0: Inventory (26 01 00) -> parse 8-byte wire UID, check TI prefix
 *   Phase 1: Patch info (02 A1 07) -> parse 6-byte patch, check EU2 family gate
 *   Phase 2..16: 15 Read-Multiple-Blocks commands (02 23 start count) -> 344B FRAM
 * Populates LcCapture with the full 17-exchange transcript and verifies
 * lc_capture_complete(capture). */
LlReaderStatus ll_nfc_acquire_sensor(
    const LlNfcHal* hal,
    uint32_t timestamp_unix,
    LcCapture* capture_out);

#ifdef __cplusplus
}
#endif
