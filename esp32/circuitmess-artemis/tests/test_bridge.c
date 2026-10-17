#include "nfc_io.h"
#include "libre2_ble.h"
#include "factory.h"
#include "vendor_tables.h"
#include "scan_history.h"
#include "bridge.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void seal_fram(uint8_t data[344]) {
    const unsigned ranges[3][2] = {{0, 24}, {24, 320}, {320, 344}};
    for(unsigned i = 0; i < 3; i++) {
        uint16_t crc = lc_integrity_crc(data + ranges[i][0] + 2, ranges[i][1] - ranges[i][0] - 2);
        data[ranges[i][0]] = (uint8_t)(crc & 0xFF);
        data[ranges[i][0] + 1] = (uint8_t)(crc >> 8);
    }
}

static void build_synthetic_eu2(
    uint8_t uid[8],
    uint8_t patch[6],
    uint8_t fram_enc[344],
    uint16_t age,
    uint16_t life,
    uint8_t status,
    uint16_t adc,
    uint16_t tcode,
    int16_t adj,
    uint16_t quality,
    bool has_error) {
    const uint8_t u[8] = {0x5e, 0x90, 0x25, 0x00, 0x00, 0xa0, 0x07, 0xe0};
    const uint8_t p[6] = {0xc5, 0x09, 0x30, 0x01, 0x00, 0x00};
    memcpy(uid, u, 8);
    memcpy(patch, p, 6);
    uint8_t plain[344] = {0};
    uint16_t hdr = (uint16_t)((866U << 3) | 0U);
    plain[2] = (uint8_t)(hdr & 0xFF);
    plain[3] = (uint8_t)(hdr >> 8);
    plain[4] = status;
    plain[26] = 1;
    plain[27] = 1;
    plain[316] = (uint8_t)(age & 0xFF);
    plain[317] = (uint8_t)(age >> 8);
    plain[322] = patch[2];
    plain[323] = patch[3];
    plain[326] = (uint8_t)(life & 0xFF);
    plain[327] = (uint8_t)(life >> 8);
    uint64_t footer = 0x6c28d300001a7312ULL;
    for(unsigned i = 0; i < 8; i++) plain[336 + i] = (uint8_t)(footer >> (8 * i));
    uint64_t word = (uint64_t)(adc & 0x3FFFU) |
                    ((uint64_t)(quality & 0x1FFU) << 14) |
                    ((uint64_t)(has_error || quality ? 1U : 0U) << 25) |
                    ((uint64_t)((tcode >> 2) & 0xFFFU) << 26) |
                    ((uint64_t)(((adj < 0 ? -adj : adj) >> 2) & 0x1FFU) << 38) |
                    ((uint64_t)(adj < 0 ? 1U : 0U) << 47);
    for(unsigned i = 0; i < 6; i++) plain[28 + i] = (uint8_t)(word >> (8 * i));
    seal_fram(plain);
    lc_integrity_transform(uid, patch, plain, fram_enc);
}


typedef struct {
    uint8_t uid[8];
    uint8_t patch[6];
    uint8_t fram[344];
    int fail_step;
    unsigned calls;
} MockTag;

static int mock_transceive(void* ctx, const uint8_t* tx, size_t tx_len, uint8_t* rx, size_t rx_cap, size_t* rx_len) {
    MockTag* tag = (MockTag*)ctx;
    assert(lc_command_allowed(tx, tx_len));
    assert(rx_cap >= 25);
    int step = (int)tag->calls++;
    if(step == tag->fail_step) return 7; /* simulated RF timeout */
    if(tx_len == 3 && tx[0] == 0x26 && tx[1] == 0x01 && tx[2] == 0x00) {
        rx[0] = 0x00; rx[1] = 0x00;
        memcpy(rx + 2, tag->uid, 8);
        *rx_len = 10;
        return 0;
    }
    if(tx_len == 3 && tx[0] == 0x02 && tx[1] == 0xa1 && tx[2] == 0x07) {
        rx[0] = 0x00;
        memcpy(rx + 1, tag->patch, 6);
        *rx_len = 7;
        return 0;
    }
    if(tx_len == 4 && tx[0] == 0x02 && tx[1] == 0x23) {
        size_t start = tx[2];
        size_t blocks = (size_t)tx[3] + 1U;
        size_t bytes = blocks * 8U;
        assert(start * 8U + bytes <= 344U);
        rx[0] = 0x00;
        memcpy(rx + 1, tag->fram + start * 8U, bytes);
        *rx_len = 1U + bytes;
        return 0;
    }
    return 99;
}

