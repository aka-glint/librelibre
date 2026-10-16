/*
 * LibreLibre ESP32 — CircuitMess Artemis Watch (ESP32-S3-MINI-1, 128x128 ST7735S, LSM6DS3TR-C IMU, 4 Buttons, SPIFFS, BLE)
 *
 * Runs the full LibreLibre C pipeline natively on the CircuitMess Artemis watch:
 *   - Hardware revision auto-detection from eFuse BLOCK3 (Rev 0/1 vs Rev 2/3 pin maps & ST7735S offsets)
 *   - 17-command ISO15693 master state machine (nfc_io.c)
 *   - EU2 XOR decryption + 3-section CRC16 (integrity.c)
 *   - Per-sensor calibration extraction + 48-point FRAM series + temperature/glucose chain (vendor_chain.c)
 *   - Sensor-clock freshness + remaining lifetime (lifecycle.c)
 *   - 5-bit UID-to-serial derivation (protocol.c)
 *   - Verified SPIFFS JSON capture save + exact readback + persistent CSV history & BLE bond
 *   - 5-page 128x128 ST7735S UI with zoomable 2D pixel graph, modal popups, and Direct Libre 2 BLE client
 *   - LSM6DS3TR-C IMU (0x6A) bump/raise-to-wake + 4-button wake
 *
 * License: MIT
 */

#include <Arduino.h>
#include <Wire.h>
#include <FS.h>
#include <SPIFFS.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "soc/efuse_reg.h"
#include <sys/stat.h>
#include <sys/unistd.h>

#include "bridge.h"
#include "nfc_io.h"
#include "scan_history.h"
#include "libre2_ble.h"
#include "factory.h"
#include "vendor_tables.h"

#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

SET_LOOP_TASK_STACK_SIZE(16 * 1024);

#define LCD_W            128
#define LCD_H            128

#define STORAGE_DIR      "/spiffs"
#define HISTORY_CSV_PATH "/spiffs/scans.csv"
#define BLE_BOND_PATH    "/spiffs/ble_bond.txt"
#define LAST_SCAN_PATH   "/spiffs/last_frame.txt"

// ─── CIRCUITMESS ARTEMIS HARDWARE REVISION & PIN MAP ─────
struct ArtemisPins {
    uint8_t rev;
    int btnDown;
    int btnUp;
    int btnSelect;
    int btnAlt;
    int ledBl;
    int buzz;
    int i2cSda;
    int i2cScl;
    int tftSck;
    int tftMosi;
    int tftDc;
    int tftRst;
};

static ArtemisPins s_pins;

static void detectArtemisHardware() {
    uint32_t usr1 = REG_READ(EFUSE_RD_USR_DATA1_REG);
    uint8_t rev = (uint8_t)(usr1 & 0xFFU);
    if(rev == 0U || rev == 1U) {
        s_pins = { rev, 10, 9, 8, 21, 37, 11, 35, 36, 41, 40, 39, 38 };
    } else {
        // Revision 2 / 3 (default for HWRev 3)
        if(rev > 3U) rev = 3U;
        s_pins = { rev, 2, 1, 3, 21, 33, 47, 40, 41, 36, 35, 48, 34 };
    }
    // Turn off the 6 perimeter red LEDs (same across all revisions)
    const int leds[6] = { 17, 18, 43, 44, 45, 46 };
    for(int i = 0; i < 6; i++) {
        pinMode(leds[i], OUTPUT);
        digitalWrite(leds[i], LOW);
    }
}

// ─── LOVYANGFX ST7735S 128x128 DISPLAY ────────────────────
class ArtemisDisplay : public lgfx::LGFX_Device {
    lgfx::Bus_SPI _bus;
    lgfx::Panel_ST7735S _panel;
public:
    void initWithPins(const ArtemisPins& p) {
        {
            auto cfg = _bus.config();
            cfg.spi_host = SPI2_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = 40000000;
            cfg.freq_read  = 40000000;
            cfg.spi_3wire  = false;
            cfg.use_lock   = false;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = p.tftSck;
            cfg.pin_mosi = p.tftMosi;
            cfg.pin_miso = -1;
            cfg.pin_dc   = p.tftDc;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs   = -1;
            cfg.pin_rst  = p.tftRst;
            cfg.pin_busy = -1;
            cfg.memory_width  = (p.rev == 3U) ? 128 : 132;
            cfg.memory_height = (p.rev == 3U) ? 160 : 132;
            cfg.panel_width   = 128;
            cfg.panel_height  = 128;
            cfg.offset_x      = (p.rev == 3U) ? 0 : 2;
            cfg.offset_y      = (p.rev == 3U) ? 0 : 1;
            cfg.offset_rotation = (p.rev == 2U || p.rev == 3U) ? 3 : 1;
            cfg.readable   = false;
            cfg.invert     = false;
            cfg.rgb_order  = false;
            cfg.dlen_16bit = false;
            cfg.bus_shared = false;
            _panel.config(cfg);
        }
        setPanel(&_panel);
        init();
    }
};

static ArtemisDisplay s_display;
static LGFX_Sprite s_canvas(&s_display);

// Colors (standard RGB565 for LovyanGFX)
static constexpr uint16_t COL_BG          = 0x0000; // pitch black
static constexpr uint16_t COL_HEADER_BG   = 0x0842;
static constexpr uint16_t COL_CARD_BG     = 0x0821;
static constexpr uint16_t COL_GRID        = 0x18E4;
static constexpr uint16_t COL_TARGET_BAND = 0x0902;
static constexpr uint16_t COL_WHITE       = 0x765B; // soft muted cyan-grey (never harsh white)
static constexpr uint16_t COL_CYAN        = 0x0599;
static constexpr uint16_t COL_GREEN       = 0x15AA;
static constexpr uint16_t COL_AMBER       = 0xD4A2;
static constexpr uint16_t COL_RED         = 0xC8E2;
static constexpr uint16_t COL_MUTED       = 0x5AEB;
#define SETTINGS_CFG_PATH "/spiffs/settings.cfg"

static bool s_cfg_raise_wake = true;
static uint8_t s_cfg_timeout_idx = 2U; // 0=15s, 1=30s, 2=45s, 3=1m, 4=2m, 5=5m, 6=NEVER
static uint8_t s_cfg_bright_idx  = 1U; // 0=10%, 1=25%, 2=45%, 3=70%, 4=100%
static uint8_t s_cfg_theme_idx   = 0U; // 0=CP2077, 1=CYAN, 2=MAGENTA, 3=MATRIX, 4=AMBER, 5=CRIMSON
static bool    s_cfg_glitch_fx   = true; // Cyberpunk 2077 Kiroshi Relic scanline/chromatic glitch effect
static uint16_t s_cfg_warn_low_tenths  = 42U;  // Default 4.2 mmol/L (3s flight-deck flash)
static uint16_t s_cfg_warn_high_tenths = 130U; // Default 13.0 mmol/L (3s flight-deck flash)
static uint16_t s_cfg_urg_low_tenths   = 36U;  // Default 3.6 mmol/L (audible + ack/15m snooze)
static uint16_t s_cfg_urg_high_tenths  = 160U; // Default 16.0 mmol/L (audible + ack/15m snooze)
static uint8_t s_settings_row    = 0U; // 0..8 cursor on PAGE_SETTINGS
static bool    s_ble_bond_dirty  = false;
static bool    s_glitch_frame_active = false;
static unsigned long s_glitch_Burst_start_ms = 0UL;
static unsigned long s_glitch_next_ms = 3500UL;

static const uint16_t kWarnLowTable[11]  = { 38U, 39U, 40U, 41U, 42U, 43U, 44U, 45U, 48U, 50U, 0U };
static const uint16_t kWarnHighTable[10] = { 100U, 110U, 120U, 125U, 130U, 135U, 140U, 150U, 160U, 0U };
static const uint16_t kUrgLowTable[10]   = { 30U, 32U, 34U, 35U, 36U, 37U, 38U, 39U, 40U, 0U };
static const uint16_t kUrgHighTable[10]  = { 140U, 145U, 150U, 155U, 160U, 165U, 170U, 180U, 200U, 0U };

static const uint32_t kTimeoutMsTable[7] = {
    15000UL, 30000UL, 45000UL, 60000UL, 120000UL, 300000UL, 0UL
};
static const char* const kTimeoutLabels[7] = {
    "15 SEC", "30 SEC", "45 SEC", "1 MIN", "2 MIN", "5 MIN", "NEVER"
};
static const uint8_t kBrightPctTable[5] = { 10U, 25U, 45U, 70U, 100U };
static const char* const kBrightLabels[5] = { "10%", "25%", "45%", "70%", "100%" };
static const char* const kThemeLabels[6] = { "CP2077", "CYAN", "MAGENTA", "MATRIX", "AMBER", "CRIMSON" };

// ─── FLIGHT DECK LEDS (6x PERIMETER RED) & PIEZO ALARM STATE ──
static const int kFlightDeckLeds[6] = { 17, 18, 43, 44, 45, 46 };
enum FlightLedMode {
    LED_MODE_IDLE = 0,
    LED_MODE_SUBTLE_BLINK = 1,
    LED_MODE_FLASH_3S = 2
};
static FlightLedMode s_led_mode = LED_MODE_IDLE;
static unsigned long s_led_start_ms = 0UL;
static unsigned long s_led_until_ms = 0UL;
static bool s_leds_cur_on = false;

static bool s_alarm_latched = false;
static bool s_alarm_is_low  = false;
static uint16_t s_alarm_mmol_tenths = 0U;
static bool s_alarm_snoozed = false;
static unsigned long s_alarm_snooze_until_ms = 0UL;
static uint32_t s_cur_buzz_freq = 0U;

static void setFlightDeckLeds(bool on) {
    if(s_leds_cur_on == on) return;
    s_leds_cur_on = on;
    for(int i = 0; i < 6; i++) {
        digitalWrite(kFlightDeckLeds[i], on ? HIGH : LOW);
    }
}

static void initPiezoBuzzer() {
    ledc_timer_config_t tcfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num       = LEDC_TIMER_1,
        .freq_hz         = 2700,
        .clk_cfg         = LEDC_AUTO_CLK
    };
    ledc_timer_config(&tcfg);

    ledc_channel_config_t ccfg = {
        .gpio_num   = s_pins.buzz,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_2,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER_1,
        .duty       = 0,
        .hpoint     = 0,
        .flags      = { .output_invert = 0 }
    };
    ledc_channel_config(&ccfg);
    s_cur_buzz_freq = 0U;
}

static void setBuzzerTone(uint32_t freq_hz) {
    if(s_cur_buzz_freq == freq_hz) return;
    s_cur_buzz_freq = freq_hz;
    if(freq_hz == 0U) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, 0U);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
    } else {
        ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_1, freq_hz);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, 512U);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
    }
}

// ─── STATE ────────────────────────────────────────────────
static bool s_spiffs_mounted = false;
static LlCompanionState compState;
static LlHistoryStore s_history;

enum ScreenPage {
    PAGE_HOME = 0,
    PAGE_MAIN = 1,
    PAGE_GRAPH = 2,
    PAGE_DIAG = 3,
    PAGE_UID = 4,
    PAGE_BLE = 5,
    PAGE_SETTINGS = 6,
    PAGE_COUNT = 7
};
static ScreenPage currentPage = PAGE_HOME;

static const uint16_t kZoomMinutes[6] = { 15U, 60U, 180U, 480U, 1440U, 0U };
static const char* const kZoomLabels[6] = { "15m", "1h", "3h", "8h", "24h", "ALL" };
static size_t s_zoom_idx = 3U; // default 8h

static uint32_t lastScanMillis = 0;
static uint32_t scanCount = 0;
static char lastTransport[8] = "NONE";
static char saveStatus[40] = "SPIFFS: checking...";
static char savedFileName[40] = "";

static char usbBuf[LL_BRIDGE_LINE_MAX];
static size_t usbLen = 0;

// Modal popup overlay state
static bool s_popup_active = false;
static unsigned long s_popup_until_ms = 0;
static char s_popup_title[24] = "";
static char s_popup_line1[24] = "";
static char s_popup_line2[24] = "";
static char s_popup_line3[24] = "";

static void renderDisplay();

static void showPopup(const char* title, const char* l1, const char* l2, const char* l3, unsigned long duration_ms = 3200U) {
    snprintf(s_popup_title, sizeof(s_popup_title), "%s", title ? title : "");
    snprintf(s_popup_line1, sizeof(s_popup_line1), "%s", l1 ? l1 : "");
    snprintf(s_popup_line2, sizeof(s_popup_line2), "%s", l2 ? l2 : "");
    snprintf(s_popup_line3, sizeof(s_popup_line3), "%s", l3 ? l3 : "");
    s_popup_active = true;
    s_popup_until_ms = millis() + duration_ms;
    renderDisplay();
}

