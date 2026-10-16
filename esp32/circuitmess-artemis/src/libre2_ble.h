#ifndef LIBRE2_BLE_H
#define LIBRE2_BLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "protocol.h"
#include "integrity.h"
#include "vendor_chain.h"
#include "nfc_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Libre 2 Direct BLE GATT UUIDs (Abbott BLE Service 0xFDE3):
 *   Login / Unlock Write Characteristic: 0xF001 (12-byte streamingUnlockPayload)
 *   Composite Raw Data Notify Char:      0xF002 (46-byte encrypted 1-minute stream split across 20B + 18B + 8B notifications)
 */
#define LL_BLE_SERVICE_UUID       "0000fde3-0000-1000-8000-00805f9b34fb"
#define LL_BLE_CHAR_LOGIN_UUID    "0000f001-0000-1000-8000-00805f9b34fb"
#define LL_BLE_CHAR_DATA_UUID     "0000f002-0000-1000-8000-00805f9b34fb"

#define LL_BLE_RAW_PACKET_LEN     46U
#define LL_BLE_DEC_PACKET_LEN     44U
#define LL_BLE_LOGIN_PAYLOAD_LEN  12U
#define LL_NFC_ENABLE_CMD_LEN     12U

typedef struct {
    bool valid;
    uint16_t wear_time_minutes;
    /* 7 sparse trend samples at offsets [0, 2, 4, 6, 7, 12, 15] min before wear_time_minutes */
    uint16_t trend_age[7];
    uint16_t trend_raw_glucose[7];
    uint16_t trend_raw_temp[7];
    int16_t  trend_temp_adj[7];
    bool     trend_has_error[7];
    /* 3 historic 15-min samples */
    uint16_t hist_age[3];
    uint16_t hist_raw_glucose[3];
    uint16_t hist_raw_temp[3];
    int16_t  hist_temp_adj[3];
    bool     hist_has_error[3];
    /* Calibrated newest reading (trend[0]) using sensor's FRAM LcVendorConfig */
    LcVendorChainResult calibrated;
} LlBleStreamFrame;

/* Computes the 4-byte Libre 2 `usefulFunction(uid, x, y)` output */
void ll_libre2_useful_function(
    const uint8_t uid[8],
    uint16_t x,
    uint16_t y,
    uint8_t out4[4]);

/* Builds the 12-byte ISO15693 `02 A1 07 1E <4B unlockCode> <4B usefulFunction>` command
 * to enable BLE streaming on an EU Libre 2 sensor and return its 6-byte BLE MAC. */
void ll_libre2_build_enable_streaming_cmd(
    const uint8_t uid[8],
    const uint8_t patch[6],
    uint32_t unlock_code,
    uint8_t cmd_out[LL_NFC_ENABLE_CMD_LEN]);

/* Sends the `02 A1 07 1E ...` command over an explicit NFC pairing session
 * (separate from the read-only `ll_nfc_acquire_sensor` allowlist) and extracts
 * the sensor's 6-byte BLE MAC address (big-endian `AA:BB:CC:DD:EE:FF`). */
bool ll_libre2_nfc_enable_streaming(
    const LlNfcHal* hal,
    const uint8_t uid[8],
    const uint8_t patch[6],
    uint32_t unlock_code,
    uint8_t mac_be_out[6]);

/* Computes the 12-byte BLE GATT login payload written to characteristic 0xF001
 * upon connecting to the sensor over BLE. */
void ll_libre2_build_ble_login_payload(
    const uint8_t uid[8],
    const uint8_t patch[6],
    uint32_t enable_time,
    uint16_t unlock_count,
    uint8_t payload_out[LL_BLE_LOGIN_PAYLOAD_LEN]);

/* Decrypts a 46-byte encrypted BLE notification packet from characteristic 0xF002
 * into 44 plaintext bytes and verifies the trailing CRC16 (`decrypted[42..43]`). */
bool ll_libre2_decrypt_ble_packet(
    const uint8_t uid[8],
    const uint8_t enc46[LL_BLE_RAW_PACKET_LEN],
    uint8_t dec44_out[LL_BLE_DEC_PACKET_LEN]);

/* Parses a 44-byte decrypted BLE packet and runs the exact EU2 vendor calibration
 * chain (`lc_vendor_correct_record` + `lc_vendor_round`) using the sensor's FRAM
 * `LcVendorConfig` on the newest 1-minute sample (`trend[0]`). */
bool ll_libre2_evaluate_ble_packet(
    const uint8_t uid[8],
    const LcVendorConfig* cal_cfg,
    const uint8_t enc46[LL_BLE_RAW_PACKET_LEN],
    LlBleStreamFrame* out_frame);

#ifdef __cplusplus
}
#endif

#endif
