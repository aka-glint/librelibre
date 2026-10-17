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
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <vector>

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
    int battRead;
    int battVref;
    int usb;
    int chrgIn;
    int chrgOut;
    int rgbR;
    int rgbG;
    int rgbB;
};

static ArtemisPins s_pins;

static void detectArtemisHardware() {
    uint32_t usr1 = REG_READ(EFUSE_RD_USR_DATA1_REG);
    uint8_t rev = (uint8_t)(usr1 & 0xFFU);
    if(rev == 0U || rev == 1U) {
        s_pins = { rev, 10, 9, 8, 21, 37, 11, 35, 36, 41, 40, 39, 38, 6, -1, 47, 1, 2, 33, 34, 48 };
    } else {
        // Revision 2 / 3 (default for HWRev 3)
        if(rev > 3U) rev = 3U;
        s_pins = { rev, 2, 1, 3, 21, 33, 47, 40, 41, 36, 35, 48, 34, 5, 4, 42, 7, 6, 14, 12, 13 };
    }
    // Turn off the 6 perimeter red LEDs (same across all revisions)
    const int leds[6] = { 17, 18, 43, 44, 45, 46 };
    for(int i = 0; i < 6; i++) {
        pinMode(leds[i], OUTPUT);
        digitalWrite(leds[i], LOW);
    }
    // Turn off onboard RGB LED (common anode: active LOW, HIGH = OFF)
    if(s_pins.rgbR >= 0) {
        pinMode(s_pins.rgbR, OUTPUT);
        pinMode(s_pins.rgbG, OUTPUT);
        pinMode(s_pins.rgbB, OUTPUT);
        digitalWrite(s_pins.rgbR, HIGH);
        digitalWrite(s_pins.rgbG, HIGH);
        digitalWrite(s_pins.rgbB, HIGH);
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
static constexpr uint16_t COL_WHITE       = 0xFFFF; // pure crisp bright white
static constexpr uint16_t COL_BRIGHT_WHITE= 0xFFFF; // pure crisp white for readable notifications
static constexpr uint16_t COL_YELLOW       = 0xFFE0; // bold yellow for notification titles
static constexpr uint16_t COL_VIVID_CYAN   = 0x07FF; // electric cyan
static constexpr uint16_t COL_VIVID_GREEN  = 0x07E0; // emerald green (WhatsApp / success)
static constexpr uint16_t COL_VIVID_RED    = 0xF800; // vivid red (dismiss / alerts)
static constexpr uint16_t COL_DARK_NAVY    = 0x0842; // dark card container background
static constexpr uint16_t COL_CYAN        = 0x07FF; // vivid electric cyan
static constexpr uint16_t COL_GREEN       = 0x07E0; // vivid emerald green
static constexpr uint16_t COL_AMBER       = 0xFFE0; // vivid cyber yellow/amber
static constexpr uint16_t COL_RED         = 0xF800; // vivid crimson red
static constexpr uint16_t COL_MUTED       = 0x8410; // clean readable neutral grey
#define SETTINGS_CFG_PATH "/spiffs/settings.cfg"

enum BleMode : uint8_t {
    BLE_MODE_DIRECT = 0,   // Direct FreeStyle Libre 2 BLE streaming (0xFDE3)
    BLE_MODE_JUGGLUCO = 1, // Phone companion over Gadgetbridge Nordic UART Service (NUS)
    BLE_MODE_OFF = 2       // Bluetooth disabled / power save
};
static uint8_t s_cfg_bt_mode     = BLE_MODE_DIRECT;
static const char* const kBtModeLabels[3] = { "DIRECT", "JUGGLUCO", "OFF" };
static bool    s_cfg_rgb_led     = true;

static bool s_cfg_raise_wake = true;
static uint8_t s_cfg_timeout_idx = 1U; // 0=5s, 1=10s, 2=15s, 3=30s, 4=1m, 5=2m, 6=NEVER
static uint8_t s_cfg_bright_idx  = 4U; // 0=10%, 1=25%, 2=45%, 3=70%, 4=100% (default 100%)
static uint8_t s_cfg_theme_idx   = 0U; // 0=CP2077, 1=CYAN, 2=MAGENTA, 3=MATRIX, 4=AMBER, 5=CRIMSON
static bool    s_cfg_glitch_fx   = true; // Cyberpunk 2077 Kiroshi Relic scanline/chromatic glitch effect
static uint16_t s_cfg_warn_low_tenths  = 42U;  // Default 4.2 mmol/L (3s flight-deck flash)
static uint16_t s_cfg_warn_high_tenths = 130U; // Default 13.0 mmol/L (3s flight-deck flash)
static uint16_t s_cfg_urg_low_tenths   = 36U;  // Default 3.6 mmol/L (audible + ack/15m snooze)
static uint16_t s_cfg_urg_high_tenths  = 160U; // Default 16.0 mmol/L (audible + ack/15m snooze)
static uint8_t s_cfg_deck_pulse_idx    = 0U;   // 0=OFF, 1=5m, 2=10m, 3=15m, 4=30m, 5=60m
static bool    s_cfg_notif_light       = true; // Flash perimeter LEDs on phone notification
static bool    s_cfg_notif_sound       = true; // Melodic chime on phone notification
static bool    s_cfg_alarm_sound       = true; // Buzzer sound for latching urgent glucose alarms
static uint8_t s_settings_row          = 0U;   // 0..22 cursor on PAGE_SETTINGS
static uint8_t s_cfg_power_saver       = 1U;   // 0=OFF, 1=AUTO (<=15%), 2=ON
static bool    s_power_saver_active    = false;
static bool    s_cfg_night_dnd         = false;// Disables raise-to-wake & routine notif alerts
static bool    s_cfg_hourly_chime      = false;// Retro double-pip on the hour
static bool    s_flashlight_active     = false;// 6 perimeter LEDs + 100% white screen floodlight
static unsigned long s_flashlight_start_ms = 0UL;
static bool    s_sos_active            = false;// Medical Emergency Morse optical/audio SOS
static unsigned long s_sos_start_ms    = 0UL;
static bool    s_hourly_chime_active   = false;
static unsigned long s_hourly_chime_start_ms = 0UL;

#define ICE_CFG_PATH "/spiffs/ice.cfg"
static char s_ice_name[32]  = "Emergency Contact";
static char s_ice_phone[32] = "Call 999 / 911";

static bool    s_ble_bond_dirty        = false;
static bool    s_glitch_frame_active   = false;
static unsigned long s_glitch_Burst_start_ms = 0UL;
static unsigned long s_glitch_next_ms  = 3500UL;
static unsigned long s_last_deck_pulse_ms = 0UL;

static const uint16_t kWarnLowTable[10]  = { 35U, 38U, 40U, 42U, 45U, 48U, 50U, 55U, 0U };
static const uint16_t kWarnHighTable[12] = { 90U, 100U, 110U, 120U, 125U, 130U, 140U, 150U, 160U, 180U, 0U };
static const uint16_t kUrgLowTable[10]   = { 28U, 30U, 32U, 34U, 35U, 36U, 38U, 40U, 0U };
static const uint16_t kUrgHighTable[11]  = { 130U, 140U, 150U, 160U, 170U, 180U, 200U, 220U, 0U };

static const uint32_t kDeckPulseMinutes[6] = { 0UL, 5UL, 10UL, 15UL, 30UL, 60UL };
static const char* const kDeckPulseLabels[6] = { "OFF", "5 MIN", "10 MIN", "15 MIN", "30 MIN", "60 MIN" };

static const uint32_t kTimeoutMsTable[7] = {
    5000UL, 10000UL, 15000UL, 30000UL, 60000UL, 120000UL, 0UL
};
static const char* const kTimeoutLabels[7] = {
    "5 SEC", "10 SEC", "15 SEC", "30 SEC", "1 MIN", "2 MIN", "NEVER"
};
static const uint8_t kBrightPctTable[5] = { 10U, 25U, 45U, 70U, 100U };
static const char* const kBrightLabels[5] = { "10%", "25%", "45%", "70%", "100%" };
static const char* const kThemeLabels[6] = { "CP2077", "CYAN", "MAGENTA", "MATRIX", "AMBER", "CRIMSON" };

// ─── FLIGHT DECK LEDS (6x PERIMETER RED) & PIEZO ALARM STATE ──
// Left side (top to bottom): GPIO 17, 18, 43
// Right side (top to bottom): GPIO 44, 45, 46
static const int kFlightDeckLeds[6] = { 17, 18, 43, 44, 45, 46 };
enum FlightLedMode {
    LED_MODE_IDLE = 0,
    LED_MODE_SUBTLE_BLINK = 1,
    LED_MODE_FLASH_3S = 2,
    LED_MODE_AESTHETIC_PULSE = 3,
    LED_MODE_NOTIF_PULSE = 4
};
static FlightLedMode s_led_mode = LED_MODE_IDLE;
static unsigned long s_led_start_ms = 0UL;
static unsigned long s_led_until_ms = 0UL;
static uint8_t s_deck_mask_cur = 0U;

static bool s_alarm_latched = false;
static bool s_alarm_is_low  = false;
static uint16_t s_alarm_mmol_tenths = 0U;
static bool s_alarm_snoozed = false;
static unsigned long s_alarm_snooze_until_ms = 0UL;
static uint32_t s_cur_buzz_freq = 0U;
static bool s_screen_asleep = false;
static bool s_notif_sound_active = false;
static unsigned long s_notif_sound_start_ms = 0UL;
static bool s_find_watch_active = false;
static unsigned long s_find_watch_until_ms = 0UL;

static void setFlightDeckMask(uint8_t mask) {
    s_deck_mask_cur = mask;
    for(int i = 0; i < 6; i++) {
        digitalWrite(kFlightDeckLeds[i], (mask & (1U << i)) ? HIGH : LOW);
    }
}

static void setFlightDeckLeds(bool on) {
    setFlightDeckMask(on ? 0x3FU : 0x00U);
}

static void startDeckAestheticPulse() {
    s_led_mode = LED_MODE_AESTHETIC_PULSE;
    s_led_start_ms = millis();
    s_led_until_ms = s_led_start_ms + 1800UL;
}

static void playNotificationChime() {
    if(!s_cfg_notif_sound) return;
    s_notif_sound_active = true;
    s_notif_sound_start_ms = millis();
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
    PAGE_HOME = 0,       // Slide 1: Cyberpunk Home HUD
    PAGE_NOTIFS = 1,     // Slide 2: Phone Notifications
    PAGE_GRAPH = 2,      // Slide 3: Glucose History Graph
    PAGE_BLE = 3,        // Slide 4: Bluetooth Management
    PAGE_DIAG = 4,       // Slide 5: Hardware Diagnostics
    PAGE_SETTINGS = 5,   // Slide 6: Settings Menu
    PAGE_COUNT = 6
};
static ScreenPage currentPage = PAGE_HOME;

// ─── PHONE NOTIFICATIONS (SLIDE 2) ────────────────────────
#define MAX_PHONE_NOTIFS 10

struct PhoneNotification {
    uint32_t id;
    char src[20];       // App name (e.g. Messages, WhatsApp, Phone)
    char title[32];     // Sender or subject
    char body[128];     // Message preview
    uint8_t hour;
    uint8_t min;
    bool active;
};

static PhoneNotification s_phone_notifs[MAX_PHONE_NOTIFS];
static uint8_t s_notif_count = 0U;
static uint8_t s_notif_selected = 0U;

// ─── BATTERY FUEL GAUGE & CHARGE SENSING ──────────────────
static uint16_t s_batt_mv = 3950U;
static uint8_t  s_batt_pct = 85U;
static bool     s_usb_plugged = false;
static bool     s_batt_charging = false;
static float    s_vref_cal_offset = 0.0f;
static unsigned long s_last_batt_poll_ms = 0UL;

static void calibrateBatteryAdc() {
    if(s_pins.battVref < 0) return;
    pinMode(s_pins.battVref, OUTPUT);
    digitalWrite(s_pins.battVref, HIGH);
    delay(20);
    uint32_t sum = 0;
    for(int i = 0; i < 16; i++) {
        sum += analogReadMilliVolts(s_pins.battRead);
        delay(2);
    }
    digitalWrite(s_pins.battVref, LOW);
    delay(20);
    float avg = (float)sum / 16.0f;
    s_vref_cal_offset = 2500.0f - avg;
    Serial.printf("BATT_CAL|read=%.1fmV|ref=2500mV|offset=%.1fmV\n", avg, s_vref_cal_offset);
}

static void updateRgbLedStatus();

static void pollBattery(bool force = false) {
    unsigned long now = millis();
    unsigned long interval = s_screen_asleep ? 30000UL : 4000UL;
    if(!force && (now - s_last_batt_poll_ms < interval)) return;
    s_last_batt_poll_ms = now;

    bool old_charging = s_batt_charging;
    if(s_pins.usb >= 0) {
        s_usb_plugged = (digitalRead(s_pins.usb) == HIGH);
    }
    if(s_pins.chrgIn >= 0) {
        s_batt_charging = s_usb_plugged && (digitalRead(s_pins.chrgIn) == LOW);
    }
    if(old_charging != s_batt_charging) {
        updateRgbLedStatus();
    }

    if(s_pins.battRead >= 0) {
        uint32_t raw_mv = analogReadMilliVolts(s_pins.battRead);
        float measured_mv = ((float)raw_mv + s_vref_cal_offset) * 4.0f + 75.0f;
        if(measured_mv < 2800.0f) measured_mv = 2800.0f;
        if(measured_mv > 4350.0f) measured_mv = 4350.0f;

        static float s_ema_mv = 3950.0f;
        if(force) s_ema_mv = measured_mv;
        else s_ema_mv = (s_ema_mv * 0.85f) + (measured_mv * 0.15f);

        s_batt_mv = (uint16_t)s_ema_mv;
        if(s_batt_mv <= 3600U) s_batt_pct = 0U;
        else if(s_batt_mv >= 4150U) s_batt_pct = 100U;
        else s_batt_pct = (uint8_t)(((uint32_t)(s_batt_mv - 3600U) * 100U) / (4150U - 3600U));
    }
}

static void initBatteryMonitoring() {
    if(s_pins.usb >= 0) {
        gpio_pullup_dis((gpio_num_t)s_pins.usb);
        gpio_pulldown_dis((gpio_num_t)s_pins.usb);
        gpio_set_direction((gpio_num_t)s_pins.usb, GPIO_MODE_INPUT);
    }
    if(s_pins.chrgIn >= 0) {
        gpio_pullup_dis((gpio_num_t)s_pins.chrgIn);
        gpio_pulldown_dis((gpio_num_t)s_pins.chrgIn);
        gpio_set_direction((gpio_num_t)s_pins.chrgIn, GPIO_MODE_INPUT);
    }
    if(s_pins.battVref >= 0) {
        pinMode(s_pins.battVref, OUTPUT);
        digitalWrite(s_pins.battVref, LOW);
    }
    analogReadResolution(12);
    calibrateBatteryAdc();
    pollBattery(true);
}

// ─── ONBOARD RGB STATUS LED (LEDC HARDWARE PWM) ────────────
static void initRgbLed() {
    if(s_pins.rgbR < 0) return;
    ledc_timer_config_t tcfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num       = LEDC_TIMER_2,
        .freq_hz         = 1000,
        .clk_cfg         = LEDC_AUTO_CLK
    };
    ledc_timer_config(&tcfg);

    const int rgb_pins[3] = { s_pins.rgbR, s_pins.rgbG, s_pins.rgbB };
    const ledc_channel_t rgb_chans[3] = { LEDC_CHANNEL_3, LEDC_CHANNEL_4, LEDC_CHANNEL_5 };

    for(int i = 0; i < 3; i++) {
        ledc_channel_config_t ccfg = {
            .gpio_num   = rgb_pins[i],
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel    = rgb_chans[i],
            .intr_type  = LEDC_INTR_DISABLE,
            .timer_sel  = LEDC_TIMER_2,
            .duty       = 0,
            .hpoint     = 0,
            .flags      = { .output_invert = 1 }
        };
        ledc_channel_config(&ccfg);
    }
}

static void setRgbColor(bool r, bool g, bool b) {
    if(s_pins.rgbR < 0) return;
    if(!s_cfg_rgb_led) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3, 0U);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_4, 0U);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_4);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_5, 0U);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_5);
        return;
    }

    // 100% brightness (1023 duty) when awake, 30% brightness (307 duty) when screen asleep
    uint32_t duty_on = (s_screen_asleep && !s_alarm_latched) ? 307U : 1023U;

    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3, r ? duty_on : 0U);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3);

    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_4, g ? duty_on : 0U);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_4);

    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_5, b ? duty_on : 0U);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_5);
}

// Forward declarations
static bool s_ble_connected = false;
static bool s_juggluco_connected = false;

static void updateRgbLedStatus() {
    if(s_pins.rgbR < 0 || !s_cfg_rgb_led) {
        setRgbColor(false, false, false);
        return;
    }
    if(s_alarm_latched) {
        // Red flashing during urgent alarm
        setRgbColor(true, false, false);
    } else if(s_batt_charging) {
        if(s_batt_pct >= 98U) {
            setRgbColor(false, true, false); // Full charge: solid green
        } else {
            setRgbColor(true, true, false);  // Charging: amber / yellow
        }
    } else if(compState.has_scan && compState.vendor_chain.available) {
        uint16_t mmolT = (uint16_t)compState.vendor_chain.mmol_tenths;
        bool isUrg = (s_cfg_urg_low_tenths > 0U && mmolT <= s_cfg_urg_low_tenths) ||
                     (s_cfg_urg_high_tenths > 0U && mmolT >= s_cfg_urg_high_tenths);
        bool isWarn = (s_cfg_warn_low_tenths > 0U && mmolT <= s_cfg_warn_low_tenths) ||
                      (s_cfg_warn_high_tenths > 0U && mmolT >= s_cfg_warn_high_tenths);
        if(isUrg) {
            setRgbColor(true, false, false); // Red
        } else if(isWarn) {
            setRgbColor(true, true, false);  // Amber/Yellow
        } else {
            setRgbColor(false, true, false); // Green (in target)
        }
    } else if(s_cfg_bt_mode == BLE_MODE_DIRECT && s_ble_connected) {
        setRgbColor(false, false, true); // Blue
    } else if(s_cfg_bt_mode == BLE_MODE_JUGGLUCO && s_juggluco_connected) {
        setRgbColor(false, true, true);  // Cyan
    } else {
        setRgbColor(false, false, false);
    }
}

static const uint16_t kZoomMinutes[7] = { 15U, 60U, 180U, 480U, 1440U, 0U, 1440U };
static const char* const kZoomLabels[7] = { "15m", "1h", "3h", "8h", "24h", "ALL", "TIR" };
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
static bool s_popup_is_notif = false;
static bool s_popup_is_call = false;
static uint32_t s_popup_notif_id = 0;
static unsigned long s_popup_until_ms = 0;
static char s_popup_title[32] = "";
static char s_popup_line1[32] = "";
static char s_popup_line2[32] = "";
static char s_popup_line3[32] = "";
static char s_popup_src[24] = "";
static char s_popup_body[128] = "";
static char s_call_caller[48] = "";
static char s_call_number[32] = "";

static void renderDisplay();

static uint16_t getAppAccentColor(const char* src) {
    if(!src || !src[0]) return COL_VIVID_CYAN;
    if(strcasestr(src, "what") != NULL) return 0x25E7;     // WhatsApp Emerald Green
    if(strcasestr(src, "msg") != NULL || strcasestr(src, "mess") != NULL || strcasestr(src, "sms") != NULL) return 0x3D7F; // Sky Blue
    if(strcasestr(src, "sign") != NULL) return 0x247F;     // Signal Blue
    if(strcasestr(src, "tele") != NULL) return 0x05DF;     // Telegram Cyan
    if(strcasestr(src, "call") != NULL || strcasestr(src, "phone") != NULL) return COL_VIVID_RED; // Phone Red
    if(strcasestr(src, "mail") != NULL || strcasestr(src, "gma") != NULL) return 0xFD20;  // Orange / Amber
    if(strcasestr(src, "inst") != NULL || strcasestr(src, "disc") != NULL) return 0xA99F; // Purple
    return COL_VIVID_CYAN;
}