static void drawPopupOverlay() {
    if(s_alarm_latched) {
        uint16_t bCol = s_alarm_is_low ? COL_RED : COL_AMBER;
        s_canvas.fillRoundRect(3, 14, 122, 100, 6, COL_BG);
        s_canvas.drawRoundRect(3, 14, 122, 100, 6, bCol);
        s_canvas.drawRoundRect(4, 15, 120, 98, 5, bCol);
        s_canvas.fillRect(5, 16, 118, 18, COL_HEADER_BG);
        s_canvas.setTextSize(1);
        s_canvas.setTextColor(bCol);
        s_canvas.setCursor(10, 21);
        s_canvas.print(s_alarm_is_low ? "!! URGENT LOW !!" : "!! URGENT HIGH !!");

        s_canvas.setTextSize(3);
        s_canvas.setTextColor(bCol);
        s_canvas.setCursor(18, 42);
        s_canvas.printf("%u.%u", (unsigned)(s_alarm_mmol_tenths / 10U), (unsigned)(s_alarm_mmol_tenths % 10U));

        s_canvas.setTextSize(1);
        s_canvas.setTextColor(COL_WHITE);
        s_canvas.setCursor(44, 72);
        s_canvas.print("mmol/L");

        s_canvas.setTextColor(COL_CYAN);
        s_canvas.setCursor(8, 92);
        s_canvas.print("ANY BTN: SNOOZE 15M");
        return;
    }
    if(!s_popup_active) return;
    if((int32_t)(millis() - s_popup_until_ms) >= 0) {
        s_popup_active = false;
        return;
    }
    s_canvas.fillRoundRect(4, 18, 120, 92, 6, COL_HEADER_BG);
    s_canvas.drawRoundRect(4, 18, 120, 92, 6, COL_AMBER);
    s_canvas.setTextSize(1);
    s_canvas.setTextColor(COL_AMBER);
    s_canvas.setCursor(10, 24);
    s_canvas.print(s_popup_title);

    s_canvas.setTextColor(COL_WHITE);
    s_canvas.setCursor(8, 42);
    s_canvas.print(s_popup_line1);
    s_canvas.setCursor(8, 56);
    s_canvas.print(s_popup_line2);
    s_canvas.setCursor(8, 70);
    s_canvas.print(s_popup_line3);

    s_canvas.setTextColor(COL_CYAN);
    s_canvas.setCursor(10, 94);
    s_canvas.print("Press any btn to close");
}

// ─── BACKLIGHT PWM (INVERTED ON ARTEMIS) & IMU WAKE ───────
#define SCREEN_TIMEOUT_MS 300000UL
static bool s_screen_asleep = false;
static unsigned long s_last_activity_ms = 0;
static bool s_imu_ready = false;
static float s_mag_filtered = 1.0f;
static bool s_wrist_down = true;
static unsigned long s_last_imu_poll_ms = 0;

static void setBacklightPercent(uint8_t pct);

static void initBacklight() {
    if(s_pins.rev >= 2U) {
        pinMode(37, INPUT);
    }
    ledc_timer_config_t tcfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK
    };
    ledc_timer_config(&tcfg);

    ledc_channel_config_t ccfg = {
        .gpio_num   = s_pins.ledBl,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_1,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 600,
        .hpoint     = 0,
        .flags      = { .output_invert = 1 }
    };
    ledc_channel_config(&ccfg);
    setBacklightPercent(kBrightPctTable[s_cfg_bright_idx % 5U]);
}

static void setBacklightPercent(uint8_t pct) {
    if(pct == 0U) {
        ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 1);
        return;
    }
    if(pct > 100U) pct = 100U;
    uint32_t duty = ((uint32_t)pct * 1023U) / 100U;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

static void wakeScreen(const char* reason) {
    s_last_activity_ms = millis();
    if(s_screen_asleep) {
        s_screen_asleep = false;
        setBacklightPercent(kBrightPctTable[s_cfg_bright_idx % 5U]);
        Serial.printf("SCREEN_WAKE|reason=%s\n", reason ? reason : "activity");
        renderDisplay();
    }
}

static void saveSettingsToSpiffs() {
    if(!s_spiffs_mounted) return;
    FILE* f = fopen(SETTINGS_CFG_PATH, "wb");
    if(!f) return;
    fprintf(f, "CFG|%u|%u|%u|%u|%u|%u|%u|%u|%u\n",
            (unsigned)(s_cfg_raise_wake ? 1U : 0U),
            (unsigned)s_cfg_timeout_idx,
            (unsigned)s_cfg_bright_idx,
            (unsigned)s_cfg_theme_idx,
            (unsigned)s_cfg_warn_low_tenths,
            (unsigned)s_cfg_warn_high_tenths,
            (unsigned)s_cfg_urg_low_tenths,
            (unsigned)s_cfg_urg_high_tenths,
            (unsigned)(s_cfg_glitch_fx ? 1U : 0U));
    fflush(f);
    fclose(f);
}

static void loadSettingsFromSpiffs() {
    if(!s_spiffs_mounted) return;
    FILE* f = fopen(SETTINGS_CFG_PATH, "rb");
    if(!f) return;
    char line[112];
    if(fgets(line, sizeof(line), f)) {
        unsigned rw = 1, to = 2, br = 1, th = 0, wl = 42, wh = 130, ul = 36, uh = 160, gl = 1;
        int n = sscanf(line, "CFG|%u|%u|%u|%u|%u|%u|%u|%u|%u", &rw, &to, &br, &th, &wl, &wh, &ul, &uh, &gl);
        if(n >= 4) {
            s_cfg_raise_wake  = (rw != 0U);
            s_cfg_timeout_idx = (uint8_t)(to % 7U);
            s_cfg_bright_idx  = (uint8_t)(br % 5U);
            s_cfg_theme_idx   = (uint8_t)(th % 6U);
        }
        if(n >= 8) {
            s_cfg_warn_low_tenths  = (uint16_t)wl;
            s_cfg_warn_high_tenths = (uint16_t)wh;
            s_cfg_urg_low_tenths   = (uint16_t)ul;
            s_cfg_urg_high_tenths  = (uint16_t)uh;
        }
        if(n >= 9) {
            s_cfg_glitch_fx = (gl != 0U);
        }
    }
    fclose(f);
}

static uint16_t cycleThresholdTable(uint16_t cur, const uint16_t* tbl, size_t len) {
    for(size_t i = 0; i < len; i++) {
        if(tbl[i] == cur) {
            return tbl[(i + 1U) % len];
        }
    }
    return tbl[0];
}

static void triggerGlucoseAlerts(uint16_t mmol_tenths) {
    unsigned long now = millis();
    if(s_alarm_snoozed && (int32_t)(now - s_alarm_snooze_until_ms) >= 0) {
        s_alarm_snoozed = false;
    }

    bool isUrgLow  = (s_cfg_urg_low_tenths > 0U && mmol_tenths <= s_cfg_urg_low_tenths);
    bool isUrgHigh = (s_cfg_urg_high_tenths > 0U && mmol_tenths >= s_cfg_urg_high_tenths);
    bool isWarnLow  = (s_cfg_warn_low_tenths > 0U && mmol_tenths <= s_cfg_warn_low_tenths);
    bool isWarnHigh = (s_cfg_warn_high_tenths > 0U && mmol_tenths >= s_cfg_warn_high_tenths);

    if(!isUrgLow && !isUrgHigh) {
        if(s_alarm_latched) {
            s_alarm_latched = false;
            setBuzzerTone(0U);
            setFlightDeckLeds(false);
        }
        s_alarm_snoozed = false;
    }

    if(isUrgLow || isUrgHigh) {
        s_alarm_is_low = isUrgLow;
        s_alarm_mmol_tenths = mmol_tenths;
        if(!s_alarm_snoozed) {
            s_alarm_latched = true;
            wakeScreen("urgent_alarm");
            Serial.printf("ALARM_TRIGGERED|type=%s|mmol=%u.%u\n",
                          isUrgLow ? "URG_LOW" : "URG_HIGH",
                          (unsigned)(mmol_tenths / 10U), (unsigned)(mmol_tenths % 10U));
            return;
        }
        // Snoozed (audio paused for 15m): still flash flight-deck LEDs for 3s
        s_led_mode = LED_MODE_FLASH_3S;
        s_led_start_ms = now;
        s_led_until_ms = now + 3000UL;
        return;
    }

    if(isWarnLow || isWarnHigh) {
        // 3 seconds of flashing all 6 flight-deck lights
        s_led_mode = LED_MODE_FLASH_3S;
        s_led_start_ms = now;
        s_led_until_ms = now + 3000UL;
        Serial.printf("ALERT_FLASH_3S|type=%s|mmol=%u.%u\n",
                      isWarnLow ? "WARN_LOW" : "WARN_HIGH",
                      (unsigned)(mmol_tenths / 10U), (unsigned)(mmol_tenths % 10U));
        return;
    }

    // Normal range update: 1 subtle blink (18ms pulse) on flight-deck LEDs
    s_led_mode = LED_MODE_SUBTLE_BLINK;
    s_led_start_ms = now;
    s_led_until_ms = now + 18UL;
    setFlightDeckLeds(true);
}

static void acknowledgeUrgentAlarm() {
    if(!s_alarm_latched) return;
    s_alarm_latched = false;
    s_alarm_snoozed = true;
    s_alarm_snooze_until_ms = millis() + (15UL * 60UL * 1000UL);
    setBuzzerTone(0U);
    setFlightDeckLeds(false);
    s_led_mode = LED_MODE_IDLE;
    Serial.println("ALARM_ACK|snooze_min=15");
    showPopup("ALARM SNOOZED", "Audio paused 15 min.", "Flight deck LEDs", "remain active.", 2500U);
}

static void pollFlightDeckAndAlarms() {
    unsigned long now = millis();
    if(s_alarm_latched) {
        s_last_activity_ms = now; // Keep screen awake while unacknowledged alarm sounds
        uint32_t phase = (uint32_t)((now - s_led_start_ms) % 1000UL);
        bool beep1 = (phase < 140UL);
        bool beep2 = (phase >= 220UL && phase < 360UL);
        bool beep3 = (phase >= 440UL && phase < 600UL);
        if(beep1 || beep3) {
            setBuzzerTone(2700U);
            setFlightDeckLeds(true);
        } else if(beep2) {
            setBuzzerTone(3200U);
            setFlightDeckLeds(true);
        } else {
            setBuzzerTone(0U);
            setFlightDeckLeds(false);
        }
        return;
    }

    setBuzzerTone(0U);

    if(s_led_mode == LED_MODE_FLASH_3S) {
        if((int32_t)(now - s_led_until_ms) >= 0) {
            setFlightDeckLeds(false);
            s_led_mode = LED_MODE_IDLE;
        } else {
            bool on = ((((now - s_led_start_ms) / 125UL) % 2UL) == 0UL);
            setFlightDeckLeds(on);
        }
    } else if(s_led_mode == LED_MODE_SUBTLE_BLINK) {
        if((int32_t)(now - s_led_until_ms) >= 0) {
            setFlightDeckLeds(false);
            s_led_mode = LED_MODE_IDLE;
        } else {
            setFlightDeckLeds(true);
        }
    } else {
        setFlightDeckLeds(false);
    }
}

// LSM6DS3TR-C IMU at I2C 0x6A
#define LSM6DS3_ADDR 0x6A
static bool imuWriteReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(LSM6DS3_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission(true) == 0;
}

static bool imuReadRegs(uint8_t reg, uint8_t* out, size_t len) {
    Wire.beginTransmission(LSM6DS3_ADDR);
    Wire.write(reg);
    if(Wire.endTransmission(false) != 0) return false;
    if(Wire.requestFrom((int)LSM6DS3_ADDR, (int)len) != (int)len) return false;
    for(size_t i = 0; i < len; i++) out[i] = (uint8_t)Wire.read();
    return true;
}

static void initLsm6ds3Imu() {
    Wire.begin(s_pins.i2cSda, s_pins.i2cScl, 400000U);
    uint8_t who = 0;
    if(imuReadRegs(0x0F, &who, 1) && (who == 0x6A || who == 0x69)) {
        // CTRL3_C (0x12) = 0x44 (BDU + auto-inc), CTRL1_XL (0x10) = 0x44 (104Hz, 16g)
        imuWriteReg(0x12, 0x44);
        imuWriteReg(0x10, 0x44);
        s_imu_ready = true;
        Serial.printf("IMU|LSM6DS3TR-C ready at 0x6A (WHO=0x%02X, bump/raise-to-wake active)\n", who);
    } else {
        Serial.printf("IMU|LSM6DS3TR-C not found (WHO=0x%02X)\n", who);
    }
}

