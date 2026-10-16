/* Experimental unsmoothed current-trend estimate and raw capture. No writes. */
#include <furi.h>
#include <furi_hal_rtc.h>
#include <gui/gui.h>
#include <input/input.h>
#include <nfc/nfc_poller.h>
#include <nfc/protocols/iso15693_3/iso15693_3_poller.h>
#include <storage/storage.h>
#include <notification/notification_messages.h>
#include <expansion/expansion.h>
#include <furi_hal_serial.h>
#include <furi_hal_serial_control.h>
#include <stdio.h>
#include <string.h>
#include "capture.h"
#include "integrity.h"
#include "factory.h"
#include "vendor_chain.h"
#include "lifecycle.h"
#include "transaction.h"

#define CAPTURE_DIR "/ext/apps_data/libre_research"
#define FRAME_WAIT_FC 270000U

/* Notification service owns the speaker. Respect user volume; never acquire HAL. */
static const NotificationSequence scan_complete_sequence = {
    &message_blue_255, &message_note_c5, &message_delay_100,
    &message_note_g5, &message_delay_100, &message_sound_off, NULL,
};
static const NotificationSequence scan_error_sequence = {
    &message_red_255, &message_note_c4, &message_delay_100, &message_sound_off, NULL,
};

typedef enum { PageIdle, PageScan, PageResult } Page;

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* keys;
    ViewPort* viewport;
    Nfc* nfc;
    NfcPoller* poller;
    NotificationApp* notification;
    BitBuffer* tx;
    BitBuffer* rx;
    Page page;
    LcCapture capture;
    LcIntegrity integrity;
    LcFactory factory;
    LcVendorChainResult vendor_chain;
    /* Sensor-clock remaining life + freshness for THIS capture's shown value.
     * Computed once in finish_scan from the same LcSensor the value came from,
     * so the value, its freshness and the remaining life cannot drift apart. */
    LcLife life;
    bool details;
    bool uid_details;
    LcTransactionGate gate;
    bool finished;
    bool started;
    unsigned phase;
    uint32_t start_tick;
    char status[32];
    char save_status[32];
    char saved_name[40];
} AppState;