static uint8_t wordWrapText(const char* srcText, char lines[][24], uint8_t maxLines, uint8_t maxCharsPerLine) {
    if(!srcText || !srcText[0] || maxLines == 0 || maxCharsPerLine == 0) return 0;
    if(maxCharsPerLine > 23) maxCharsPerLine = 23;

    uint8_t lineCount = 0;
    const char* p = srcText;

    while(*p && lineCount < maxLines) {
        while(*p == ' ' || *p == '\t') p++;
        if(!*p) break;

        char* curLine = lines[lineCount];
        uint8_t len = 0;

        while(*p && *p != '\n' && *p != '\r') {
            const char* wEnd = p;
            while(*wEnd && *wEnd != ' ' && *wEnd != '\t' && *wEnd != '\n' && *wEnd != '\r') {
                wEnd++;
            }
            uint8_t wordLen = (uint8_t)(wEnd - p);

            if(len == 0) {
                if(wordLen <= maxCharsPerLine) {
                    memcpy(curLine + len, p, wordLen);
                    len += wordLen;
                    p = wEnd;
                } else {
                    memcpy(curLine + len, p, maxCharsPerLine);
                    len += maxCharsPerLine;
                    p += maxCharsPerLine;
                }
            } else {
                if(len + 1 + wordLen <= maxCharsPerLine) {
                    curLine[len++] = ' ';
                    memcpy(curLine + len, p, wordLen);
                    len += wordLen;
                    p = wEnd;
                } else {
                    break;
                }
            }
            while(*p == ' ' || *p == '\t') p++;
        }

        if(*p == '\n' || *p == '\r') p++;

        curLine[len] = '\0';
        lineCount++;
    }

    if(*p && lineCount == maxLines) {
        char* lastLine = lines[lineCount - 1];
        size_t lLen = strlen(lastLine);
        if(lLen >= 3) {
            lastLine[lLen - 3] = '.';
            lastLine[lLen - 2] = '.';
            lastLine[lLen - 1] = '.';
        }
    }

    return lineCount;
}

static void showPopup(const char* title, const char* l1, const char* l2, const char* l3, unsigned long duration_ms = 3200U) {
    s_popup_is_notif = false;
    s_popup_is_call = false;
    s_popup_notif_id = 0U;
    snprintf(s_popup_title, sizeof(s_popup_title), "%s", title ? title : "");
    snprintf(s_popup_line1, sizeof(s_popup_line1), "%s", l1 ? l1 : "");
    snprintf(s_popup_line2, sizeof(s_popup_line2), "%s", l2 ? l2 : "");
    snprintf(s_popup_line3, sizeof(s_popup_line3), "%s", l3 ? l3 : "");
    s_popup_active = true;
    s_popup_until_ms = millis() + duration_ms;
    renderDisplay();
}

static void showNotifPopup(uint32_t id, const char* src, const char* title, const char* body, unsigned long duration_ms = 5000U) {
    s_popup_is_notif = true;
    s_popup_is_call = false;
    s_popup_notif_id = id;
    snprintf(s_popup_src, sizeof(s_popup_src), "%s", (src && src[0]) ? src : "Phone");
    snprintf(s_popup_title, sizeof(s_popup_title), "%s", title ? title : "");
    snprintf(s_popup_body, sizeof(s_popup_body), "%s", body ? body : "");
    s_popup_active = true;
    s_popup_until_ms = millis() + duration_ms;
    renderDisplay();
}

static void showCallPopup(const char* caller, const char* number) {
    s_popup_is_notif = false;
    s_popup_is_call = true;
    snprintf(s_call_caller, sizeof(s_call_caller), "%s", (caller && caller[0]) ? caller : "Incoming Call");
    snprintf(s_call_number, sizeof(s_call_number), "%s", number ? number : "");
    s_popup_active = true;
    s_popup_until_ms = millis() + 30000U; // 30s ringing timeout
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
        s_popup_is_notif = false;
        s_popup_is_call = false;
        setBuzzerTone(0U);
        setFlightDeckLeds(false);
        return;
    }

    if(s_popup_is_call) {
        s_canvas.fillRoundRect(3, 14, 122, 102, 5, COL_DARK_NAVY);
        s_canvas.drawRoundRect(3, 14, 122, 102, 5, COL_VIVID_RED);
        s_canvas.drawRoundRect(4, 15, 120, 100, 4, COL_VIVID_RED);

        // Header strip
        s_canvas.fillRoundRect(6, 17, 74, 12, 2, COL_VIVID_RED);
        s_canvas.setTextSize(1);
        s_canvas.setTextColor(COL_BRIGHT_WHITE);
        s_canvas.setCursor(9, 19);
        s_canvas.print("CALL INCOMING");

        s_canvas.setTextColor(COL_YELLOW);
        s_canvas.setCursor(88, 19);
        s_canvas.print("RING");

        s_canvas.drawLine(4, 32, 124, 32, COL_VIVID_RED);

        // Caller name
        s_canvas.setTextColor(COL_YELLOW);
        s_canvas.setCursor(7, 38);
        char truncName[20];
        snprintf(truncName, sizeof(truncName), "> %.17s", s_call_caller);
        s_canvas.print(truncName);

        // Phone number
        s_canvas.setTextColor(COL_BRIGHT_WHITE);
        s_canvas.setCursor(7, 54);
        s_canvas.print(s_call_number);

        s_canvas.setTextColor(COL_MUTED);
        s_canvas.setCursor(7, 72);
        s_canvas.print("Ringing on phone...");

        // Action footer: SEL: REJECT CALL
        s_canvas.drawLine(4, 94, 124, 94, COL_GRID);
        s_canvas.fillRoundRect(10, 98, 108, 14, 2, 0x6000);
        s_canvas.drawRoundRect(10, 98, 108, 14, 2, COL_VIVID_RED);
        s_canvas.setTextColor(COL_BRIGHT_WHITE);
        s_canvas.setCursor(15, 101);
        s_canvas.print("SEL: REJECT CALL");
        return;
    }

    if(s_popup_is_notif) {
        uint16_t accentCol = getAppAccentColor(s_popup_src);
        s_canvas.fillRoundRect(3, 14, 122, 102, 5, COL_DARK_NAVY);
        s_canvas.drawRoundRect(3, 14, 122, 102, 5, accentCol);
        s_canvas.fillRect(4, 15, 120, 16, 0x0000);

        // App badge pill on left
        s_canvas.fillRoundRect(6, 17, 52, 12, 2, accentCol);
        s_canvas.setTextSize(1);
        s_canvas.setTextColor(0x0000);
        s_canvas.setCursor(8, 19);
        char truncSrc[9];
        snprintf(truncSrc, sizeof(truncSrc), "%.8s", s_popup_src);
        s_canvas.print(truncSrc);

        // Right side tag
        s_canvas.setTextColor(COL_YELLOW);
        s_canvas.setCursor(72, 19);
        s_canvas.print("NEW MSG");

        // Divider
        s_canvas.drawLine(4, 32, 124, 32, accentCol);

        // Sender / Title
        s_canvas.setTextColor(COL_YELLOW);
        s_canvas.setCursor(6, 36);
        char truncTitle[20];
        snprintf(truncTitle, sizeof(truncTitle), "> %.17s", s_popup_title);
        s_canvas.print(truncTitle);

        // Body: up to 3 word-wrapped lines in crisp pure white
        char lines[3][24];
        uint8_t nLines = wordWrapText(s_popup_body, lines, 3, 19);
        s_canvas.setTextColor(COL_BRIGHT_WHITE);
        int y = 49;
        for(int li = 0; li < nLines; li++) {
            s_canvas.setCursor(6, y);
            s_canvas.print(lines[li]);
            y += 11;
        }

        // Action footer
        s_canvas.drawLine(4, 94, 124, 94, COL_GRID);

        // Left pill: SEL:DISMISS
        s_canvas.fillRoundRect(6, 98, 54, 14, 2, 0x6000);
        s_canvas.drawRoundRect(6, 98, 54, 14, 2, COL_VIVID_RED);
        s_canvas.setTextColor(COL_BRIGHT_WHITE);
        s_canvas.setCursor(8, 101);
        s_canvas.print("SEL:DISMISS");

        // Right pill: DN:VIEW
        s_canvas.fillRoundRect(66, 98, 54, 14, 2, 0x0208);
        s_canvas.drawRoundRect(66, 98, 54, 14, 2, COL_VIVID_CYAN);
        s_canvas.setTextColor(COL_VIVID_CYAN);
        s_canvas.setCursor(73, 101);
        s_canvas.print("DN:VIEW");
        return;
    }

    s_canvas.fillRoundRect(4, 18, 120, 92, 6, COL_DARK_NAVY);
    s_canvas.drawRoundRect(4, 18, 120, 92, 6, COL_AMBER);
    s_canvas.setTextSize(1);
    s_canvas.setTextColor(COL_AMBER);
    s_canvas.setCursor(10, 24);
    s_canvas.print(s_popup_title);

    s_canvas.setTextColor(COL_BRIGHT_WHITE);
    s_canvas.setCursor(8, 42);
    s_canvas.print(s_popup_line1);
    s_canvas.setCursor(8, 56);
    s_canvas.print(s_popup_line2);
    s_canvas.setCursor(8, 70);
    s_canvas.print(s_popup_line3);

    s_canvas.setTextColor(COL_VIVID_CYAN);
    s_canvas.setCursor(10, 94);
    s_canvas.print("Press any btn to close");
}

static void drawSosOverlay() {
    s_canvas.fillScreen(COL_BG);
    s_canvas.drawRect(0, 0, LCD_W, LCD_H, COL_VIVID_RED);
    s_canvas.drawRect(1, 1, LCD_W - 2, LCD_H - 2, COL_VIVID_RED);

    // Flashing header
    bool flash = ((millis() / 400UL) % 2UL) == 0UL;
    uint16_t hdrBg = flash ? COL_VIVID_RED : 0x7800;
    s_canvas.fillRect(3, 3, 122, 16, hdrBg);
    s_canvas.setTextSize(1);
    s_canvas.setTextColor(COL_BRIGHT_WHITE);
    s_canvas.setCursor(7, 7);
    s_canvas.print("! MEDICAL EMERGENCY !");

    // Condition
    s_canvas.setTextColor(COL_YELLOW);
    s_canvas.setCursor(7, 23);
    s_canvas.print("TYPE 1 DIABETES");

    // Current Glucose
    uint16_t mmolT = 0U;
    if(compState.vendor_chain.available) {
        mmolT = (uint16_t)compState.vendor_chain.mmol_tenths;
    } else if(s_history.count > 0U) {
        mmolT = s_history.points[s_history.count - 1U].mmol_tenths;
    }
    s_canvas.setTextSize(1);
    s_canvas.setTextColor(COL_BRIGHT_WHITE);
    s_canvas.setCursor(7, 36);
    if(mmolT > 0U) {
        s_canvas.printf("GLUCOSE: %u.%u mmol/L", (unsigned)(mmolT / 10U), (unsigned)(mmolT % 10U));
    } else {
        s_canvas.print("GLUCOSE: MONITOR ACTIVE");
    }

    s_canvas.drawLine(3, 48, 125, 48, COL_VIVID_RED);

    // Rescue guidance for bystanders
    s_canvas.setTextColor(COL_VIVID_GREEN);
    s_canvas.setCursor(5, 52);
    s_canvas.print("* IF CONSCIOUS:");
    s_canvas.setTextColor(COL_BRIGHT_WHITE);
    s_canvas.setCursor(12, 62);
    s_canvas.print("Give sugar/juice!");

    s_canvas.setTextColor(COL_AMBER);
    s_canvas.setCursor(5, 73);
    s_canvas.print("* IF UNCONSCIOUS:");
    s_canvas.setTextColor(COL_VIVID_RED);
    s_canvas.setCursor(12, 83);
    s_canvas.print("CALL 999/911 - NO INSULIN");

    // ICE Contact
    s_canvas.drawLine(3, 95, 125, 95, COL_GRID);
    s_canvas.setTextColor(COL_YELLOW);
    s_canvas.setCursor(5, 99);
    char iceL1[22];
    snprintf(iceL1, sizeof(iceL1), "ICE: %.16s", s_ice_name);
    s_canvas.print(iceL1);

    s_canvas.setTextColor(COL_BRIGHT_WHITE);
    s_canvas.setCursor(5, 109);
    char iceL2[22];
    snprintf(iceL2, sizeof(iceL2), "TEL: %.16s", s_ice_phone);
    s_canvas.print(iceL2);

    s_canvas.setTextColor(COL_MUTED);
    s_canvas.setCursor(12, 119);
    s_canvas.print("SEL/ALT: EXIT SOS");
}

// ─── BACKLIGHT PWM (INVERTED ON ARTEMIS) & IMU WAKE ───────
#define SCREEN_TIMEOUT_MS 300000UL
static unsigned long s_last_activity_ms = 0;
static bool s_imu_ready = false;
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
        .freq_hz         = 1000,
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
    if(pct > 100U) pct = 100U;
    if(pct == 0U) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0U);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
        return;
    }
    uint32_t duty = (pct == 100U) ? 1024U : (((uint32_t)pct * 1024U) / 100U);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

static bool s_woke_by_raise = false;
static unsigned long s_wrist_down_since_ms = 0UL;

static void wakeScreen(const char* reason) {
    s_last_activity_ms = millis();
    s_wrist_down_since_ms = 0UL;
    if(reason && strcmp(reason, "raise") == 0) {
        s_woke_by_raise = true;
    } else {
        s_woke_by_raise = false;
    }
    if(s_screen_asleep) {
        if(s_power_saver_active) {
            setCpuFrequencyMhz(80);
        } else {
            setCpuFrequencyMhz(240);
        }
        s_screen_asleep = false;
        uint8_t target_bl = kBrightPctTable[s_cfg_bright_idx % 5U];
        if(s_power_saver_active && target_bl > 20U) target_bl = 20U;
        if(s_cfg_night_dnd && target_bl > 15U) target_bl = 15U;
        setBacklightPercent(target_bl);
        updateRgbLedStatus();
        Serial.printf("SCREEN_WAKE|reason=%s|cpu=%uMHz\n", reason ? reason : "activity", (unsigned)getCpuFrequencyMhz());
        renderDisplay();
    }
}

static void triggerFindWatch() {
    wakeScreen("find_watch");
    s_find_watch_active = true;
    s_find_watch_until_ms = millis() + 4000UL;
    showPopup("FIND MY WATCH", "Locator ping", "received from phone", "ANY BTN: SILENCE", 4000U);
    Serial.println("GB_FIND_WATCH|triggered_4s_chime_and_leds");
}

static void sendJugglucoTx(const char* fmt, ...);

// ─── PCF8563 / BM8563 HARDWARE I2C RTC (0x51) ─────────────────
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

static void setFlashlight(bool on) {
    s_flashlight_active = on;
    if(on) {
        s_flashlight_start_ms = millis();
        wakeScreen("flashlight");
        setFlightDeckLeds(true);
        setBacklightPercent(100U);
        Serial.println("FLASHLIGHT|ON|6_LEDS_100PCT_TFT");
    } else {
        setFlightDeckLeds(false);
        uint8_t target_bl = kBrightPctTable[s_cfg_bright_idx % 5U];
        if(s_power_saver_active && target_bl > 20U) target_bl = 20U;
        if(s_cfg_night_dnd && target_bl > 15U) target_bl = 15U;
        setBacklightPercent(target_bl);
        updateRgbLedStatus();
        Serial.println("FLASHLIGHT|OFF");
    }
    renderDisplay();
}

static void toggleFlashlight() {
    setFlashlight(!s_flashlight_active);
}

static void setMedicalSos(bool on) {
    s_sos_active = on;
    if(on) {
        s_sos_start_ms = millis();
        wakeScreen("sos");
        sendJugglucoTx("{\"t\":\"notify\",\"title\":\"EMERGENCY SOS\",\"body\":\"Watch SOS Beacon Activated! Check User Immediately!\"}\n");
        sendJugglucoTx("{\"t\":\"act\",\"n\":\"SOS\"}\n");
        Serial.println("SOS_BEACON|ON|MORSE_LIGHT_AUDIO_ACTIVE");
    } else {
        setFlightDeckLeds(false);
        setBuzzerTone(0U);
        updateRgbLedStatus();
        Serial.println("SOS_BEACON|OFF");
    }
    renderDisplay();
}

static void toggleMedicalSos() {
    setMedicalSos(!s_sos_active);
}

static void saveIceToSpiffs() {
    if(!s_spiffs_mounted) return;
    FILE* f = fopen(ICE_CFG_PATH, "wb");
    if(!f) return;
    fprintf(f, "ICE|%s|%s\n", s_ice_name, s_ice_phone);
    fflush(f);
    fclose(f);
}

static void loadIceFromSpiffs() {
    if(!s_spiffs_mounted) return;
    FILE* f = fopen(ICE_CFG_PATH, "rb");
    if(!f) return;
    char line[96];
    if(fgets(line, sizeof(line), f)) {
        if(strncmp(line, "ICE|", 4) == 0) {
            char* name = strtok(line + 4, "|\r\n");
            char* phone = strtok(NULL, "|\r\n");
            if(name && name[0]) snprintf(s_ice_name, sizeof(s_ice_name), "%s", name);
            if(phone && phone[0]) snprintf(s_ice_phone, sizeof(s_ice_phone), "%s", phone);
        }
    }
    fclose(f);
}

static void saveSettingsToSpiffs() {
    if(!s_spiffs_mounted) return;
    FILE* f = fopen(SETTINGS_CFG_PATH, "wb");
    if(!f) return;
    fprintf(f, "CFG|%u|%u|%u|%u|%u|%u|%u|%u|%u|%u|%u|%u|%u|%u|%u|%u|%u|%u\n",
            (unsigned)(s_cfg_raise_wake ? 1U : 0U),
            (unsigned)s_cfg_timeout_idx,
            (unsigned)s_cfg_bright_idx,
            (unsigned)s_cfg_theme_idx,
            (unsigned)s_cfg_warn_low_tenths,
            (unsigned)s_cfg_warn_high_tenths,
            (unsigned)s_cfg_urg_low_tenths,
            (unsigned)s_cfg_urg_high_tenths,
            (unsigned)(s_cfg_glitch_fx ? 1U : 0U),
            (unsigned)(s_cfg_bt_mode % 3U),
            (unsigned)(s_cfg_rgb_led ? 1U : 0U),
            (unsigned)(s_cfg_deck_pulse_idx % 6U),
            (unsigned)(s_cfg_notif_light ? 1U : 0U),
            (unsigned)(s_cfg_notif_sound ? 1U : 0U),
            (unsigned)(s_cfg_alarm_sound ? 1U : 0U),
            (unsigned)(s_cfg_power_saver % 3U),
            (unsigned)(s_cfg_night_dnd ? 1U : 0U),
            (unsigned)(s_cfg_hourly_chime ? 1U : 0U));
    fflush(f);
    fclose(f);
}