static void pollImuAndSleepTimeout() {
    unsigned long now = millis();
    uint32_t timeoutMs = kTimeoutMsTable[s_cfg_timeout_idx % 7U];
    if(!s_screen_asleep && timeoutMs > 0UL && (now - s_last_activity_ms > timeoutMs)) {
        s_screen_asleep = true;
        setBacklightPercent(0U);
        Serial.printf("SCREEN_SLEEP|timeout=%s\n", kTimeoutLabels[s_cfg_timeout_idx % 7U]);
    }
    if(!s_imu_ready || !s_cfg_raise_wake || (now - s_last_imu_poll_ms < 40U)) return;
    s_last_imu_poll_ms = now;

    uint8_t raw[6];
    if(!imuReadRegs(0x28, raw, 6)) return;
    int16_t ax_raw = (int16_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8));
    int16_t ay_raw = (int16_t)((uint16_t)raw[2] | ((uint16_t)raw[3] << 8));
    int16_t az_raw = (int16_t)((uint16_t)raw[4] | ((uint16_t)raw[5] << 8));

    // 16g full-scale: 0.488 mg/LSB => ~2048 LSB/g
    float ax = (float)ax_raw * 0.000488f;
    float ay = (float)ay_raw * 0.000488f;
    float az = (float)az_raw * 0.000488f;
    float mag = sqrtf(ax * ax + ay * ay + az * az);
    s_mag_filtered = 0.55f * s_mag_filtered + 0.45f * mag;
    float dev = fabsf(s_mag_filtered - 1.0f);

    bool is_viewing = (az > 0.30f && fabsf(ay) < 0.85f && fabsf(ax) < 0.85f);
    bool is_down    = (az < 0.15f || fabsf(ay) > 0.70f);

    if(s_screen_asleep) {
        if(dev > 0.12f) {
            wakeScreen("bump");
        } else if(s_wrist_down && is_viewing) {
            s_wrist_down = false;
            wakeScreen("raise");
        }
    } else {
        if(dev > 0.18f) s_last_activity_ms = now;
        if(is_down) s_wrist_down = true;
        else if(is_viewing) s_wrist_down = false;
    }
}

// ─── 4-BUTTON INPUT (ACTIVE-HIGH WITH PULLDOWN) ───────────
static bool s_btn_prev[4] = { false, false, false, false };
static unsigned long s_last_btn_ms = 0;

static void initButtons() {
    const int bpins[4] = { s_pins.btnUp, s_pins.btnDown, s_pins.btnSelect, s_pins.btnAlt };
    for(int i = 0; i < 4; i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << bpins[i]),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type    = GPIO_INTR_DISABLE
        };
        gpio_config(&cfg);
        s_btn_prev[i] = (gpio_get_level((gpio_num_t)bpins[i]) == 1);
    }
}

// ─── SPIFFS STORAGE & HISTORY PERSISTENCE ─────────────────
static void loadHistoryFromSpiffs() {
    if(!s_spiffs_mounted) return;
    FILE* f = fopen(HISTORY_CSV_PATH, "r");
    if(!f) return;
    char line[96];
    while(fgets(line, sizeof(line), f)) {
        LlHistoryPoint pt;
        if(ll_history_parse_csv_line(line, &pt)) {
            ll_history_add(&s_history, pt.serial, pt.age_minutes, pt.mmol_tenths, pt.mg_dl, pt.boot_sec);
        }
    }
    fclose(f);
}

static void appendHistoryToSpiffs(const LlHistoryPoint* pt) {
    if(!s_spiffs_mounted || !pt) return;
    char line[96];
    size_t n = ll_history_format_csv_line(pt, line, sizeof(line));
    if(n == 0U) return;
    FILE* f = fopen(HISTORY_CSV_PATH, "a");
    if(!f) return;
    fwrite(line, 1, n, f);
    fflush(f);
    fclose(f);
}

static void saveCaptureToSpiffs(LcCapture* cap) {
    if(!s_spiffs_mounted || !cap) {
        snprintf(saveStatus, sizeof(saveStatus), "SPIFFS not mounted");
        return;
    }
    cap->save_timestamp = (uint32_t)(millis() / 1000U);
    cap->save_time_recorded = true;
    static char jsonBuf[LC_JSON_CAPACITY];
    static char verBuf[LC_JSON_CAPACITY];
    size_t len = lc_capture_json(cap, jsonBuf, sizeof(jsonBuf));
    if(len == 0U) {
        snprintf(saveStatus, sizeof(saveStatus), "JSON encode err");
        return;
    }
    snprintf(savedFileName, sizeof(savedFileName), "/spiffs/last_cap.json");
    FILE* f = fopen(savedFileName, "wb");
    if(!f) {
        snprintf(saveStatus, sizeof(saveStatus), "SPIFFS open err");
        return;
    }
    size_t written = fwrite(jsonBuf, 1, len, f);
    fflush(f);
    fclose(f);
    if(written != len) {
        unlink(savedFileName);
        snprintf(saveStatus, sizeof(saveStatus), "SPIFFS write short");
        return;
    }
    FILE* vf = fopen(savedFileName, "rb");
    size_t rlen = vf ? fread(verBuf, 1, len, vf) : 0U;
    if(vf) fclose(vf);
    if(rlen == len && memcmp(jsonBuf, verBuf, len) == 0) {
        snprintf(saveStatus, sizeof(saveStatus), "Saved + verified");
    } else {
        snprintf(saveStatus, sizeof(saveStatus), "Verify mismatch");
    }
}

static void saveFullHistoryToSpiffs() {
    if(!s_spiffs_mounted) return;
    FILE* f = fopen(HISTORY_CSV_PATH, "wb");
    if(!f) return;
    char line[96];
    for(size_t i = 0; i < s_history.count; i++) {
        size_t n = ll_history_format_csv_line(&s_history.points[i], line, sizeof(line));
        if(n > 0U) fwrite(line, 1, n, f);
    }
    fflush(f);
    fclose(f);
}

// ─── DIRECT LIBRE 2 BLE CLIENT ────────────────────────────
static bool s_ble_enabled = false;
static bool s_ble_stack_inited = false;
static bool s_ble_connected = false;
static bool s_ble_do_connect = false;
static volatile bool s_ble_scanning = false;
static bool s_ble_has_mac = false;
static uint8_t s_ble_target_mac[6] = {0};
static uint32_t s_ble_unlock_code = 42U;
static uint16_t s_ble_unlock_count = 0U;
static uint32_t s_ble_packets_ok = 0U;
static char s_ble_status[32] = "Standby (NFC Read-Only)";
static LcVendorConfig s_ble_cal_cfg;
static bool s_ble_cal_valid = false;

static uint8_t s_ble_frag_buf[LL_BLE_RAW_PACKET_LEN];
static size_t s_ble_frag_len = 0U;
static unsigned long s_ble_last_frag_ms = 0U;
static volatile bool s_ble_packet_ready = false;
static unsigned long s_ble_last_scan_ms = 0U;

static BLEAdvertisedDevice* s_ble_adv_dev = nullptr;
static BLEClient* s_ble_client = nullptr;

static void onLibre2BleNotify(BLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
    (void)pChar;
    (void)isNotify;
    Serial.printf("BLE_F002_CHUNK|len=%u|total=%u\n", (unsigned)length, (unsigned)(s_ble_frag_len + length));
    unsigned long now = millis();
    if(now - s_ble_last_frag_ms > 2500U) {
        s_ble_frag_len = 0U;
    }
    s_ble_last_frag_ms = now;
    if(s_ble_frag_len + length <= LL_BLE_RAW_PACKET_LEN) {
        memcpy(&s_ble_frag_buf[s_ble_frag_len], pData, length);
        s_ble_frag_len += length;
        if(s_ble_frag_len == LL_BLE_RAW_PACKET_LEN) {
            s_ble_packet_ready = true;
            s_ble_frag_len = 0U;
        }
    } else {
        s_ble_frag_len = 0U;
    }
}

static volatile int s_last_disc_reason = 0;
static volatile int s_last_write_status = -1;
static volatile bool s_ble_connecting = false;
static unsigned long s_ble_conn_start_ms = 0U;
static uint16_t s_hdl_f001 = 0x0023U; // Verified 0xF001 login handle on sensor
static uint16_t s_hdl_f002 = 0x0026U; // Verified 0xF002 data handle on sensor
static uint16_t s_hdl_cccd = 0x0027U; // Verified 0x2902 CCCD handle on sensor
static uint8_t s_cand_login12[4][LL_BLE_LOGIN_PAYLOAD_LEN];
static uint8_t s_cand_idx = 0U;
static uint8_t s_wake_try = 0U;
static bool s_cccd_sent_after_login = false;

static void myGattcEventHandler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t* param) {
    if(event == ESP_GATTC_OPEN_EVT) {
        s_ble_conn_start_ms = millis();
        Serial.printf("GATTC_OPEN|status=0x%X|conn_id=%u|dt=0ms\n",
                      (unsigned)param->open.status, (unsigned)param->open.conn_id);
        if(param->open.status != ESP_GATT_OK) {
            s_ble_connected = false;
            s_ble_connecting = false;
        }
    } else if(event == ESP_GATTC_WRITE_DESCR_EVT) {
        Serial.printf("GATTC_WRITE_DESCR|hdl=0x%X|status=0x%X|dt=%lums\n",
                      (unsigned)param->write.handle, (unsigned)param->write.status,
                      (unsigned long)(millis() - s_ble_conn_start_ms));
        // Waveshare order: once 0x2902 CCCD is enabled (at dt=~520ms), immediately write 0xF001 login!
        if(!s_cccd_sent_after_login && s_cand_idx == 0U) {
            s_cand_idx = 1U;
            esp_ble_gattc_write_char(
                gattc_if,
                param->write.conn_id,
                s_hdl_f001,
                LL_BLE_LOGIN_PAYLOAD_LEN,
                s_cand_login12[0],
                ESP_GATT_WRITE_TYPE_RSP,
                ESP_GATT_AUTH_REQ_NONE);
        }
    } else if(event == ESP_GATTC_WRITE_CHAR_EVT) {
        s_last_write_status = (int)param->write.status;
        Serial.printf("GATTC_WRITE_CHAR|hdl=0x%X|try=%u|status=0x%X|dt=%lums\n",
                      (unsigned)param->write.handle, (unsigned)s_cand_idx, (unsigned)param->write.status,
                      (unsigned long)(millis() - s_ble_conn_start_ms));
        if(param->write.handle == s_hdl_f001) {
            if(param->write.status == ESP_GATT_OK) {
                // Login accepted! Advance monotonic unlock_count and mark bond dirty for immediate SPIFFS save
                s_ble_unlock_count = (uint16_t)(s_ble_unlock_count + 1U);
                s_ble_bond_dirty = true;
                s_cccd_sent_after_login = true;
                uint8_t cccdOn[2] = { 0x01, 0x00 };
                esp_ble_gattc_write_char_descr(
                    gattc_if,
                    param->write.conn_id,
                    s_hdl_cccd,
                    sizeof(cccdOn),
                    cccdOn,
                    ESP_GATT_WRITE_TYPE_RSP,
                    ESP_GATT_AUTH_REQ_NONE);
            } else {
                // Sensor rejected b = enable_time + unlock_count (replay protection if b <= last_b).
                // Bump unlock_count by +5 and persist so the next wake is guaranteed > last_b!
                s_ble_unlock_count = (uint16_t)(s_ble_unlock_count + 5U);
                s_ble_bond_dirty = true;
                if(s_cand_idx < 4U) {
                // Candidate rejected with 0x85; try next candidate immediately while still in 4.3s window!
                uint8_t nextIdx = s_cand_idx++;
                esp_ble_gattc_write_char(
                    gattc_if,
                    param->write.conn_id,
                    s_hdl_f001,
                    LL_BLE_LOGIN_PAYLOAD_LEN,
                    s_cand_login12[nextIdx],
                    ESP_GATT_WRITE_TYPE_RSP,
                    ESP_GATT_AUTH_REQ_NONE);
                }
            }
        }
    } else if(event == ESP_GATTC_NOTIFY_EVT) {
        onLibre2BleNotify(nullptr, param->notify.value, param->notify.value_len, param->notify.is_notify);
    } else if(event == ESP_GATTC_DISCONNECT_EVT) {
        s_last_disc_reason = (int)param->disconnect.reason;
        s_ble_connected = false;
        s_ble_connecting = false;
        Serial.printf("GATTC_DISC|reason=0x%X|dt=%lums\n",
                      (unsigned)param->disconnect.reason,
                      (unsigned long)(millis() - s_ble_conn_start_ms));
    }
}

class Libre2ClientCallbacks : public BLEClientCallbacks {
    void onConnect(BLEClient* pclient) override {
        (void)pclient;
        s_ble_connected = true;
        snprintf(s_ble_status, sizeof(s_ble_status), "Connected (0xFDE3)");
    }
    void onDisconnect(BLEClient* pclient) override {
        (void)pclient;
        s_ble_connected = false;
        s_ble_connecting = false;
        if(s_ble_enabled) {
            snprintf(s_ble_status, sizeof(s_ble_status), "Waiting 60s wake...");
        }
    }
};

class Libre2AdvertisedCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        if(!s_ble_enabled || s_ble_connected || s_ble_do_connect || s_ble_connecting) return;
        bool match = false;
        std::string addrObj = advertisedDevice.getAddress().toString();
        const char* adv_addr = addrObj.c_str();
        if(s_ble_has_mac) {
            char macFwd[20], macRev[20];
            snprintf(macFwd, sizeof(macFwd), "%02x:%02x:%02x:%02x:%02x:%02x",
                     s_ble_target_mac[0], s_ble_target_mac[1], s_ble_target_mac[2],
                     s_ble_target_mac[3], s_ble_target_mac[4], s_ble_target_mac[5]);
            snprintf(macRev, sizeof(macRev), "%02x:%02x:%02x:%02x:%02x:%02x",
                     s_ble_target_mac[5], s_ble_target_mac[4], s_ble_target_mac[3],
                     s_ble_target_mac[2], s_ble_target_mac[1], s_ble_target_mac[0]);
            if(strcasecmp(adv_addr, macFwd) == 0 || strcasecmp(adv_addr, macRev) == 0) {
                match = true;
            }
        } else if(advertisedDevice.haveServiceUUID() &&
                  advertisedDevice.isAdvertisingService(BLEUUID(LL_BLE_SERVICE_UUID))) {
            match = true;
        } else if(advertisedDevice.haveName() &&
                  strncmp(advertisedDevice.getName().c_str(), "ABB", 3) == 0) {
            match = true;
        }
        if(match) {
            s_ble_connecting = true;
            Serial.printf("BLE_MATCH|addr=%s|rssi=%d\n", adv_addr, advertisedDevice.getRSSI());
            BLEDevice::getScan()->stop();
            s_ble_scanning = false;
            if(s_ble_adv_dev) delete s_ble_adv_dev;
            s_ble_adv_dev = new BLEAdvertisedDevice(advertisedDevice);
            s_ble_do_connect = true;
        }
    }
};