static void draw(Canvas* canvas, void* context) {
    AppState* app = context;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    if(app->page == PageResult) {
        /* Acquisition's naive local HH:MM, NOT sample time or UTC. */
        char local[20], clock[6] = "--:--";
        if(lc_capture_local_datetime(app->capture.timestamp, local, sizeof local)) {
            memcpy(clock, local+11, 5);
            clock[5] = 0;
        }
        canvas_draw_str(canvas, 98, 10, clock);
    }
    canvas_set_font(canvas, FontSecondary);
    /* At-a-glance DERIVED sensor SERIAL (11 chars), from this scan's accepted
     * wire-order UID plus the patch-info family nibble (see SERIAL-IN-FRAM.md);
     * it is DERIVED, never read from FRAM. Requires this scan's accepted UID,
     * a valid patch and an uncancelled gate, otherwise it shows a neutral
     * placeholder. Never borrows bytes from a previous scan, so a new scan,
     * sensor change or cancel can never leave a stale serial. The full 16-hex
     * wire UID stays on the UID detail page. */
    char serial[24] = "Serial unavailable";
    if(app->page == PageResult) {
        char derived[13];
        if(app->capture.uid_valid && app->capture.patch_valid && !app->gate.cancelled &&
           lc_serial_from_uid(app->capture.uid, app->capture.patch, derived, sizeof(derived)))
            snprintf(serial, sizeof(serial), "Serial %s", derived);
    }
    if(app->page == PageIdle) {
        canvas_draw_str(canvas, 0, 23, "EU2 sensor reader");
        canvas_draw_str(canvas, 0, 34, "Read-only; SD logs the scan");
        canvas_draw_str(canvas, 0, 45, "EU2 only; SD stores sensor ID");
        canvas_draw_str(canvas, 0, 62, "OK: start     Back: exit");
    } else if(app->page == PageScan) {
        canvas_draw_str(canvas, 0, 23, "Hold one sensor at NFC back");
        char progress[32];
        snprintf(progress, sizeof(progress), "FRAM %u/344", (unsigned)app->capture.fram_length);
        canvas_draw_str(canvas, 0, 34, app->gate.cancelled ? "Stopping; SDK may stall" :
                        (app->phase >= 2 ? progress : app->phase ? "UID; query patch-info" : "Waiting for UID..."));
        canvas_draw_str(canvas, 0, 45, "Scanning until tag or Back");
        canvas_draw_str(canvas, 0, 54, "Stuck? Hold Left+Back 5s");
        canvas_draw_str(canvas, 0, 62, "Back: cancel (no auto-save)");
    } else if(app->uid_details) {
        char uid[17];
        canvas_draw_str(canvas, 0, 22, "UID wire LSB-first");
        if(app->capture.uid_valid && !app->gate.cancelled &&
           lc_hex(app->capture.uid, sizeof(app->capture.uid), uid, sizeof(uid)))
            canvas_draw_str(canvas, 0, 32, uid);
        else
            canvas_draw_str(canvas, 0, 32, "UID unavailable");
        canvas_draw_str(canvas, 0, 43, "Full 16-hex wire UID");
        canvas_draw_str(canvas, 0, 53, "Serial derived from UID");
        canvas_draw_str(canvas, 0, 63, "OK:save <:est >:scan");
    } else if(!app->details && app->integrity.status == LcIntegrityPass &&
              app->vendor_chain.available) {
        /* Result screen: corrected value only. The historical estimate is NOT
         * drawn here (it remains computed and is written to the SD capture), and
         * the dosing/experimental banners were removed at the owner's request.
         * The corrected value is the recovered closed chain, unsmoothed. */
        char value[32];
        canvas_set_font(canvas, FontPrimary);
        snprintf(value, sizeof(value), "%lu.%lu mmol/L",
                 (unsigned long)(app->vendor_chain.mmol_tenths/10U),
                 (unsigned long)(app->vendor_chain.mmol_tenths%10U));
        canvas_draw_str(canvas, 0, 22, value);
        canvas_set_font(canvas, FontSecondary);
        /* Sensor time and remaining life, both from the SAME capture's sensor
         * clock as the value above (app->life, set in finish_scan). */
        char fresh[32], left[24];
        if(lc_freshness_format(&app->life, fresh, sizeof(fresh)) &&
           lc_life_format_remaining(&app->life, left, sizeof(left))) {
            char pair[64];
            snprintf(pair, sizeof(pair), "%s; %s", fresh, left);
            canvas_draw_str(canvas, 0, 38, pair);
        } else {
            canvas_draw_str(canvas, 0, 38, "Fresh unknown");
        }
        canvas_draw_str(canvas, 0, 46, serial);
        canvas_draw_str(canvas, 0, 54, app->save_status);
        canvas_draw_str(canvas, 0, 63, "<:info >:scan");
    } else {
        char line[40];
        const LcSensor* sensor = &app->integrity.sensor;
        bool parsed = app->integrity.status == LcIntegrityPass &&
            sensor->parse_status == LcSensorParsed;
        if(!app->details) {
            /* Any unavailable current result: plain rescan prompt only.
             * No glucose value paired with an error record, and no raw quality
             * hex dump on this path. Pressing LEFT opens the full raw
             * diagnostics below, so the information is not lost. */
            static const char* const states[] = {
                "unknown", "not activated", "warming up", "active", "expired", "shutdown", "failure"};
            if(!parsed)
                snprintf(line, sizeof(line), "No usable value");
            else if(sensor->status_code < COUNT_OF(states))
                snprintf(line, sizeof(line), "CRC OK; %s", states[sensor->status_code]);
            else
                snprintf(line, sizeof(line), "CRC OK; unknown(%u)", sensor->status_code);
            canvas_draw_str(canvas, 0, 22, line);
            canvas_draw_str(canvas, 0, 30, "No usable value");
            canvas_draw_str(canvas, 0, 38, "Rescan the sensor");
        } else if(parsed) {
            static const char* const states[] = {
                "unknown", "not activated", "warming up", "active", "expired", "shutdown", "failure"};
            if(sensor->status_code < COUNT_OF(states))
                snprintf(line, sizeof(line), "CRC OK; %s", states[sensor->status_code]);
            else
                snprintf(line, sizeof(line), "CRC OK; unknown(%u)", sensor->status_code);
            canvas_draw_str(canvas, 0, 22, line);
            /* Remaining life from the SAME capture's sensor clock. Life unknown
             * (max_life 0) and Life exceeded (age > max_life) keep their exact
             * semantics; otherwise the compact Left Xd Yh / Left Xh Ym form. */
            char left[24];
            if(!lc_life_format_remaining(&app->life, left, sizeof(left)))
                snprintf(left, sizeof(left), "Life unknown");
            const char* record = sensor->has_error ? "record error" :
                (sensor->quality_bits || sensor->quality_flags) ? "quality" : "record clear";
            snprintf(line, sizeof(line), "%s; %s", left, record);
            bool invalid_factory = app->factory.status == LcFactoryRejected &&
                sensor->status_code == 3 && sensor->max_life_minutes &&
                sensor->age_minutes <= sensor->max_life_minutes &&
                !sensor->quality_bits && !sensor->quality_flags;
            canvas_draw_str(canvas, 0, 30, invalid_factory ? "No estimate: coeff/domain" : line);
            snprintf(line, sizeof(line), "Age%u/%um Q%03X/%u", sensor->age_minutes,
                     sensor->max_life_minutes, sensor->quality_bits, sensor->quality_flags);
            canvas_draw_str(canvas, 0, 38, line);
        } else {
            canvas_draw_str(canvas, 0, 22, app->integrity.status == LcIntegrityPass ? "CRC OK; parse rejected" :
                app->integrity.status == LcIntegrityFail ? "CRC FAIL; data untrusted" :
                app->integrity.status == LcIntegrityUnsupported ? "Decode unsupported" : "Integrity not checked");
            canvas_draw_str(canvas, 0, 30, sensor->parse_status == LcSensorInvalidIndex ? "Invalid ring index" :
                sensor->parse_status == LcSensorProfileMismatch ? "Footer profile mismatch" :
                sensor->parse_status == LcSensorUnsupported ? "Status unsupported" : "Sensor status untrusted");
            canvas_draw_str(canvas, 0, 38, app->status);
        }
        char fresh[48];
        snprintf(fresh, sizeof(fresh), "%s  %s", serial, "<:UID >:scan");
        canvas_draw_str(canvas, 0, 46, fresh);
        canvas_draw_str(canvas, 0, 54, app->save_status);
        /* Freshness of the shown sample, sensor-clock only. Explicitly unknown
         * when this capture cannot establish it OR when the newest record is an
         * error record -- an errored sample is never labelled "current". */
        char fresh_line[32];
        if((!app->details && !app->vendor_chain.available) || sensor->has_error ||
           !lc_freshness_format(&app->life, fresh_line, sizeof(fresh_line)))
            snprintf(fresh_line, sizeof(fresh_line), "Freshness unknown");
        canvas_draw_str(canvas, 0, 63, fresh_line);
    }
    furi_mutex_release(app->mutex);
}