static void loadSettingsFromSpiffs() {
    if(!s_spiffs_mounted) return;
    FILE* f = fopen(SETTINGS_CFG_PATH, "rb");
    if(!f) return;
    char line[192];
    if(fgets(line, sizeof(line), f)) {
        unsigned rw = 1, to = 1, br = 4, th = 0, wl = 42, wh = 130, ul = 36, uh = 160, gl = 1, bm = 0, rgb = 1;
        unsigned dp = 0, nl = 1, ns = 1, as = 1;
        unsigned ps = 1, nd = 0, hc = 0;
        int n = sscanf(line, "CFG|%u|%u|%u|%u|%u|%u|%u|%u|%u|%u|%u|%u|%u|%u|%u|%u|%u|%u",
                       &rw, &to, &br, &th, &wl, &wh, &ul, &uh, &gl, &bm, &rgb, &dp, &nl, &ns, &as, &ps, &nd, &hc);
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
        if(n >= 10) {
            s_cfg_bt_mode = (uint8_t)(bm % 3U);
        }
        if(n >= 11) {
            s_cfg_rgb_led = (rgb != 0U);
        }
        if(n >= 12) {
            s_cfg_deck_pulse_idx = (uint8_t)(dp % 6U);
        }
        if(n >= 13) {
            s_cfg_notif_light = (nl != 0U);
        }
        if(n >= 14) {
            s_cfg_notif_sound = (ns != 0U);
        }
        if(n >= 15) {
            s_cfg_alarm_sound = (as != 0U);
        }
        if(n >= 16) {
            s_cfg_power_saver = (uint8_t)(ps % 3U);
        }
        if(n >= 17) {
            s_cfg_night_dnd = (nd != 0U);
        }
        if(n >= 18) {
            s_cfg_hourly_chime = (hc != 0U);
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

    if(s_flashlight_active) {
        if(now - s_flashlight_start_ms >= 180000UL) {
            setFlashlight(false);
            return;
        }
        s_last_activity_ms = now; // Keep screen awake
        setFlightDeckLeds(true);
        return;
    }

    if(s_sos_active) {
        s_last_activity_ms = now; // Keep screen awake
        uint32_t t = (uint32_t)((now - s_sos_start_ms) % 5100UL);
        bool on = false;
        if(t < 1200UL) { // S: 3 short dots (150 on, 150 off)
            uint32_t sub = t;
            on = (sub < 150UL) || (sub >= 300UL && sub < 450UL) || (sub >= 600UL && sub < 750UL);
        } else if(t < 3300UL) { // O: 3 long dashes (450 on, 150 off)
            uint32_t sub = t - 1200UL;
            on = (sub < 450UL) || (sub >= 600UL && sub < 1050UL) || (sub >= 1200UL && sub < 1650UL);
        } else { // S: 3 short dots (150 on, 150 off)
            uint32_t sub = t - 3300UL;
            on = (sub < 150UL) || (sub >= 300UL && sub < 450UL) || (sub >= 600UL && sub < 750UL);
        }
        if(on) {
            setFlightDeckMask(0x3FU);
            setBuzzerTone(2800U);
        } else {
            setFlightDeckMask(0x00U);
            setBuzzerTone(0U);
        }
        return;
    }

    if(s_alarm_latched) {
        s_last_activity_ms = now; // Keep screen awake while unacknowledged alarm sounds
        uint32_t phase = (uint32_t)((now - s_led_start_ms) % 1000UL);
        bool beep1 = (phase < 140UL);
        bool beep2 = (phase >= 220UL && phase < 360UL);
        bool beep3 = (phase >= 440UL && phase < 600UL);
        if(beep1 || beep3) {
            setBuzzerTone(s_cfg_alarm_sound ? 2700U : 0U);
            setFlightDeckLeds(true);
        } else if(beep2) {
            setBuzzerTone(s_cfg_alarm_sound ? 3200U : 0U);
            setFlightDeckLeds(true);
        } else {
            setBuzzerTone(0U);
            setFlightDeckLeds(false);
        }
        return;
    }

    if(s_popup_active && s_popup_is_call) {
        s_last_activity_ms = now; // Keep screen awake while call is ringing
        uint32_t phase = (uint32_t)((now - (s_popup_until_ms - 30000UL)) % 2000UL);
        bool ringTone1 = (phase < 180UL);
        bool ringTone2 = (phase >= 260UL && phase < 460UL);
        if(ringTone1) {
            setBuzzerTone(s_cfg_notif_sound ? 2600U : 0U);
            setFlightDeckLeds(s_cfg_notif_light);
        } else if(ringTone2) {
            setBuzzerTone(s_cfg_notif_sound ? 3000U : 0U);
            setFlightDeckLeds(s_cfg_notif_light);
        } else {
            setBuzzerTone(0U);
            setFlightDeckLeds(false);
        }
        return;
    }

    if(s_find_watch_active) {
        s_last_activity_ms = now; // Keep awake during find
        if((int32_t)(now - s_find_watch_until_ms) >= 0) {
            s_find_watch_active = false;
            setBuzzerTone(0U);
            setFlightDeckLeds(false);
        } else {
            uint32_t elapsed = (uint32_t)(now - (s_find_watch_until_ms - 4000UL));
            uint32_t phase = elapsed % 400UL;
            bool buzzOn = (phase < 200UL);
            if(buzzOn) {
                uint32_t freq = ((elapsed / 400UL) % 2UL == 0UL) ? 2700U : 3300U;
                setBuzzerTone(freq);
                setFlightDeckMask(0x3FU);
            } else {
                setBuzzerTone(0U);
                setFlightDeckMask(0x00U);
            }
        }
        return;
    }

    // Hourly retro double-pip chime check (:00)
    static int8_t s_last_chime_hr = -1;
    WatchDateTime rtcDt;
    if(readPcf8563Rtc(&rtcDt)) {
        if(rtcDt.min == 0U && rtcDt.sec < 2U && (int8_t)rtcDt.hour != s_last_chime_hr) {
            s_last_chime_hr = (int8_t)rtcDt.hour;
            if(s_cfg_hourly_chime && !s_cfg_night_dnd && !s_alarm_latched && !s_popup_active && !s_sos_active && !s_flashlight_active) {
                s_hourly_chime_active = true;
                s_hourly_chime_start_ms = now;
            }
        }
        if(rtcDt.min != 0U) {
            s_last_chime_hr = -1;
        }
    }

    if(s_hourly_chime_active) {
        uint32_t elapsed = (uint32_t)(now - s_hourly_chime_start_ms);
        if(elapsed < 45UL) {
            setBuzzerTone(3000U);
        } else if(elapsed >= 45UL && elapsed < 80UL) {
            setBuzzerTone(0U);
        } else if(elapsed >= 80UL && elapsed < 125UL) {
            setBuzzerTone(3000U);
        } else {
            setBuzzerTone(0U);
            s_hourly_chime_active = false;
        }
    } else if(s_notif_sound_active) {
        uint32_t elapsed = (uint32_t)(now - s_notif_sound_start_ms);
        if(elapsed < 70UL) {
            setBuzzerTone(2400U);
        } else if(elapsed >= 70UL && elapsed < 110UL) {
            setBuzzerTone(0U);
        } else if(elapsed >= 110UL && elapsed < 200UL) {
            setBuzzerTone(3000U);
        } else {
            setBuzzerTone(0U);
            s_notif_sound_active = false;
        }
    } else {
        setBuzzerTone(0U);
    }

    // Aesthetic periodic pulse check (suppressed in power saver)
    if(!s_power_saver_active && s_cfg_deck_pulse_idx > 0U && s_led_mode == LED_MODE_IDLE) {
        unsigned long intervalMs = (unsigned long)kDeckPulseMinutes[s_cfg_deck_pulse_idx % 6U] * 60UL * 1000UL;
        if(intervalMs > 0UL && (now - s_last_deck_pulse_ms >= intervalMs)) {
            s_last_deck_pulse_ms = now;
            startDeckAestheticPulse();
        }
    }

    if(s_led_mode == LED_MODE_AESTHETIC_PULSE) {
        if((int32_t)(now - s_led_until_ms) >= 0) {
            setFlightDeckMask(0x00U);
            s_led_mode = LED_MODE_IDLE;
        } else {
            uint32_t elapsed = (uint32_t)(now - s_led_start_ms);
            uint32_t phase = elapsed % 900UL;
            if(phase < 200UL) setFlightDeckMask(0x09U);       // Top pair (GPIO 17 & 44)
            else if(phase < 400UL) setFlightDeckMask(0x12U);  // Mid pair (GPIO 18 & 45)
            else if(phase < 600UL) setFlightDeckMask(0x24U);  // Bot pair (GPIO 43 & 46)
            else if(phase < 850UL) setFlightDeckMask(0x3FU);  // All on
            else setFlightDeckMask(0x00U);
        }
    } else if(s_led_mode == LED_MODE_NOTIF_PULSE) {
        if((int32_t)(now - s_led_until_ms) >= 0) {
            setFlightDeckMask(0x00U);
            s_led_mode = LED_MODE_IDLE;
        } else {
            uint32_t elapsed = (uint32_t)(now - s_led_start_ms);
            bool on = (elapsed < 120UL) || (elapsed >= 240UL && elapsed < 360UL);
            setFlightDeckMask(on ? 0x3FU : 0x00U);
        }
    } else if(s_led_mode == LED_MODE_FLASH_3S) {
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

    // Check low battery power saver
    bool is_low_batt = (s_batt_pct > 0 && s_batt_pct <= 15U && !s_batt_charging);
    s_power_saver_active = (s_cfg_power_saver == 2U) || (s_cfg_power_saver == 1U && is_low_batt);

    uint32_t timeoutMs = kTimeoutMsTable[s_cfg_timeout_idx % 7U];
    if(s_power_saver_active && (timeoutMs == 0UL || timeoutMs > 5000UL)) {
        timeoutMs = 5000UL;
    }

    if(!s_screen_asleep) {
        if(s_power_saver_active && getCpuFrequencyMhz() != 80) {
            setCpuFrequencyMhz(80);
        }

        // 1. If woken by wrist raise gesture without button interaction, enforce 5-second glance limit
        uint32_t glanceLimit = (timeoutMs > 0UL && timeoutMs < 5000UL) ? timeoutMs : 5000UL;
        if(s_woke_by_raise && (now - s_last_activity_ms > glanceLimit)) {
            s_screen_asleep = true;
            s_woke_by_raise = false;
            s_wrist_down_since_ms = 0UL;
            setBacklightPercent(0U);
            updateRgbLedStatus();
            setCpuFrequencyMhz(80);
            Serial.println("SCREEN_SLEEP|raise_glance_timeout|cpu=80MHz");
            return;
        }

        // 2. Standard user activity timeout (from button interactions or menus)
        if(timeoutMs > 0UL && (now - s_last_activity_ms > timeoutMs)) {
            s_screen_asleep = true;
            s_woke_by_raise = false;
            s_wrist_down_since_ms = 0UL;
            setBacklightPercent(0U);
            updateRgbLedStatus();
            setCpuFrequencyMhz(80);
            Serial.printf("SCREEN_SLEEP|timeout=%s|cpu=80MHz\n", kTimeoutLabels[s_cfg_timeout_idx % 7U]);
            return;
        }
    }

    if(!s_imu_ready || !s_cfg_raise_wake || s_cfg_night_dnd || (now - s_last_imu_poll_ms < 40U)) return;
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
    bool is_viewing = (az > 0.35f && fabsf(ay) < 0.80f && fabsf(ax) < 0.80f);
    bool is_down    = (az < 0.15f || fabsf(ay) > 0.65f);

    if(s_screen_asleep) {
        if(s_wrist_down && is_viewing) {
            s_wrist_down = false;
            s_wrist_down_since_ms = 0UL;
            wakeScreen("raise");
        } else if(is_down) {
            s_wrist_down = true;
        }
    } else {
        // Screen is currently awake
        if(is_down) {
            s_wrist_down = true;
            if(s_wrist_down_since_ms == 0UL) {
                s_wrist_down_since_ms = now;
            } else if(now - s_wrist_down_since_ms > 250UL) {
                // Wrist lowered continuously for 250ms -> SLEEP IMMEDIATELY!
                s_screen_asleep = true;
                s_woke_by_raise = false;
                s_wrist_down_since_ms = 0UL;
                setBacklightPercent(0U);
                updateRgbLedStatus();
                setCpuFrequencyMhz(80);
                Serial.println("SCREEN_SLEEP|wrist_dropped|cpu=80MHz");
            }
        } else {
            s_wrist_down_since_ms = 0UL;
            if(is_viewing) s_wrist_down = false;
        }
    }
}

// ─── 4-BUTTON INPUT (ACTIVE-HIGH WITH PULLDOWN) ───────────
static bool s_btn_prev[4] = { false, false, false, false };
static unsigned long s_last_btn_ms = 0;
static unsigned long s_btn_press_ms[4] = { 0, 0, 0, 0 };
static bool s_btn_long_fired[4] = { false, false, false, false };
static bool s_btn_woke_sleep[4] = { false, false, false, false };

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

// ─── TREND ARROW HELPERS ──────────────────────────────────────

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

// ─── NORDIC UART SERVICE (GADGETBRIDGE / JUGGLUCO PHONE BRIDGE) ──
#define NUS_SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_CHARACTERISTIC_RX_UUID "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_CHARACTERISTIC_TX_UUID "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

static BLEServer* s_pServer = nullptr;
static BLECharacteristic* s_pTxCharacteristic = nullptr;
static BLECharacteristic* s_pRxCharacteristic = nullptr;
static uint32_t s_juggluco_pkts_rx = 0U;
static portMUX_TYPE s_nus_rx_mux = portMUX_INITIALIZER_UNLOCKED;
static std::string s_nus_raw_buf = "";
static std::vector<std::string> s_nus_pending_lines;
static volatile bool s_ble_conn_event = false;
static volatile bool s_ble_disc_event = false;
static volatile uint8_t s_ble_last_disc_reason = 0;
static TrendDir s_last_juggluco_trend = TREND_UNKNOWN;

static void sendJugglucoTx(const char* fmt, ...) {
    if(!s_juggluco_connected || !s_pTxCharacteristic) return;
    char buf[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // Gadgetbridge Bangle.js driver splits on '\n' and slices line up to (p - 1),
    // strictly assuming each line terminates in CRLF ("\r\n"). If '\r' is omitted,
    // Gadgetbridge chops off the closing '}' of the JSON payload, triggering
    // "Malformed JSON from Bangle.js". We enforce clean "\r\n" termination here.
    size_t len = strlen(buf);
    while(len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n')) {
        buf[--len] = '\0';
    }
    if(len + 2 < sizeof(buf)) {
        buf[len++] = '\r';
        buf[len++] = '\n';
        buf[len] = '\0';
    }
    s_pTxCharacteristic->setValue((uint8_t*)buf, len);
    s_pTxCharacteristic->notify();
    Serial.printf("GB_TX|%s", buf);
}

static float parseGlucoseValue(const char* str, bool* out_is_mmol, int* out_priority = nullptr) {
    if(out_priority) *out_priority = 0;
    if(!str || !*str) return 0.0f;
    float best_val = 0.0f;
    bool best_is_mmol = false;
    int best_priority = 0;

    const char* p = str;
    while(*p) {
        // Skip unicode escapes \uXXXX or bare uXXXX (e.g. \u2192 arrow)
        if(*p == '\\' && *(p + 1) == 'u' && p[2] && p[3] && p[4] && p[5]) {
            p += 6;
            continue;
        }
        if((*p == 'u' || *p == 'U') && p[1] && p[2] && p[3] && p[4] &&
           isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2]) &&
           isxdigit((unsigned char)p[3]) && isxdigit((unsigned char)p[4]) &&
           (p == str || !isalpha((unsigned char)*(p - 1)))) {
            p += 5;
            continue;
        }

        if(isdigit((unsigned char)*p)) {
            // Check preceding context to reject sensor models, versions, battery, pins, etc.
            bool is_version_or_model = false;
            if(p > str) {
                char prev = *(p - 1);
                // Delimiters indicating non-glucose (e.g. time 12:30, pin #1, +10%, etc.)
                if(prev == ':' || prev == '+' || prev == '-' || prev == '#') {
                    while(*p && (isdigit((unsigned char)*p) || *p == '.' || *p == ',')) p++;
                    continue;
                }
                // Check if preceded by word like "Libre", "Dexcom", "Ver", "Version", "v", "Sensor", "Model"
                const char* back = p - 1;
                while(back > str && (*back == ' ' || *back == '\t')) back--;
                if(isalpha((unsigned char)*back)) {
                    const char* word_end = back + 1;
                    while(back > str && isalpha((unsigned char)*(back - 1))) back--;
                    size_t wlen = word_end - back;
                    if(wlen > 0) {
                        if((wlen == 5 && strncasecmp(back, "Libre", 5) == 0) ||
                           (wlen == 6 && strncasecmp(back, "Dexcom", 6) == 0) ||
                           (wlen == 6 && strncasecmp(back, "Sensor", 6) == 0) ||
                           (wlen == 7 && strncasecmp(back, "Version", 7) == 0) ||
                           (wlen == 3 && strncasecmp(back, "Ver", 3) == 0) ||
                           (wlen == 1 && (*back == 'v' || *back == 'V' || *back == 'g' || *back == 'G')) ||
                           (wlen == 5 && strncasecmp(back, "Build", 5) == 0) ||
                           (wlen == 5 && strncasecmp(back, "Model", 5) == 0)) {
                            is_version_or_model = true;
                        }
                    }
                }
            }

            char num_buf[32];
            int ni = 0;
            bool has_decimal = false;
            while(*p && ni < (int)sizeof(num_buf) - 1) {
                if(isdigit((unsigned char)*p)) {
                    num_buf[ni++] = *p++;
                } else if((*p == '.' || *p == ',') && !has_decimal && isdigit((unsigned char)*(p + 1))) {
                    has_decimal = true;
                    num_buf[ni++] = '.';
                    p++;
                } else {
                    break;
                }
            }
            num_buf[ni] = '\0';

            if(is_version_or_model) {
                continue;
            }

            float v = strtof(num_buf, nullptr);

            const char* s = p;
            while(*s == ' ' || *s == '\t') s++;

            bool is_time_or_date = false;
            if(*s == ':') {
                is_time_or_date = true;
            } else if(strncasecmp(s, "mmol", 4) == 0 || strncasecmp(s, "mg", 2) == 0) {
                is_time_or_date = false;
            } else if(*s == 'h' || *s == 'H' || *s == 's' || *s == 'S' ||
                      strncasecmp(s, "min", 3) == 0 || strncasecmp(s, "sec", 3) == 0 ||
                      strncasecmp(s, "hr", 2) == 0 || strncasecmp(s, "hour", 4) == 0 ||
                      strncasecmp(s, "ago", 3) == 0 || strncasecmp(s, "day", 3) == 0 ||
                      strncasecmp(s, "days", 4) == 0 ||
                      *s == '/' || *s == '%') {
                is_time_or_date = true;
            }

            if(!is_time_or_date && v > 0.0f) {
                // Priority 4: Explicit mmol/L or mg/dL
                if(strncasecmp(s, "mmol", 4) == 0 && v >= 1.0f && v <= 35.0f) {
                    best_val = v;
                    best_is_mmol = true;
                    best_priority = 4;
                    break; // Can't get better than explicit mmol/L
                } else if(strncasecmp(s, "mg", 2) == 0 && v >= 35.0f && v <= 500.0f) {
                    best_val = v;
                    best_is_mmol = false;
                    best_priority = 4;
                    break; // Can't get better than explicit mg/dL
                } else if(has_decimal && v >= 1.0f && v <= 35.0f) {
                    // Priority 3: Decimal value formatted as mmol/L (e.g. "3.8")
                    if(best_priority < 3) {
                        best_val = v;
                        best_is_mmol = true;
                        best_priority = 3;
                    }
                } else if(!has_decimal && v >= 35.0f && v <= 500.0f) {
                    // Priority 2: Integer value formatted as mg/dL (e.g. "106")
                    if(best_priority < 2) {
                        best_val = v;
                        best_is_mmol = false;
                        best_priority = 2;
                    }
                }
                // Bare integer < 35 without units is intentionally not accepted as glucose (e.g. "2" from "Libre 2")
            }
            continue;
        }
        p++;
    }

    // Only if NO numeric glucose was found in the text, check for standalone out-of-range "LO" or "HI"
    if(best_priority == 0) {
        if(strcmp(str, "HI") == 0 || strstr(str, " HI ") || strstr(str, " HI") || strcmp(str, "HIGH") == 0) {
            if(out_is_mmol) *out_is_mmol = true;
            if(out_priority) *out_priority = 1;
            return 22.2f;
        }
        if(strcmp(str, "LO") == 0 || strstr(str, " LO ") || strcmp(str, "LOW") == 0) {
            if(out_is_mmol) *out_is_mmol = true;
            if(out_priority) *out_priority = 1;
            return 2.2f;
        }
    }

    if(best_priority > 0) {
        if(out_is_mmol) *out_is_mmol = best_is_mmol;
        if(out_priority) *out_priority = best_priority;
        return best_val;
    }
    return 0.0f;
}

static TrendDir parseTrendDirection(const char* text) {
    if(!text || !*text) return TREND_UNKNOWN;

    // Fast Rising (Double Up)
    if(strstr(text, "↑↑") || strstr(text, "⇈") || strstr(text, "\\u21c8") || strstr(text, "u21c8") ||
       strstr(text, "DoubleUp") || strstr(text, "FastUp") || strstr(text, "RisingFast") ||
       strstr(text, "^^") || strstr(text, "++")) {
        return TREND_RISE_FAST;
    }

    // Fast Falling (Double Down)
    if(strstr(text, "↓↓") || strstr(text, "⇊") || strstr(text, "\\u21ca") || strstr(text, "u21ca") ||
       strstr(text, "DoubleDown") || strstr(text, "FastDown") || strstr(text, "FallingFast") ||
       strstr(text, "vv") || strstr(text, "--")) {
        return TREND_FALL_FAST;
    }

    // Rising Slow (45 deg Up)
    if(strstr(text, "↗") || strstr(text, "⬈") || strstr(text, "\\u2197") || strstr(text, "u2197") ||
       strstr(text, "\\u2b08") || strstr(text, "u2b08") ||
       strstr(text, "FortyFiveUp") || strstr(text, "RisingSlowly") ||
       strstr(text, "/^") || strstr(text, ">^") || strstr(text, " / ")) {
        return TREND_RISE_SLOW;
    }

    // Falling Slow (45 deg Down)
    if(strstr(text, "↘") || strstr(text, "⬊") || strstr(text, "\\u2198") || strstr(text, "u2198") ||
       strstr(text, "\\u2b0a") || strstr(text, "u2b0a") ||
       strstr(text, "FortyFiveDown") || strstr(text, "FallingSlowly") ||
       strstr(text, "\\v") || strstr(text, ">v") || strstr(text, " \\ ")) {
        return TREND_FALL_SLOW;
    }

    // Rising (Single Up)
    if(strstr(text, "↑") || strstr(text, "⬆") || strstr(text, "\\u2191") || strstr(text, "u2191") ||
       strstr(text, "\\u2b06") || strstr(text, "u2b06") ||
       strstr(text, "SingleUp") || strstr(text, "Rising") || strstr(text, "RISING") ||
       strstr(text, " ^ ") || strstr(text, "^")) {
        return TREND_RISE;
    }

    // Falling (Single Down)
    if(strstr(text, "↓") || strstr(text, "⬇") || strstr(text, "\\u2193") || strstr(text, "u2193") ||
       strstr(text, "\\u2b07") || strstr(text, "u2b07") ||
       strstr(text, "SingleDown") || strstr(text, "Falling") || strstr(text, "FALLING") ||
       strstr(text, " v ") || strstr(text, " v") || strstr(text, "v ")) {
        return TREND_FALL;
    }

    // Flat / Steady
    if(strstr(text, "→") || strstr(text, "->") || strstr(text, "-->") ||
       strstr(text, "\\u2192") || strstr(text, "u2192") ||
       strstr(text, "Flat") || strstr(text, "FLAT") || strstr(text, "Steady") || strstr(text, "STEADY")) {
        return TREND_FLAT;
    }

    return TREND_UNKNOWN;
}

static void processJugglucoReading(float val, bool is_mmol, TrendDir parsed_trend) {
    if(val <= 0.0f) return;
    if(parsed_trend != TREND_UNKNOWN) {
        s_last_juggluco_trend = parsed_trend;
    }
    float mmol = is_mmol ? val : (val / 18.0182f);
    float mg   = is_mmol ? (val * 18.0182f) : val;
    uint16_t mmolT = (uint16_t)(mmol * 10.0f + 0.5f);
    uint16_t mgDl  = (uint16_t)(mg + 0.5f);

    compState.has_scan = true;
    compState.vendor_chain.available = true;
    compState.vendor_chain.mmol_tenths = mmolT;
    compState.vendor_chain.mg_dl = mgDl;
    snprintf(compState.primary_line, sizeof(compState.primary_line), "%u.%u mmol/L",
             (unsigned)(mmolT / 10U), (unsigned)(mmolT % 10U));
    lastScanMillis = millis();
    scanCount++;
    snprintf(lastTransport, sizeof(lastTransport), "BLE-J");

    // Add point to history
    uint32_t bootSec = (uint32_t)(millis() / 1000U);
    const char* ser = (compState.has_scan && compState.serial_valid) ? compState.serial : "JUGGLUCO";
    uint16_t ageMin = compState.has_scan ? compState.integrity.sensor.age_minutes : (uint16_t)(bootSec / 60U);
    bool isNewPt = ll_history_add(&s_history, ser, ageMin, mmolT, mgDl, bootSec);
    if(isNewPt && s_history.count > 0U) {
        appendHistoryToSpiffs(&s_history.points[s_history.count - 1U]);
    }

    triggerGlucoseAlerts(mmolT);
    updateRgbLedStatus();

    if(s_alarm_latched) {
        wakeScreen("urgent_juggluco");
    } else {
        if(!s_screen_asleep) renderDisplay();
    }

    sendJugglucoTx("{\"t\":\"status\",\"bat\":%u}\n", s_batt_pct);
}

// Helper to extract string or numeric value of a key from a JSON-like string
static bool extractJsonField(const char* json, const char* key, char* out, size_t max_out) {
    if(!json || !key || !out || max_out == 0) return false;
    out[0] = '\0';

    char key_buf[48];
    snprintf(key_buf, sizeof(key_buf), "\"%s\"", key);
    const char* p = strstr(json, key_buf);
    if(!p) return false;
    p += strlen(key_buf);

    while(*p && (*p == ' ' || *p == '\t')) p++;
    if(*p != ':') return false;
    p++;

    while(*p && (*p == ' ' || *p == '\t')) p++;
    if(!*p) return false;

    size_t idx = 0;
    if(*p == '\"') {
        p++; // skip open quote
        while(*p && *p != '\"' && idx + 1 < max_out) {
            if(*p == '\\' && *(p + 1)) {
                if(*(p + 1) == 'u' && p[2] && p[3] && p[4] && p[5] &&
                   isxdigit((unsigned char)p[2]) && isxdigit((unsigned char)p[3]) &&
                   isxdigit((unsigned char)p[4]) && isxdigit((unsigned char)p[5])) {
                    uint16_t cp = 0;
                    for(int i = 2; i <= 5; i++) {
                        char c = p[i];
                        uint8_t d = (c >= '0' && c <= '9') ? (c - '0') :
                                    (c >= 'a' && c <= 'f') ? (c - 'a' + 10) :
                                    (c >= 'A' && c <= 'F') ? (c - 'A' + 10) : 0;
                        cp = (cp << 4) | d;
                    }
                    p += 6;
                    if(cp <= 0x7F) {
                        out[idx++] = (char)cp;
                    } else if(cp <= 0x7FF) {
                        if(idx + 2 < max_out) {
                            out[idx++] = (char)(0xC0 | (cp >> 6));
                            out[idx++] = (char)(0x80 | (cp & 0x3F));
                        }
                    } else {
                        if(idx + 3 < max_out) {
                            out[idx++] = (char)(0xE0 | (cp >> 12));
                            out[idx++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            out[idx++] = (char)(0x80 | (cp & 0x3F));
                        }
                    }
                    continue;
                } else if(*(p + 1) == 'n') {
                    p += 2;
                    out[idx++] = '\n';
                    continue;
                } else if(*(p + 1) == 'r') {
                    p += 2;
                    out[idx++] = '\r';
                    continue;
                } else if(*(p + 1) == 't') {
                    p += 2;
                    out[idx++] = '\t';
                    continue;
                } else {
                    p++; // skip backslash escape for \", \\, \/
                }
            }
            out[idx++] = *p++;
        }
    } else {
        // Unquoted numeric or bare string
        while(*p && *p != ',' && *p != '}' && *p != ')' && *p != '\r' && *p != '\n' && *p != ' ' && idx + 1 < max_out) {
            out[idx++] = *p++;
        }
    }
    out[idx] = '\0';
    return (idx > 0);
}

static void addPhoneNotification(uint32_t id, const char* src, const char* title, const char* body) {
    if((!title || !title[0]) && (!body || !body[0])) return;

    // Check for Magic Settings Notifications (#ICE: or #CFG:)
    char magicCheck[256];
    snprintf(magicCheck, sizeof(magicCheck), "%s %s", title ? title : "", body ? body : "");
    const char* iceTag = strstr(magicCheck, "#ICE:");
    if(!iceTag) iceTag = strstr(magicCheck, "!ICE:");
    if(!iceTag) iceTag = strstr(magicCheck, "#ice:");
    if(iceTag) {
        const char* p = iceTag + 5;
        while(*p == ' ' || *p == '\t') p++;
        const char* sep = strpbrk(p, "/|,");
        if(sep && sep > p) {
            char name[32] = "";
            char phone[32] = "";
            size_t nlen = (size_t)(sep - p);
            if(nlen >= sizeof(name)) nlen = sizeof(name) - 1;
            strncpy(name, p, nlen);
            name[nlen] = '\0';
            while(nlen > 0 && (name[nlen - 1] == ' ' || name[nlen - 1] == '\t')) name[--nlen] = '\0';

            const char* ph = sep + 1;
            while(*ph == ' ' || *ph == '\t') ph++;
            snprintf(phone, sizeof(phone), "%s", ph);
            char* end = phone + strlen(phone);
            while(end > phone && (*(end - 1) == ' ' || *(end - 1) == '\r' || *(end - 1) == '\n' || *(end - 1) == '\t')) *(--end) = '\0';

            if(name[0] && phone[0]) {
                snprintf(s_ice_name, sizeof(s_ice_name), "%s", name);
                snprintf(s_ice_phone, sizeof(s_ice_phone), "%s", phone);
                saveIceToSpiffs();
                wakeScreen("ice_magic");
                showPopup("ICE UPDATED", s_ice_name, s_ice_phone, "Saved from Phone", 4000U);
                sendJugglucoTx("{\"t\":\"notify\",\"title\":\"Watch ICE Saved\",\"body\":\"%s: %s\"}\n", s_ice_name, s_ice_phone);
                Serial.printf("MAGIC_NOTIF_ICE|name=%s|phone=%s\n", s_ice_name, s_ice_phone);
                return;
            }
        }
    }

    const char* cfgTag = strstr(magicCheck, "#CFG:");
    if(!cfgTag) cfgTag = strstr(magicCheck, "!CFG:");
    if(!cfgTag) cfgTag = strstr(magicCheck, "#cfg:");
    if(cfgTag) {
        const char* dp = strcasestr(cfgTag, "DND=");
        if(dp) {
            if(strncasecmp(dp + 4, "ON", 2) == 0 || *(dp + 4) == '1') s_cfg_night_dnd = true;
            else if(strncasecmp(dp + 4, "OFF", 3) == 0 || *(dp + 4) == '0') s_cfg_night_dnd = false;
        }
        const char* sp = strcasestr(cfgTag, "SAVER=");
        if(sp) {
            if(strncasecmp(sp + 6, "AUTO", 4) == 0 || *(sp + 6) == '1') s_cfg_power_saver = 1;
            else if(strncasecmp(sp + 6, "ON", 2) == 0 || *(sp + 6) == '2') s_cfg_power_saver = 2;
            else if(strncasecmp(sp + 6, "OFF", 3) == 0 || *(sp + 6) == '0') s_cfg_power_saver = 0;
        }
        const char* cp = strcasestr(cfgTag, "CHIME=");
        if(cp) {
            if(strncasecmp(cp + 6, "ON", 2) == 0 || *(cp + 6) == '1') s_cfg_hourly_chime = true;
            else if(strncasecmp(cp + 6, "OFF", 3) == 0 || *(cp + 6) == '0') s_cfg_hourly_chime = false;
        }
        const char* bp = strcasestr(cfgTag, "BRIGHT=");
        if(bp) {
            int b = atoi(bp + 7);
            if(b >= 0 && b <= 100) {
                setBacklightPercent((uint8_t)b);
                for(uint8_t bi = 0; bi < 5U; bi++) {
                    if(kBrightPctTable[bi] == (uint8_t)b) s_cfg_bright_idx = bi;
                }
            }
        }
        const char* tp = strcasestr(cfgTag, "THEME=");
        if(tp) {
            int th = atoi(tp + 6);
            if(th >= 0 && th < 6) s_cfg_theme_idx = (uint8_t)th;
        }
        const char* rp = strcasestr(cfgTag, "RGB=");
        if(rp) {
            if(strncasecmp(rp + 4, "ON", 2) == 0 || *(rp + 4) == '1') s_cfg_rgb_led = true;
            else if(strncasecmp(rp + 4, "OFF", 3) == 0 || *(rp + 4) == '0') s_cfg_rgb_led = false;
            updateRgbLedStatus();
        }
        const char* wp = strcasestr(cfgTag, "WAKE=");
        if(!wp) wp = strcasestr(cfgTag, "RW=");
        if(wp) {
            const char* eq = strchr(wp, '=');
            if(eq) {
                eq++;
                if(strncasecmp(eq, "ON", 2) == 0 || *eq == '1') s_cfg_raise_wake = true;
                else if(strncasecmp(eq, "OFF", 3) == 0 || *eq == '0') s_cfg_raise_wake = false;
            }
        }
        saveSettingsToSpiffs();
        renderDisplay();
        wakeScreen("cfg_magic");
        showPopup("CONFIG UPDATED", "Settings saved", "from notification", "OK", 3500U);
        sendJugglucoTx("{\"t\":\"notify\",\"title\":\"Watch Settings Updated\",\"body\":\"New configuration saved\"}\n");
        Serial.println("MAGIC_NOTIF_CFG|saved");
        return;
    }

    // Check if ID already exists to update in place
    if(id != 0U) {
        for(uint8_t i = 0; i < s_notif_count; i++) {
            if(s_phone_notifs[i].id == id) {
                if(src && src[0]) snprintf(s_phone_notifs[i].src, sizeof(s_phone_notifs[i].src), "%s", src);
                if(title && title[0]) snprintf(s_phone_notifs[i].title, sizeof(s_phone_notifs[i].title), "%s", title);
                if(body && body[0]) snprintf(s_phone_notifs[i].body, sizeof(s_phone_notifs[i].body), "%s", body);
                s_phone_notifs[i].active = true;
                return;
            }
        }
    }

    // Shift older notifications down
    for(int i = (int)MAX_PHONE_NOTIFS - 1; i > 0; i--) {
        s_phone_notifs[i] = s_phone_notifs[i - 1];
    }

    // Insert at index 0 (newest)
    s_phone_notifs[0].id = id;
    snprintf(s_phone_notifs[0].src, sizeof(s_phone_notifs[0].src), "%s", (src && src[0]) ? src : "Phone");
    snprintf(s_phone_notifs[0].title, sizeof(s_phone_notifs[0].title), "%s", title ? title : "");
    snprintf(s_phone_notifs[0].body, sizeof(s_phone_notifs[0].body), "%s", body ? body : "");
    s_phone_notifs[0].hour = 12U;
    s_phone_notifs[0].min = 0U;
    s_phone_notifs[0].active = true;

    WatchDateTime dt;
    if(readPcf8563Rtc(&dt)) {
        s_phone_notifs[0].hour = dt.hour;
        s_phone_notifs[0].min = dt.min;
    }

    if(s_notif_count < MAX_PHONE_NOTIFS) s_notif_count++;
    s_notif_selected = 0U;

    // Notification Alerts (suppressed during Night DND)
    if(!s_cfg_night_dnd) {
        if(s_cfg_notif_light && !s_power_saver_active) {
            s_led_mode = LED_MODE_NOTIF_PULSE;
            s_led_start_ms = millis();
            s_led_until_ms = s_led_start_ms + 600UL;
        }
        if(s_cfg_notif_sound) {
            playNotificationChime();
        }
        wakeScreen("notif");
        showNotifPopup(s_phone_notifs[0].id, s_phone_notifs[0].src, s_phone_notifs[0].title, s_phone_notifs[0].body, 5000U);
    }
}

static void removePhoneNotification(uint32_t id) {
    if(id == 0U) {
        s_notif_count = 0U;
        s_notif_selected = 0U;
        renderDisplay();
        return;
    }
    for(uint8_t i = 0; i < s_notif_count; i++) {
        if(s_phone_notifs[i].id == id) {
            for(uint8_t j = i; j < s_notif_count - 1U; j++) {
                s_phone_notifs[j] = s_phone_notifs[j + 1U];
            }
            s_notif_count--;
            if(s_notif_selected >= s_notif_count && s_notif_count > 0U) {
                s_notif_selected = s_notif_count - 1U;
            }
            renderDisplay();
            return;
        }
    }
}

static void dismissPhoneNotification(uint8_t index) {
    if(index >= s_notif_count) return;
    uint32_t nid = s_phone_notifs[index].id;
    if(nid != 0U) {
        sendJugglucoTx("{\"t\":\"notify\",\"id\":%lu,\"n\":\"DISMISS\"}\n", (unsigned long)nid);
    }
    for(uint8_t j = index; j < s_notif_count - 1U; j++) {
        s_phone_notifs[j] = s_phone_notifs[j + 1U];
    }
    s_notif_count--;
    if(s_notif_selected >= s_notif_count && s_notif_count > 0U) {
        s_notif_selected = s_notif_count - 1U;
    }
    if(s_popup_active && s_popup_is_notif) {
        s_popup_active = false;
        s_popup_is_notif = false;
    }
    Serial.printf("NOTIF_DISMISSED|id=%lu|remaining=%u\n", (unsigned long)nid, (unsigned)s_notif_count);
    renderDisplay();
}

static void dismissAllPhoneNotifications() {
    sendJugglucoTx("{\"t\":\"notify\",\"n\":\"DISMISS_ALL\"}\n");
    s_notif_count = 0U;
    s_notif_selected = 0U;
    if(s_popup_active && s_popup_is_notif) {
        s_popup_active = false;
        s_popup_is_notif = false;
    }
    Serial.println("NOTIF_DISMISSED_ALL");
    renderDisplay();
}

static void handleIncomingLine(const char* line, const char* source);

static bool parseGadgetbridgeLine(const char* raw_line) {
    if(!raw_line) return false;

    // Skip leading control chars (e.g. \x10 DLE prefix sent by Gadgetbridge Bangle.js driver)
    const char* line = raw_line;
    while(*line && (uint8_t)*line <= 0x20) line++;
    if(!*line) return false;

    Serial.printf("GB_RX|len=%u|line=%s\n", (unsigned)strlen(line), line);

    // 1. Pipe-delimited serial commands or plain control words over BLE NUS (from Tasker com.banglejs.uart.tx, Web Bluetooth, scripts)
    if(line[0] != '{' && strncmp(line, "GB(", 3) != 0 &&
       (strchr(line, '|') != NULL || strcmp(line, "STATUS") == 0 || strcmp(line, "status") == 0 ||
        strcmp(line, "WAKE_NOW") == 0 || strcmp(line, "SLEEP_NOW") == 0)) {
        handleIncomingLine(line, "BLE");
        return true;
    }

    // Timezone synchronization: setTimeZone(1.0) or setTimeZone(-5)
    static double s_gb_tz_offset_hours = 0.0;
    const char* stz = strstr(line, "setTimeZone(");
    if(stz) {
        s_gb_tz_offset_hours = strtod(stz + 12, NULL);
        Serial.printf("RTC_TZ_GB|offset_hours=%.2f\n", s_gb_tz_offset_hours);
    } else {
        char tz_val[16];
        if(extractJsonField(line, "tz", tz_val, sizeof(tz_val))) {
            s_gb_tz_offset_hours = strtod(tz_val, NULL);
            Serial.printf("RTC_TZ_GB|offset_hours=%.2f\n", s_gb_tz_offset_hours);
        }
    }

    // Time synchronization: setTime(1729158000) or GB({"t":"setTime","time":...})
    const char* st = strstr(line, "setTime(");
    unsigned long unix_secs = 0UL;
    if(st) {
        unix_secs = strtoul(st + 8, NULL, 10);
    } else {
        char t_type[24];
        if(extractJsonField(line, "t", t_type, sizeof(t_type)) && strcmp(t_type, "setTime") == 0) {
            char time_val[32];
            if(extractJsonField(line, "time", time_val, sizeof(time_val))) {
                unix_secs = strtoul(time_val, NULL, 10);
            }
        } else {
            char time_val[32];
            if(extractJsonField(line, "time", time_val, sizeof(time_val))) {
                unsigned long t = strtoul(time_val, NULL, 10);
                if(t > 1600000000UL) unix_secs = t;
            }
        }
    }
    if(unix_secs > 1600000000UL) {
        time_t t = (time_t)(unix_secs + (long)(s_gb_tz_offset_hours * 3600.0));
        struct tm tm;
        gmtime_r(&t, &tm);
        WatchDateTime dt = {
            (uint16_t)(1900 + tm.tm_year),
            (uint8_t)(tm.tm_mon + 1),
            (uint8_t)tm.tm_mday,
            (uint8_t)tm.tm_wday,
            (uint8_t)tm.tm_hour,
            (uint8_t)tm.tm_min,
            (uint8_t)tm.tm_sec
        };
        writePcf8563Rtc(dt);
        Serial.printf("RTC_SYNC_GB|unix=%lu|tz=%.1fh|%04u-%02u-%02u %02u:%02u:%02u\n",
                      unix_secs, s_gb_tz_offset_hours, dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec);
        renderDisplay();
        return true;
    }

    // Status / GPS queries
    char t_type[24];
    if(extractJsonField(line, "t", t_type, sizeof(t_type))) {
        if(strcmp(t_type, "status") == 0 || strcmp(t_type, "ver") == 0) {
            sendJugglucoTx("{\"t\":\"status\",\"bat\":%u}\n", s_batt_pct);
            return true;
        }
        if(strcmp(t_type, "is_gps_active") == 0) {
            sendJugglucoTx("{\"t\":\"gps_power\",\"status\":false}\n");
            return true;
        }
    } else if(strstr(line, "\"t\":\"status\"") || strstr(line, "\"t\":\"ver\"")) {
        sendJugglucoTx("{\"t\":\"status\",\"bat\":%u}\n", s_batt_pct);
        return true;
    } else if(strstr(line, "\"is_gps_active\"")) {
        sendJugglucoTx("{\"t\":\"gps_power\",\"status\":false}\n");
        return true;
    }

    // ICE Contact Configuration: GB({"t":"set_ice","name":"Jane Doe","phone":"+447700900123"})
    if(extractJsonField(line, "t", t_type, sizeof(t_type)) &&
       (strcmp(t_type, "set_ice") == 0 || strcmp(t_type, "ice") == 0)) {
        char name[32] = "";
        char phone[32] = "";
        extractJsonField(line, "name", name, sizeof(name));
        extractJsonField(line, "phone", phone, sizeof(phone));
        if(name[0] && phone[0]) {
            snprintf(s_ice_name, sizeof(s_ice_name), "%s", name);
            snprintf(s_ice_phone, sizeof(s_ice_phone), "%s", phone);
            saveIceToSpiffs();
            wakeScreen("ice_sync");
            showPopup("ICE UPDATED", s_ice_name, s_ice_phone, "Saved via Phone", 3500U);
            sendJugglucoTx("{\"t\":\"ice\",\"status\":\"ok\",\"name\":\"%s\",\"phone\":\"%s\"}\n", s_ice_name, s_ice_phone);
            Serial.printf("GB_ICE_SET|name=%s|phone=%s\n", s_ice_name, s_ice_phone);
            return true;
        }
    }
    if(extractJsonField(line, "t", t_type, sizeof(t_type)) && strcmp(t_type, "get_ice") == 0) {
        sendJugglucoTx("{\"t\":\"ice\",\"name\":\"%s\",\"phone\":\"%s\"}\n", s_ice_name, s_ice_phone);
        return true;
    }

    // Settings Configuration: GB({"t":"set_cfg","bright":100,"dnd":1,"saver":1,...})
    if(extractJsonField(line, "t", t_type, sizeof(t_type)) &&
       (strcmp(t_type, "set_cfg") == 0 || strcmp(t_type, "cfg") == 0)) {
        char val[32];
        if(extractJsonField(line, "bright", val, sizeof(val))) {
            uint8_t b = (uint8_t)atoi(val);
            if(b > 100U) b = 100U;
            setBacklightPercent(b);
            for(uint8_t bi = 0; bi < 5U; bi++) {
                if(kBrightPctTable[bi] == b) s_cfg_bright_idx = bi;
            }
        }
        if(extractJsonField(line, "dnd", val, sizeof(val))) {
            s_cfg_night_dnd = (atoi(val) != 0);
        }
        if(extractJsonField(line, "saver", val, sizeof(val)) || extractJsonField(line, "ps", val, sizeof(val))) {
            s_cfg_power_saver = (uint8_t)(atoi(val) % 3);
        }
        if(extractJsonField(line, "theme", val, sizeof(val))) {
            s_cfg_theme_idx = (uint8_t)(atoi(val) % 6);
        }
        if(extractJsonField(line, "chime", val, sizeof(val)) || extractJsonField(line, "hchm", val, sizeof(val))) {
            s_cfg_hourly_chime = (atoi(val) != 0);
        }
        if(extractJsonField(line, "raise_wake", val, sizeof(val)) || extractJsonField(line, "rw", val, sizeof(val))) {
            s_cfg_raise_wake = (atoi(val) != 0);
        }
        if(extractJsonField(line, "rgb_led", val, sizeof(val)) || extractJsonField(line, "rgb", val, sizeof(val))) {
            s_cfg_rgb_led = (atoi(val) != 0);
            updateRgbLedStatus();
        }
        if(extractJsonField(line, "glitch", val, sizeof(val))) {
            s_cfg_glitch_fx = (atoi(val) != 0);
        }
        saveSettingsToSpiffs();
        renderDisplay();
        wakeScreen("cfg_sync");
        showPopup("CONFIG UPDATED", "Settings saved", "from Phone app", "OK", 3000U);
        sendJugglucoTx("{\"t\":\"cfg\",\"status\":\"ok\"}\n");
        return true;
    }
    if(extractJsonField(line, "t", t_type, sizeof(t_type)) && strcmp(t_type, "get_cfg") == 0) {
        sendJugglucoTx("{\"t\":\"cfg\",\"bright\":%u,\"dnd\":%u,\"saver\":%u,\"theme\":%u,\"chime\":%u,\"rw\":%u,\"rgb\":%u,\"ice_name\":\"%s\",\"ice_phone\":\"%s\"}\n",
                       (unsigned)kBrightPctTable[s_cfg_bright_idx % 5U],
                       (unsigned)(s_cfg_night_dnd ? 1U : 0U),
                       (unsigned)s_cfg_power_saver,
                       (unsigned)s_cfg_theme_idx,
                       (unsigned)(s_cfg_hourly_chime ? 1U : 0U),
                       (unsigned)(s_cfg_raise_wake ? 1U : 0U),
                       (unsigned)(s_cfg_rgb_led ? 1U : 0U),
                       s_ice_name, s_ice_phone);
        return true;
    }

    // Native Gadgetbridge Alarms: GB({"t":"alarm","d":[{"h":7,"m":30,"rep":127,"on":true},...]})
    if(extractJsonField(line, "t", t_type, sizeof(t_type)) && strcmp(t_type, "alarm") == 0) {
        char h_str[16] = "", m_str[16] = "";
        if(extractJsonField(line, "h", h_str, sizeof(h_str)) && extractJsonField(line, "m", m_str, sizeof(m_str))) {
            uint8_t ah = (uint8_t)atoi(h_str);
            uint8_t am = (uint8_t)atoi(m_str);
            char alarmBuf[24];
            snprintf(alarmBuf, sizeof(alarmBuf), "Alarm: %02u:%02u", ah, am);
            showPopup("ALARM SYNCED", alarmBuf, "From Gadgetbridge", "OK", 3000U);
            Serial.printf("GB_ALARM_SYNC|h=%u|m=%u\n", ah, am);
        }
        return true;
    }

    // Direct CGM / Glucose packet: GB({"t":"cgm","val":"5.4","trend":"->"})
    if(strstr(line, "\"cgm\"") || strstr(line, "\"glucose\"") ||
       (extractJsonField(line, "t", t_type, sizeof(t_type)) && (strcmp(t_type, "cgm") == 0 || strcmp(t_type, "glucose") == 0))) {
        char val_str[32] = "";
        char trend_str[32] = "";
        if(!extractJsonField(line, "val", val_str, sizeof(val_str))) {
            if(!extractJsonField(line, "glucose", val_str, sizeof(val_str))) {
                extractJsonField(line, "value", val_str, sizeof(val_str));
            }
        }
        if(!extractJsonField(line, "trend", trend_str, sizeof(trend_str))) {
            extractJsonField(line, "dir", trend_str, sizeof(trend_str));
        }

        if(val_str[0]) {
            bool is_mmol = false;
            float val = parseGlucoseValue(val_str, &is_mmol);
            if(val > 0.0f) {
                TrendDir tdir = trend_str[0] ? parseTrendDirection(trend_str) : TREND_FLAT;
                processJugglucoReading(val, is_mmol, tdir);
                s_juggluco_pkts_rx++;
                Serial.printf("JUGGLUCO_CGM_PKT|val=%.1f|trend=%s\n", val, trend_str);
                return true;
            }
        }
    }

    // Weather packet: GB({"t":"weather","temp":...,"hi":...,"lo":...,"loc":"5.4 ->",...})
    // Juggluco routes glucose to Gadgetbridge as weather data!
    if(strstr(line, "\"weather\"") || (extractJsonField(line, "t", t_type, sizeof(t_type)) && strcmp(t_type, "weather") == 0)) {
        char loc_str[64] = "";
        char txt_str[64] = "";
        char hi_str[16] = "";
        char lo_str[16] = "";
        char temp_str[16] = "";
        extractJsonField(line, "loc", loc_str, sizeof(loc_str));
        extractJsonField(line, "txt", txt_str, sizeof(txt_str));
        extractJsonField(line, "hi", hi_str, sizeof(hi_str));
        extractJsonField(line, "lo", lo_str, sizeof(lo_str));
        extractJsonField(line, "temp", temp_str, sizeof(temp_str));

        bool is_mmol = false;
        float val = 0.0f;

        // 1. Try parsing glucose from "loc" field (e.g. "5.4 ->", "106 v", "5.4 mmol/L")
        if(loc_str[0]) {
            val = parseGlucoseValue(loc_str, &is_mmol);
        }
        // 2. Try parsing glucose from "txt" field
        if(val <= 0.0f && txt_str[0]) {
            val = parseGlucoseValue(txt_str, &is_mmol);
        }
        // 3. If loc didn't contain glucose directly, reconstruct from Juggluco's hi/lo encoding
        if(val <= 0.0f && hi_str[0] && lo_str[0]) {
            int hi = atoi(hi_str);
            int lo = atoi(lo_str);
            if(hi >= 1 && hi <= 35 && lo >= 0 && lo <= 9) {
                // mmol/L: hi is integer part, lo is decimal tenths (e.g. hi=5, lo=4 -> 5.4)
                val = (float)hi + ((float)lo) / 10.0f;
                is_mmol = true;
            } else if(hi >= 4 && hi <= 50 && lo >= 0 && lo <= 9) {
                // mg/dL: lo is last digit, hi is preceding digits (e.g. hi=10, lo=6 -> 106)
                val = (float)hi * 10.0f + (float)lo;
                is_mmol = false;
            }
        }

        if(val > 0.0f) {
            char combined[128];
            snprintf(combined, sizeof(combined), "%s %s", loc_str, txt_str);
            TrendDir tdir = parseTrendDirection(combined);
            if(tdir == TREND_UNKNOWN && temp_str[0]) {
                int temp = atoi(temp_str);
                if(temp > 0) tdir = TREND_RISE;
                else if(temp < 0) tdir = TREND_FALL;
                else tdir = TREND_FLAT;
            }
            processJugglucoReading(val, is_mmol, tdir);
            s_juggluco_pkts_rx++;
            Serial.printf("JUGGLUCO_WEATHER_RECV|loc=%s|val=%.1f|is_mmol=%d|trend=%d\n", loc_str, val, (int)is_mmol, (int)tdir);
            return true;
        }
    }

    // Call handling: GB({"t":"call","n":"INCOMING","name":"Alice","tel":"+1555..."})
    bool is_call = (extractJsonField(line, "t", t_type, sizeof(t_type)) && strcmp(t_type, "call") == 0);
    if(!is_call && (strstr(line, "\"t\":\"call\"") || strstr(line, "\"t\": \"call\""))) {
        is_call = true;
    }
    if(is_call) {
        char n_cmd[24] = "";
        if(!extractJsonField(line, "n", n_cmd, sizeof(n_cmd))) {
            extractJsonField(line, "cmd", n_cmd, sizeof(n_cmd));
        }
        char caller_name[48] = "";
        char caller_tel[32] = "";
        extractJsonField(line, "name", caller_name, sizeof(caller_name));
        if(!extractJsonField(line, "tel", caller_tel, sizeof(caller_tel))) {
            extractJsonField(line, "number", caller_tel, sizeof(caller_tel));
        }

        if(strcmp(n_cmd, "INCOMING") == 0 || strcmp(n_cmd, "START") == 0 || strcasecmp(n_cmd, "incoming") == 0 ||
           (n_cmd[0] == '\0' && (caller_name[0] || caller_tel[0]))) {
            wakeScreen("call");
            showCallPopup(caller_name[0] ? caller_name : "Incoming Call", caller_tel[0] ? caller_tel : "Phone Call");
            Serial.printf("GB_CALL_INCOMING|name=%s|tel=%s\n", caller_name, caller_tel);
            return true;
        } else if(strcmp(n_cmd, "END") == 0 || strcmp(n_cmd, "REJECT") == 0 || strcmp(n_cmd, "IGNORE") == 0 || strcasecmp(n_cmd, "end") == 0) {
            if(s_popup_active && s_popup_is_call) {
                s_popup_active = false;
                s_popup_is_call = false;
                setBuzzerTone(0U);
                setFlightDeckLeds(false);
                renderDisplay();
            }
            Serial.println("GB_CALL_ENDED");
            return true;
        } else if(strcmp(n_cmd, "MISSED") == 0 || strcasecmp(n_cmd, "missed") == 0) {
            if(s_popup_active && s_popup_is_call) {
                s_popup_active = false;
                s_popup_is_call = false;
                setBuzzerTone(0U);
                setFlightDeckLeds(false);
            }
            char missedBody[96];
            snprintf(missedBody, sizeof(missedBody), "Missed call from %s", caller_tel[0] ? caller_tel : (caller_name[0] ? caller_name : "unknown"));
            addPhoneNotification(0, "Phone", caller_name[0] ? caller_name : "Missed Call", missedBody);
            showPopup("MISSED CALL", caller_name[0] ? caller_name : "Missed Call", caller_tel, "Saved to Slide 2", 3000U);
            Serial.printf("GB_CALL_MISSED|name=%s|tel=%s\n", caller_name, caller_tel);
            return true;
        }
    }

    // Find My Watch / Vibrate / Buzz locator
    bool is_find = (extractJsonField(line, "t", t_type, sizeof(t_type)) &&
                   (strcmp(t_type, "find") == 0 || strcmp(t_type, "vibrate") == 0 || strcmp(t_type, "buzz") == 0));
    if(!is_find && (strstr(line, "\"t\":\"find\"") || strstr(line, "\"t\": \"find\"") ||
                    strstr(line, "\"t\":\"vibrate\"") || strstr(line, "\"t\": \"vibrate\"") ||
                    strstr(line, "Bangle.buzz") || strstr(line, "Bangle.beep"))) {
        is_find = true;
    }
    if(is_find) {
        triggerFindWatch();
        return true;
    }

    // Music track info: GB({"t":"musicinfo","artist":"Queen","track":"Bohemian Rhapsody",...})
    if(extractJsonField(line, "t", t_type, sizeof(t_type)) && strcmp(t_type, "musicinfo") == 0) {
        char track[48] = "";
        char artist[48] = "";
        char album[48] = "";
        extractJsonField(line, "track", track, sizeof(track));
        extractJsonField(line, "artist", artist, sizeof(artist));
        extractJsonField(line, "album", album, sizeof(album));
        if(track[0] || artist[0]) {
            wakeScreen("music");
            showNotifPopup(0, "Music", track[0] ? track : "Now Playing", artist[0] ? artist : album, 3500U);
            Serial.printf("GB_MUSICINFO|track=%s|artist=%s\n", track, artist);
            return true;
        }
    }
    if(extractJsonField(line, "t", t_type, sizeof(t_type)) && strcmp(t_type, "musicstate") == 0) {
        char state[24] = "";
        extractJsonField(line, "state", state, sizeof(state));
        Serial.printf("GB_MUSICSTATE|state=%s\n", state);
        return true;
    }

    // Notification dismiss: GB({"t":"notify-","id":...})
    if(extractJsonField(line, "t", t_type, sizeof(t_type)) && strcmp(t_type, "notify-") == 0) {
        char id_str[24] = "";
        extractJsonField(line, "id", id_str, sizeof(id_str));
        uint32_t nid = (uint32_t)strtoul(id_str, NULL, 10);
        removePhoneNotification(nid);
        return true;
    }

    // Notification: GB({"t":"notify","id":...,"src":"Messages","title":"Mom","body":"Call me",...})
    if(strstr(line, "\"notify\"") ||
       (extractJsonField(line, "t", t_type, sizeof(t_type)) && (strncmp(t_type, "notify", 6) == 0))) {
        char title[64] = "";
        char body[128] = "";
        char subject[64] = "";
        char text[128] = "";
        char sender[64] = "";
        char src[32] = "";
        char id_str[24] = "";
        char trend_str[32] = "";
        extractJsonField(line, "id", id_str, sizeof(id_str));
        extractJsonField(line, "src", src, sizeof(src));
        extractJsonField(line, "title", title, sizeof(title));
        extractJsonField(line, "body", body, sizeof(body));
        extractJsonField(line, "subject", subject, sizeof(subject));
        extractJsonField(line, "text", text, sizeof(text));
        extractJsonField(line, "sender", sender, sizeof(sender));
        extractJsonField(line, "trend", trend_str, sizeof(trend_str));

        uint32_t nid = (uint32_t)strtoul(id_str, NULL, 10);
        const char* disp_title = title[0] ? title : (sender[0] ? sender : (subject[0] ? subject : "Alert"));
        const char* disp_body = body[0] ? body : (text[0] ? text : (subject[0] ? subject : ""));
        const char* disp_src = src[0] ? src : "Phone";

        bool is_mmol = false;
        float val = 0.0f;
        int best_prio = 0;

        // Scan candidate fields in order of priority (body is where measurement is, title is app/sensor name)
        const char* candidate_fields[4] = { body, text, title, subject };
        for(size_t fi = 0; fi < 4; fi++) {
            if(!candidate_fields[fi][0]) continue;
            bool cand_mmol = false;
            int cand_prio = 0;
            float cand_val = parseGlucoseValue(candidate_fields[fi], &cand_mmol, &cand_prio);
            if(cand_val > 0.0f && cand_prio > best_prio) {
                best_prio = cand_prio;
                val = cand_val;
                is_mmol = cand_mmol;
            }
        }

        if(val > 0.0f) {
            char combined[448];
            snprintf(combined, sizeof(combined), "%s %s %s %s %s", body, text, title, subject, trend_str);
            TrendDir tdir = parseTrendDirection(combined);
            processJugglucoReading(val, is_mmol, tdir);
            s_juggluco_pkts_rx++;
            Serial.printf("JUGGLUCO_RECV|val=%.1f|is_mmol=%d|prio=%d|title=%s|body=%s\n",
                          val, (int)is_mmol, best_prio, title, body);
        }

        // Save into Slide 2 if not a routine Juggluco background glucose broadcast
        bool is_glucose_stream = (val > 0.0f && (strcasestr(disp_src, "jugg") != NULL || strcasestr(disp_title, "jugg") != NULL || strcasestr(disp_src, "xdrip") != NULL));
        if(!is_glucose_stream) {
            addPhoneNotification(nid, disp_src, disp_title, disp_body);
        }
        return true;
    }

    // Fallback: Check if line itself contains a glucose reading
    bool is_mmol = false;
    float val = parseGlucoseValue(line, &is_mmol);
    if(val > 0.0f) {
        TrendDir tdir = parseTrendDirection(line);
        processJugglucoReading(val, is_mmol, tdir);
        s_juggluco_pkts_rx++;
        Serial.printf("JUGGLUCO_FALLBACK|line=%s|val=%.1f\n", line, val);
        return true;
    }

    return false;
}

static bool s_restart_adv_needed = false;
static unsigned long s_juggluco_conn_time_ms = 0UL;

class JugglucoServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) override {
        (void)pServer;
        s_juggluco_connected = true;
        s_juggluco_conn_time_ms = millis();
        s_ble_conn_event = true;
    }
    void onDisconnect(BLEServer* pServer, esp_ble_gatts_cb_param_t* param) override {
        (void)pServer;
        s_juggluco_connected = false;
        s_restart_adv_needed = true;
        s_ble_last_disc_reason = param ? (uint8_t)param->disconnect.reason : 0;
        s_ble_disc_event = true;
    }
    void onDisconnect(BLEServer* pServer) override {
        onDisconnect(pServer, nullptr);
    }
};

class JugglucoRxCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pCharacteristic) override {
        std::string rx = pCharacteristic->getValue();
        if(rx.empty()) return;
        portENTER_CRITICAL(&s_nus_rx_mux);
        s_nus_raw_buf += rx;
        size_t pos;
        while((pos = s_nus_raw_buf.find('\n')) != std::string::npos) {
            std::string line = s_nus_raw_buf.substr(0, pos);
            s_nus_raw_buf.erase(0, pos + 1);
            while(!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ';')) line.pop_back();
            if(!line.empty() && s_nus_pending_lines.size() < 16) {
                s_nus_pending_lines.push_back(line);
            }
        }
        size_t gb_pos = s_nus_raw_buf.find("GB(");
        if(gb_pos != std::string::npos) {
            size_t gb_end = s_nus_raw_buf.find(')', gb_pos);
            if(gb_end != std::string::npos) {
                std::string cmd = s_nus_raw_buf.substr(gb_pos, gb_end - gb_pos + 1);
                s_nus_raw_buf.erase(0, gb_end + 1);
                if(s_nus_pending_lines.size() < 16) {
                    s_nus_pending_lines.push_back(cmd);
                }
            }
        }
        size_t bang_pos = s_nus_raw_buf.find("Bangle.");
        if(bang_pos != std::string::npos) {
            size_t bang_end = s_nus_raw_buf.find(')', bang_pos);
            if(bang_end != std::string::npos) {
                std::string cmd = s_nus_raw_buf.substr(bang_pos, bang_end - bang_pos + 1);
                s_nus_raw_buf.erase(0, bang_end + 1);
                if(s_nus_pending_lines.size() < 16) {
                    s_nus_pending_lines.push_back(cmd);
                }
            }
        } else if(s_nus_raw_buf.length() > 512) {
            s_nus_raw_buf.clear();
        }
        portEXIT_CRITICAL(&s_nus_rx_mux);
    }
};