static void saveBleBondToSpiffs();

static bool connectAndLoginLibre2Ble() {
    if(!s_ble_adv_dev || !compState.has_scan) {
        s_ble_connecting = false;
        return false;
    }
    if(!s_ble_client) {
        s_ble_client = BLEDevice::createClient();
        s_ble_client->setClientCallbacks(new Libre2ClientCallbacks());
    }

    if(s_ble_unlock_count == 0U) s_ble_unlock_count = 1U;
    uint16_t baseCnt = s_ble_unlock_count;
    ll_libre2_build_ble_login_payload(compState.capture.uid, compState.capture.patch, s_ble_unlock_code, baseCnt,        s_cand_login12[0]);
    ll_libre2_build_ble_login_payload(compState.capture.uid, compState.capture.patch, s_ble_unlock_code, baseCnt + 4U,   s_cand_login12[1]);
    ll_libre2_build_ble_login_payload(compState.capture.uid, compState.capture.patch, s_ble_unlock_code, baseCnt + 12U,  s_cand_login12[2]);
    ll_libre2_build_ble_login_payload(compState.capture.uid, compState.capture.patch, s_ble_unlock_code, baseCnt + 32U,  s_cand_login12[3]);
    s_cand_idx = 1U;
    s_cccd_sent_after_login = false;

    // CRITICAL FIX: Set preferred BLE connection interval to 7.5ms (6 * 1.25ms) BEFORE connect()!
    // Default Bluedroid interval is 50ms (40 * 1.25ms), which makes 21 GATT discovery round-trips
    // take 1051ms and hit the Libre 2 sensor's 1150ms handshake timeout (reason=0x13 / status=19)!
    esp_ble_gap_set_prefer_conn_params(*s_ble_adv_dev->getAddress().getNative(), 6, 8, 0, 400);

    char logHex[25];
    lc_hex(s_cand_login12[0], 12U, logHex, sizeof(logHex));
    Serial.printf("BLE_CONNECTING|addr=%s|cnt=%u|login=%s|heap=%u\n",
                  s_ble_adv_dev->getAddress().toString().c_str(),
                  (unsigned)baseCnt, logHex, (unsigned)ESP.getFreeHeap());

    if(!s_ble_client->connect(s_ble_adv_dev)) {
        s_ble_connecting = false;
        snprintf(s_ble_status, sizeof(s_ble_status), "BLE connect failed");
        return false;
    }

    // Populate Bluedroid's local GATTC cache (at 7.5ms conn_interval, this finishes in ~160ms instead of 1051ms!)
    s_ble_client->getServices();
    BLERemoteService* pSvc = s_ble_client->getService(BLEUUID(LL_BLE_SERVICE_UUID));
    if(pSvc) {
        BLERemoteCharacteristic* pLogin = pSvc->getCharacteristic(BLEUUID(LL_BLE_CHAR_LOGIN_UUID));
        BLERemoteCharacteristic* pData  = pSvc->getCharacteristic(BLEUUID(LL_BLE_CHAR_DATA_UUID));
        if(pLogin && pData && s_ble_client->isConnected()) {
            s_hdl_f001 = pLogin->getHandle();
            s_hdl_f002 = pData->getHandle();
            BLERemoteDescriptor* pDescData = pData->getDescriptor(BLEUUID((uint16_t)0x2902));
            if(pDescData) s_hdl_cccd = pDescData->getHandle();
            Serial.printf("BLE_SVC_READY|f001=0x%X|f002=0x%X|cccd=0x%X|dt=%lums\n",
                          (unsigned)s_hdl_f001, (unsigned)s_hdl_f002, (unsigned)s_hdl_cccd,
                          (unsigned long)(millis() - s_ble_conn_start_ms));
            // Align with Waveshare order: register notify & enable 0x2902 CCCD on 0xF002 FIRST,
            // then write 12-byte login to 0xF001 when 0x2902 completes (~520ms, well before 1200ms)!
            s_cand_idx = 0U;
            s_cccd_sent_after_login = false;
            esp_ble_gattc_register_for_notify(
                s_ble_client->getGattcIf(),
                *s_ble_client->getPeerAddress().getNative(),
                s_hdl_f002);
            uint8_t cccdOn[2] = { 0x01, 0x00 };
            esp_ble_gattc_write_char_descr(
                s_ble_client->getGattcIf(),
                s_ble_client->getConnId(),
                s_hdl_cccd,
                sizeof(cccdOn),
                cccdOn,
                ESP_GATT_WRITE_TYPE_RSP,
                ESP_GATT_AUTH_REQ_NONE);
        }
    }

    s_ble_connecting = false;
    snprintf(s_ble_status, sizeof(s_ble_status), "Streaming 1m active");
    return true;
}

static void saveBleBondToSpiffs() {
    if(!s_spiffs_mounted || !s_ble_has_mac || !compState.has_scan) return;
    char uid_hex[17], patch_hex[13], mac_hex[13];
    if(!lc_hex(compState.capture.uid, 8U, uid_hex, sizeof(uid_hex)) ||
       !lc_hex(compState.capture.patch, 6U, patch_hex, sizeof(patch_hex)) ||
       !lc_hex(s_ble_target_mac, 6U, mac_hex, sizeof(mac_hex))) return;
    FILE* f = fopen(BLE_BOND_PATH, "wb");
    if(!f) return;
    fprintf(f, "LL2_BLE|%s|%s|%s|%lu|%u\n",
            uid_hex, patch_hex, mac_hex,
            (unsigned long)s_ble_unlock_code,
            (unsigned)s_ble_unlock_count);
    fflush(f);
    fclose(f);
}

static void saveLastFrameToSpiffs(const uint8_t uid[8], const uint8_t patch[6], const uint8_t fram[LC_FRAM_SIZE]) {
    if(!s_spiffs_mounted) return;
    static char line[LL_BRIDGE_LINE_MAX];
    if(ll_bridge_format_frame(uid, patch, fram, line, sizeof(line)) == 0U) return;
    FILE* f = fopen(LAST_SCAN_PATH, "wb");
    if(!f) return;
    fwrite(line, 1, strlen(line), f);
    fflush(f);
    fclose(f);
}

static void initBleStackIfNeeded() {
    if(s_ble_stack_inited) return;
    BLEDevice::init("LibreLibre-Art");
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_CONN_HDL0, ESP_PWR_LVL_P9);
    BLEDevice::setCustomGattcHandler(myGattcEventHandler);
    BLEScan* pScan = BLEDevice::getScan();
    pScan->setAdvertisedDeviceCallbacks(new Libre2AdvertisedCallbacks(), true);
    pScan->setActiveScan(true);
    pScan->setInterval(100);
    pScan->setWindow(100);
    s_ble_stack_inited = true;
}

static void toggleBlePairing() {
    if(s_ble_enabled) {
        s_ble_enabled = false;
        s_ble_do_connect = false;
        if(s_ble_client && s_ble_connected) s_ble_client->disconnect();
        s_ble_connected = false;
        snprintf(s_ble_status, sizeof(s_ble_status), "Standby (Read-Only)");
        return;
    }
    if(!compState.has_scan) {
        showPopup("Scan Required", "1. Scan Libre 2 first", "   via NFC/USB.", "2. Then press SELECT.", 3500U);
        return;
    }
    initBleStackIfNeeded();
    s_ble_enabled = true;
    s_ble_last_scan_ms = millis();
    if(s_ble_has_mac) {
        snprintf(s_ble_status, sizeof(s_ble_status), "MAC %02X:%02X:%02X:%02X:%02X:%02X",
                 s_ble_target_mac[0], s_ble_target_mac[1], s_ble_target_mac[2],
                 s_ble_target_mac[3], s_ble_target_mac[4], s_ble_target_mac[5]);
        showPopup("BLE Armed", "Listening for sensor", s_ble_status, "60s stream active", 2500U);
    } else {
        snprintf(s_ble_status, sizeof(s_ble_status), "Listening 0xFDE3...");
        showPopup("BLE Scanner On", "Listening for 0xFDE3", "Send LL2_BLE| or", "scan with BLE pair", 2800U);
    }
}

// ─── 128x128 COLOR HUD RENDERING ──────────────────────────
// ─── PCF8563 / BM8563 HARDWARE I2C RTC (0x51) & TREND ARROW ──
#define PCF8563_ADDR 0x51
static bool s_rtc_valid = false;

static uint8_t bcd2dec(uint8_t bcd) { return (uint8_t)(((bcd >> 4) * 10U) + (bcd & 0x0FU)); }
static uint8_t dec2bcd(uint8_t dec) { return (uint8_t)(((dec / 10U) << 4) | (dec % 10U)); }

static void initPcf8563Rtc() {
    Wire.beginTransmission(PCF8563_ADDR);
    Wire.write(0x00);
    Wire.write(0x00);
    Wire.write(0x00);
    if(Wire.endTransmission(true) == 0) {
        s_rtc_valid = true;
        Serial.println("RTC|PCF8563 ready at 0x51");
    } else {
        Serial.println("RTC|PCF8563 not found at 0x51");
    }
}

struct WatchDateTime {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t wday;
    uint8_t hour;
    uint8_t min;
    uint8_t sec;
};

static bool readPcf8563Rtc(WatchDateTime* dt) {
    if(!dt) return false;
    Wire.beginTransmission(PCF8563_ADDR);
    Wire.write(0x02);
    if(Wire.endTransmission(false) != 0) return false;
    if(Wire.requestFrom(PCF8563_ADDR, 7) != 7) return false;
    uint8_t d[7];
    for(int i = 0; i < 7; i++) d[i] = (uint8_t)Wire.read();
    dt->sec   = bcd2dec(d[0] & 0x7FU);
    dt->min   = bcd2dec(d[1] & 0x7FU);
    dt->hour  = bcd2dec(d[2] & 0x3FU);
    dt->day   = bcd2dec(d[3] & 0x3FU);
    dt->wday  = bcd2dec(d[4] & 0x07U);
    dt->month = bcd2dec(d[5] & 0x1FU);
    dt->year  = (uint16_t)(2000U + bcd2dec(d[6]));
    if(dt->month < 1U || dt->month > 12U || dt->day < 1U || dt->day > 31U || dt->hour > 23U || dt->min > 59U) {
        return false;
    }
    return true;
}

static bool writePcf8563Rtc(const WatchDateTime& dt) {
    Wire.beginTransmission(PCF8563_ADDR);
    Wire.write(0x02);
    Wire.write(dec2bcd(dt.sec % 60U) & 0x7FU);
    Wire.write(dec2bcd(dt.min % 60U) & 0x7FU);
    Wire.write(dec2bcd(dt.hour % 24U) & 0x3FU);
    Wire.write(dec2bcd(dt.day) & 0x3FU);
    Wire.write(dec2bcd(dt.wday % 7U) & 0x07U);
    Wire.write(dec2bcd(dt.month) | 0x80U);
    Wire.write(dec2bcd((uint8_t)(dt.year % 100U)));
    return Wire.endTransmission(true) == 0;
}

enum TrendDir {
    TREND_UNKNOWN = 0,
    TREND_FALL_FAST,
    TREND_FALL,
    TREND_FALL_SLOW,
    TREND_FLAT,
    TREND_RISE_SLOW,
    TREND_RISE,
    TREND_RISE_FAST
};