static void input(InputEvent* event, void* context) {
    AppState* app = context;
    if(event->type == InputTypeShort || event->type == InputTypeLong)
        furi_message_queue_put(app->keys, event, 0);
}

/* Sole RF transaction boundary. Exact-length allowlist disallows A1 payloads.
 * Only called by the extended ISO15693 poller callback; CRC handled by SDK. */
static Iso15693_3Error send_read(AppState* app, Iso15693_3Poller* poller, const uint8_t* command, size_t command_length) {
    if(!lc_command_allowed(command, command_length)) return Iso15693_3ErrorNotSupported;
    /* Linearization point shared with every cancel path. Do not hold this
     * mutex during SDK I/O: cancel suppresses only not-yet-committed frames. */
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    bool committed = !app->finished && lc_transaction_commit(&app->gate);
    furi_mutex_release(app->mutex);
    if(!committed) return Iso15693_3ErrorTimeout;
    bit_buffer_reset(app->tx);
    bit_buffer_reset(app->rx);
    bit_buffer_append_bytes(app->tx, command, command_length);
    return iso15693_3_poller_send_frame(poller, app->tx, app->rx, FRAME_WAIT_FC);
}

/* start_ex is essential: ordinary start invokes SDK activation, which reads
 * all reported memory BEFORE Ready. We bypass that path entirely. */