static void pollNusIncomingLines() {
    std::string line_to_process = "";
    portENTER_CRITICAL(&s_nus_rx_mux);
    if(!s_nus_pending_lines.empty()) {
        line_to_process = s_nus_pending_lines.front();
        s_nus_pending_lines.erase(s_nus_pending_lines.begin());
    }
    portEXIT_CRITICAL(&s_nus_rx_mux);

    if(!line_to_process.empty()) {
        handleIncomingLine(line_to_process.c_str(), "BLE");
    }
}

static void initBleStackIfNeeded() {
    if(s_ble_stack_inited) return;
    BLEDevice::init("Bangle.js Artemis");
    BLEDevice::setMTU(185);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, ESP_PWR_LVL_P9);
    BLEDevice::setCustomGattcHandler(myGattcEventHandler);

    // Setup GATT Client scanner for Direct Sensor mode
    BLEScan* pScan = BLEDevice::getScan();
    pScan->setAdvertisedDeviceCallbacks(new Libre2AdvertisedCallbacks(), true);
    pScan->setActiveScan(true);
    pScan->setInterval(100);
    pScan->setWindow(100);

    // Setup GATT Server with Nordic UART Service for Juggluco / Phone mode
    s_pServer = BLEDevice::createServer();
    s_pServer->setCallbacks(new JugglucoServerCallbacks());
    BLEService* pService = s_pServer->createService(NUS_SERVICE_UUID);
    s_pTxCharacteristic = pService->createCharacteristic(
        NUS_CHARACTERISTIC_TX_UUID,
        BLECharacteristic::PROPERTY_NOTIFY
    );
    s_pTxCharacteristic->addDescriptor(new BLE2902());
    s_pRxCharacteristic = pService->createCharacteristic(
        NUS_CHARACTERISTIC_RX_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
    );
    s_pRxCharacteristic->setCallbacks(new JugglucoRxCallbacks());
    pService->start();

    BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(NUS_SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x0C);
    pAdvertising->setMaxPreferred(0x18);

    s_ble_stack_inited = true;
}

