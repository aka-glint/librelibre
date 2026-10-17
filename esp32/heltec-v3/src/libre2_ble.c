#include "libre2_ble.h"
#include "factory.h"
#include "vendor_tables.h"

#include <math.h>
#include <string.h>

static uint16_t u16_le(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint16_t crypto_op(uint16_t value) {
    uint16_t res = (uint16_t)(value >> 2);
    if((value & 1U) != 0U) res ^= 0x6860U;
    if((value & 2U) != 0U) res ^= 0xA0C5U;
    return res;
}

static void process_crypto(const uint16_t in[4], uint16_t out[4]) {
    uint16_t r0 = crypto_op(in[0]) ^ in[3];
    uint16_t r1 = crypto_op(r0) ^ in[2];
    uint16_t r2 = crypto_op(r1) ^ in[1];
    uint16_t r3 = crypto_op(r2) ^ in[0];
    uint16_t r4 = crypto_op(r3);
    uint16_t r5 = crypto_op((uint16_t)(r4 ^ r0));
    uint16_t r6 = crypto_op((uint16_t)(r5 ^ r1));
    uint16_t r7 = crypto_op((uint16_t)(r6 ^ r2));
    out[0] = (uint16_t)(r3 ^ r7);
    out[1] = (uint16_t)(r2 ^ r6);
    out[2] = (uint16_t)(r1 ^ r5);
    out[3] = (uint16_t)(r0 ^ r4);
}

static void prepare_variables(const uint8_t uid[8], uint16_t x, uint16_t y, uint16_t s[4]) {
    s[0] = (uint16_t)(u16_le(&uid[4]) + x + y);
    s[1] = (uint16_t)(u16_le(&uid[2]) + 0x0000U);
    s[2] = (uint16_t)(u16_le(&uid[0]) + (uint16_t)(x * 2U));
    s[3] = (uint16_t)(0x241AU ^ 0x14C6U);
}

static void prepare_variables2(
    const uint8_t uid[8],
    uint16_t i1,
    uint16_t i2,
    uint16_t i3,
    uint16_t i4,
    uint16_t s[4]) {
    s[0] = (uint16_t)(u16_le(&uid[4]) + i1);
    s[1] = (uint16_t)(u16_le(&uid[2]) + i2);
    s[2] = (uint16_t)(u16_le(&uid[0]) + i3 + 0x0000U);
    s[3] = (uint16_t)(i4 + 0x14C6U);
}

void ll_libre2_useful_function(
    const uint8_t uid[8],
    uint16_t x,
    uint16_t y,
    uint8_t out4[4]) {
    uint16_t in[4], blk[4];
    prepare_variables(uid, x, y, in);
    process_crypto(in, blk);
    uint16_t r1 = (uint16_t)(blk[0] ^ 0x4163U);
    uint16_t r2 = (uint16_t)(blk[1] ^ 0x4344U);
    out4[0] = (uint8_t)(r1 & 0xFFU);
    out4[1] = (uint8_t)((r1 >> 8) & 0xFFU);
    out4[2] = (uint8_t)(r2 & 0xFFU);
    out4[3] = (uint8_t)((r2 >> 8) & 0xFFU);
}

void ll_libre2_build_enable_streaming_cmd(
    const uint8_t uid[8],
    const uint8_t patch[6],
    uint32_t unlock_code,
    uint8_t cmd_out[LL_NFC_ENABLE_CMD_LEN]) {
    uint8_t params[4] = {
        (uint8_t)(unlock_code & 0xFFU),
        (uint8_t)((unlock_code >> 8) & 0xFFU),
        (uint8_t)((unlock_code >> 16) & 0xFFU),
        (uint8_t)((unlock_code >> 24) & 0xFFU),
    };
    uint16_t secret = (uint16_t)(u16_le(&patch[4]) ^ u16_le(&params[0]));
    uint8_t uf[4];
    ll_libre2_useful_function(uid, 0x001EU, secret, uf);

    cmd_out[0] = 0x02U; /* High data rate */
    cmd_out[1] = 0xA1U; /* Abbott custom command */
    cmd_out[2] = 0x07U; /* TI manufacturer code */
    cmd_out[3] = 0x1EU; /* Subcommand: enableStreaming */
    memcpy(&cmd_out[4], params, 4U);
    memcpy(&cmd_out[8], uf, 4U);
}

bool ll_libre2_nfc_enable_streaming(
    const LlNfcHal* hal,
    const uint8_t uid[8],
    const uint8_t patch[6],
    uint32_t unlock_code,
    uint8_t mac_be_out[6]) {
    if(!hal || !hal->transceive || !uid || !patch || !mac_be_out) return false;
    uint8_t cmd[LL_NFC_ENABLE_CMD_LEN];
    ll_libre2_build_enable_streaming_cmd(uid, patch, unlock_code, cmd);

    uint8_t rx[16];
    size_t rx_len = 0U;
    int rc = hal->transceive(hal->ctx, cmd, sizeof(cmd), rx, sizeof(rx), &rx_len);
    if(rc != 0 || rx_len != 7U || (rx[0] & 0x01U) != 0U) return false;

    /* Sensor returns 6-byte MAC reversed in rx[1..6]; reverse to big-endian AA:BB:CC:DD:EE:FF */
    for(size_t i = 0; i < 6U; i++) {
        mac_be_out[i] = rx[6U - i];
    }
    return true;
}

void ll_libre2_build_ble_login_payload(
    const uint8_t uid[8],
    const uint8_t patch[6],
    uint32_t enable_time,
    uint16_t unlock_count,
    uint8_t payload_out[LL_BLE_LOGIN_PAYLOAD_LEN]) {
    uint32_t time_val = enable_time + (uint32_t)unlock_count;
    uint8_t b[4] = {
        (uint8_t)(time_val & 0xFFU),
        (uint8_t)((time_val >> 8) & 0xFFU),
        (uint8_t)((time_val >> 16) & 0xFFU),
        (uint8_t)((time_val >> 24) & 0xFFU),
    };

    uint8_t ad[4], ed[4];
    ll_libre2_useful_function(uid, 0x001BU, 0x1B6AU, ad);
    uint16_t ed_y = (uint16_t)((enable_time & 0xFFFFU) ^ u16_le(&patch[4]));
    ll_libre2_useful_function(uid, 0x001EU, ed_y, ed);

    uint16_t t11 = (uint16_t)(u16_le(&ed[0]) ^ u16_le(&b[2]));
    uint16_t t12 = u16_le(&ad[0]);
    uint16_t t13 = (uint16_t)(u16_le(&ed[2]) ^ u16_le(&b[0]));
    uint16_t t14 = u16_le(&ad[2]);

    uint16_t in2[4], t2[4];
    prepare_variables2(uid, t11, t12, t13, t14, in2);
    process_crypto(in2, t2);

    const uint8_t buf31[10] = {
        0xC1U, 0xC4U, 0xC3U, 0xC0U, 0xD4U, 0xE1U, 0xE7U, 0xBAU,
        (uint8_t)(t2[0] & 0xFFU), (uint8_t)((t2[0] >> 8) & 0xFFU)
    };
    const uint8_t buf32[6] = {
        (uint8_t)(t2[1] & 0xFFU), (uint8_t)((t2[1] >> 8) & 0xFFU),
        (uint8_t)(t2[2] & 0xFFU), (uint8_t)((t2[2] >> 8) & 0xFFU),
        (uint8_t)(t2[3] & 0xFFU), (uint8_t)((t2[3] >> 8) & 0xFFU)
    };
    const uint8_t buf33[6] = { ad[0], ad[1], ad[2], ad[3], ed[0], ed[1] };
    const uint8_t buf34[6] = { ed[2], ed[3], b[0], b[1], b[2], b[3] };

    uint16_t t31 = lc_integrity_crc(buf31, sizeof(buf31));
    uint16_t t32 = lc_integrity_crc(buf32, sizeof(buf32));
    uint16_t t33 = lc_integrity_crc(buf33, sizeof(buf33));
    uint16_t t34 = lc_integrity_crc(buf34, sizeof(buf34));

    uint16_t in4[4], t4[4];
    prepare_variables2(uid, t31, t32, t33, t34, in4);
    process_crypto(in4, t4);

    memcpy(&payload_out[0], b, 4U);
    for(size_t i = 0; i < 4U; i++) {
        payload_out[4U + i * 2U]     = (uint8_t)(t4[i] & 0xFFU);
        payload_out[4U + i * 2U + 1] = (uint8_t)((t4[i] >> 8) & 0xFFU);
    }
}

bool ll_libre2_decrypt_ble_packet(
    const uint8_t uid[8],
    const uint8_t enc46[LL_BLE_RAW_PACKET_LEN],
    uint8_t dec44_out[LL_BLE_DEC_PACKET_LEN]) {
    if(!uid || !enc46 || !dec44_out) return false;

    uint8_t d[4];
    ll_libre2_useful_function(uid, 0x001BU, 0x1B6AU, d);
    uint16_t x = (uint16_t)((u16_le(&d[0]) ^ u16_le(&d[2])) | 0x0063U);
    uint16_t y = (uint16_t)(u16_le(&enc46[0]) ^ 0x0063U);

    uint16_t state[4], next[4];
    prepare_variables(uid, x, y, state);
    process_crypto(state, state);

    uint8_t keystream[64];
    for(size_t blk = 0; blk < 8U; blk++) {
        for(size_t w = 0; w < 4U; w++) {
            keystream[blk * 8U + w * 2U]     = (uint8_t)(state[w] & 0xFFU);
            keystream[blk * 8U + w * 2U + 1] = (uint8_t)((state[w] >> 8) & 0xFFU);
        }
        process_crypto(state, next);
        memcpy(state, next, sizeof(state));
    }

    for(size_t i = 0; i < LL_BLE_DEC_PACKET_LEN; i++) {
        dec44_out[i] = (uint8_t)(enc46[2U + i] ^ keystream[i]);
    }

    uint16_t expected_crc = u16_le(&dec44_out[42]);
    uint16_t computed_crc = lc_integrity_crc(dec44_out, 42U);
    return expected_crc == computed_crc;
}

static uint32_t read_bits_le(const uint8_t* buf, size_t byte_off, size_t bit_off, size_t bit_count) {
    uint32_t res = 0U;
    for(size_t i = 0; i < bit_count; i++) {
        size_t total_bit = byte_off * 8U + bit_off + i;
        size_t b = total_bit / 8U;
        size_t bit = total_bit % 8U;
        if(((buf[b] >> bit) & 1U) != 0U) {
            res |= (1UL << i);
        }
    }
    return res;
}

bool ll_libre2_evaluate_ble_packet(
    const uint8_t uid[8],
    const LcVendorConfig* cal_cfg,
    const uint8_t enc46[LL_BLE_RAW_PACKET_LEN],
    LlBleStreamFrame* out_frame) {
    if(!uid || !enc46 || !out_frame) return false;
    memset(out_frame, 0, sizeof(*out_frame));

    uint8_t dec[LL_BLE_DEC_PACKET_LEN];
    if(!ll_libre2_decrypt_ble_packet(uid, enc46, dec)) return false;

    uint16_t wear_min = u16_le(&dec[40]);
    out_frame->valid = true;
    out_frame->wear_time_minutes = wear_min;

    static const uint16_t kTrendOffsets[7] = { 0U, 2U, 4U, 6U, 7U, 12U, 15U };
    for(size_t i = 0; i < 10U; i++) {
        uint16_t raw_val = (uint16_t)read_bits_le(dec, i * 4U, 0U, 14U);
        uint16_t raw_temp = (uint16_t)(read_bits_le(dec, i * 4U, 14U, 12U) << 2U);
        int16_t temp_adj = (int16_t)(read_bits_le(dec, i * 4U, 26U, 5U) << 2U);
        if(read_bits_le(dec, i * 4U, 31U, 1U) != 0U) {
            temp_adj = (int16_t)(-temp_adj);
        }
        bool has_err = (raw_val == 0U);
        if(i < 7U) {
            out_frame->trend_age[i] = (wear_min > kTrendOffsets[i]) ? (uint16_t)(wear_min - kTrendOffsets[i]) : 0U;
            out_frame->trend_raw_glucose[i] = raw_val;
            out_frame->trend_raw_temp[i] = has_err ? 0U : raw_temp;
            out_frame->trend_temp_adj[i] = temp_adj;
            out_frame->trend_has_error[i] = has_err;
        } else {
            size_t h = i - 7U;
            uint16_t base_hist = (wear_min > 2U) ? (uint16_t)(((wear_min - 2U) / 15U) * 15U) : 0U;
            uint16_t h_off = (uint16_t)(15U * h);
            out_frame->hist_age[h] = (base_hist > h_off) ? (uint16_t)(base_hist - h_off) : 0U;
            out_frame->hist_raw_glucose[h] = raw_val;
            out_frame->hist_raw_temp[h] = has_err ? 0U : raw_temp;
            out_frame->hist_temp_adj[h] = temp_adj;
            out_frame->hist_has_error[h] = has_err;
        }
    }

    if(cal_cfg && cal_cfg->i2 >= 1 && cal_cfg->i2 <= 1023 && !out_frame->trend_has_error[0]) {
        uint32_t mg = 0U, mmol = 0U;
        if(lc_vendor_calibrate_ble_packet(
               cal_cfg,
               wear_min,
               out_frame->trend_raw_glucose,
               out_frame->trend_raw_temp,
               out_frame->trend_temp_adj,
               out_frame->trend_has_error,
               out_frame->hist_raw_glucose,
               out_frame->hist_raw_temp,
               out_frame->hist_temp_adj,
               out_frame->hist_has_error,
               &mg,
               &mmol) == LcVendorChainOk && mmol > 0U) {
            out_frame->calibrated.available = true;
            out_frame->calibrated.mg_dl = mg;
            out_frame->calibrated.mmol_tenths = mmol;
            out_frame->calibrated.cfg_i2 = cal_cfg->i2;
            out_frame->calibrated.cfg_i3 = cal_cfg->i3;
            out_frame->calibrated.cfg_i4 = cal_cfg->i4;
            out_frame->calibrated.cfg_i6 = cal_cfg->i6;
        }
    }

    return true;
}