static NfcCommand poller_callback(NfcGenericEventEx event, void* context) {
    AppState* app = context;
    const NfcEvent* parent = event.parent_event_data;
    if(parent->type != NfcEventTypePollerReady) return NfcCommandContinue;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    bool stop = app->gate.cancelled || app->finished;
    unsigned phase = app->phase;
    furi_mutex_release(app->mutex);
    if(stop) return NfcCommandStop;

    static const uint8_t inventory[3] = {0x26, 0x01, 0x00};
    static const uint8_t patch_info[3] = {0x02, 0xa1, 0x07};
    uint8_t read_command[4];
    const uint8_t* command = phase == 0 ? inventory : patch_info;
    size_t command_length = 3;
    if(phase >= 2) {
        /* Gate rechecked under the same mutex before every FRAM commitment. */
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        bool allowed = app->capture.uid_valid && app->capture.patch_valid &&
            lc_family_allowed(app->capture.uid, app->capture.patch, 6) &&
            app->capture.fram_length == (phase-2U)*24U;
        furi_mutex_release(app->mutex);
        if(!allowed || !lc_read_command(phase-2U, read_command)) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->capture.error = Iso15693_3ErrorNotSupported;
            app->finished = true;
            snprintf(app->status, sizeof(app->status), "Unsupported; stopped");
            furi_mutex_release(app->mutex);
            return NfcCommandStop;
        }
        command = read_command;
        command_length = 4;
    }
    Iso15693_3Error error = send_read(app, event.poller, command, command_length);
    /* SDK link CRC trimming loses original alignment. Preserve returned bytes. */
    size_t length = bit_buffer_get_size_bytes(app->rx);
    const uint8_t* response = bit_buffer_get_data(app->rx);
    uint8_t parsed[24] = {0};
    bool valid = error == Iso15693_3ErrorNone &&
        (phase == 0 ? lc_uid_parse(response, length, parsed) :
         phase == 1 ? lc_patch_parse(response, length, parsed) :
         lc_read_parse(response, length, read_command[3]+1U, parsed));

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    if(app->gate.cancelled) {
        furi_mutex_release(app->mutex);
        return NfcCommandStop;
    }
    if(phase == 0 && app->capture.inventory_attempts != UINT32_MAX) app->capture.inventory_attempts++;
    app->capture.error = valid ? 0U : (error == Iso15693_3ErrorNone ? (unsigned)Iso15693_3ErrorFormat : (unsigned)error);
    /* Persistent inventory cannot keep an unbounded transcript. Keep accepted
     * UID frame plus all subsequent attempts, including errors. No truncation. */
    if(valid || phase > 0) {
        if(length > LC_RESPONSE_MAX || app->capture.transcript_length >= LC_TRANSCRIPT_MAX) {
            app->capture.error = Iso15693_3ErrorFormat;
            app->finished = true;
            snprintf(app->status, sizeof(app->status), "Transcript overflow; stop");
            furi_mutex_release(app->mutex);
            return NfcCommandStop;
        }
        LcExchange* exchange = &app->capture.transcript[app->capture.transcript_length++];
        memcpy(exchange->command, command, command_length);
        exchange->command_length = command_length;
        memcpy(exchange->response, response, length);
        exchange->response_length = length;
        exchange->error = app->capture.error;
    }
    if(valid && phase == 0) {
        memcpy(app->capture.uid, parsed, 8);
        app->capture.uid_valid = true;
        app->capture.timestamp = furi_hal_rtc_get_timestamp();
        if(lc_uid_is_ti(parsed)) app->phase = 1;
        else {
            snprintf(app->status, sizeof(app->status), "Non-TI: UID only; stopped");
            app->finished = true;
        }
    } else if(phase == 1) {
        if(valid) {
            memcpy(app->capture.patch, parsed, 6);
            app->capture.patch_valid = true;
            if(lc_family_allowed(app->capture.uid, parsed, 6)) app->phase = 2;
            else {
                snprintf(app->status, sizeof(app->status), "Unsupported family/gen; stop");
                app->finished = true;
            }
        } else {
            snprintf(app->status, sizeof(app->status), "Patch error %u; UID only", app->capture.error);
            app->finished = true; /* Never retry/escalate a custom command. */
        }
    } else if(phase >= 2) {
        if(valid) {
            size_t bytes = (read_command[3]+1U)*8U;
            memcpy(app->capture.fram + app->capture.fram_length, parsed, bytes);
            app->capture.fram_length += bytes;
            app->phase++;
            if(app->capture.fram_length == LC_FRAM_SIZE) {
                app->finished = true;
                snprintf(app->status, sizeof(app->status), "Raw 344 bytes; no glucose");
            }
        } else {
            snprintf(app->status, sizeof(app->status), "Read error %u; partial", app->capture.error);
            app->finished = true; /* No retries or fallback after FRAM error. */
        }
    }
    stop = app->finished;
    furi_mutex_release(app->mutex);
    return stop ? NfcCommandStop : NfcCommandContinue;
}