static void applyBtModeChange() {
    initBleStackIfNeeded();
    if(s_cfg_bt_mode == BLE_MODE_DIRECT) {
        if(BLEDevice::getAdvertising()) {
            BLEDevice::getAdvertising()->stop();
        }
        s_ble_enabled = true;
        s_ble_last_scan_ms = millis();
        snprintf(s_ble_status, sizeof(s_ble_status), s_ble_has_mac ? "MAC saved, listening" : "Listening 0xFDE3...");
        showPopup("BT Mode", "Direct Sensor BLE", "Listening for Libre 2", "60s direct client", 2400U);
    } else if(s_cfg_bt_mode == BLE_MODE_JUGGLUCO) {
        s_ble_enabled = false;
        s_ble_do_connect = false;
        if(s_ble_client && s_ble_connected) {
            s_ble_client->disconnect();
        }
        s_ble_connected = false;
        BLEScan* pScan = BLEDevice::getScan();
        if(pScan) pScan->stop();

        if(BLEDevice::getAdvertising()) {
            BLEDevice::startAdvertising();
        }
        snprintf(s_ble_status, sizeof(s_ble_status), s_juggluco_connected ? "Phone Connected" : "Adv: Bangle.js Artemis");
        showPopup("BT Mode", "Juggluco / Phone", "Nordic UART Active", "Adv: Bangle.js Artemis", 2400U);
    } else {
        s_ble_enabled = false;
        s_ble_do_connect = false;
        if(s_ble_client && s_ble_connected) {
            s_ble_client->disconnect();
        }
        s_ble_connected = false;
        BLEScan* pScan = BLEDevice::getScan();
        if(pScan) pScan->stop();
        if(BLEDevice::getAdvertising()) {
            BLEDevice::getAdvertising()->stop();
        }
        snprintf(s_ble_status, sizeof(s_ble_status), "Bluetooth OFF");
        showPopup("BT Mode", "Bluetooth OFF", "Radios disabled", "Maximum power save", 2400U);
    }
    updateRgbLedStatus();
    saveSettingsToSpiffs();
}