// Computes glucose rate of change over the last 5..18 sensor minutes from s_history
static TrendDir computeTrendDir(int* delta_tenths_15m_out) {
    if(delta_tenths_15m_out) *delta_tenths_15m_out = 0;
    if(!compState.has_scan || !compState.vendor_chain.available || s_history.count < 2U) {
        return TREND_UNKNOWN;
    }
    const char* activeSerial = compState.serial_valid ? compState.serial : NULL;
    static const LlHistoryPoint* pts[LL_HISTORY_MAX_POINTS];
    uint16_t qMinAge = 0, qMaxAge = 0, qMinMmol = 0, qMaxMmol = 0;
    size_t n = ll_history_query_window(&s_history, activeSerial, 30U, pts, LL_HISTORY_MAX_POINTS, &qMinAge, &qMaxAge, &qMinMmol, &qMaxMmol);
    if(n < 2U) return TREND_FLAT;

    const LlHistoryPoint* newest = pts[n - 1U];
    // Find a reference point ~8..16 minutes earlier (or earliest in the 30m window)
    const LlHistoryPoint* ref = pts[0];
    for(size_t i = 0; i + 1U < n; i++) {
        uint16_t dt = (newest->age_minutes > pts[i]->age_minutes) ? (newest->age_minutes - pts[i]->age_minutes) : 0U;
        if(dt >= 5U && dt <= 16U) {
            ref = pts[i];
            break;
        }
    }
    int dtMin = (int)newest->age_minutes - (int)ref->age_minutes;
    if(dtMin <= 0) return TREND_FLAT;

    int dTenths = (int)newest->mmol_tenths - (int)ref->mmol_tenths;
    // Normalize to 15-minute delta in mmol_tenths (e.g. +9 => +0.9 mmol/L per 15m => +0.06 mmol/L/min)
    int d15 = (dTenths * 15) / dtMin;
    if(delta_tenths_15m_out) *delta_tenths_15m_out = d15;

    if(d15 >= 17)  return TREND_RISE_FAST; // >= +1.7 mmol/L per 15m (~2 mg/dL/min)
    if(d15 >= 9)   return TREND_RISE;      // >= +0.9 mmol/L per 15m (~1 mg/dL/min)
    if(d15 >= 5)   return TREND_RISE_SLOW; // >= +0.5 mmol/L per 15m (~0.5 mg/dL/min)
    if(d15 <= -17) return TREND_FALL_FAST;
    if(d15 <= -9)  return TREND_FALL;
    if(d15 <= -5)  return TREND_FALL_SLOW;
    return TREND_FLAT;
}

static void drawThickLine(int x0, int y0, int x1, int y1, uint16_t col) {
    s_canvas.drawLine(x0, y0, x1, y1, col);
    s_canvas.drawLine(x0 + 1, y0, x1 + 1, y1, col);
    s_canvas.drawLine(x0, y0 + 1, x1, y1 + 1, col);
}

static void drawTrendArrow(int cx, int cy, TrendDir dir, uint16_t col) {
    switch(dir) {
        case TREND_FLAT: // →
            drawThickLine(cx - 10, cy, cx + 8, cy, col);
            s_canvas.fillTriangle(cx + 11, cy, cx + 3, cy - 6, cx + 3, cy + 6, col);
            break;
        case TREND_RISE_SLOW: // ↗
            drawThickLine(cx - 8, cy + 8, cx + 6, cy - 6, col);
            s_canvas.fillTriangle(cx + 9, cy - 9, cx + 0, cy - 8, cx + 8, cy - 0, col);
            break;
        case TREND_FALL_SLOW: // ↘
            drawThickLine(cx - 8, cy - 8, cx + 6, cy + 6, col);
            s_canvas.fillTriangle(cx + 9, cy + 9, cx + 0, cy + 8, cx + 8, cy + 0, col);
            break;
        case TREND_RISE: // ↑
            drawThickLine(cx, cy + 10, cx, cy - 8, col);
            s_canvas.fillTriangle(cx, cy - 11, cx - 6, cy - 3, cx + 6, cy - 3, col);
            break;
        case TREND_FALL: // ↓
            drawThickLine(cx, cy - 10, cx, cy + 8, col);
            s_canvas.fillTriangle(cx, cy + 11, cx - 6, cy + 3, cx + 6, cy + 3, col);
            break;
        case TREND_RISE_FAST: // ↑↑
            for(int dx = -5; dx <= 5; dx += 10) {
                drawThickLine(cx + dx, cy + 10, cx + dx, cy - 8, col);
                s_canvas.fillTriangle(cx + dx, cy - 11, cx + dx - 5, cy - 3, cx + dx + 5, cy - 3, col);
            }
            break;
        case TREND_FALL_FAST: // ↓↓
            for(int dx = -5; dx <= 5; dx += 10) {
                drawThickLine(cx + dx, cy - 10, cx + dx, cy + 8, col);
                s_canvas.fillTriangle(cx + dx, cy + 11, cx + dx - 5, cy + 3, cx + dx + 5, cy + 3, col);
            }
            break;
        default:
            s_canvas.drawLine(cx - 6, cy, cx + 6, cy, COL_MUTED);
            break;
    }
}

static void renderHomePage128() {
    static const char* const kWdays[7] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
    static const char* const kMonths[12] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };

    // 6 Dark Cyberpunk Home Palettes (Zero #FFFFFF pure white anywhere):
    // 0: CP2077   (Cyberpunk 2077 Game HUD: Cyber Yellow + Relic Red shadow + Kiroshi Cyan accents)
    // 1: CYAN     (Cyber Teal/Cyan + Magenta shadow/accents)
    // 2: MAGENTA  (Neon Hot Magenta + Electric Cyan shadow/accents)
    // 3: MATRIX   (Phosphor Matrix Green + Deep Emerald shadow/accents)
    // 4: AMBER    (Cyber Gold Amber + Deep Rust shadow/accents)
    // 5: CRIMSON  (Stealth Night Red + Dark Amber shadow/accents)
    static const uint16_t kPalPrimary[6] = { 0xCE41, 0x0576, 0xD89B, 0x1668, 0xD502, 0xC882 };
    static const uint16_t kPalShadow[6]  = { 0x9002, 0x500A, 0x0250, 0x0262, 0x5140, 0x3800 };
    static const uint16_t kPalAccent1[6] = { 0x0599, 0x0451, 0xB816, 0x04A4, 0xB3E0, 0x9802 };
    static const uint16_t kPalAccent2[6] = { 0xC882, 0x8811, 0x0576, 0x0388, 0x9182, 0x8260 };

    const uint8_t th        = s_cfg_theme_idx % 6U;
    const uint16_t cBg      = 0x0000;
    const uint16_t cTime    = kPalPrimary[th];
    const uint16_t cShadow  = kPalShadow[th];
    const uint16_t cCyan    = kPalAccent1[th];
    const uint16_t cMagenta = kPalAccent2[th];
    const uint16_t cAmber   = 0xB3E0;
    const uint16_t cGreen   = 0x0568;
    const uint16_t cRed     = 0xA802;
    const uint16_t cDim     = 0x18C3;

    s_canvas.fillScreen(cBg);

    WatchDateTime dt = { 2026, 10, 16, 5, 0, 0, 0 };
    bool haveRtc = readPcf8563Rtc(&dt);

    if(th == 0U) {
        // CP2077 Kiroshi HUD notched frame + chamfered yellow/red/cyan telemetry bar
        s_canvas.fillRect(0, 0, 4, 16, cTime);
        s_canvas.drawFastHLine(0, 0, 74, cTime);
        s_canvas.drawLine(74, 0, 80, 3, cTime);
        s_canvas.drawFastHLine(80, 3, 47, cMagenta);
        s_canvas.drawFastVLine(127, 3, 18, cMagenta);
        s_canvas.drawFastHLine(0, 127, 94, cCyan);
        s_canvas.drawLine(94, 127, 100, 123, cCyan);
        s_canvas.drawFastHLine(100, 123, 27, cTime);
        s_canvas.drawFastVLine(0, 110, 17, cCyan);
        s_canvas.drawFastVLine(127, 108, 16, cTime);
    } else {
        // Cyberpunk corner brackets
        s_canvas.drawFastHLine(0, 0, 14, cMagenta);
        s_canvas.drawFastVLine(0, 0, 14, cMagenta);
        s_canvas.drawFastHLine(114, 0, 14, cMagenta);
        s_canvas.drawFastVLine(127, 0, 14, cMagenta);
        s_canvas.drawFastHLine(0, 127, 14, cCyan);
        s_canvas.drawFastVLine(0, 113, 14, cCyan);
        s_canvas.drawFastHLine(114, 127, 14, cCyan);
        s_canvas.drawFastVLine(127, 113, 14, cCyan);
    }

    // Top row: Date + subtle status dot
    s_canvas.setTextSize(1);
    s_canvas.setTextColor(th == 0U ? cTime : cCyan);
    s_canvas.setCursor(7, 5);
    if(haveRtc) {
        s_canvas.printf("%s %02u %s", kWdays[dt.wday % 7U], dt.day, kMonths[(dt.month - 1U) % 12U]);
    } else {
        s_canvas.print("NC // 2077");
    }
    if(th == 0U) {
        s_canvas.setTextColor(cMagenta);
        s_canvas.setCursor(82, 6);
        s_canvas.print("v2.77");
    }
    uint16_t bleDot = s_ble_connected ? cGreen : (s_ble_enabled ? cAmber : cDim);
    s_canvas.fillCircle(120, 9, 2, bleDot);

    // Big Cyberpunk Time (HH:MM in TextSize 4 = 120px wide)
    uint8_t hh = haveRtc ? dt.hour : (uint8_t)(((millis() / 1000U) / 3600U) % 24U);
    uint8_t mm = haveRtc ? dt.min  : (uint8_t)(((millis() / 1000U) / 60U) % 60U);
    s_canvas.setTextSize(4);
    if(s_glitch_frame_active) {
        // Chromatic aberration RGB split during glitch frame
        s_canvas.setTextColor(0xA802);
        s_canvas.setCursor(3, 21);
        s_canvas.printf("%02u:%02u", hh, mm);
        s_canvas.setTextColor(0x0599);
        s_canvas.setCursor(8, 21);
        s_canvas.printf("%02u:%02u", hh, mm);
    }
    s_canvas.setTextColor(cShadow);
    s_canvas.setCursor(7, 23);
    s_canvas.printf("%02u:%02u", hh, mm);
    s_canvas.setTextColor(cTime);
    s_canvas.setCursor(5, 21);
    s_canvas.printf("%02u:%02u", hh, mm);

    // Cyber angular divider (y = 60..65)
    if(th == 0U) {
        s_canvas.fillRect(6, 59, 22, 3, cTime);
        s_canvas.drawFastHLine(28, 60, 56, cMagenta);
        s_canvas.drawLine(84, 60, 89, 65, cMagenta);
        s_canvas.drawFastHLine(89, 65, 33, cCyan);
    } else {
        s_canvas.drawFastHLine(6, 61, 82, cCyan);
        s_canvas.drawLine(88, 61, 92, 65, cCyan);
        s_canvas.drawFastHLine(92, 65, 30, cMagenta);
    }

    // Bottom section: Glucose + Trend Arrow ONLY
    if(!compState.has_scan) {
        s_canvas.setTextSize(2);
        s_canvas.setTextColor(cAmber);
        s_canvas.setCursor(10, 82);
        s_canvas.print("--.-");
        drawTrendArrow(104, 90, TREND_UNKNOWN, cDim);
    } else if(compState.vendor_chain.available) {
        uint16_t mmolT = (uint16_t)compState.vendor_chain.mmol_tenths;
        uint16_t gCol = (mmolT < 39U) ? cRed : (mmolT > 100U ? cAmber : cGreen);

        s_canvas.setTextSize(3);
        s_canvas.setTextColor(gCol);
        s_canvas.setCursor(8, 78);
        s_canvas.printf("%u.%u", mmolT / 10U, mmolT % 10U);

        s_canvas.setTextSize(1);
        s_canvas.setTextColor(cCyan);
        s_canvas.setCursor(8, 108);
        s_canvas.print("MMOL/L");

        int d15 = 0;
        TrendDir tdir = computeTrendDir(&d15);
        s_canvas.drawRect(86, 74, 34, 34, cCyan);
        s_canvas.drawFastHLine(86, 74, 8, cMagenta);
        s_canvas.drawFastVLine(86, 74, 8, cMagenta);
        drawTrendArrow(103, 91, tdir, gCol);
    } else {
        s_canvas.setTextSize(2);
        s_canvas.setTextColor(cRed);
        s_canvas.setCursor(8, 84);
        s_canvas.print("RESCAN");
    }
}

static void applyKiroshiGlitchToCanvas() {
    if(!s_cfg_glitch_fx || !s_glitch_frame_active) return;
    uint16_t* buf = (uint16_t*)s_canvas.getBuffer();
    if(!buf) return;
    uint32_t seed = (uint32_t)millis();
    // 4 horizontal scanline tear slices + cyan/red edge aberration
    static const uint8_t kBandY[4] = { 18, 36, 76, 94 };
    static const uint8_t kBandH[4] = { 6,  7,  6,  5  };
    for(int b = 0; b < 4; b++) {
        seed = seed * 1664525UL + 1013904223UL;
        int shift = (int)((seed >> 28) & 0x07) - 3;
        if(shift == 0) shift = (b & 1) ? 4 : -4;
        int y0 = (int)kBandY[b] + (int)((seed >> 24) & 0x03);
        int h  = (int)kBandH[b];
        for(int y = y0; y < y0 + h && y < LCD_H; y++) {
            uint16_t* row = buf + (y * LCD_W);
            if(shift > 0) {
                for(int x = LCD_W - 1; x >= shift; x--) row[x] = row[x - shift];
                for(int x = 0; x < shift; x++) row[x] = 0x0000;
            } else {
                int s = -shift;
                for(int x = 0; x < LCD_W - s; x++) row[x] = row[x + s];
                for(int x = LCD_W - s; x < LCD_W; x++) row[x] = 0x0000;
            }
        }
        // Kiroshi glitch slice accent bars (Cyan & Relic Red)
        uint16_t glitchCol = (b & 1) ? 0x0599 : 0xC882;
        s_canvas.drawFastHLine((shift > 0) ? 4 : 78, y0, 36, glitchCol);
    }
}