int main(void) {
    uint8_t uid[8], patch[6], fram[344];
    build_synthetic_eu2(uid, patch, fram, 2466, 20854, 3, 1500, 7008, 8, 0, false);

    char frame[LL_BRIDGE_LINE_MAX];
    size_t flen = ll_bridge_format_frame(uid, patch, fram, frame, sizeof(frame));
    assert(flen == 728U);
    assert(strncmp(frame, "LL2|5E90250000A007E0|C50930010000|", 34) == 0);
    puts("PASS bridge frame formatting (728 bytes with CRC16)");

    uint8_t rx_uid[8], rx_patch[6], rx_fram[344];
    assert(ll_bridge_parse_line(frame, rx_uid, rx_patch, rx_fram) == LlBridgeOk);
    assert(memcmp(uid, rx_uid, 8) == 0);
    assert(memcmp(patch, rx_patch, 6) == 0);
    assert(memcmp(fram, rx_fram, 344) == 0);
    puts("PASS bridge frame roundtrip parse + CRC16 verification");

    /* Corrupt 1 nibble in FRAM -> must fail with LlBridgeBadCrc */
    char corrupt[LL_BRIDGE_LINE_MAX];
    memcpy(corrupt, frame, sizeof(frame));
    corrupt[100] = (corrupt[100] == '0') ? '1' : '0';
    assert(ll_bridge_parse_line(corrupt, rx_uid, rx_patch, rx_fram) == LlBridgeBadCrc);
    assert(ll_bridge_parse_line("HELLO|WORLD", rx_uid, rx_patch, rx_fram) == LlBridgeBadPrefix);
    assert(ll_bridge_parse_line("LL2|SHORT", rx_uid, rx_patch, rx_fram) == LlBridgeBadFormat);
    puts("PASS corrupted / truncated / wrong-prefix bridge lines fail closed");

    /* Evaluate clean active scan */
    LlCompanionState st;
    ll_companion_evaluate(rx_uid, rx_patch, rx_fram, &st);
    assert(st.has_scan && st.vendor_chain.available);
    assert(strcmp(st.serial, "3M00009DHCR") == 0);
    assert(strcmp(st.serial_line, "Serial 3M00009DHCR") == 0);
    assert(strcmp(st.fresh_life_line, "Fresh 0m=now; Left 12d 18h") == 0);
    assert(strstr(st.primary_line, "mmol/L") != NULL);
    printf("PASS active scan evaluation: %s | %s | %s\n",
           st.primary_line, st.fresh_life_line, st.serial_line);

    /* Evaluate error-flagged scan -> must suppress number and show Rescan */
    build_synthetic_eu2(uid, patch, fram, 2466, 20854, 3, 1500, 7008, 8, 0x020, true);
    ll_companion_evaluate(uid, patch, fram, &st);
    assert(st.has_scan && !st.vendor_chain.available);
    assert(strcmp(st.primary_line, "Rescan the sensor") == 0);
    assert(strcmp(st.fresh_life_line, "No usable value") == 0);
    assert(strstr(st.diag_line3, "Q020") != NULL);
    puts("PASS flagged/error scan suppresses value and shows Rescan + diagnostics");

    /* Evaluate expired/shutdown scan -> must suppress number and show Rescan */
    build_synthetic_eu2(uid, patch, fram, 21348, 20628, 5, 1500, 7008, 8, 0x100, true);
    ll_companion_evaluate(uid, patch, fram, &st);
    assert(st.has_scan && !st.vendor_chain.available);
    assert(strcmp(st.primary_line, "Rescan the sensor") == 0);
    assert(strstr(st.diag_line1, "shutdown") != NULL);
    assert(strstr(st.diag_line2, "Life exceeded") != NULL);
    puts("PASS shutdown/expired scan suppresses value and reports shutdown/Life exceeded");


    /* Test ESP32 master 17-command ISO15693 acquisition state machine (nfc_io.c) */
    {
        MockTag tag;
        build_synthetic_eu2(tag.uid, tag.patch, tag.fram, 2466, 20854, 3, 1500, 7008, 8, 0, false);
        tag.fail_step = -1;
        tag.calls = 0;
        LlNfcHal hal = { .transceive = mock_transceive, .ctx = &tag };
        LcCapture cap;
        assert(ll_nfc_acquire_sensor(&hal, 1792108800U, &cap) == LlReaderOk);
        assert(tag.calls == 17U);
        assert(lc_capture_complete(&cap));
        LlCompanionState st2;
        ll_companion_evaluate(cap.uid, cap.patch, cap.fram, &st2);
        assert(st2.vendor_chain.available && strcmp(st2.primary_line, "6.8 mmol/L") == 0);
        puts("PASS ESP32 master 17-command ISO15693 state machine (inventory -> patch -> 15 reads -> 6.8 mmol/L)");

        /* Forbidden write command rejected at sole RF send boundary */
        const uint8_t bad_write[4] = {0x02, 0x21, 0x00, 0xaa};
        uint8_t dummy_rx[32]; size_t dummy_len = 0;
        assert(ll_nfc_send_allowed(&hal, bad_write, 4, dummy_rx, sizeof(dummy_rx), &dummy_len) == -2);
        puts("PASS ESP32 RF send boundary rejects forbidden write/custom commands");

        /* Mid-read RF drop on frame 5 fails closed */
        tag.fail_step = 5;
        tag.calls = 0;
        assert(ll_nfc_acquire_sensor(&hal, 1792108800U, &cap) == LlReaderFramError);
        assert(!lc_capture_complete(&cap));
        puts("PASS mid-read RF drop fails closed without complete capture");

        /* Libre 2 Direct BLE Streaming: 46-byte BLE packet decryption, CRC16,
         * NFC `02 A1 07 1E` enableStreaming command, and 12-byte 0xF001 login payload */
        static const uint8_t ble_uid[8] = { 0x2FU, 0xE7U, 0xB1U, 0x00U, 0x00U, 0xA4U, 0x07U, 0xE0U };
        static const uint8_t ble_enc46[46] = {
            0xB1U, 0x94U, 0xFAU, 0xEDU, 0x2CU, 0xDEU, 0xA1U, 0x69U,
            0x46U, 0x57U, 0xCFU, 0xD0U, 0xD8U, 0x5AU, 0xAAU, 0xF1U,
            0xE2U, 0x89U, 0x1CU, 0xE9U, 0xACU, 0x82U, 0x16U, 0xFBU,
            0x67U, 0xA1U, 0xD3U, 0xB6U, 0x3FU, 0x91U, 0xCDU, 0x18U,
            0x4BU, 0x95U, 0x31U, 0x6CU, 0x04U, 0x5FU, 0xE1U, 0x96U,
            0xC4U, 0xFDU, 0x14U, 0xFCU, 0x68U, 0xE0U
        };
        uint8_t ble_dec44[44];
        assert(ll_libre2_decrypt_ble_packet(ble_uid, ble_enc46, ble_dec44));

        /* Corrupt 1 bit of encrypted BLE packet -> must fail CRC16 check */
        uint8_t ble_corrupt[46];
        memcpy(ble_corrupt, ble_enc46, 46U);
        ble_corrupt[10] ^= 0x01U;
        assert(!ll_libre2_decrypt_ble_packet(ble_uid, ble_corrupt, ble_dec44));

        /* Evaluate full 46-byte BLE packet with active sensor calibration params */
        LlBleStreamFrame bframe;
        uint8_t plain_fram[344];
        lc_integrity_transform(cap.uid, cap.patch, cap.fram, plain_fram);
        LcVendorConfig vcfg;
        assert(lc_vendor_config_from_fram(plain_fram, LC_FACTORY_DIABLE_T2, LC_FACTORY_DIABLE_T1, LC_VENDOR_SLOPE_TABLE, &vcfg));
        assert(ll_libre2_evaluate_ble_packet(ble_uid, &vcfg, ble_enc46, &bframe));
        assert(bframe.valid);
        assert(bframe.wear_time_minutes > 0U);
        assert(!bframe.trend_has_error[0]);
        assert(bframe.calibrated.available);
        assert(bframe.calibrated.mmol_tenths > 0U);

        /* Build NFC `02 A1 07 1E` enableStreaming command and 12-byte 0xF001 BLE login payload */
        uint8_t nfc_enable_cmd[12];
        ll_libre2_build_enable_streaming_cmd(cap.uid, cap.patch, 42U, nfc_enable_cmd);
        assert(nfc_enable_cmd[0] == 0x02U && nfc_enable_cmd[1] == 0xA1U && nfc_enable_cmd[2] == 0x07U && nfc_enable_cmd[3] == 0x1EU);
        assert(nfc_enable_cmd[4] == 42U && nfc_enable_cmd[5] == 0U && nfc_enable_cmd[6] == 0U && nfc_enable_cmd[7] == 0U);

        uint8_t login12[12];
        ll_libre2_build_ble_login_payload(cap.uid, cap.patch, 42U, 0U, login12);
        assert(login12[0] == 42U && login12[1] == 0U && login12[2] == 0U && login12[3] == 0U);
        puts("PASS Libre 2 direct BLE 46-byte packet decryption, CRC16, 0x1E NFC command, and 12-byte 0xF001 login payload");

    }
    return 0;
}