static void toggleBlePairing() {
    if(s_cfg_bt_mode == BLE_MODE_JUGGLUCO) {
        showPopup("Juggluco Active", "Phone BLE receiver", "Adv: Bangle.js Artemis", "Send from Juggluco", 2500U);
        return;
    }
    if(s_cfg_bt_mode == BLE_MODE_OFF) {
        showPopup("Bluetooth is OFF", "Enable in Settings", "Row: BT MODE", "Page 7/7", 2500U);
        return;
    }
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

static void renderHomePage128() {
    static const char* const kWdays[7] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
    static const char* const kMonths[12] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };

    // 6 Vibrant Cyberpunk Home Palettes:
    // 0: CP2077   (Cyberpunk 2077 Game HUD: Pure Cyber Yellow + Crimson shadow + Kiroshi Cyan accents)
    // 1: CYAN     (Electric Vivid Cyan + Neon Magenta accents)
    // 2: MAGENTA  (Neon Hot Magenta + Electric Cyan accents)
    // 3: MATRIX   (Phosphor Matrix Green + Emerald accents)
    // 4: AMBER    (Cyber Gold Amber + Cyan accents)
    // 5: CRIMSON  (Vivid Neon Red + Amber accents)
    static const uint16_t kPalPrimary[6] = { 0xFFE0, 0x07FF, 0xF81F, 0x07E0, 0xFDE0, 0xF800 };
    static const uint16_t kPalShadow[6]  = { 0x9000, 0x0115, 0x4008, 0x0280, 0x61A0, 0x4800 };
    static const uint16_t kPalAccent1[6] = { 0x07FF, 0x05DF, 0x07FF, 0x07F4, 0xFFE0, 0xFDE0 };
    static const uint16_t kPalAccent2[6] = { 0xF81F, 0xF81F, 0xFFE0, 0xFFE0, 0x07FF, 0xFFFF };

    const uint8_t th        = s_cfg_theme_idx % 6U;
    const uint16_t cBg      = 0x0000;
    const uint16_t cPrimary = kPalPrimary[th];
    const uint16_t cTime    = cPrimary;
    const uint16_t cShadow  = kPalShadow[th];
    const uint16_t cCyan    = kPalAccent1[th];
    const uint16_t cMagenta = kPalAccent2[th];
    const uint16_t cAmber   = 0xFFE0;
    const uint16_t cGreen   = 0x07E0;
    const uint16_t cRed     = 0xF800;
    const uint16_t cDim     = 0x4208;

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

    // Top row: Date + battery gauge + BLE mode dot
    s_canvas.setTextSize(1);
    s_canvas.setTextColor(th == 0U ? cTime : cCyan);
    s_canvas.setCursor(7, 5);
    if(haveRtc) {
        s_canvas.printf("%s %02u %s", kWdays[dt.wday % 7U], dt.day, kMonths[(dt.month - 1U) % 12U]);
    } else {
        s_canvas.print("NC // 2077");
    }

    // Battery readout (x = 76..114)
    char battStr[16];
    if(s_batt_charging) {
        snprintf(battStr, sizeof(battStr), "+%u%%", (unsigned)s_batt_pct);
    } else {
        snprintf(battStr, sizeof(battStr), "%u%%", (unsigned)s_batt_pct);
    }
    uint16_t bCol = s_batt_charging ? cGreen : ((s_batt_pct <= 15U) ? cRed : ((s_batt_pct <= 35U) ? cAmber : (th == 0U ? cMagenta : cCyan)));
    s_canvas.setTextColor(bCol);
    int bStrW = (int)strlen(battStr) * 6;
    s_canvas.setCursor(114 - bStrW, 5);
    s_canvas.print(battStr);

    // Status indicators: DND (crescent moon icon) & Power Saver (amber [E] pill)
    // Positioned safely in the margin between date (x <= 66) and battery (x >= 84)
    int battLeft = 114 - bStrW;
    int maxStatusX = battLeft - 4;
    if(s_cfg_night_dnd && s_power_saver_active) {
        int mx = maxStatusX - 16, my = 8;
        s_canvas.fillCircle(mx, my, 4, cCyan);
        s_canvas.fillCircle(mx + 2, my - 1, 3, cBg);

        s_canvas.drawRoundRect(maxStatusX - 9, 3, 9, 11, 2, cAmber);
        s_canvas.setTextColor(cAmber);
        s_canvas.setCursor(maxStatusX - 7, 5);
        s_canvas.print("E");
    } else if(s_cfg_night_dnd) {
        int mx = 70 + (maxStatusX - 70) / 2, my = 8;
        if(mx < 72) mx = 72;
        s_canvas.fillCircle(mx, my, 4, cCyan);
        s_canvas.fillCircle(mx + 2, my - 1, 3, cBg);
    } else if(s_power_saver_active) {
        int px = 68 + (maxStatusX - 68 - 14) / 2;
        if(px < 68) px = 68;
        s_canvas.drawRoundRect(px, 3, 14, 11, 2, cAmber);
        s_canvas.setTextColor(cAmber);
        s_canvas.setCursor(px + 4, 5);
        s_canvas.print("E");
    }

    uint16_t bleDot = cDim;
    if(s_cfg_bt_mode == BLE_MODE_DIRECT) {
        bleDot = s_ble_connected ? cGreen : (s_ble_enabled ? cAmber : cDim);
    } else if(s_cfg_bt_mode == BLE_MODE_JUGGLUCO) {
        bleDot = s_juggluco_connected ? cCyan : cAmber;
    } else {
        bleDot = cDim;
    }
    s_canvas.fillCircle(121, 9, 2, bleDot);

    // Big Cyberpunk Time (HH:MM in TextSize 4 = 120px wide)
    uint8_t hh = haveRtc ? dt.hour : (uint8_t)(((millis() / 1000U) / 3600U) % 24U);
    uint8_t mm = haveRtc ? dt.min  : (uint8_t)(((millis() / 1000U) / 60U) % 60U);
    s_canvas.setTextSize(4);
    if(s_glitch_frame_active) {
        // Chromatic aberration RGB split during glitch frame
        s_canvas.setTextColor(0xF800);
        s_canvas.setCursor(3, 21);
        s_canvas.printf("%02u:%02u", hh, mm);
        s_canvas.setTextColor(0x07FF);
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

        // Reading age & link status indicator
        if(s_cfg_bt_mode == BLE_MODE_JUGGLUCO && !s_juggluco_connected) {
            s_canvas.setTextColor(cAmber);
            s_canvas.setCursor(48, 108);
            s_canvas.print("NO BT");
        } else if(lastScanMillis > 0UL) {
            uint32_t minAgo = (uint32_t)((millis() - lastScanMillis) / 60000UL);
            if(minAgo < 1U) {
                s_canvas.setTextColor(cGreen);
                s_canvas.setCursor(52, 108);
                s_canvas.print("NOW");
            } else if(minAgo <= 15U) {
                s_canvas.setTextColor(cDim);
                s_canvas.setCursor(50, 108);
                s_canvas.printf("%um", (unsigned)minAgo);
            } else if(minAgo < 100U) {
                s_canvas.setTextColor(cAmber);
                s_canvas.setCursor(46, 108);
                s_canvas.printf("%um!", (unsigned)minAgo);
            } else {
                s_canvas.setTextColor(cAmber);
                s_canvas.setCursor(48, 108);
                s_canvas.print(">1h");
            }
        }

        int d15 = 0;
        TrendDir tdir = (s_last_juggluco_trend != TREND_UNKNOWN) ? s_last_juggluco_trend : computeTrendDir(&d15);
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
        uint16_t glitchCol = (b & 1) ? 0x07FF : 0xF800;
        s_canvas.drawFastHLine((shift > 0) ? 4 : 78, y0, 36, glitchCol);
    }
}

static void renderSettingsPage128() {
    static const uint16_t kPalPrimary[6] = { 0xFFE0, 0x07FF, 0xF81F, 0x07E0, 0xFDE0, 0xF800 };
    static const uint16_t kPalAccent2[6] = { 0xF81F, 0xF81F, 0xFFE0, 0xFFE0, 0x07FF, 0xFFFF };
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

    char bHour[8], bMin[8], bDate[12];
    WatchDateTime rtcDt;
    if(readPcf8563Rtc(&rtcDt)) {
        snprintf(bHour, sizeof(bHour), "%02u", rtcDt.hour);
        snprintf(bMin, sizeof(bMin), "%02u", rtcDt.min);
        snprintf(bDate, sizeof(bDate), "%02u/%02u", rtcDt.day, rtcDt.month);
    } else {
        snprintf(bHour, sizeof(bHour), "--");
        snprintf(bMin, sizeof(bMin), "--");
        snprintf(bDate, sizeof(bDate), "--/--");
    }

    char bSaver[8], bDnd[8], bChm[8];
    snprintf(bSaver, sizeof(bSaver), (s_cfg_power_saver == 0U) ? "OFF" : (s_cfg_power_saver == 1U ? "AUTO" : "ON"));
    snprintf(bDnd, sizeof(bDnd), s_cfg_night_dnd ? "ON" : "OFF");
    snprintf(bChm, sizeof(bChm), s_cfg_hourly_chime ? "ON" : "OFF");

    const char* labels[23] = {
        "BT MODE",
        "RGB LED",
        "RAISE WAKE",
        "DECK PULSE",
        "NOTIF LIGHT",
        "NOTIF SOUND",
        "WARN LOW",
        "WARN HIGH",
        "ALARM LOW",
        "ALARM HIGH",
        "ALM SOUND",
        "TIMEOUT",
        "BRIGHTNESS",
        "HOME COLOR",
        "GLITCH FX",
        "PWR SAVER",
        "NIGHT DND",
        "HOURLY CHM",
        "FLASHLIGHT",
        "SOS BEACON",
        "SET HOUR",
        "SET MIN",
        "SET DATE"
    };
    const char* values[23] = {
        kBtModeLabels[s_cfg_bt_mode % 3U],
        s_cfg_rgb_led ? "ON" : "OFF",
        s_cfg_raise_wake ? "ON" : "OFF",
        kDeckPulseLabels[s_cfg_deck_pulse_idx % 6U],
        s_cfg_notif_light ? "ON" : "OFF",
        s_cfg_notif_sound ? "ON" : "OFF",
        bWarnLow,
        bWarnHigh,
        bUrgLow,
        bUrgHigh,
        s_cfg_alarm_sound ? "ON" : "OFF",
        kTimeoutLabels[s_cfg_timeout_idx % 7U],
        kBrightLabels[s_cfg_bright_idx % 5U],
        kThemeLabels[s_cfg_theme_idx % 6U],
        s_cfg_glitch_fx ? "ON" : "OFF",
        bSaver,
        bDnd,
        bChm,
        "TOGGLE",
        "TEST",
        bHour,
        bMin,
        bDate
    };

    uint8_t winStart = (s_settings_row >= 4U) ? (uint8_t)(s_settings_row - 3U) : 0U;
    if(winStart > 19U) winStart = 19U;
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

        uint16_t vCol = (r == 0U) ? cAccent :
                        ((r == 13U) ? cPrimary :
                        ((r >= 6U && r <= 7U) ? COL_AMBER :
                        ((r >= 8U && r <= 9U) ? COL_RED :
                        ((r == 15U) ? (s_power_saver_active ? COL_AMBER : COL_WHITE) :
                        ((r == 18U) ? COL_YELLOW :
                        ((r == 19U) ? COL_VIVID_RED :
                        ((r >= 20U && r <= 22U) ? (sel ? COL_CYAN : COL_WHITE) :
                        (sel ? COL_AMBER : COL_WHITE))))))));
        int vw = (int)strlen(values[r]) * 6;
        s_canvas.setTextColor(vCol);
        s_canvas.setCursor(117 - vw, ry + 7);
        s_canvas.print(values[r]);
    }
    // Cyberpunk vertical scrollbar track (x = 123, y = 22..118)
    s_canvas.drawFastVLine(123, 22, 96, COL_GRID);
    int thumbY = 22 + ((int)s_settings_row * 82) / 22;
    s_canvas.fillRect(122, thumbY, 3, 14, cAccent);
}