static void renderSettingsPage128() {
    static const uint16_t kPalPrimary[6] = { 0xCE41, 0x0576, 0xD89B, 0x1668, 0xD502, 0xC882 };
    static const uint16_t kPalAccent2[6] = { 0xC882, 0x8811, 0x0576, 0x0388, 0x9182, 0x8260 };
    const uint8_t th = s_cfg_theme_idx % 6U;
    const uint16_t cPrimary = kPalPrimary[th];
    const uint16_t cAccent  = kPalAccent2[th];

    // Cyberpunk corner brackets around settings card
    s_canvas.drawFastHLine(2, 18, 10, cAccent);
    s_canvas.drawFastVLine(2, 18, 10, cAccent);
    s_canvas.drawFastHLine(116, 18, 10, cAccent);
    s_canvas.drawFastVLine(125, 18, 10, cAccent);
    s_canvas.drawFastHLine(2, 125, 10, cPrimary);
    s_canvas.drawFastVLine(2, 115, 10, cPrimary);
    s_canvas.drawFastHLine(116, 125, 10, cPrimary);
    s_canvas.drawFastVLine(125, 115, 10, cPrimary);

    char bWarnLow[12], bWarnHigh[12], bUrgLow[12], bUrgHigh[12];
    if(s_cfg_warn_low_tenths == 0U) snprintf(bWarnLow, sizeof(bWarnLow), "OFF");
    else snprintf(bWarnLow, sizeof(bWarnLow), "%u.%u", s_cfg_warn_low_tenths / 10U, s_cfg_warn_low_tenths % 10U);

    if(s_cfg_warn_high_tenths == 0U) snprintf(bWarnHigh, sizeof(bWarnHigh), "OFF");
    else snprintf(bWarnHigh, sizeof(bWarnHigh), "%u.%u", s_cfg_warn_high_tenths / 10U, s_cfg_warn_high_tenths % 10U);

    if(s_cfg_urg_low_tenths == 0U) snprintf(bUrgLow, sizeof(bUrgLow), "OFF");
    else snprintf(bUrgLow, sizeof(bUrgLow), "%u.%u", s_cfg_urg_low_tenths / 10U, s_cfg_urg_low_tenths % 10U);

    if(s_cfg_urg_high_tenths == 0U) snprintf(bUrgHigh, sizeof(bUrgHigh), "OFF");
    else snprintf(bUrgHigh, sizeof(bUrgHigh), "%u.%u", s_cfg_urg_high_tenths / 10U, s_cfg_urg_high_tenths % 10U);

    const char* labels[9] = {
        "RAISE WAKE",
        "TIMEOUT",
        "BRIGHTNESS",
        "HOME COLOR",
        "GLITCH FX",
        "LED <= LOW",
        "LED >= HIGH",
        "ALM <= LOW",
        "ALM >= HIGH"
    };
    const char* values[9] = {
        s_cfg_raise_wake ? "ON" : "OFF",
        kTimeoutLabels[s_cfg_timeout_idx % 7U],
        kBrightLabels[s_cfg_bright_idx % 5U],
        kThemeLabels[s_cfg_theme_idx % 6U],
        s_cfg_glitch_fx ? "ON" : "OFF",
        bWarnLow,
        bWarnHigh,
        bUrgLow,
        bUrgHigh
    };

    uint8_t winStart = (s_settings_row >= 4U) ? (uint8_t)(s_settings_row - 3U) : 0U;
    for(uint8_t vis = 0; vis < 4U; vis++) {
        uint8_t r = (uint8_t)(winStart + vis);
        int ry = 22 + (int)vis * 25;
        bool sel = (s_settings_row == r);
        if(sel) {
            s_canvas.fillRoundRect(5, ry, 115, 22, 3, COL_HEADER_BG);
            s_canvas.drawRoundRect(5, ry, 115, 22, 3, cPrimary);
            s_canvas.fillRect(5, ry, 3, 22, cAccent);
        } else {
            s_canvas.fillRoundRect(5, ry, 115, 22, 3, COL_CARD_BG);
        }
        s_canvas.setTextSize(1);
        s_canvas.setTextColor(sel ? cPrimary : COL_MUTED);
        s_canvas.setCursor(10, ry + 7);
        s_canvas.print(labels[r]);

        uint16_t vCol = (r == 3U) ? cPrimary : ((r >= 7U) ? COL_RED : (sel ? COL_AMBER : COL_WHITE));
        int vw = (int)strlen(values[r]) * 6;
        s_canvas.setTextColor(vCol);
        s_canvas.setCursor(117 - vw, ry + 7);
        s_canvas.print(values[r]);
    }
    // Cyberpunk vertical scrollbar track (x = 123, y = 22..118)
    s_canvas.drawFastVLine(123, 22, 96, COL_GRID);
    int thumbY = 22 + ((int)s_settings_row * 82) / 8;
    s_canvas.fillRect(122, thumbY, 3, 14, cAccent);
}

static void renderGraphPage128() {
    const int gx = 22, gy = 18, gw = 104, gh = 92;
    s_canvas.fillRect(gx, gy, gw, gh, COL_CARD_BG);
    s_canvas.drawRect(gx, gy, gw, gh, COL_GRID);

    const char* activeSerial = (compState.has_scan && compState.serial_valid) ? compState.serial : NULL;
    static const LlHistoryPoint* pts[LL_HISTORY_MAX_POINTS];
    uint16_t winMin = kZoomMinutes[s_zoom_idx];
    uint16_t qMinAge = 0, qMaxAge = 0, qMinMmol = 0, qMaxMmol = 0;
    size_t n = ll_history_query_window(&s_history, activeSerial, winMin, pts, LL_HISTORY_MAX_POINTS, &qMinAge, &qMaxAge, &qMinMmol, &qMaxMmol);

    if(n == 0U) {
        s_canvas.setTextSize(1);
        s_canvas.setTextColor(COL_MUTED);
        s_canvas.setCursor(gx + 8, gy + 36);
        s_canvas.print("No points in window");
        s_canvas.setTextColor(COL_CYAN);
        s_canvas.setCursor(4, 116);
        s_canvas.printf("SEL:zoom(%s) v:page", kZoomLabels[s_zoom_idx]);
        return;
    }

    uint16_t minV = 35U, maxV = 110U;
    for(size_t i = 0; i < n; i++) {
        if(pts[i]->mmol_tenths < minV) minV = pts[i]->mmol_tenths;
        if(pts[i]->mmol_tenths > maxV) maxV = pts[i]->mmol_tenths;
    }
    minV = (minV > 5U) ? (minV - 5U) : 20U;
    maxV = maxV + 5U;
    if(maxV <= minV + 20U) maxV = minV + 20U;

    // Target band 3.9 .. 10.0 mmol/L
    auto mapY = [&](uint16_t v) -> int {
        if(v <= minV) return gy + gh - 2;
        if(v >= maxV) return gy + 2;
        return (gy + gh - 2) - (int)(((uint32_t)(v - minV) * (uint32_t)(gh - 4)) / (uint32_t)(maxV - minV));
    };
    int y100 = mapY(100U);
    int y39  = mapY(39U);
    if(y39 > y100) {
        s_canvas.fillRect(gx + 1, y100, gw - 2, y39 - y100, COL_TARGET_BAND);
    }

    // Y-axis labels
    s_canvas.setTextSize(1);
    s_canvas.setTextColor(COL_MUTED);
    uint16_t midV = (uint16_t)((minV + maxV) / 2U);
    s_canvas.setCursor(1, gy + 1);
    s_canvas.printf("%u.%u", maxV / 10U, maxV % 10U);
    s_canvas.setCursor(1, gy + (gh / 2) - 3);
    s_canvas.printf("%u.%u", midV / 10U, midV % 10U);
    s_canvas.setCursor(1, gy + gh - 8);
    s_canvas.printf("%u.%u", minV / 10U, minV % 10U);

    uint16_t newestMin = pts[n - 1U]->age_minutes;
    uint16_t oldestMin = (winMin > 0U && newestMin > winMin) ? (newestMin - winMin) : pts[0]->age_minutes;
    uint16_t spanMin = (newestMin > oldestMin) ? (newestMin - oldestMin) : 1U;

    int prevX = -1, prevY = -1;
    for(size_t i = 0; i < n; i++) {
        uint16_t age = pts[i]->age_minutes;
        uint32_t dx = (age > oldestMin) ? (age - oldestMin) : 0U;
        int px = (n == 1U) ? (gx + gw / 2) : (gx + 3 + (int)((dx * (uint32_t)(gw - 6)) / spanMin));
        int py = mapY(pts[i]->mmol_tenths);
        if(prevX >= 0) {
            s_canvas.drawLine(prevX, prevY, px, py, COL_CYAN);
        }
        uint16_t dotCol = (pts[i]->mmol_tenths < 39U) ? COL_RED : (pts[i]->mmol_tenths > 100U ? COL_AMBER : COL_GREEN);
        s_canvas.fillCircle(px, py, 2, dotCol);
        prevX = px;
        prevY = py;
    }

    s_canvas.setTextColor(COL_CYAN);
    s_canvas.setCursor(2, 116);
    s_canvas.printf("SEL:%s(%u) v:next", kZoomLabels[s_zoom_idx], (unsigned)n);
}

static void renderDisplay() {
    s_canvas.fillScreen(COL_BG);
    s_canvas.setTextWrap(false);

    if(currentPage == PAGE_HOME) {
        renderHomePage128();
        applyKiroshiGlitchToCanvas();
        drawPopupOverlay();
        s_canvas.pushSprite(0, 0);
        return;
    }

    // Header (128x15)
    s_canvas.fillRect(0, 0, LCD_W, 15, COL_HEADER_BG);
    s_canvas.setTextSize(1);
    s_canvas.setTextColor(COL_CYAN);
    s_canvas.setCursor(3, 4);
    s_canvas.print("LibreLibre");

    s_canvas.setTextColor(COL_WHITE);
    s_canvas.setCursor(74, 4);
    s_canvas.printf("Pg %d/%d", (int)currentPage + 1, (int)PAGE_COUNT);

    if(currentPage == PAGE_SETTINGS) {
        renderSettingsPage128();
        drawPopupOverlay();
        s_canvas.pushSprite(0, 0);
        return;
    }

    if(currentPage == PAGE_GRAPH) {
        renderGraphPage128();
        drawPopupOverlay();
        s_canvas.pushSprite(0, 0);
        return;
    }

    if(currentPage == PAGE_BLE) {
        s_canvas.fillRoundRect(3, 18, 122, 92, 5, COL_CARD_BG);
        s_canvas.setTextSize(1);
        s_canvas.setTextColor(COL_CYAN);
        s_canvas.setCursor(8, 23);
        s_canvas.print("Direct Sensor BLE");

        s_canvas.setTextColor(s_ble_connected ? COL_GREEN : (s_ble_enabled ? COL_AMBER : COL_MUTED));
        s_canvas.setCursor(8, 37);
        s_canvas.print(s_ble_status);

        s_canvas.setTextColor(COL_WHITE);
        s_canvas.setCursor(8, 51);
        s_canvas.printf("S/N: %s", (compState.has_scan && compState.serial_valid) ? compState.serial : "none");

        s_canvas.setCursor(8, 65);
        if(s_ble_has_mac) {
            s_canvas.printf("MAC %02X:%02X:%02X:%02X:%02X:%02X",
                            s_ble_target_mac[0], s_ble_target_mac[1], s_ble_target_mac[2],
                            s_ble_target_mac[3], s_ble_target_mac[4], s_ble_target_mac[5]);
        } else {
            s_canvas.print("MAC: awaiting 0x1E");
        }

        s_canvas.setCursor(8, 79);
        s_canvas.printf("1m pkts ok: %lu", (unsigned long)s_ble_packets_ok);

        s_canvas.setTextColor(COL_AMBER);
        s_canvas.setCursor(8, 95);
        s_canvas.print(s_ble_enabled ? "SEL: Stop BLE" : "SEL: Pair/Start BLE");

        s_canvas.setTextColor(COL_MUTED);
        s_canvas.setCursor(4, 116);
        s_canvas.print("SEL:toggle  v:main");
        drawPopupOverlay();
        s_canvas.pushSprite(0, 0);
        return;
    }

    if(!compState.has_scan) {
        s_canvas.fillRoundRect(3, 18, 122, 92, 5, COL_CARD_BG);
        s_canvas.setTextSize(1);
        s_canvas.setTextColor(COL_WHITE);
        s_canvas.setCursor(8, 28);
        s_canvas.print("Waiting for scan...");
        s_canvas.setTextColor(COL_MUTED);
        s_canvas.setCursor(8, 46);
        s_canvas.print("Real values only");
        s_canvas.setCursor(8, 62);
        s_canvas.print(saveStatus);
        s_canvas.setTextColor(COL_CYAN);
        s_canvas.setCursor(8, 84);
        s_canvas.printf("HW Rev %u (ST7735S)", (unsigned)s_pins.rev);
        s_canvas.setCursor(4, 116);
        s_canvas.print("UP/DN:page SEL:graph");
        drawPopupOverlay();
        s_canvas.pushSprite(0, 0);
        return;
    }

    if(currentPage == PAGE_MAIN) {
        s_canvas.fillRoundRect(3, 18, 122, 52, 5, COL_CARD_BG);
        if(compState.vendor_chain.available) {
            s_canvas.setTextSize(2);
            s_canvas.setTextColor(COL_GREEN);
            s_canvas.setCursor(8, 25);
            s_canvas.print(compState.primary_line);

            s_canvas.setTextSize(1);
            s_canvas.setTextColor(COL_MUTED);
            s_canvas.setCursor(8, 50);
            s_canvas.printf("(%lu mg/dL) [%s]", (unsigned long)compState.vendor_chain.mg_dl, lastTransport);
        } else {
            s_canvas.setTextSize(2);
            s_canvas.setTextColor(COL_RED);
            s_canvas.setCursor(8, 25);
            s_canvas.print("Rescan");
            s_canvas.setTextSize(1);
            s_canvas.setTextColor(COL_AMBER);
            s_canvas.setCursor(8, 50);
            s_canvas.print(compState.diag_line1);
        }

        s_canvas.setTextSize(1);
        s_canvas.setTextColor(COL_WHITE);
        s_canvas.setCursor(4, 76);
        s_canvas.print(compState.vendor_chain.available ? compState.fresh_life_line : "Freshness unknown");

        s_canvas.setTextColor(COL_CYAN);
        s_canvas.setCursor(4, 90);
        s_canvas.print(compState.serial_line);

        s_canvas.setTextColor(COL_GREEN);
        s_canvas.setCursor(4, 103);
        s_canvas.print(saveStatus);

        s_canvas.setTextColor(COL_MUTED);
        s_canvas.setCursor(4, 116);
        s_canvas.print("SEL:graph  UP/DN:page");
    } else if(currentPage == PAGE_DIAG) {
        s_canvas.fillRoundRect(3, 18, 122, 92, 5, COL_CARD_BG);
        s_canvas.setTextSize(1);
        s_canvas.setTextColor(COL_AMBER);
        s_canvas.setCursor(8, 24);
        s_canvas.print("Sensor Diagnostics");

        s_canvas.setTextColor(COL_WHITE);
        s_canvas.setCursor(8, 42);
        s_canvas.print(compState.diag_line1);
        s_canvas.setCursor(8, 56);
        s_canvas.print(compState.diag_line2);
        s_canvas.setCursor(8, 70);
        s_canvas.print(compState.diag_line3);

        s_canvas.setTextColor(COL_MUTED);
        s_canvas.setCursor(8, 88);
        s_canvas.print(saveStatus);
        s_canvas.setCursor(4, 116);
        s_canvas.print("UP/DN:page  BACK:main");
    } else {
        s_canvas.fillRoundRect(3, 18, 122, 92, 5, COL_CARD_BG);
        s_canvas.setTextSize(1);
        s_canvas.setTextColor(COL_CYAN);
        s_canvas.setCursor(8, 24);
        s_canvas.print("UID wire LSB-first");

        s_canvas.setTextColor(COL_WHITE);
        s_canvas.setCursor(8, 42);
        s_canvas.print(compState.uid_hex);

        s_canvas.setTextColor(COL_GREEN);
        s_canvas.setCursor(8, 58);
        s_canvas.print(compState.serial_line);

        s_canvas.setTextColor(COL_MUTED);
        s_canvas.setCursor(8, 76);
        s_canvas.printf("History: %u pts", (unsigned)s_history.count);
        s_canvas.setCursor(8, 90);
        s_canvas.print(savedFileName[0] ? savedFileName + 8 : saveStatus);
        s_canvas.setCursor(4, 116);
        s_canvas.print("UP/DN:page  BACK:main");
    }

    drawPopupOverlay();
    s_canvas.pushSprite(0, 0);
}