static void stop_scan(AppState* app) {
    /* Stop feedback BEFORE a potentially stalled SDK join, including cancel. */
    notification_message_block(app->notification, &sequence_blink_stop);
    notification_message_block(app->notification, &sequence_reset_rgb);
    if(app->started) {
        nfc_poller_stop(app->poller); /* Join can stall in SDK I/O; never free a live worker. */
        app->started = false;
        nfc_poller_free(app->poller);
        app->poller = NULL;
    }
}

static void start_scan(AppState* app) {
    stop_scan(app);
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    memset(&app->capture, 0, sizeof(app->capture));
    memset(&app->integrity, 0, sizeof(app->integrity));
    memset(&app->factory, 0, sizeof(app->factory));
    memset(&app->vendor_chain, 0, sizeof(app->vendor_chain));
    memset(&app->life, 0, sizeof(app->life));
    app->details = false;
    app->uid_details = false;
    app->gate = (LcTransactionGate){0};
    app->finished = false;
    app->phase = 0;
    app->saved_name[0] = 0;
    snprintf(app->save_status, sizeof(app->save_status), "Not saved yet");
    app->page = PageScan;
    app->start_tick = furi_get_tick();
    furi_mutex_release(app->mutex);
    app->poller = nfc_poller_alloc(app->nfc, NfcProtocolIso15693_3);
    app->started = true;
    notification_message_block(app->notification, &sequence_blink_start_blue);
    nfc_poller_start_ex(app->poller, poller_callback, app);
}

/* Main thread only, NFC stopped. Never overwrite, even after clock resets.
 * Save success requires sync/close and exact file readback. */