static void drawTirStatsCard() {
    s_canvas.fillRoundRect(3, 18, 122, 106, 5, COL_DARK_NAVY);
    s_canvas.drawRoundRect(3, 18, 122, 106, 5, COL_VIVID_CYAN);

    // Header badge
    s_canvas.fillRoundRect(6, 21, 56, 11, 2, COL_VIVID_CYAN);
    s_canvas.setTextSize(1);
    s_canvas.setTextColor(COL_BG);
    s_canvas.setCursor(9, 23);
    s_canvas.print("24H TIR");

    s_canvas.setTextColor(COL_YELLOW);
    s_canvas.setCursor(68, 23);
    s_canvas.print("DASHBOARD");

    s_canvas.drawLine(4, 34, 124, 34, COL_GRID);

    const char* activeSerial = (compState.has_scan && compState.serial_valid) ? compState.serial : NULL;
    static const LlHistoryPoint* pts[LL_HISTORY_MAX_POINTS];
    uint16_t qMinAge = 0, qMaxAge = 0, qMinMmol = 0, qMaxMmol = 0;
    size_t n = ll_history_query_window(&s_history, activeSerial, 1440U, pts, LL_HISTORY_MAX_POINTS, &qMinAge, &qMaxAge, &qMinMmol, &qMaxMmol);

    if(n == 0U) {
        s_canvas.setTextColor(COL_MUTED);
        s_canvas.setCursor(12, 54);
        s_canvas.print("No 24h glucose");
        s_canvas.setCursor(12, 68);
        s_canvas.print("points recorded.");
        s_canvas.setTextColor(COL_CYAN);
        s_canvas.setCursor(8, 110);
        s_canvas.print("SEL:zoom  v:next");
        return;
    }

    uint32_t in_range = 0, low = 0, high = 0;
    uint32_t sum_tenths = 0;
    for(size_t i = 0; i < n; i++) {
        uint16_t v = pts[i]->mmol_tenths;
        sum_tenths += v;
        if(v < 39U) low++;
        else if(v > 100U) high++;
        else in_range++;
    }

    uint32_t pct_in   = (in_range * 100U) / (uint32_t)n;
    uint32_t pct_high = (high * 100U) / (uint32_t)n;
    uint32_t pct_low  = (low * 100U) / (uint32_t)n;
    uint32_t avg_tenths = sum_tenths / (uint32_t)n;
    uint32_t avg_mgdl   = (avg_tenths * 180U) / 100U;

    // Row 1: IN RANGE (3.9 - 10.0)
    s_canvas.setTextColor(COL_VIVID_GREEN);
    s_canvas.setCursor(7, 39);
    s_canvas.printf("IN RANGE: %u%%", (unsigned)pct_in);
    s_canvas.drawRoundRect(7, 49, 114, 7, 2, COL_GRID);
    int barInW = (int)((pct_in * 112U) / 100U);
    if(barInW > 0) s_canvas.fillRoundRect(8, 50, barInW, 5, 2, COL_VIVID_GREEN);

    // Row 2: ABOVE 10.0
    s_canvas.setTextColor(COL_AMBER);
    s_canvas.setCursor(7, 59);
    s_canvas.printf("HIGH >10: %u%%", (unsigned)pct_high);
    s_canvas.drawRoundRect(7, 69, 114, 7, 2, COL_GRID);
    int barHighW = (int)((pct_high * 112U) / 100U);
    if(barHighW > 0) s_canvas.fillRoundRect(8, 70, barHighW, 5, 2, COL_AMBER);

    // Row 3: BELOW 3.9
    s_canvas.setTextColor(COL_VIVID_RED);
    s_canvas.setCursor(7, 79);
    s_canvas.printf("LOW <3.9: %u%%", (unsigned)pct_low);
    s_canvas.drawRoundRect(7, 89, 114, 7, 2, COL_GRID);
    int barLowW = (int)((pct_low * 112U) / 100U);
    if(barLowW > 0) s_canvas.fillRoundRect(8, 90, barLowW, 5, 2, COL_VIVID_RED);

    // Average glucose & count footer
    s_canvas.drawLine(4, 99, 124, 99, COL_GRID);
    s_canvas.setTextColor(COL_BRIGHT_WHITE);
    s_canvas.setCursor(7, 102);
    s_canvas.printf("AVG: %u.%u (%u mg)", (unsigned)(avg_tenths / 10U), (unsigned)(avg_tenths % 10U), (unsigned)avg_mgdl);

    s_canvas.setTextColor(COL_MUTED);
    s_canvas.setCursor(7, 113);
    s_canvas.printf("%u pts | SEL:zoom", (unsigned)n);
}

