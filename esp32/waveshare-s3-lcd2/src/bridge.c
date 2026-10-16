#include "bridge.h"
#include <stdio.h>
#include <string.h>

static int hex_nibble(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool unhex_exact(const char* s, size_t hex_len, uint8_t* out) {
    if(!s || !out || (hex_len % 2U) != 0U) return false;
    for(size_t i = 0; i < hex_len; i += 2U) {
        int hi = hex_nibble(s[i]);
        int lo = hex_nibble(s[i + 1U]);
        if(hi < 0 || lo < 0) return false;
        out[i / 2U] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static uint16_t bridge_payload_crc(
    const uint8_t uid[8],
    const uint8_t patch[6],
    const uint8_t fram[LC_FRAM_SIZE]) {
    uint8_t buf[8U + 6U + LC_FRAM_SIZE];
    memcpy(buf, uid, 8U);
    memcpy(buf + 8U, patch, 6U);
    memcpy(buf + 14U, fram, LC_FRAM_SIZE);
    return lc_integrity_crc(buf, sizeof(buf));
}

size_t ll_bridge_format_frame(
    const uint8_t uid[8],
    const uint8_t patch[6],
    const uint8_t fram[LC_FRAM_SIZE],
    char* out,
    size_t capacity) {
    if(!uid || !patch || !fram || !out || capacity < 727U) {
        if(out && capacity) out[0] = 0;
        return 0;
    }
    char uid_hex[17], patch_hex[13], fram_hex[689];
    if(!lc_hex(uid, 8U, uid_hex, sizeof(uid_hex)) ||
       !lc_hex(patch, 6U, patch_hex, sizeof(patch_hex)) ||
       !lc_hex(fram, LC_FRAM_SIZE, fram_hex, sizeof(fram_hex))) {
        out[0] = 0;
        return 0;
    }
    uint16_t crc = bridge_payload_crc(uid, patch, fram);
    int n = snprintf(out, capacity, "LL2|%s|%s|%s|%04X\n", uid_hex, patch_hex, fram_hex, (unsigned)crc);
    if(n < 0 || (size_t)n >= capacity) {
        out[0] = 0;
        return 0;
    }
    return (size_t)n;
}

LlBridgeParseStatus ll_bridge_parse_line(
    const char* line,
    uint8_t uid_out[8],
    uint8_t patch_out[6],
    uint8_t fram_out[LC_FRAM_SIZE]) {
    if(!line || !uid_out || !patch_out || !fram_out) return LlBridgeBadFormat;
    /* Strip leading whitespace and trailing CR/LF */
    while(*line == ' ' || *line == '\t') line++;
    size_t len = strlen(line);
    while(len > 0 && (line[len - 1U] == '\r' || line[len - 1U] == '\n' || line[len - 1U] == ' ')) {
        len--;
    }
    if(len < 4U || strncmp(line, "LL2|", 4U) != 0) return LlBridgeBadPrefix;
    /* Exact expected length: 4 ("LL2|") + 16 (uid) + 1 ("|") + 12 (patch) + 1 ("|") + 688 (fram) + 1 ("|") + 4 (crc) = 727 */
    if(len != 727U) return LlBridgeBadFormat;
    if(line[20] != '|' || line[33] != '|' || line[722] != '|') return LlBridgeBadFormat;

    uint8_t uid[8], patch[6], fram[LC_FRAM_SIZE], crc_be[2];
    if(!unhex_exact(line + 4U, 16U, uid) ||
       !unhex_exact(line + 21U, 12U, patch) ||
       !unhex_exact(line + 34U, 688U, fram) ||
       !unhex_exact(line + 723U, 4U, crc_be)) {
        return LlBridgeBadFormat;
    }
    uint16_t rx_crc = (uint16_t)((crc_be[0] << 8) | crc_be[1]);
    uint16_t calc_crc = bridge_payload_crc(uid, patch, fram);
    if(rx_crc != calc_crc) return LlBridgeBadCrc;

    memcpy(uid_out, uid, 8U);
    memcpy(patch_out, patch, 6U);
    memcpy(fram_out, fram, LC_FRAM_SIZE);
    return LlBridgeOk;
}

static void build_complete_capture(
    const uint8_t uid[8],
    const uint8_t patch[6],
    const uint8_t fram[LC_FRAM_SIZE],
    LcCapture* c) {
    memset(c, 0, sizeof(*c));
    memcpy(c->uid, uid, 8U);
    memcpy(c->patch, patch, 6U);
    memcpy(c->fram, fram, LC_FRAM_SIZE);
    c->uid_valid = true;
    c->patch_valid = true;
    c->fram_length = LC_FRAM_SIZE;
    c->transcript_length = LC_TRANSCRIPT_MAX;
    const uint8_t inv_cmd[3] = {0x26, 0x01, 0x00};
    memcpy(c->transcript[0].command, inv_cmd, 3U);
    c->transcript[0].command_length = 3U;
    c->transcript[0].response[0] = 0;
    c->transcript[0].response[1] = 0;
    memcpy(c->transcript[0].response + 2U, uid, 8U);
    c->transcript[0].response_length = 10U;

    const uint8_t pat_cmd[3] = {0x02, 0xa1, 0x07};
    memcpy(c->transcript[1].command, pat_cmd, 3U);
    c->transcript[1].command_length = 3U;
    c->transcript[1].response[0] = 0;
    memcpy(c->transcript[1].response + 1U, patch, 6U);
    c->transcript[1].response_length = 7U;

    for(size_t i = 0; i < 15U; i++) {
        LcExchange* e = &c->transcript[i + 2U];
        lc_read_command(i, e->command);
        e->command_length = 4U;
        size_t bytes = (e->command[3] + 1U) * 8U;
        e->response[0] = 0;
        memcpy(e->response + 1U, fram + i * 24U, bytes);
        e->response_length = 1U + bytes;
    }
}

void ll_companion_evaluate(
    const uint8_t uid[8],
    const uint8_t patch[6],
    const uint8_t fram[LC_FRAM_SIZE],
    LlCompanionState* state) {
    if(!state || !uid || !patch || !fram) return;
    memset(state, 0, sizeof(*state));
    state->has_scan = true;
    memcpy(state->uid, uid, 8U);
    memcpy(state->patch, patch, 6U);
    memcpy(state->fram, fram, LC_FRAM_SIZE);
    lc_hex(uid, 8U, state->uid_hex, sizeof(state->uid_hex));
    state->serial_valid = lc_serial_from_uid(uid, patch, state->serial, sizeof(state->serial));
    if(state->serial_valid) {
        snprintf(state->serial_line, sizeof(state->serial_line), "Serial %s", state->serial);
    } else {
        snprintf(state->serial_line, sizeof(state->serial_line), "Serial unavailable");
    }

    build_complete_capture(uid, patch, fram, &state->capture);
    lc_integrity_check(&state->capture, &state->integrity);
    lc_factory_check(&state->capture, &state->factory);

    const LcSensor* sensor = &state->integrity.sensor;
    bool parsed = state->integrity.status == LcIntegrityPass &&
                  sensor->parse_status == LcSensorParsed;
    lc_life_compute(sensor->age_minutes, sensor->max_life_minutes, 0U, parsed, &state->life);

    /* Evaluate newest-trend vendor chain with strict admission gates */
    if(parsed && sensor->status_code == 3U && sensor->max_life_minutes > 0U &&
       sensor->age_minutes < sensor->max_life_minutes &&
       !sensor->has_error && !sensor->quality_bits && !sensor->quality_flags) {
        uint8_t plain[LC_FRAM_SIZE];
        lc_integrity_transform(uid, patch, fram, plain);
        uint32_t mg = 0, mmol = 0;
        LcVendorConfig cfg;
        LcVendorTrendSample sample;
        bool ok = lc_vendor_current_trend(plain, &cfg, &sample, &mg, &mmol) == LcVendorChainOk;
        ok = ok && sample.sensor_minute == (int32_t)sensor->age_minutes;
        volatile uint8_t* wipe = plain;
        for(size_t i = 0; i < sizeof(plain); i++) wipe[i] = 0;
        if(ok) {
            state->vendor_chain.available = true;
            state->vendor_chain.mg_dl = mg;
            state->vendor_chain.mmol_tenths = mmol;
            state->vendor_chain.cfg_i2 = cfg.i2;
            state->vendor_chain.cfg_i3 = cfg.i3;
            state->vendor_chain.cfg_i4 = cfg.i4;
            state->vendor_chain.cfg_i6 = cfg.i6;
        }
    }

    if(state->vendor_chain.available) {
        snprintf(state->primary_line, sizeof(state->primary_line), "%lu.%lu mmol/L",
                 (unsigned long)(state->vendor_chain.mmol_tenths / 10U),
                 (unsigned long)(state->vendor_chain.mmol_tenths % 10U));
        char fresh[24], left[24];
        if(lc_freshness_format(&state->life, fresh, sizeof(fresh)) &&
           lc_life_format_remaining(&state->life, left, sizeof(left))) {
            snprintf(state->fresh_life_line, sizeof(state->fresh_life_line), "%s; %s", fresh, left);
        } else {
            snprintf(state->fresh_life_line, sizeof(state->fresh_life_line), "Fresh unknown");
        }
    } else {
        snprintf(state->primary_line, sizeof(state->primary_line), "Rescan the sensor");
        snprintf(state->fresh_life_line, sizeof(state->fresh_life_line), "No usable value");
    }

    static const char* const states[] = {
        "unknown", "not activated", "warming up", "active", "expired", "shutdown", "failure"};
    if(parsed) {
        if(sensor->status_code < 7U)
            snprintf(state->diag_line1, sizeof(state->diag_line1), "CRC OK; %s", states[sensor->status_code]);
        else
            snprintf(state->diag_line1, sizeof(state->diag_line1), "CRC OK; unknown(%u)", sensor->status_code);
        char left[24];
        if(!lc_life_format_remaining(&state->life, left, sizeof(left)))
            snprintf(left, sizeof(left), "Life unknown");
        const char* rec = sensor->has_error ? "record error" :
            (sensor->quality_bits || sensor->quality_flags) ? "quality" : "record clear";
        snprintf(state->diag_line2, sizeof(state->diag_line2), "%s; %s", left, rec);
        snprintf(state->diag_line3, sizeof(state->diag_line3), "Age%u/%um Q%03X/%u",
                 sensor->age_minutes, sensor->max_life_minutes,
                 sensor->quality_bits, sensor->quality_flags);
    } else {
        snprintf(state->diag_line1, sizeof(state->diag_line1), "%s",
                 state->integrity.status == LcIntegrityPass ? "CRC OK; parse rejected" :
                 state->integrity.status == LcIntegrityFail ? "CRC FAIL; data untrusted" :
                 state->integrity.status == LcIntegrityUnsupported ? "Decode unsupported" : "Integrity not checked");
        snprintf(state->diag_line2, sizeof(state->diag_line2), "%s",
                 sensor->parse_status == LcSensorInvalidIndex ? "Invalid ring index" :
                 sensor->parse_status == LcSensorProfileMismatch ? "Footer profile mismatch" : "Sensor status untrusted");
        snprintf(state->diag_line3, sizeof(state->diag_line3), "Rescan the sensor");
    }
}