static bool save_capture(AppState* app) {
    /* No identity guess or stale identity after cancellation, including manual saves. */
    if(!app->capture.uid_valid || app->gate.cancelled) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->saved_name[0] = 0;
        snprintf(app->save_status, sizeof(app->save_status), "No UID/cancel: not saved");
        furi_mutex_release(app->mutex);
        return false;
    }
    /* Separate save-attempt wall time: never replace acquisition on manual save. */
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->capture.save_timestamp = furi_hal_rtc_get_timestamp();
    app->capture.save_time_recorded = true;
    furi_mutex_release(app->mutex);
    char* json = malloc(LC_JSON_CAPACITY);
    char* verified = malloc(LC_JSON_CAPACITY);
    char path[128];
    size_t length = json && verified ? lc_capture_json(&app->capture, json, LC_JSON_CAPACITY) : 0;
    bool saved = false;
    bool created = false;
    const char* reason = !app->capture.uid_valid ? "No UID: nothing to save" :
        (!json || !verified ? "No save memory; OK: retry" :
         (length ? "SD save failed; OK: retry" : "Capture invalid; OK: retry"));
    if(length) {
        Storage* storage = furi_record_open(RECORD_STORAGE);
        File* file = storage_file_alloc(storage);
        FS_Error dir_error = storage_common_mkdir(storage, CAPTURE_DIR);
        if(dir_error == FSE_OK || dir_error == FSE_EXIST) {
            for(unsigned i = 0; i < 100; i++) {
                char uid[17];
                lc_hex(app->capture.uid, sizeof(app->capture.uid), uid, sizeof(uid));
                int path_length = snprintf(path, sizeof(path), CAPTURE_DIR "/cap_%s_%08lx_%02u.json",
                                           uid, (unsigned long)furi_get_tick(), i);
                if(path_length < 0 || (size_t)path_length >= sizeof(path)) break;
                if(storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_NEW)) {
                    created = true;
                    saved = storage_file_write(file, json, length) == length;
                    if(saved) saved = storage_file_sync(file);
                    bool closed = storage_file_close(file);
                    saved = saved && closed;
                    break;
                }
                FS_Error open_error = storage_file_get_error(file);
                storage_file_close(file); /* Required even on unsuccessful open. */
                if(open_error != FSE_EXIST) break;
            }
            if(saved) {
                if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
                    saved = storage_file_size(file) == length &&
                            storage_file_read(file, verified, length) == length &&
                            memcmp(json, verified, length) == 0;
                } else saved = false;
                bool closed = storage_file_close(file);
                saved = saved && closed;
            }
            /* Remove only the new incomplete file owned by this invocation. */
            if(created && !saved) storage_common_remove(storage, path);
        }
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
    }
    free(verified);
    free(json);
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    snprintf(app->save_status, sizeof(app->save_status), "%s", saved ? "Saved + verified on SD" : reason);
    if(saved) snprintf(app->saved_name, sizeof(app->saved_name), "%s", strrchr(path, '/') + 1);
    furi_mutex_release(app->mutex);
    return saved;
}

/* RECOVERED closed-chain evaluation for the UI. Re-derives the decoded FRAM
 * (same transform integrity.c uses), runs the bounded vendor chain, and wipes
 * the plaintext. Never retains or serialises the decrypted FRAM. Returns false
 * (available=false) when the capture is incomplete or the chain is empty. */
static void vendor_chain_evaluate(const LcCapture* capture, LcVendorChainResult* out) {
    memset(out, 0, sizeof(*out));
    if(!capture || !lc_capture_complete(capture)) return;
    LcIntegrity checked;
    lc_integrity_check(capture, &checked);
    const LcSensor* sensor = &checked.sensor;
    /* Historical factory math is not admission for the vendor current path. */
    if(checked.status != LcIntegrityPass || sensor->parse_status != LcSensorParsed ||
       sensor->status_code != 3 || !sensor->max_life_minutes ||
       sensor->age_minutes >= sensor->max_life_minutes || sensor->has_error ||
       sensor->quality_bits || sensor->quality_flags) return;
    uint8_t plain[LC_FRAM_SIZE];
    lc_integrity_transform(capture->uid, capture->patch, capture->fram, plain);
    uint32_t mg = 0, mmol = 0;
    LcVendorConfig cfg;
    LcVendorTrendSample sample;
    bool ok = lc_vendor_current_trend(plain, &cfg, &sample, &mg, &mmol) == LcVendorChainOk;
    ok = ok && sample.sensor_minute == checked.sensor.age_minutes;
    volatile uint8_t* wipe = plain;
    for(size_t i = 0; i < sizeof(plain); i++) wipe[i] = 0;
    if(!ok) return;
    out->available = true;
    out->mg_dl = mg;
    out->mmol_tenths = mmol;
    out->cfg_i2 = cfg.i2;
    out->cfg_i3 = cfg.i3;
    out->cfg_i4 = cfg.i4;
    out->cfg_i6 = cfg.i6;
}