static void renderGraphPage128() {
    if(s_zoom_idx == 6U) {
        drawTirStatsCard();
        return;
    }
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

static void renderNotifsPage128() {
    if(s_notif_count == 0U) {
        s_canvas.fillRoundRect(3, 18, 122, 106, 5, COL_DARK_NAVY);
        s_canvas.drawRoundRect(3, 18, 122, 106, 5, COL_GRID);

        s_canvas.setTextSize(1);
        s_canvas.setTextColor(COL_VIVID_CYAN);
        s_canvas.setCursor(12, 25);
        s_canvas.print("// NOTIFICATIONS //");

        // Glowing checkmark badge in center
        s_canvas.fillCircle(64, 52, 12, 0x03E0);
        s_canvas.drawCircle(64, 52, 12, COL_VIVID_GREEN);
        s_canvas.drawLine(58, 52, 62, 57, COL_BRIGHT_WHITE);
        s_canvas.drawLine(62, 57, 70, 47, COL_BRIGHT_WHITE);

        s_canvas.setTextColor(COL_VIVID_GREEN);
        s_canvas.setCursor(22, 70);
        s_canvas.print("ALL CAUGHT UP");

        s_canvas.setTextColor(COL_BRIGHT_WHITE);
        s_canvas.setCursor(16, 83);
        s_canvas.print("No unread alerts");

        s_canvas.setTextColor(COL_MUTED);
        s_canvas.setCursor(13, 95);
        s_canvas.print("Synced from phone");

        s_canvas.drawFastHLine(6, 107, 116, COL_GRID);
        s_canvas.setTextColor(COL_VIVID_CYAN);
        s_canvas.setCursor(12, 111);
        s_canvas.print("^:Home      v:Graph");
        return;
    }

    if(s_notif_selected >= s_notif_count) s_notif_selected = 0U;
    const PhoneNotification& notif = s_phone_notifs[s_notif_selected];
    uint16_t accentCol = getAppAccentColor(notif.src);

    s_canvas.fillRoundRect(3, 18, 122, 106, 5, COL_DARK_NAVY);
    s_canvas.drawRoundRect(3, 18, 122, 106, 5, accentCol);

    // Header strip
    s_canvas.fillRoundRect(6, 21, 52, 12, 2, accentCol);
    s_canvas.setTextSize(1);
    s_canvas.setTextColor(0x0000);
    s_canvas.setCursor(8, 23);
    char truncSrc[9];
    snprintf(truncSrc, sizeof(truncSrc), "%.8s", notif.src);
    s_canvas.print(truncSrc);

    // Timestamp
    s_canvas.setTextColor(COL_BRIGHT_WHITE);
    s_canvas.setCursor(62, 23);
    s_canvas.printf("%02u:%02u", (unsigned)notif.hour, (unsigned)notif.min);

    // Counter pill [1/3]
    s_canvas.fillRoundRect(94, 21, 28, 12, 2, 0x0208);
    s_canvas.drawRoundRect(94, 21, 28, 12, 2, COL_VIVID_CYAN);
    s_canvas.setTextColor(COL_VIVID_CYAN);
    s_canvas.setCursor(97, 23);
    s_canvas.printf("%u/%u", (unsigned)(s_notif_selected + 1U), (unsigned)s_notif_count);

    // Divider line
    s_canvas.drawLine(4, 34, 124, 34, accentCol);

    // Title / Sender (bold yellow)
    s_canvas.setTextColor(COL_YELLOW);
    s_canvas.setCursor(6, 37);
    char truncTitle[20];
    snprintf(truncTitle, sizeof(truncTitle), "> %.17s", notif.title);
    s_canvas.print(truncTitle);

    // Word-wrapped body text (up to 5 lines of 19 chars)
    char lines[5][24];
    uint8_t nLines = wordWrapText(notif.body, lines, 5, 19);
    s_canvas.setTextColor(COL_BRIGHT_WHITE);
    int y = 49;
    for(int li = 0; li < nLines; li++) {
        s_canvas.setCursor(6, y);
        s_canvas.print(lines[li]);
        y += 10;
    }

    // Bottom action bar
    s_canvas.drawLine(4, 104, 124, 104, COL_GRID);

    // Left pill: SEL:DISMISS
    s_canvas.fillRoundRect(6, 107, 58, 14, 2, 0x6000);
    s_canvas.drawRoundRect(6, 107, 58, 14, 2, COL_VIVID_RED);
    s_canvas.setTextColor(COL_BRIGHT_WHITE);
    s_canvas.setCursor(9, 110);
    s_canvas.print("SEL:DISMISS");

    // Right action hint
    s_canvas.setTextColor(COL_VIVID_CYAN);
    s_canvas.setCursor(70, 110);
    if(s_notif_count > 1U) {
        s_canvas.print("UP/DN:NAV");
    } else {
        s_canvas.print("ALT:HOME");
    }
}

static void renderDisplay() {
    if(s_flashlight_active) {
        s_canvas.fillScreen(COL_BRIGHT_WHITE);
        s_canvas.fillRoundRect(14, 98, 100, 16, 4, COL_DARK_NAVY);
        s_canvas.drawRoundRect(14, 98, 100, 16, 4, COL_YELLOW);
        s_canvas.setTextSize(1);
        s_canvas.setTextColor(COL_YELLOW);
        s_canvas.setCursor(22, 102);
        s_canvas.print("ANY BTN: EXIT");
        s_canvas.pushSprite(0, 0);
        return;
    }

    if(s_sos_active) {
        drawSosOverlay();
        s_canvas.pushSprite(0, 0);
        return;
    }

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
    s_canvas.setCursor(2, 4);
    s_canvas.print("LibreLibre");

    s_canvas.setTextColor(s_batt_charging ? COL_GREEN : (s_batt_pct <= 15U ? COL_RED : COL_MUTED));
    s_canvas.setCursor(68, 4);
    s_canvas.printf("%s%u%%", s_batt_charging ? "+" : "", (unsigned)s_batt_pct);

    s_canvas.setTextColor(COL_WHITE);
    s_canvas.setCursor(102, 4);
    s_canvas.printf("P%d", (int)currentPage + 1);

    if(currentPage == PAGE_NOTIFS) {
        renderNotifsPage128();
        drawPopupOverlay();
        s_canvas.pushSprite(0, 0);
        return;
    }

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
        if(s_cfg_bt_mode == BLE_MODE_JUGGLUCO) {
            s_canvas.setTextColor(COL_CYAN);
            s_canvas.setCursor(8, 23);
            s_canvas.print("Juggluco / Phone");

            s_canvas.setTextColor(s_juggluco_connected ? COL_GREEN : COL_AMBER);
            s_canvas.setCursor(8, 37);
            s_canvas.print(s_juggluco_connected ? "Status: Connected" : "Status: Advertising");

            s_canvas.setTextColor(COL_WHITE);
            s_canvas.setCursor(8, 51);
            s_canvas.print("Adv: Bangle.js Artemis");

            s_canvas.setCursor(8, 65);
            s_canvas.print("NUS: 6E400001 GATT");

            s_canvas.setCursor(8, 79);
            s_canvas.printf("GB pkts rx: %lu", (unsigned long)s_juggluco_pkts_rx);

            s_canvas.setTextColor(COL_AMBER);
            s_canvas.setCursor(8, 95);
            s_canvas.print("Mode: Companion");
        } else if(s_cfg_bt_mode == BLE_MODE_OFF) {
            s_canvas.setTextColor(COL_MUTED);
            s_canvas.setCursor(8, 23);
            s_canvas.print("Bluetooth Radio");

            s_canvas.setTextColor(COL_RED);
            s_canvas.setCursor(8, 37);
            s_canvas.print("Status: Disabled (OFF)");

            s_canvas.setTextColor(COL_WHITE);
            s_canvas.setCursor(8, 55);
            s_canvas.print("Enable in Settings");
            s_canvas.setCursor(8, 69);
            s_canvas.print("Row: BT MODE");

            s_canvas.setTextColor(COL_AMBER);
            s_canvas.setCursor(8, 95);
            s_canvas.print("SEL: Quick toggle");
        } else {
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
        }

        s_canvas.setTextColor(COL_MUTED);
        s_canvas.setCursor(4, 116);
        s_canvas.print("SEL:toggle  v:main");
        drawPopupOverlay();
        s_canvas.pushSprite(0, 0);
        return;
    }

    if(currentPage == PAGE_DIAG) {
        s_canvas.fillRoundRect(3, 18, 122, 92, 5, COL_CARD_BG);
        s_canvas.setTextSize(1);
        s_canvas.setTextColor(COL_AMBER);
        s_canvas.setCursor(8, 23);
        s_canvas.print("Hardware & Sensor");

        s_canvas.setTextColor(s_batt_charging ? COL_GREEN : (s_batt_pct <= 15U ? COL_RED : COL_CYAN));
        s_canvas.setCursor(8, 36);
        s_canvas.printf("Bat: %.2fV %u%%%s", s_batt_mv / 1000.0f, (unsigned)s_batt_pct,
                        s_batt_charging ? " [CHG]" : (s_usb_plugged ? " [USB]" : ""));

        s_canvas.setTextColor(COL_WHITE);
        s_canvas.setCursor(8, 49);
        const char* btName = (s_cfg_bt_mode == BLE_MODE_DIRECT) ? "Direct" :
                             (s_cfg_bt_mode == BLE_MODE_JUGGLUCO) ? "Juggluco" : "OFF";
        s_canvas.printf("BT:%s LED:%s", btName, s_cfg_rgb_led ? "ON" : "OFF");

        if(compState.has_scan) {
            s_canvas.setTextColor(COL_WHITE);
            s_canvas.setCursor(8, 62);
            s_canvas.print(compState.diag_line1);
            s_canvas.setCursor(8, 74);
            s_canvas.print(compState.diag_line2);
            s_canvas.setTextColor(COL_MUTED);
            s_canvas.setCursor(8, 86);
            s_canvas.printf("Pts:%u %s", (unsigned)s_history.count, saveStatus);
        } else {
            s_canvas.setTextColor(COL_MUTED);
            s_canvas.setCursor(8, 62);
            s_canvas.printf("HW Rev %u (ST7735S)", (unsigned)s_pins.rev);
            s_canvas.setCursor(8, 74);
            s_canvas.print("No sensor scanned");
            s_canvas.setCursor(8, 86);
            s_canvas.print(saveStatus);
        }

        s_canvas.setTextColor(COL_MUTED);
        s_canvas.setCursor(4, 116);
        s_canvas.print("UP/DN:page  BACK:main");
        drawPopupOverlay();
        s_canvas.pushSprite(0, 0);
        return;
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
    if(strncmp(line, "GB(", 3) == 0 || strncmp(line, "setTime(", 8) == 0 || line[0] == '{' || strncmp(line, "Bangle.", 7) == 0 || (uint8_t)line[0] == 0x10) {
        parseGadgetbridgeLine(line);
        return;
    }
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
            if(s_cfg_bt_mode == BLE_MODE_DIRECT) {
                initBleStackIfNeeded();
                s_ble_enabled = true;
            }
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
    if(strncmp(line, "BT_MODE|", 8) == 0) {
        s_cfg_bt_mode = (uint8_t)(atoi(line + 8) % 3);
        applyBtModeChange();
        saveSettingsToSpiffs();
        renderDisplay();
        Serial.printf("OK|BT_MODE=%u\n", (unsigned)s_cfg_bt_mode);
        return;
    }
    if(strcmp(line, "PING") == 0 || strcmp(line, "ping") == 0) {
        Serial.println("PONG|LibreLibre-ESP32-CircuitMess-Artemis");
        return;
    }
    if(strncmp(line, "GB(", 3) == 0) {
        parseGadgetbridgeLine(line);
        return;
    }
    if(strcmp(line, "SLEEP_NOW") == 0) {
        s_screen_asleep = true;
        setBacklightPercent(0U);
        updateRgbLedStatus();
        setCpuFrequencyMhz(80);
        Serial.println("OK|SCREEN_SLEEP");
        return;
    }
    if(strcmp(line, "WAKE_NOW") == 0) {
        wakeScreen("serial_cmd");
        Serial.println("OK|SCREEN_WAKE");
        return;
    }
    if(strncmp(line, "SET_BL|", 7) == 0) {
        uint8_t pct = (uint8_t)atoi(line + 7);
        if(pct > 100U) pct = 100U;
        setBacklightPercent(pct);
        for(uint8_t bi = 0; bi < 5U; bi++) {
            if(kBrightPctTable[bi] == pct) s_cfg_bright_idx = bi;
        }
        saveSettingsToSpiffs();
        Serial.printf("OK|SET_BL=%u|idx=%u\n", (unsigned)pct, (unsigned)s_cfg_bright_idx);
        return;
    }
    if(strncmp(line, "BRIGHT_IDX|", 11) == 0) {
        s_cfg_bright_idx = (uint8_t)(atoi(line + 11) % 5);
        setBacklightPercent(kBrightPctTable[s_cfg_bright_idx]);
        saveSettingsToSpiffs();
        Serial.printf("OK|BRIGHT_IDX=%u|pct=%u\n", (unsigned)s_cfg_bright_idx, (unsigned)kBrightPctTable[s_cfg_bright_idx]);
        return;
    }
    if(strncmp(line, "RAISE_WAKE|", 11) == 0) {
        s_cfg_raise_wake = (atoi(line + 11) != 0);
        saveSettingsToSpiffs();
        renderDisplay();
        Serial.printf("OK|RAISE_WAKE=%u\n", (unsigned)(s_cfg_raise_wake ? 1U : 0U));
        return;
    }
    if(strncmp(line, "RGB_LED|", 8) == 0) {
        s_cfg_rgb_led = (atoi(line + 8) != 0);
        updateRgbLedStatus();
        saveSettingsToSpiffs();
        renderDisplay();
        Serial.printf("OK|RGB_LED=%u\n", (unsigned)(s_cfg_rgb_led ? 1U : 0U));
        return;
    }
    if(strncmp(line, "DECK_PULSE|", 11) == 0) {
        s_cfg_deck_pulse_idx = (uint8_t)(atoi(line + 11) % 6);
        if(s_cfg_deck_pulse_idx > 0U) startDeckAestheticPulse();
        saveSettingsToSpiffs();
        renderDisplay();
        Serial.printf("OK|DECK_PULSE=%u\n", (unsigned)s_cfg_deck_pulse_idx);
        return;
    }
    if(strncmp(line, "NOTIF_LIGHT|", 12) == 0) {
        s_cfg_notif_light = (atoi(line + 12) != 0);
        saveSettingsToSpiffs();
        renderDisplay();
        Serial.printf("OK|NOTIF_LIGHT=%u\n", (unsigned)(s_cfg_notif_light ? 1U : 0U));
        return;
    }
    if(strncmp(line, "NOTIF_SOUND|", 12) == 0) {
        s_cfg_notif_sound = (atoi(line + 12) != 0);
        saveSettingsToSpiffs();
        renderDisplay();
        Serial.printf("OK|NOTIF_SOUND=%u\n", (unsigned)(s_cfg_notif_sound ? 1U : 0U));
        return;
    }
    if(strncmp(line, "ALM_SOUND|", 10) == 0) {
        s_cfg_alarm_sound = (atoi(line + 10) != 0);
        saveSettingsToSpiffs();
        renderDisplay();
        Serial.printf("OK|ALM_SOUND=%u\n", (unsigned)(s_cfg_alarm_sound ? 1U : 0U));
        return;
    }
    if(strncmp(line, "TEST_NOTIF|", 11) == 0) {
        char nsrc[32] = "TestApp";
        char ntitle[32] = "Test";
        char nbody[128] = "Notification test message";
        char buf[200];
        strncpy(buf, line + 11, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char* p1 = strchr(buf, '|');
        if(p1) {
            *p1 = '\0';
            snprintf(nsrc, sizeof(nsrc), "%.31s", buf);
            char* p2 = strchr(p1 + 1, '|');
            if(p2) {
                *p2 = '\0';
                snprintf(ntitle, sizeof(ntitle), "%.31s", p1 + 1);
                snprintf(nbody, sizeof(nbody), "%.127s", p2 + 1);
            } else {
                snprintf(ntitle, sizeof(ntitle), "%.31s", p1 + 1);
            }
        } else {
            snprintf(nbody, sizeof(nbody), "%.127s", buf);
        }
        static uint32_t s_test_nid = 9000;
        addPhoneNotification(++s_test_nid, nsrc, ntitle, nbody);
        Serial.printf("OK|TEST_NOTIF|src=%s|title=%s|body=%s\n", nsrc, ntitle, nbody);
        return;
    }
    if(strncmp(line, "DISMISS_NOTIF|", 14) == 0) {
        uint32_t did = (uint32_t)strtoul(line + 14, NULL, 10);
        for(uint8_t ni = 0; ni < s_notif_count; ni++) {
            if(s_phone_notifs[ni].id == did) {
                dismissPhoneNotification(ni);
                Serial.printf("OK|DISMISS_NOTIF=%lu\n", (unsigned long)did);
                return;
            }
        }
        removePhoneNotification(did);
        Serial.printf("OK|DISMISS_NOTIF=%lu\n", (unsigned long)did);
        return;
    }
    if(strcmp(line, "CLEAR_NOTIFS") == 0) {
        dismissAllPhoneNotifications();
        Serial.println("OK|CLEAR_NOTIFS");
        return;
    }
    if(strncmp(line, "FLASHLIGHT|", 11) == 0) {
        bool on = (atoi(line + 11) != 0);
        setFlashlight(on);
        Serial.printf("OK|FLASHLIGHT=%u\n", (unsigned)(s_flashlight_active ? 1U : 0U));
        return;
    }
    if(strncmp(line, "SOS|", 4) == 0) {
        bool on = (atoi(line + 4) != 0);
        setMedicalSos(on);
        Serial.printf("OK|SOS=%u\n", (unsigned)(s_sos_active ? 1U : 0U));
        return;
    }
    if(strncmp(line, "PWR_SAVER|", 10) == 0) {
        s_cfg_power_saver = (uint8_t)(atoi(line + 10) % 3);
        saveSettingsToSpiffs();
        renderDisplay();
        Serial.printf("OK|PWR_SAVER=%u\n", (unsigned)s_cfg_power_saver);
        return;
    }
    if(strncmp(line, "NIGHT_DND|", 10) == 0) {
        s_cfg_night_dnd = (atoi(line + 10) != 0);
        saveSettingsToSpiffs();
        renderDisplay();
        Serial.printf("OK|NIGHT_DND=%u\n", (unsigned)(s_cfg_night_dnd ? 1U : 0U));
        return;
    }
    if(strncmp(line, "HOURLY_CHM|", 11) == 0) {
        s_cfg_hourly_chime = (atoi(line + 11) != 0);
        saveSettingsToSpiffs();
        renderDisplay();
        Serial.printf("OK|HOURLY_CHM=%u\n", (unsigned)(s_cfg_hourly_chime ? 1U : 0U));
        return;
    }
    if(strncmp(line, "SET_ICE|", 8) == 0) {
        const char* p = line + 8;
        const char* sep = strchr(p, '|');
        if(sep) {
            size_t nlen = (size_t)(sep - p);
            if(nlen >= sizeof(s_ice_name)) nlen = sizeof(s_ice_name) - 1;
            memcpy(s_ice_name, p, nlen);
            s_ice_name[nlen] = '\0';
            snprintf(s_ice_phone, sizeof(s_ice_phone), "%s", sep + 1);
            char* nl = strpbrk(s_ice_phone, "\r\n");
            if(nl) *nl = '\0';
            saveIceToSpiffs();
            wakeScreen("ice_cmd");
            showPopup("ICE UPDATED", s_ice_name, s_ice_phone, "Saved to Watch", 3500U);
            Serial.printf("OK|SET_ICE|name=%s|phone=%s\n", s_ice_name, s_ice_phone);
            sendJugglucoTx("OK|SET_ICE|name=%s|phone=%s\n", s_ice_name, s_ice_phone);
        } else {
            Serial.println("ERR|SET_ICE_USAGE:SET_ICE|<Name>|<Phone>");
            sendJugglucoTx("ERR|SET_ICE_USAGE:SET_ICE|<Name>|<Phone>\n");
        }
        return;
    }
    if(strcmp(line, "STATUS") == 0 || strcmp(line, "status") == 0) {
        sendJugglucoTx("STATUS|bt_mode=%u|bat=%u|bl=%u|dnd=%u|ps=%u|ice=%s\n",
                       (unsigned)s_cfg_bt_mode, (unsigned)s_batt_pct,
                       (unsigned)kBrightPctTable[s_cfg_bright_idx % 5U],
                       (unsigned)(s_cfg_night_dnd ? 1U : 0U),
                       (unsigned)s_cfg_power_saver, s_ice_name);
        Serial.printf("STATUS|bt_mode=%u|ble_conn=%d|jg_conn=%d|jg_pkts=%u|has_scan=%d|mmol_t=%u|last_sec_ago=%lu|bat=%u|rw=%u|rgb=%u|dp=%u|nl=%u|ns=%u|as=%u|notifs=%u|asleep=%d|flash=%u|sos=%u|ps=%u|dnd=%u|hchm=%u|bl_idx=%u|bl_pct=%u\n",
                      (unsigned)s_cfg_bt_mode,
                      (int)s_ble_connected,
                      (int)s_juggluco_connected,
                      (unsigned)s_juggluco_pkts_rx,
                      (int)compState.has_scan,
                      (unsigned)compState.vendor_chain.mmol_tenths,
                      (unsigned long)((millis() - lastScanMillis) / 1000UL),
                      (unsigned)s_batt_pct,
                      (unsigned)(s_cfg_raise_wake ? 1U : 0U),
                      (unsigned)(s_cfg_rgb_led ? 1U : 0U),
                      (unsigned)s_cfg_deck_pulse_idx,
                      (unsigned)(s_cfg_notif_light ? 1U : 0U),
                      (unsigned)(s_cfg_notif_sound ? 1U : 0U),
                      (unsigned)(s_cfg_alarm_sound ? 1U : 0U),
                      (unsigned)s_notif_count,
                      (int)s_screen_asleep,
                      (unsigned)(s_flashlight_active ? 1U : 0U),
                      (unsigned)(s_sos_active ? 1U : 0U),
                      (unsigned)s_cfg_power_saver,
                      (unsigned)(s_cfg_night_dnd ? 1U : 0U),
                      (unsigned)(s_cfg_hourly_chime ? 1U : 0U),
                      (unsigned)s_cfg_bright_idx,
                      (unsigned)kBrightPctTable[s_cfg_bright_idx % 5U]);
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
    initBatteryMonitoring();

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
        loadIceFromSpiffs();
        loadHistoryFromSpiffs();
        snprintf(saveStatus, sizeof(saveStatus), "SPIFFS ready (%u pts)", (unsigned)s_history.count);
    } else {
        snprintf(saveStatus, sizeof(saveStatus), "SPIFFS mount err");
    }

    initBacklight();
    initPiezoBuzzer();
    initRgbLed();
    setBacklightPercent(kBrightPctTable[s_cfg_bright_idx % 5U]);

    initLsm6ds3Imu();
    initPcf8563Rtc();
    s_last_activity_ms = millis();
    applyBtModeChange();
    pollBattery();
    updateRgbLedStatus();

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
    pollNusIncomingLines();
    pollBattery();
    pollImuAndSleepTimeout();
    pollFlightDeckAndAlarms();

    if(s_ble_conn_event) {
        s_ble_conn_event = false;
        snprintf(s_ble_status, sizeof(s_ble_status), "Phone Connected");
        Serial.println("BLE_SERVER|Phone/Gadgetbridge connected");
        updateRgbLedStatus();
        renderDisplay();
    }
    if(s_ble_disc_event) {
        s_ble_disc_event = false;
        snprintf(s_ble_status, sizeof(s_ble_status), "Adv: Bangle.js Artemis");
        Serial.printf("BLE_SERVER|Phone disconnected (reason=0x%02X), scheduling adv restart\n", (unsigned)s_ble_last_disc_reason);
        updateRgbLedStatus();
        renderDisplay();
    }

    if(s_restart_adv_needed) {
        s_restart_adv_needed = false;
        delay(120);
        if(s_cfg_bt_mode == BLE_MODE_JUGGLUCO && !s_juggluco_connected) {
            BLEDevice::startAdvertising();
            Serial.println("BLE_SERVER|Advertising restarted cleanly");
        }
    }

    static unsigned long s_last_adv_poll_ms = 0UL;
    if(s_cfg_bt_mode == BLE_MODE_JUGGLUCO && !s_juggluco_connected && (millis() - s_last_adv_poll_ms > 8000UL)) {
        s_last_adv_poll_ms = millis();
        if(BLEDevice::getAdvertising()) {
            BLEDevice::startAdvertising();
        }
    }

    static unsigned long s_last_gb_poll_ms = 0UL;
    if(s_juggluco_connected && s_cfg_bt_mode == BLE_MODE_JUGGLUCO && 
       (millis() - s_juggluco_conn_time_ms > 15000UL) &&
       (millis() - s_last_gb_poll_ms > 60000UL)) {
        s_last_gb_poll_ms = millis();
        sendJugglucoTx("{\"t\":\"weather\"}\n");
    }

    if(s_popup_active && (int32_t)(millis() - s_popup_until_ms) >= 0) {
        s_popup_active = false;
        s_popup_is_notif = false;
        s_popup_is_call = false;
        setBuzzerTone(0U);
        setFlightDeckLeds(false);
        renderDisplay();
    }

    const int bpins[4] = { s_pins.btnUp, s_pins.btnDown, s_pins.btnSelect, s_pins.btnAlt };
    for(int i = 0; i < 4; i++) {
        bool cur = (gpio_get_level((gpio_num_t)bpins[i]) == 1);
        if(cur && !s_btn_prev[i]) {
            s_btn_press_ms[i] = millis();
            s_btn_long_fired[i] = false;
            s_btn_woke_sleep[i] = false;

            if(s_flashlight_active) {
                setFlashlight(false);
                s_btn_long_fired[i] = true;
                s_last_btn_ms = millis();
                s_last_activity_ms = millis();
                s_btn_prev[i] = cur;
                continue;
            }

            if(s_sos_active) {
                setMedicalSos(false);
                s_btn_long_fired[i] = true;
                s_last_btn_ms = millis();
                s_last_activity_ms = millis();
                s_btn_prev[i] = cur;
                continue;
            }

            if(s_alarm_latched) {
                acknowledgeUrgentAlarm();
                s_btn_long_fired[i] = true;
                s_last_btn_ms = millis();
                s_last_activity_ms = millis();
                s_btn_prev[i] = cur;
                continue;
            }

            if(s_find_watch_active) {
                s_find_watch_active = false;
                setBuzzerTone(0U);
                setFlightDeckLeds(false);
                s_popup_active = false;
                renderDisplay();
                s_btn_long_fired[i] = true;
                s_last_btn_ms = millis();
                s_last_activity_ms = millis();
                s_btn_prev[i] = cur;
                continue;
            }

            if(s_screen_asleep) {
                wakeScreen("button");
                s_btn_woke_sleep[i] = true;
                s_last_btn_ms = millis();
                s_last_activity_ms = millis();
            } else if(s_popup_active) {
                if(s_popup_is_call) {
                    if(i == 2) { // SELECT: Reject call on phone
                        sendJugglucoTx("{\"t\":\"call\",\"n\":\"REJECT\"}\n");
                        Serial.println("GB_CALL_REJECT|sent_to_phone");
                    }
                    s_popup_active = false;
                    s_popup_is_call = false;
                    setBuzzerTone(0U);
                    setFlightDeckLeds(false);
                    renderDisplay();
                } else if(s_popup_is_notif) {
                    if(i == 2) { // SELECT: Dismiss notification immediately
                        dismissPhoneNotification(0);
                    } else if(i == 1) { // DOWN: Open Slide 2 to view full notification
                        s_popup_active = false;
                        s_popup_is_notif = false;
                        currentPage = PAGE_NOTIFS;
                        s_notif_selected = 0U;
                        renderDisplay();
                    } else { // UP or ALT: Close popup, keep saved on Slide 2
                        s_popup_active = false;
                        s_popup_is_notif = false;
                        renderDisplay();
                    }
                } else {
                    s_popup_active = false;
                    setBuzzerTone(0U);
                    setFlightDeckLeds(false);
                    renderDisplay();
                }
                s_btn_long_fired[i] = true;
                s_last_btn_ms = millis();
                s_last_activity_ms = millis();
                s_btn_prev[i] = cur;
                continue;
            }
        }

        if(cur && s_btn_prev[i] && !s_btn_long_fired[i]) {
            unsigned long holdDuration = millis() - s_btn_press_ms[i];
            if(i == 3 && holdDuration >= 1500UL) {
                s_btn_long_fired[3] = true;
                s_last_activity_ms = millis();
                toggleFlashlight();
            } else if(i == 2 && holdDuration >= 3000UL) {
                s_btn_long_fired[2] = true;
                s_last_activity_ms = millis();
                toggleMedicalSos();
            }
        }

        if(!cur && s_btn_prev[i]) {
            if(!s_btn_long_fired[i] && !s_btn_woke_sleep[i] && (millis() - s_last_btn_ms > 120U)) {
                s_last_btn_ms = millis();
                s_last_activity_ms = millis();
                s_woke_by_raise = false;
                s_wrist_down_since_ms = 0UL;

                if(i == 0) { // UP
                    if(currentPage == PAGE_SETTINGS && s_settings_row > 0U) {
                        s_settings_row--;
                    } else if(currentPage == PAGE_NOTIFS && s_notif_count > 1U && s_notif_selected > 0U) {
                        s_notif_selected--;
                    } else {
                        currentPage = (ScreenPage)(((int)currentPage + PAGE_COUNT - 1) % PAGE_COUNT);
                        if(currentPage == PAGE_SETTINGS) s_settings_row = 22U;
                    }
                    renderDisplay();
                } else if(i == 1) { // DOWN
                    if(currentPage == PAGE_SETTINGS && s_settings_row < 22U) {
                        s_settings_row++;
                    } else if(currentPage == PAGE_NOTIFS && s_notif_count > 1U && (s_notif_selected + 1U) < s_notif_count) {
                        s_notif_selected++;
                    } else {
                        currentPage = (ScreenPage)(((int)currentPage + 1) % PAGE_COUNT);
                        if(currentPage == PAGE_SETTINGS) s_settings_row = 0U;
                    }
                    renderDisplay();
                } else if(i == 2) { // SELECT
                    if(currentPage == PAGE_NOTIFS) {
                        if(s_notif_count > 0U) {
                            dismissPhoneNotification(s_notif_selected);
                        }
                    } else if(currentPage == PAGE_SETTINGS) {
                        if(s_settings_row == 0U) {
                            s_cfg_bt_mode = (uint8_t)((s_cfg_bt_mode + 1U) % 3U);
                            applyBtModeChange();
                        } else if(s_settings_row == 1U) {
                            s_cfg_rgb_led = !s_cfg_rgb_led;
                            updateRgbLedStatus();
                        } else if(s_settings_row == 2U) {
                            s_cfg_raise_wake = !s_cfg_raise_wake;
                        } else if(s_settings_row == 3U) {
                            s_cfg_deck_pulse_idx = (uint8_t)((s_cfg_deck_pulse_idx + 1U) % 6U);
                            if(s_cfg_deck_pulse_idx > 0U) startDeckAestheticPulse();
                        } else if(s_settings_row == 4U) {
                            s_cfg_notif_light = !s_cfg_notif_light;
                            if(s_cfg_notif_light) {
                                s_led_mode = LED_MODE_NOTIF_PULSE;
                                s_led_start_ms = millis();
                                s_led_until_ms = s_led_start_ms + 600UL;
                            }
                        } else if(s_settings_row == 5U) {
                            s_cfg_notif_sound = !s_cfg_notif_sound;
                            if(s_cfg_notif_sound) playNotificationChime();
                        } else if(s_settings_row == 6U) {
                            s_cfg_warn_low_tenths = cycleThresholdTable(s_cfg_warn_low_tenths, kWarnLowTable, 10U);
                        } else if(s_settings_row == 7U) {
                            s_cfg_warn_high_tenths = cycleThresholdTable(s_cfg_warn_high_tenths, kWarnHighTable, 12U);
                        } else if(s_settings_row == 8U) {
                            s_cfg_urg_low_tenths = cycleThresholdTable(s_cfg_urg_low_tenths, kUrgLowTable, 10U);
                        } else if(s_settings_row == 9U) {
                            s_cfg_urg_high_tenths = cycleThresholdTable(s_cfg_urg_high_tenths, kUrgHighTable, 11U);
                        } else if(s_settings_row == 10U) {
                            s_cfg_alarm_sound = !s_cfg_alarm_sound;
                        } else if(s_settings_row == 11U) {
                            s_cfg_timeout_idx = (uint8_t)((s_cfg_timeout_idx + 1U) % 7U);
                        } else if(s_settings_row == 12U) {
                            s_cfg_bright_idx = (uint8_t)((s_cfg_bright_idx + 1U) % 5U);
                            setBacklightPercent(kBrightPctTable[s_cfg_bright_idx]);
                        } else if(s_settings_row == 13U) {
                            s_cfg_theme_idx = (uint8_t)((s_cfg_theme_idx + 1U) % 6U);
                        } else if(s_settings_row == 14U) {
                            s_cfg_glitch_fx = !s_cfg_glitch_fx;
                        } else if(s_settings_row == 15U) { // PWR SAVER
                            s_cfg_power_saver = (uint8_t)((s_cfg_power_saver + 1U) % 3U);
                        } else if(s_settings_row == 16U) { // NIGHT DND
                            s_cfg_night_dnd = !s_cfg_night_dnd;
                        } else if(s_settings_row == 17U) { // HOURLY CHM
                            s_cfg_hourly_chime = !s_cfg_hourly_chime;
                        } else if(s_settings_row == 18U) { // FLASHLIGHT
                            toggleFlashlight();
                        } else if(s_settings_row == 19U) { // SOS BEACON
                            toggleMedicalSos();
                        } else if(s_settings_row == 20U) { // SET HOUR
                            WatchDateTime dt;
                            if(!readPcf8563Rtc(&dt)) dt = { 2026, 10, 17, 6, 12, 0, 0 };
                            dt.hour = (uint8_t)((dt.hour + 1U) % 24U);
                            dt.sec = 0U;
                            writePcf8563Rtc(dt);
                        } else if(s_settings_row == 21U) { // SET MIN
                            WatchDateTime dt;
                            if(!readPcf8563Rtc(&dt)) dt = { 2026, 10, 17, 6, 12, 0, 0 };
                            dt.min = (uint8_t)((dt.min + 1U) % 60U);
                            dt.sec = 0U;
                            writePcf8563Rtc(dt);
                        } else if(s_settings_row == 22U) { // SET DATE
                            WatchDateTime dt;
                            if(!readPcf8563Rtc(&dt)) dt = { 2026, 10, 17, 6, 12, 0, 0 };
                            dt.day = (uint8_t)((dt.day % 31U) + 1U);
                            writePcf8563Rtc(dt);
                        }
                        saveSettingsToSpiffs();
                        renderDisplay();
                    } else if(currentPage == PAGE_GRAPH) {
                        s_zoom_idx = (s_zoom_idx + 1U) % 7U;
                        if(s_history.count == 0U && s_zoom_idx < 6U) {
                            showPopup("No Scans Yet", "0 scans stored.", "Scan Libre 2 first", "to plot graph.", 2800U);
                        }
                        renderDisplay();
                    } else if(currentPage == PAGE_BLE) {
                        toggleBlePairing();
                        renderDisplay();
                    } else {
                        currentPage = PAGE_HOME;
                        renderDisplay();
                    }
                } else if(i == 3) { // ALT (Back)
                    currentPage = (currentPage == PAGE_HOME) ? PAGE_SETTINGS : PAGE_HOME;
                    if(currentPage == PAGE_SETTINGS) s_settings_row = 0U;
                    renderDisplay();
                }
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
            triggerGlucoseAlerts((uint16_t)bf.calibrated.mmol_tenths);
            if(s_alarm_latched) {
                wakeScreen("urgent_direct");
            } else {
                if(!s_screen_asleep) renderDisplay();
            }
            notifyResultSummary("BLE-S");
        }
    }

    if(s_ble_enabled && s_cfg_bt_mode == BLE_MODE_DIRECT) {
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
