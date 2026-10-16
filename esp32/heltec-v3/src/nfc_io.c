#include "nfc_io.h"
#include <string.h>

int ll_nfc_send_allowed(
    const LlNfcHal* hal,
    const uint8_t* cmd,
    size_t cmd_len,
    uint8_t* rx_buf,
    size_t rx_cap,
    size_t* rx_len_out) {
    if(!hal || !hal->transceive || !cmd || !rx_buf || !rx_len_out) return -1;
    *rx_len_out = 0;
    /* Hard allowlist gate: only 26 01 00, 02 A1 07, and bounded 02 23 reads */
    if(!lc_command_allowed(cmd, cmd_len)) return -2;
    return hal->transceive(hal->ctx, cmd, cmd_len, rx_buf, rx_cap, rx_len_out);
}

LlReaderStatus ll_nfc_acquire_sensor(
    const LlNfcHal* hal,
    uint32_t timestamp_unix,
    LcCapture* capture) {
    if(!hal || !capture) return LlReaderFramError;
    memset(capture, 0, sizeof(*capture));

    uint8_t rx[LC_RESPONSE_MAX];
    size_t rx_len = 0;
    uint8_t parsed[24];

    /* Phase 0: ISO15693 Inventory (26 01 00) */
    static const uint8_t inv_cmd[3] = {0x26, 0x01, 0x00};
    int err = ll_nfc_send_allowed(hal, inv_cmd, sizeof(inv_cmd), rx, sizeof(rx), &rx_len);
    capture->inventory_attempts = 1U;
    if(err != 0 || !lc_uid_parse(rx, rx_len, parsed)) {
        capture->error = (unsigned)(err != 0 ? err : 1);
        return LlReaderNoTag;
    }

    /* Record inventory exchange */
    LcExchange* ex0 = &capture->transcript[capture->transcript_length++];
    memcpy(ex0->command, inv_cmd, 3U);
    ex0->command_length = 3U;
    memcpy(ex0->response, rx, rx_len);
    ex0->response_length = rx_len;
    ex0->error = 0U;

    memcpy(capture->uid, parsed, 8U);
    capture->uid_valid = true;
    capture->timestamp = timestamp_unix;
    if(!lc_uid_is_ti(capture->uid)) {
        return LlReaderNonTi;
    }

    /* Phase 1: TI Patch Info (02 A1 07) */
    static const uint8_t patch_cmd[3] = {0x02, 0xa1, 0x07};
    err = ll_nfc_send_allowed(hal, patch_cmd, sizeof(patch_cmd), rx, sizeof(rx), &rx_len);
    bool patch_ok = (err == 0) && lc_patch_parse(rx, rx_len, parsed);
    LcExchange* ex1 = &capture->transcript[capture->transcript_length++];
    memcpy(ex1->command, patch_cmd, 3U);
    ex1->command_length = 3U;
    if(rx_len <= LC_RESPONSE_MAX) {
        memcpy(ex1->response, rx, rx_len);
        ex1->response_length = rx_len;
    }
    ex1->error = patch_ok ? 0U : (unsigned)(err != 0 ? err : 2);
    if(!patch_ok) {
        capture->error = ex1->error;
        return LlReaderPatchError;
    }

    memcpy(capture->patch, parsed, 6U);
    capture->patch_valid = true;
    if(!lc_family_allowed(capture->uid, capture->patch, 6U)) {
        return LlReaderUnsupportedFamily;
    }

    /* Phases 2..16: 15 Read-Multiple-Blocks commands (blocks 0..42, 344 bytes) */
    for(size_t i = 0; i < 15U; i++) {
        uint8_t rcmd[4];
        if(!lc_read_command(i, rcmd)) {
            capture->error = 3U;
            return LlReaderFramError;
        }
        size_t blocks = (size_t)rcmd[3] + 1U;
        err = ll_nfc_send_allowed(hal, rcmd, sizeof(rcmd), rx, sizeof(rx), &rx_len);
        bool read_ok = (err == 0) && lc_read_parse(rx, rx_len, blocks, parsed);

        LcExchange* ex = &capture->transcript[capture->transcript_length++];
        memcpy(ex->command, rcmd, 4U);
        ex->command_length = 4U;
        if(rx_len <= LC_RESPONSE_MAX) {
            memcpy(ex->response, rx, rx_len);
            ex->response_length = rx_len;
        }
        ex->error = read_ok ? 0U : (unsigned)(err != 0 ? err : 3);
        if(!read_ok) {
            capture->error = ex->error;
            return LlReaderFramError;
        }
        size_t bytes = blocks * 8U;
        memcpy(capture->fram + capture->fram_length, parsed, bytes);
        capture->fram_length += bytes;
    }

    if(!lc_capture_complete(capture)) {
        capture->error = 4U;
        return LlReaderFramError;
    }
    return LlReaderOk;
}