static void notifyResultSummary(const char* source) {
    char reply[220];
    if(compState.vendor_chain.available) {
        snprintf(reply, sizeof(reply),
                 "OK|src=%s|mmol=%lu.%lu|mgdl=%lu|serial=%s|age=%u|life=%u|pts=%u|sd=%s\n",
                 source,
                 (unsigned long)(compState.vendor_chain.mmol_tenths / 10U),
                 (unsigned long)(compState.vendor_chain.mmol_tenths % 10U),
                 (unsigned long)compState.vendor_chain.mg_dl,
                 compState.serial_valid ? compState.serial : "NA",
                 compState.integrity.sensor.age_minutes,
                 compState.integrity.sensor.max_life_minutes,
                 (unsigned)s_history.count,
                 saveStatus);
    } else {
        snprintf(reply, sizeof(reply),
                 "RESCAN|src=%s|diag=%s|serial=%s|age=%u|life=%u|pts=%u|sd=%s\n",
                 source,
                 compState.diag_line1,
                 compState.serial_valid ? compState.serial : "NA",
                 compState.integrity.sensor.age_minutes,
                 compState.integrity.sensor.max_life_minutes,
                 (unsigned)s_history.count,
                 saveStatus);
    }
    Serial.print(reply);
}

static void processCompletedScan(const uint8_t uid[8], const uint8_t patch[6], const uint8_t fram[LC_FRAM_SIZE], const char* source) {
    ll_companion_evaluate(uid, patch, fram, &compState);
    {
        static uint8_t plain[LC_FRAM_SIZE];
        lc_integrity_transform(uid, patch, fram, plain);
        s_ble_cal_valid = lc_vendor_config_from_fram(
            plain, LC_FACTORY_DIABLE_T2, LC_FACTORY_DIABLE_T1, LC_VENDOR_SLOPE_TABLE, &s_ble_cal_cfg);
        if(compState.integrity.status == LcIntegrityPass && compState.serial_valid) {
            static LcVendorGraphSeries fseries;
            if(lc_vendor_extract_fram_series(plain, &fseries)) {
                uint32_t bSec = (uint32_t)(millis() / 1000U);
                for(uint8_t k = 0; k < fseries.count; k++) {
                    ll_history_add(
                        &s_history,
                        compState.serial,
                        fseries.points[k].sensor_minute,
                        fseries.points[k].mmol_tenths,
                        fseries.points[k].mg_dl,
                        bSec);
                }
            }
        }
    }
    uint32_t bootSec = (uint32_t)(millis() / 1000U);
    compState.capture.timestamp = bootSec;
    saveCaptureToSpiffs(&compState.capture);
    saveLastFrameToSpiffs(uid, patch, fram);
    saveFullHistoryToSpiffs();

    if(compState.vendor_chain.available && compState.serial_valid) {
        bool isNewPoint = ll_history_add(
            &s_history,
            compState.serial,
            compState.integrity.sensor.age_minutes,
            (uint16_t)compState.vendor_chain.mmol_tenths,
            (uint16_t)compState.vendor_chain.mg_dl,
            bootSec);
        (void)isNewPoint;
    }

    lastScanMillis = millis();
    scanCount++;
    snprintf(lastTransport, sizeof(lastTransport), "%s", source);
    wakeScreen("scan");
    if(compState.vendor_chain.available) {
        triggerGlucoseAlerts((uint16_t)compState.vendor_chain.mmol_tenths);
    }
    currentPage = PAGE_HOME;
    renderDisplay();
    notifyResultSummary(source);
}

static void handleIncomingLine(const char* line, const char* source) {
    if(strncmp(line, "LL2_BLE|", 8) == 0) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%s", line + 8);
        char* t_uid = strtok(buf, "|\r\n");
        char* t_pat = strtok(NULL, "|\r\n");
        char* t_mac = strtok(NULL, "|\r\n");
        char* t_cod = strtok(NULL, "|\r\n");
        char* t_cnt = strtok(NULL, "|\r\n");
        if(t_uid && t_pat && strlen(t_pat) == 12U && t_mac && strlen(t_mac) == 12U) {
            for(size_t i = 0; i < 6U; i++) {
                char p1 = t_pat[i * 2U], p2 = t_pat[i * 2U + 1U];
                int pv1 = (p1 >= '0' && p1 <= '9') ? (p1 - '0') : (p1 >= 'A' && p1 <= 'F') ? (p1 - 'A' + 10) : (p1 >= 'a' && p1 <= 'f') ? (p1 - 'a' + 10) : 0;
                int pv2 = (p2 >= '0' && p2 <= '9') ? (p2 - '0') : (p2 >= 'A' && p2 <= 'F') ? (p2 - 'A' + 10) : (p2 >= 'a' && p2 <= 'f') ? (p2 - 'a' + 10) : 0;
                compState.capture.patch[i] = (uint8_t)((pv1 << 4) | pv2);
                compState.patch[i] = compState.capture.patch[i];

                char c1 = t_mac[i * 2U], c2 = t_mac[i * 2U + 1U];
                int v1 = (c1 >= '0' && c1 <= '9') ? (c1 - '0') : (c1 >= 'A' && c1 <= 'F') ? (c1 - 'A' + 10) : (c1 >= 'a' && c1 <= 'f') ? (c1 - 'a' + 10) : 0;
                int v2 = (c2 >= '0' && c2 <= '9') ? (c2 - '0') : (c2 >= 'A' && c2 <= 'F') ? (c2 - 'A' + 10) : (c2 >= 'a' && c2 <= 'f') ? (c2 - 'a' + 10) : 0;
                s_ble_target_mac[i] = (uint8_t)((v1 << 4) | v2);
            }
            s_ble_has_mac = true;
            s_ble_unlock_code = t_cod ? (uint32_t)strtoul(t_cod, NULL, 10) : 42U;
            s_ble_unlock_count = t_cnt ? (uint16_t)strtoul(t_cnt, NULL, 10) : 6U;
            initBleStackIfNeeded();
            s_ble_enabled = true;
            snprintf(s_ble_status, sizeof(s_ble_status), "MAC %02X:%02X:%02X:%02X:%02X:%02X",
                     s_ble_target_mac[0], s_ble_target_mac[1], s_ble_target_mac[2],
                     s_ble_target_mac[3], s_ble_target_mac[4], s_ble_target_mac[5]);
            saveBleBondToSpiffs();
            renderDisplay();
            Serial.println("OK|LL2_BLE_ARMED");
        }
        return;
    }
    if(strncmp(line, "BLE46|", 6) == 0) {
        const char* hex = line + 6;
        if(strlen(hex) == 92U && compState.has_scan) {
            uint8_t enc46[LL_BLE_RAW_PACKET_LEN];
            for(size_t i = 0; i < 46U; i++) {
                char c1 = hex[i * 2U], c2 = hex[i * 2U + 1U];
                int v1 = (c1 >= '0' && c1 <= '9') ? (c1 - '0') : (c1 >= 'A' && c1 <= 'F') ? (c1 - 'A' + 10) : (c1 >= 'a' && c1 <= 'f') ? (c1 - 'a' + 10) : -1;
                int v2 = (c2 >= '0' && c2 <= '9') ? (c2 - '0') : (c2 >= 'A' && c2 <= 'F') ? (c2 - 'A' + 10) : (c2 >= 'a' && c2 <= 'f') ? (c2 - 'a' + 10) : -1;
                if(v1 < 0 || v2 < 0) return;
                enc46[i] = (uint8_t)((v1 << 4) | v2);
            }
            memcpy(s_ble_frag_buf, enc46, LL_BLE_RAW_PACKET_LEN);
            s_ble_packet_ready = true;
        }
        return;
    }
    if(strncmp(line, "TIME|", 5) == 0) {
        unsigned y = 2026, mo = 10, d = 16, wd = 5, h = 0, mi = 0, s = 0;
        if(sscanf(line + 5, "%u|%u|%u|%u|%u|%u|%u", &y, &mo, &d, &wd, &h, &mi, &s) == 7) {
            WatchDateTime dt = { (uint16_t)y, (uint8_t)mo, (uint8_t)d, (uint8_t)wd, (uint8_t)h, (uint8_t)mi, (uint8_t)s };
            writePcf8563Rtc(dt);
            renderDisplay();
            Serial.println("OK|RTC_SET");
        }
        return;
    }
    if(strcmp(line, "PING") == 0 || strcmp(line, "ping") == 0) {
        Serial.println("PONG|LibreLibre-ESP32-CircuitMess-Artemis");
        return;
    }
    if(strncmp(line, "TEST_ALARM|", 11) == 0) {
        uint16_t tMmol = (uint16_t)strtoul(line + 11, NULL, 10);
        triggerGlucoseAlerts(tMmol);
        renderDisplay();
        Serial.printf("OK|TEST_ALARM|mmol=%u.%u\n", (unsigned)(tMmol / 10U), (unsigned)(tMmol % 10U));
        return;
    }
    if(strcmp(line, "ACK_ALARM") == 0) {
        acknowledgeUrgentAlarm();
        return;
    }
    static uint8_t uid[8], patch[6], fram[LC_FRAM_SIZE];
    LlBridgeParseStatus st = ll_bridge_parse_line(line, uid, patch, fram);
    if(st == LlBridgeOk) {
        processCompletedScan(uid, patch, fram, source);
    } else if(st != LlBridgeBadPrefix) {
        Serial.printf("ERR|src=%s|code=%d\n", source, (int)st);
    }
}