/* Main-thread completion: stop/join first, then elapsed duration and SD I/O.
 * Acquisition wall time was already recorded when UID was accepted. */

/* Best-effort GPIO UART companion frame emitter (Pins 13 TX / 14 RX @ 115200).
 * Sends LL2|<16-hex-uid>|<12-hex-patch>|<688-hex-fram>|<4-hex-crc16>\n after SD
 * save so a connected Heltec V3 ESP32 companion immediately decodes and shows
 * the scan. Never blocks if no companion is wired. */
static void emit_uart_bridge_frame(const LcCapture* c) {
    if(!c || !lc_capture_complete(c)) return;
    char* line = malloc(736U);
    if(!line) return;
    char uid_hex[17], patch_hex[13], fram_hex[689];
    uint8_t raw[8U + 6U + LC_FRAM_SIZE];
    memcpy(raw, c->uid, 8U);
    memcpy(raw + 8U, c->patch, 6U);
    memcpy(raw + 14U, c->fram, LC_FRAM_SIZE);
    uint16_t crc = lc_integrity_crc(raw, sizeof(raw));
    if(lc_hex(c->uid, 8U, uid_hex, sizeof(uid_hex)) &&
       lc_hex(c->patch, 6U, patch_hex, sizeof(patch_hex)) &&
       lc_hex(c->fram, LC_FRAM_SIZE, fram_hex, sizeof(fram_hex))) {
        int n = snprintf(line, 736U, "LL2|%s|%s|%s|%04X\r\n",
                         uid_hex, patch_hex, fram_hex, (unsigned)crc);
        if(n > 0 && n < 736) {
            Expansion* exp = furi_record_open(RECORD_EXPANSION);
            expansion_disable(exp);
            FuriHalSerialHandle* serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
            if(serial) {
                furi_hal_serial_init(serial, 115200U);
                furi_hal_serial_tx(serial, (const uint8_t*)line, (size_t)n);
                furi_hal_serial_tx_wait_complete(serial);
                furi_hal_serial_deinit(serial);
                furi_hal_serial_control_release(serial);
            }
            expansion_enable(exp);
            furi_record_close(RECORD_EXPANSION);
        }
    }
    free(line);
}

static void finish_scan(AppState* app) {
    if(!app->started) return;
    view_port_update(app->viewport);
    stop_scan(app);
    uint32_t elapsed = furi_get_tick() - app->start_tick;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->capture.elapsed_ms = (uint32_t)((uint64_t)elapsed * 1000U / furi_kernel_get_tick_frequency());
    app->page = PageResult;
    furi_mutex_release(app->mutex);
    if(app->gate.cancelled || !app->capture.uid_valid) return;
    LcIntegrity checked;
    lc_integrity_check(&app->capture, &checked);
    LcFactory factory;
    lc_factory_check(&app->capture, &factory);
    /* Newest trend only. Historical math remains archived, never a UI fallback. */
    LcVendorChainResult vendor_chain;
    vendor_chain_evaluate(&app->capture, &vendor_chain);
    /* Remaining life + freshness, derived from the SAME decoded sensor as the
     * value above: both the factory estimate and this use the newest trend
     * record (sample index 0) of this one capture. Sensor clock only, no RTC. */
    LcLife life;
    const LcSensor* life_sensor = &checked.sensor;
    bool life_parsed = checked.status == LcIntegrityPass &&
        life_sensor->parse_status == LcSensorParsed;
    lc_life_compute(life_sensor->age_minutes, life_sensor->max_life_minutes,
                    0, life_parsed, &life);
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->integrity = checked;
    app->factory = factory;
    app->vendor_chain = vendor_chain;
    app->life = life;
    furi_mutex_release(app->mutex);
    bool saved = save_capture(app);
    emit_uart_bridge_frame(&app->capture);
    const LcSensor* sensor = &app->integrity.sensor;
    /* Neutral completion requires the admitted current result plus verified save. */
    bool diagnostics_clear = app->integrity.status == LcIntegrityPass &&
        sensor->parse_status == LcSensorParsed && sensor->status_code == 3 &&
        sensor->max_life_minutes != 0 && sensor->age_minutes <= sensor->max_life_minutes &&
        !sensor->has_error && !sensor->quality_bits && !sensor->quality_flags;
    if(saved && diagnostics_clear && app->vendor_chain.available)
        notification_message_block(app->notification, &scan_complete_sequence);
    else
        notification_message_block(app->notification, &scan_error_sequence);
    notification_message_block(app->notification, &sequence_blink_stop);
    notification_message_block(app->notification, &sequence_reset_rgb);
}

int32_t libre_capture_app(void* argument) {
    UNUSED(argument);
    AppState* app = malloc(sizeof(AppState));
    memset(app, 0, sizeof(AppState));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->keys = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->tx = bit_buffer_alloc(16);
    app->rx = bit_buffer_alloc(256);
    app->nfc = nfc_alloc();
    app->notification = furi_record_open(RECORD_NOTIFICATION);
    app->viewport = view_port_alloc();
    view_port_draw_callback_set(app->viewport, draw, app);
    view_port_input_callback_set(app->viewport, input, app);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, app->viewport, GuiLayerFullscreen);
    /* Auto-start scanning on open: no idle OK press required. The idle page is
     * only reached again by cancelling a scan with Back (or a completed read's
     * Back-to-exit); start_scan() is reentrant and zeroes all prior state. */
    start_scan(app);
    bool running = true;
    while(running) {
        InputEvent key;
        if(furi_message_queue_get(app->keys, &key, furi_ms_to_ticks(50)) == FuriStatusOk) {
            if(key.key == InputKeyBack) {
                if(app->page == PageScan) {
                    furi_mutex_acquire(app->mutex, FuriWaitForever);
                    lc_transaction_cancel(&app->gate);
                    furi_mutex_release(app->mutex);
                    view_port_update(app->viewport);
                    stop_scan(app);
                    furi_mutex_acquire(app->mutex, FuriWaitForever);
                    app->page = PageIdle;
                    furi_mutex_release(app->mutex);
                } else running = false;
            } else if(key.type == InputTypeShort && key.key == InputKeyOk) {
                if(app->page == PageIdle) start_scan(app);
                else if(app->page == PageResult) save_capture(app);
            } else if(key.type == InputTypeShort && key.key == InputKeyRight && app->page == PageResult) {
                start_scan(app);
            } else if(key.type == InputTypeShort && key.key == InputKeyLeft && app->page == PageResult) {
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                if(app->uid_details) {
                    app->uid_details = false;
                    app->details = false;
                } else if(app->details) {
                    app->uid_details = true;
                } else app->details = true;
                furi_mutex_release(app->mutex);
            }
        }
        if(app->started) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            bool finished = app->finished;
            furi_mutex_release(app->mutex);
            if(finished) {
                finish_scan(app);
            }
        }
        view_port_update(app->viewport);
    }
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    lc_transaction_cancel(&app->gate);
    furi_mutex_release(app->mutex);
    stop_scan(app);
    view_port_enabled_set(app->viewport, false);
    furi_record_close(RECORD_NOTIFICATION);
    gui_remove_view_port(gui, app->viewport);
    view_port_free(app->viewport);
    furi_record_close(RECORD_GUI);
    nfc_free(app->nfc);
    bit_buffer_free(app->rx);
    bit_buffer_free(app->tx);
    furi_message_queue_free(app->keys);
    furi_mutex_free(app->mutex);
    memset(&app->capture, 0, sizeof(app->capture));
    free(app);
    return 0;
}