static void pollUsbSerial() {
    while(Serial.available() > 0) {
        int ch = Serial.read();
        if(ch < 0) break;
        if(ch == '\n' || ch == '\r') {
            if(usbLen > 0) {
                usbBuf[usbLen] = 0;
                handleIncomingLine(usbBuf, "USB");
                usbLen = 0;
            }
        } else if(usbLen + 1U < sizeof(usbBuf)) {
            usbBuf[usbLen++] = (char)ch;
        } else {
            usbLen = 0;
        }
    }
}

void setup() {
    gpio_pullup_dis(GPIO_NUM_37);
    gpio_pulldown_dis(GPIO_NUM_37);
    gpio_set_direction(GPIO_NUM_37, GPIO_MODE_INPUT);
    gpio_pullup_dis(GPIO_NUM_42);
    gpio_pulldown_dis(GPIO_NUM_42);
    gpio_set_direction(GPIO_NUM_42, GPIO_MODE_INPUT);
    Serial.begin(115200);
    detectArtemisHardware();
    initButtons();

    memset(&compState, 0, sizeof(compState));
    ll_history_init(&s_history);

    s_display.initWithPins(s_pins);
    s_canvas.setPsram(false);
    s_canvas.setColorDepth(16);
    void* sp = s_canvas.createSprite(LCD_W, LCD_H);
    Serial.printf("DISPLAY_INIT|rev=%u|sprite=%p\n", (unsigned)s_pins.rev, sp);
    if(SPIFFS.begin(true, STORAGE_DIR, 8)) {
        s_spiffs_mounted = true;
        loadSettingsFromSpiffs();
        loadHistoryFromSpiffs();
        snprintf(saveStatus, sizeof(saveStatus), "SPIFFS ready (%u pts)", (unsigned)s_history.count);
    } else {
        snprintf(saveStatus, sizeof(saveStatus), "SPIFFS mount err");
    }

    initBacklight();
    initPiezoBuzzer();
    setBacklightPercent(kBrightPctTable[s_cfg_bright_idx % 5U]);

    initLsm6ds3Imu();
    initPcf8563Rtc();
    s_last_activity_ms = millis();

    if(s_spiffs_mounted) {
        static char rline[LL_BRIDGE_LINE_MAX];
        FILE* lf = fopen(LAST_SCAN_PATH, "rb");
        if(lf) {
            if(fgets(rline, sizeof(rline), lf)) {
                size_t l = strlen(rline);
                while(l > 0 && (rline[l - 1] == '\n' || rline[l - 1] == '\r')) rline[--l] = 0;
                uint8_t uid[8], patch[6], fram[LC_FRAM_SIZE];
                if(ll_bridge_parse_line(rline, uid, patch, fram) == LlBridgeOk) {
                    ll_companion_evaluate(uid, patch, fram, &compState);
                    uint8_t plain[LC_FRAM_SIZE];
                    lc_integrity_transform(uid, patch, fram, plain);
                    s_ble_cal_valid = lc_vendor_config_from_fram(
                        plain, LC_FACTORY_DIABLE_T2, LC_FACTORY_DIABLE_T1, LC_VENDOR_SLOPE_TABLE, &s_ble_cal_cfg);
                    // Override with the newest persisted point in s_history (/spiffs/scans.csv) if newer than last_frame.txt!
                    if(compState.serial_valid && s_history.count > 0U) {
                        const LlHistoryPoint* newestPt = NULL;
                        for(size_t hi = 0; hi < s_history.count; hi++) {
                            if(strcmp(s_history.points[hi].serial, compState.serial) == 0) {
                                if(!newestPt || s_history.points[hi].age_minutes >= newestPt->age_minutes) {
                                    newestPt = &s_history.points[hi];
                                }
                            }
                        }
                        if(newestPt && newestPt->age_minutes >= compState.integrity.sensor.age_minutes) {
                            compState.vendor_chain.available = true;
                            compState.vendor_chain.mmol_tenths = newestPt->mmol_tenths;
                            compState.vendor_chain.mg_dl = newestPt->mg_dl;
                            compState.integrity.sensor.age_minutes = newestPt->age_minutes;
                            snprintf(compState.primary_line, sizeof(compState.primary_line), "%u.%u mmol/L",
                                     (unsigned)(newestPt->mmol_tenths / 10U),
                                     (unsigned)(newestPt->mmol_tenths % 10U));
                        }
                    }
                    snprintf(lastTransport, sizeof(lastTransport), "FLS");
                    snprintf(saveStatus, sizeof(saveStatus), "Restored (%u pts)", (unsigned)s_history.count);
                }
            }
            fclose(lf);
        }
        FILE* bf = fopen(BLE_BOND_PATH, "rb");
        if(bf) {
            if(fgets(rline, sizeof(rline), bf)) {
                size_t l = strlen(rline);
                while(l > 0 && (rline[l - 1] == '\n' || rline[l - 1] == '\r')) rline[--l] = 0;
                handleIncomingLine(rline, "FLS");
            }
            fclose(bf);
        }
    }

    renderDisplay();
    Serial.printf("READY|LibreLibre-ESP32-Artemis|HWRev=%u|LCD=128x128_ST7735S|HEAP=%u\n",
                  (unsigned)s_pins.rev, (unsigned)ESP.getFreeHeap());
}

void loop() {
    pollUsbSerial();
    pollImuAndSleepTimeout();
    pollFlightDeckAndAlarms();

    if(s_popup_active && (int32_t)(millis() - s_popup_until_ms) >= 0) {
        s_popup_active = false;
        renderDisplay();
    }

    const int bpins[4] = { s_pins.btnUp, s_pins.btnDown, s_pins.btnSelect, s_pins.btnAlt };
    for(int i = 0; i < 4; i++) {
        bool cur = (gpio_get_level((gpio_num_t)bpins[i]) == 1);
        if(cur && !s_btn_prev[i] && (millis() - s_last_btn_ms > 180U)) {
            s_last_btn_ms = millis();
            s_last_activity_ms = millis();
            if(s_alarm_latched) {
                acknowledgeUrgentAlarm();
            } else if(s_screen_asleep) {
                wakeScreen("button");
            } else if(s_popup_active) {
                s_popup_active = false;
                renderDisplay();
            } else if(i == 0) { // UP
                if(currentPage == PAGE_SETTINGS && s_settings_row > 0U) {
                    s_settings_row--;
                } else {
                    currentPage = (ScreenPage)(((int)currentPage + PAGE_COUNT - 1) % PAGE_COUNT);
                    if(currentPage == PAGE_SETTINGS) s_settings_row = 8U;
                }
                renderDisplay();
            } else if(i == 1) { // DOWN
                if(currentPage == PAGE_SETTINGS && s_settings_row < 8U) {
                    s_settings_row++;
                } else {
                    currentPage = (ScreenPage)(((int)currentPage + 1) % PAGE_COUNT);
                    if(currentPage == PAGE_SETTINGS) s_settings_row = 0U;
                }
                renderDisplay();
            } else if(i == 2) { // SELECT
                if(currentPage == PAGE_SETTINGS) {
                    if(s_settings_row == 0U) {
                        s_cfg_raise_wake = !s_cfg_raise_wake;
                    } else if(s_settings_row == 1U) {
                        s_cfg_timeout_idx = (uint8_t)((s_cfg_timeout_idx + 1U) % 7U);
                    } else if(s_settings_row == 2U) {
                        s_cfg_bright_idx = (uint8_t)((s_cfg_bright_idx + 1U) % 5U);
                        setBacklightPercent(kBrightPctTable[s_cfg_bright_idx]);
                    } else if(s_settings_row == 3U) {
                        s_cfg_theme_idx = (uint8_t)((s_cfg_theme_idx + 1U) % 6U);
                    } else if(s_settings_row == 4U) {
                        s_cfg_glitch_fx = !s_cfg_glitch_fx;
                    } else if(s_settings_row == 5U) {
                        s_cfg_warn_low_tenths = cycleThresholdTable(s_cfg_warn_low_tenths, kWarnLowTable, 11U);
                    } else if(s_settings_row == 6U) {
                        s_cfg_warn_high_tenths = cycleThresholdTable(s_cfg_warn_high_tenths, kWarnHighTable, 10U);
                    } else if(s_settings_row == 7U) {
                        s_cfg_urg_low_tenths = cycleThresholdTable(s_cfg_urg_low_tenths, kUrgLowTable, 10U);
                    } else if(s_settings_row == 8U) {
                        s_cfg_urg_high_tenths = cycleThresholdTable(s_cfg_urg_high_tenths, kUrgHighTable, 10U);
                    }
                    saveSettingsToSpiffs();
                    renderDisplay();
                } else if(currentPage == PAGE_GRAPH) {
                    s_zoom_idx = (s_zoom_idx + 1U) % 6U;
                    if(s_history.count == 0U) {
                        showPopup("No Scans Yet", "0 scans stored.", "Scan Libre 2 first", "to plot graph.", 2800U);
                    }
                    renderDisplay();
                } else if(currentPage == PAGE_BLE) {
                    toggleBlePairing();
                    renderDisplay();
                } else {
                    currentPage = PAGE_GRAPH;
                    renderDisplay();
                }
            } else if(i == 3) { // ALT (Back)
                currentPage = (currentPage == PAGE_HOME) ? PAGE_SETTINGS : PAGE_HOME;
                if(currentPage == PAGE_SETTINGS) s_settings_row = 0U;
                renderDisplay();
            }
        }
        s_btn_prev[i] = cur;
    }

    if(s_ble_packet_ready && compState.has_scan && s_ble_cal_valid) {
        uint8_t local46[LL_BLE_RAW_PACKET_LEN];
        noInterrupts();
        memcpy(local46, s_ble_frag_buf, LL_BLE_RAW_PACKET_LEN);
        s_ble_packet_ready = false;
        interrupts();

        LlBleStreamFrame bf;
        if(ll_libre2_evaluate_ble_packet(compState.capture.uid, &s_ble_cal_cfg, local46, &bf) && bf.calibrated.available) {
            s_ble_packets_ok++;
            s_ble_unlock_count++;
            compState.vendor_chain = bf.calibrated;
            compState.integrity.sensor.age_minutes = bf.wear_time_minutes;
            snprintf(compState.primary_line, sizeof(compState.primary_line), "%lu.%lu mmol/L",
                     (unsigned long)(bf.calibrated.mmol_tenths / 10U),
                     (unsigned long)(bf.calibrated.mmol_tenths % 10U));
            uint32_t bootSec = (uint32_t)(millis() / 1000U);
            bool isNewPt = ll_history_add(
                &s_history,
                compState.serial,
                bf.wear_time_minutes,
                (uint16_t)bf.calibrated.mmol_tenths,
                (uint16_t)bf.calibrated.mg_dl,
                bootSec);
            if(isNewPt && s_history.count > 0U) {
                appendHistoryToSpiffs(&s_history.points[s_history.count - 1U]);
            }
            saveBleBondToSpiffs();
            lastScanMillis = millis();
            scanCount++;
            snprintf(lastTransport, sizeof(lastTransport), "BLE-S");
            wakeScreen("ble_stream");
            triggerGlucoseAlerts((uint16_t)bf.calibrated.mmol_tenths);
            renderDisplay();
            notifyResultSummary("BLE-S");
        }
    }

    if(s_ble_enabled) {
        if(s_ble_connected && s_ble_client && !s_ble_client->isConnected() && (millis() - s_ble_conn_start_ms > 3000U)) {
            s_ble_connected = false;
            s_ble_connecting = false;
        }
        if(s_ble_do_connect) {
            s_ble_do_connect = false;
            connectAndLoginLibre2Ble();
            renderDisplay();
        } else if(!s_ble_connected && !s_ble_scanning && compState.has_scan && (millis() - s_ble_last_scan_ms > 500U)) {
            s_ble_last_scan_ms = millis();
            s_ble_scanning = true;
            BLEScan* pScan = BLEDevice::getScan();
            pScan->clearResults();
            if(!pScan->start(5, [](BLEScanResults results) { (void)results; s_ble_scanning = false; }, false)) {
                s_ble_scanning = false;
            }
        }
    }

    if(s_ble_bond_dirty) {
        s_ble_bond_dirty = false;
        saveBleBondToSpiffs();
    }

    static unsigned long s_last_clock_ms = 0;
    unsigned long nowMs = millis();
    if(!s_screen_asleep && currentPage == PAGE_HOME) {
        if(s_cfg_glitch_fx) {
            if(!s_glitch_frame_active && (int32_t)(nowMs - s_glitch_next_ms) >= 0) {
                s_glitch_frame_active = true;
                s_glitch_Burst_start_ms = nowMs;
                renderDisplay();
            } else if(s_glitch_frame_active && (nowMs - s_glitch_Burst_start_ms >= 140UL)) {
                s_glitch_frame_active = false;
                s_glitch_next_ms = nowMs + 3400UL + (nowMs % 1700UL);
                renderDisplay();
            }
        } else if(s_glitch_frame_active) {
            s_glitch_frame_active = false;
            renderDisplay();
        }
        if(nowMs - s_last_clock_ms >= 1000U) {
            s_last_clock_ms = nowMs;
            renderDisplay();
        }
    }

    delay(5);
}
