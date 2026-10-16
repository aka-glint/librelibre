/*
 * LibreLibre ESP32 — Waveshare ESP32-S3-Touch-LCD-2 (320x240 ST7789 + SD + CST816D Touch)
 *
 * Strict offline / air-gapped build: WiFi and BLE radios are NOT initialized.
 * Runs the full LibreLibre C pipeline natively on the ESP32-S3:
 *   - 17-command ISO15693 master state machine (nfc_io.c)
 *   - EU2 XOR decryption + 3-section CRC16 (integrity.c)
 *   - Per-sensor calibration extraction + temperature/glucose chain (vendor_chain.c)
 *   - Sensor-clock freshness + remaining lifetime (lifecycle.c)
 *   - 5-bit UID-to-serial derivation (protocol.c)
 *   - Verified microSD JSON capture save + exact readback (capture.c)
 *   - Persistent per-sensor scan history log + interactive zoomable graph (scan_history.c)
 *
 * License: MIT
 */

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/sdspi_host.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include <sys/stat.h>
#include <sys/unistd.h>

#include "bridge.h"
#include "nfc_io.h"
#include "scan_history.h"
#include "libre2_ble.h"
#include "factory.h"
#include "vendor_tables.h"

static int uartPassthroughTransceive(void* ctx, const uint8_t* tx_buf, size_t tx_len, uint8_t* rx_buf, size_t rx_cap, size_t* rx_len_out);
#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// ─── WAVESHARE ESP32-S3-TOUCH-LCD-2 PINOUT ────────────────
#define WS_SPI_HOST      SPI2_HOST
#define WS_LCD_SCLK      GPIO_NUM_39
#define WS_LCD_MOSI      GPIO_NUM_38
#define WS_SD_MISO       GPIO_NUM_40
#define WS_LCD_CS        GPIO_NUM_45
#define WS_LCD_DC        GPIO_NUM_42
#define WS_LCD_RST       GPIO_NUM_NC
#define WS_LCD_BL        GPIO_NUM_1
#define WS_SD_CS         GPIO_NUM_41

#define WS_TOUCH_SDA     48
#define WS_TOUCH_SCL     47
#define WS_TOUCH_INT     46
#define WS_TOUCH_ADDR    0x15

#define WS_BOOT_BTN      0

// External UART for Flipper Zero GPIO 13 (TX) / 14 (RX)
#define FLIPPER_UART_RX  16
#define FLIPPER_UART_TX  17
#define FLIPPER_BAUD     115200

#define LCD_W            320
#define LCD_H            240

SET_LOOP_TASK_STACK_SIZE(16 * 1024);

#define SD_MOUNT_POINT   "/sdcard"
#define CAPTURE_DIR      "/sdcard/libre_research"
#define HISTORY_CSV_PATH "/sdcard/libre_research/scans.csv"
#define BLE_BOND_PATH    "/sdcard/libre_research/ble_bond.txt"
#define LAST_SCAN_PATH   "/sdcard/libre_research/last_frame.txt"

// ─── RGB565 COLORS (byte-swapped for ST7789 SPI big-endian stream) ──
static inline uint16_t rgb565_be(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t c = ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
    return (uint16_t)((c >> 8) | (c << 8));
}

#define COL_BG           rgb565_be(10, 14, 22)
#define COL_HEADER_BG    rgb565_be(20, 30, 48)
#define COL_CARD_BG      rgb565_be(18, 26, 40)
#define COL_GRID         rgb565_be(36, 48, 68)
#define COL_TARGET_BAND  rgb565_be(16, 42, 34)
#define COL_WHITE        rgb565_be(255, 255, 255)
#define COL_CYAN         rgb565_be(0, 230, 255)
#define COL_GREEN        rgb565_be(40, 230, 120)
#define COL_AMBER        rgb565_be(255, 190, 40)
#define COL_RED          rgb565_be(255, 70, 70)
#define COL_MUTED        rgb565_be(150, 165, 185)

// ─── HARDWARE HANDLES & STATE ─────────────────────────────
static esp_lcd_panel_handle_t s_panel = nullptr;
static esp_lcd_panel_io_handle_t s_panel_io = nullptr;
static DMA_ATTR uint16_t s_dma_band[LCD_W * 40];
class BandedCanvas16 : public Adafruit_GFX {
public:
    int16_t band_y0 = 0;
    BandedCanvas16() : Adafruit_GFX(LCD_W, LCD_H) {}
    void initBuffer() {}
    void drawPixel(int16_t x, int16_t y, uint16_t color) override {
        int16_t ly = y - band_y0;
        if(x >= 0 && x < LCD_W && ly >= 0 && ly < 40) {
            s_dma_band[ly * LCD_W + x] = color;
        }
    }
    void fillScreen(uint16_t color) override {
        for(size_t i = 0; i < (size_t)(LCD_W * 40); i++) s_dma_band[i] = color;
    }
};
static BandedCanvas16 s_static_canvas;
static BandedCanvas16* s_canvas = &s_static_canvas;

static sdmmc_card_t* s_card = nullptr;
static sdspi_dev_handle_t s_sdspi_handle = -1;
static bool s_sd_mounted = false;

enum ScreenPage { PAGE_MAIN = 0, PAGE_GRAPH = 1, PAGE_DIAG = 2, PAGE_UID = 3, PAGE_BLE = 4, PAGE_COUNT = 5 };
static ScreenPage currentPage = PAGE_MAIN;

// Zoom levels on PAGE_GRAPH (minutes of sensor time, 0 = ALL sensor life)
static const uint32_t kZoomWindowsMin[5] = { 180U, 360U, 720U, 1440U, 0U };
static const char* const kZoomLabels[5]  = { "3h", "6h", "12h", "24h", "ALL" };
static size_t s_zoom_idx = 2U; // Default 12h

static LlCompanionState compState;
static LlHistoryStore s_history;
static uint32_t lastScanMillis = 0;
static uint32_t scanCount = 0;
static char lastTransport[16] = "none";
static char saveStatus[40] = "SD: not checked";
static char savedFileName[40] = "";

// Line buffers
static char usbBuf[LL_BRIDGE_LINE_MAX];
static size_t usbLen = 0;
static char uartBuf[LL_BRIDGE_LINE_MAX];
static size_t uartLen = 0;

// Button / touch debounce
static bool bootPrev = false;
static unsigned long lastNavMs = 0;
static volatile bool s_touch_irq = false;
static bool s_touch_was_down = false;

// ─── ST7789 LCD + BACKLIGHT INIT (exact artemis2-7 sequence) ──
static void initBacklight() {
    ledc_timer_config_t ledc_timer = {};
    ledc_timer.speed_mode = LEDC_LOW_SPEED_MODE;
    ledc_timer.timer_num = LEDC_TIMER_0;
    ledc_timer.duty_resolution = LEDC_TIMER_8_BIT;
    ledc_timer.freq_hz = 5000;
    ledc_timer.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {};
    ledc_channel.speed_mode = LEDC_LOW_SPEED_MODE;
    ledc_channel.channel = LEDC_CHANNEL_0;
    ledc_channel.timer_sel = LEDC_TIMER_0;
    ledc_channel.intr_type = LEDC_INTR_DISABLE;
    ledc_channel.gpio_num = WS_LCD_BL;
    ledc_channel.duty = 255;
    ledc_channel.hpoint = 0;
    ledc_channel_config(&ledc_channel);
}

static void initLcdAndSpi2() {
    initBacklight();

    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num = WS_LCD_SCLK;
    buscfg.mosi_io_num = WS_LCD_MOSI;
    buscfg.miso_io_num = WS_SD_MISO;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = LCD_W * 40 * sizeof(uint16_t);
    ESP_ERROR_CHECK(spi_bus_initialize(WS_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.dc_gpio_num = WS_LCD_DC;
    io_config.cs_gpio_num = WS_LCD_CS;
    io_config.pclk_hz = 40 * 1000 * 1000;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.spi_mode = 0;
    io_config.trans_queue_depth = 10;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)WS_SPI_HOST, &io_config, &s_panel_io));

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = WS_LCD_RST;
    panel_config.color_space = ESP_LCD_COLOR_SPACE_RGB;
    panel_config.bits_per_pixel = 16;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_panel_io, &panel_config, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    esp_lcd_panel_io_tx_param(s_panel_io, 0x11, NULL, 0); // explicit SLPOUT
    delay(120);
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    s_static_canvas.initBuffer();
    Serial.printf("FREE_HEAP|internal=%u|psram=%u\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
}

static void renderDisplayBand();
static void flushCanvas() {}
static void renderDisplay() {
    if(!s_panel || !s_canvas) return;
    for(int16_t y = 0; y < LCD_H; y += 40) {
        s_canvas->band_y0 = y;
        renderDisplayBand();
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_W, y + 40, s_dma_band);
        delay(8);
    }
}

// ─── SHARED SPI2 MICROSD MOUNT & PERSISTENT HISTORY LOG ───
static esp_err_t sd_noop_deinit(void) { return ESP_OK; }
static esp_err_t sd_noop_deinit_p(int slot) { (void)slot; return ESP_OK; }

static void loadHistoryFromSd() {
    if(!s_sd_mounted) return;
    FILE* f = fopen(HISTORY_CSV_PATH, "rb");
    if(!f) return;
    char line[128];
    while(fgets(line, sizeof(line), f)) {
        LlHistoryPoint pt;
        if(ll_history_parse_csv_line(line, &pt)) {
            ll_history_add(&s_history, pt.serial, pt.age_minutes, pt.mmol_tenths, pt.mg_dl, pt.boot_sec);
        }
    }
    fclose(f);
}

static void appendHistoryToSd(const LlHistoryPoint* pt) {
    if(!s_sd_mounted || !pt) return;
    struct stat st;
    bool needHeader = (stat(HISTORY_CSV_PATH, &st) != 0);
    FILE* f = fopen(HISTORY_CSV_PATH, "ab");
    if(!f) return;
    if(needHeader) {
        const char* hdr = "serial,age_minutes,mmol_tenths,mg_dl,boot_sec\n";
        fwrite(hdr, 1, strlen(hdr), f);
    }
    char row[96];
    size_t n = ll_history_format_csv_line(pt, row, sizeof(row));
    if(n > 0U) {
        fwrite(row, 1, n, f);
        fflush(f);
        fsync(fileno(f));
    }
    fclose(f);
}

static bool initMicroSd() {
    if(s_sd_mounted) return true;
    if(s_sdspi_handle < 0) {
        sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot_config.gpio_cs = WS_SD_CS;
        slot_config.host_id = WS_SPI_HOST;
        if(sdspi_host_init_device(&slot_config, &s_sdspi_handle) != ESP_OK) {
            snprintf(saveStatus, sizeof(saveStatus), "SD: bus init err");
            return false;
        }
    }
    gpio_set_pull_mode(WS_SD_CS, GPIO_PULLUP_ONLY);

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 5;
    mount_config.allocation_unit_size = 16 * 1024;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = WS_SD_CS;
    slot_config.host_id = WS_SPI_HOST;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = s_sdspi_handle;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    host.flags = SDMMC_HOST_FLAG_SPI | SDMMC_HOST_FLAG_DEINIT_ARG;
    host.deinit = &sd_noop_deinit;
    host.deinit_p = &sd_noop_deinit_p;

    if(esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card) != ESP_OK) {
        s_sd_mounted = false;
        snprintf(saveStatus, sizeof(saveStatus), "SD: no card (live scan OK)");
        return false;
    }
    s_sd_mounted = true;
    struct stat st;
    if(stat(CAPTURE_DIR, &st) != 0) {
        mkdir(CAPTURE_DIR, 0777);
    }
    loadHistoryFromSd();
    snprintf(saveStatus, sizeof(saveStatus), "SD ready (%u scans logged)", (unsigned)s_history.count);
    return true;
}

static bool saveCaptureToSd(LcCapture* cap) {
    if(!cap || !cap->uid_valid) {
        snprintf(saveStatus, sizeof(saveStatus), "No UID: not saved");
        return false;
    }
    if(!s_sd_mounted && !initMicroSd()) {
        snprintf(saveStatus, sizeof(saveStatus), "SD absent (RAM graph only)");
        return false;
    }
    cap->save_timestamp = (uint32_t)(millis() / 1000U);
    cap->save_time_recorded = true;

    char* json = (char*)malloc(LC_JSON_CAPACITY);
    char* verified = (char*)malloc(LC_JSON_CAPACITY);
    if(!json || !verified) {
        free(verified);
        free(json);
        snprintf(saveStatus, sizeof(saveStatus), "SD: out of memory");
        return false;
    }
    size_t length = lc_capture_json(cap, json, LC_JSON_CAPACITY);
    bool saved = false;
    char path[128] = "";
    if(length > 0) {
        char uid_hex[17];
        lc_hex(cap->uid, 8U, uid_hex, sizeof(uid_hex));
        uint32_t tick = (uint32_t)millis();
        for(unsigned i = 0; i < 100U; i++) {
            snprintf(path, sizeof(path), CAPTURE_DIR "/cap_%s_%08lx_%02u.json",
                     uid_hex, (unsigned long)tick, i);
            struct stat st;
            if(stat(path, &st) == 0) continue;
            FILE* f = fopen(path, "wb");
            if(!f) break;
            size_t w = fwrite(json, 1, length, f);
            fflush(f);
            fsync(fileno(f));
            fclose(f);
            if(w == length) {
                FILE* rf = fopen(path, "rb");
                if(rf) {
                    size_t r = fread(verified, 1, length, rf);
                    fclose(rf);
                    if(r == length && memcmp(json, verified, length) == 0) {
                        saved = true;
                        const char* base = strrchr(path, '/');
                        snprintf(savedFileName, sizeof(savedFileName), "%s", base ? base + 1 : path);
                    }
                }
            }
            if(!saved) unlink(path);
            break;
        }
    }
    free(verified);
    free(json);
    snprintf(saveStatus, sizeof(saveStatus), "%s",
             saved ? "Saved + verified on SD" : "SD save failed; tap to retry");
    return saved;
}

// ─── CST816D TOUCH INIT & COORDINATE READ (320x240 landscape) ──
static void IRAM_ATTR onTouchIrq() {
    s_touch_irq = true;
}

static void initTouchCst816() {
    pinMode(WS_TOUCH_INT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(WS_TOUCH_INT), onTouchIrq, FALLING);
    Wire.begin(WS_TOUCH_SDA, WS_TOUCH_SCL, 400000);
    delay(20);
    Wire.beginTransmission(WS_TOUCH_ADDR);
    Wire.write(0xFE);
    Wire.write(0xFE);
    Wire.endTransmission(true);

    Wire.beginTransmission(WS_TOUCH_ADDR);
    Wire.write(0xFB);
    Wire.write(0x00);
    Wire.endTransmission(true);
}

static bool pollTouchTap(uint16_t* x_out, uint16_t* y_out) {
    bool pinLow = (digitalRead(WS_TOUCH_INT) == LOW);
    bool irq = s_touch_irq;
    s_touch_irq = false;

    if(!pinLow && !irq) {
        s_touch_was_down = false;
        return false;
    }
    Wire.beginTransmission(WS_TOUCH_ADDR);
    Wire.write(0x01);
    if(Wire.endTransmission(true) != 0) {
        if(!pinLow) s_touch_was_down = false;
        return false;
    }
    if(Wire.requestFrom((uint8_t)WS_TOUCH_ADDR, (uint8_t)6) != 6) {
        if(!pinLow) s_touch_was_down = false;
        return false;
    }
    uint8_t raw[6];
    for(int i = 0; i < 6; i++) raw[i] = Wire.read();
    bool is_down = ((raw[1] & 0x0F) == 1);
    bool rising_tap = (is_down && !s_touch_was_down);
    s_touch_was_down = is_down && pinLow;
    if(rising_tap) {
        uint16_t raw_x = ((uint16_t)(raw[2] & 0x0F) << 8) | raw[3];
        uint16_t raw_y = ((uint16_t)(raw[4] & 0x0F) << 8) | raw[5];
        uint16_t lx = raw_y > 319U ? 319U : raw_y;
        uint16_t ly = 239U - (raw_x > 239U ? 239U : raw_x);
        if(x_out) *x_out = lx;
        if(y_out) *y_out = ly;
    }
    return rising_tap;
}

// ─── ZOOMABLE SCAN GRAPH RENDERER (PAGE_GRAPH) ────────────
static void renderGraphPage() {
    const char* activeSerial = (compState.has_scan && compState.serial_valid) ? compState.serial : NULL;
    static const LlHistoryPoint* pts[LL_HISTORY_MAX_POINTS];
    uint16_t minAge = 0, maxAge = 0, minMmol = 0, maxMmol = 0;
    uint32_t winMin = kZoomWindowsMin[s_zoom_idx];
    size_t n = ll_history_query_window(
        &s_history, activeSerial, winMin, pts, LL_HISTORY_MAX_POINTS,
        &minAge, &maxAge, &minMmol, &maxMmol);

    // Sub-header bar: active S/N on left, zoom pill buttons on right
    s_canvas->setTextSize(1);
    s_canvas->setTextColor(COL_CYAN);
    s_canvas->setCursor(10, 38);
    if(n > 0U) {
        s_canvas->print("S/N ");
        s_canvas->print(pts[n - 1U]->serial);
    } else if(activeSerial) {
        s_canvas->print("S/N ");
        s_canvas->print(activeSerial);
    } else {
        s_canvas->print("Scan Trend Graph");
    }

    // Zoom pill buttons along top-right (x=138..314, y=34..50)
    for(size_t z = 0; z < 5U; z++) {
        int bx = 136 + (int)z * 36;
        uint16_t bg = (z == s_zoom_idx) ? COL_CYAN : COL_CARD_BG;
        uint16_t fg = (z == s_zoom_idx) ? COL_BG : COL_MUTED;
        s_canvas->fillRoundRect(bx, 34, 33, 15, 4, bg);
        s_canvas->setTextColor(fg);
        int textX = bx + (33 - (int)strlen(kZoomLabels[z]) * 6) / 2;
        s_canvas->setCursor(textX, 38);
        s_canvas->print(kZoomLabels[z]);
    }

    // Plot area: x = 36..308 (width 272), y = 54..190 (height 136)
    const int plotX = 36, plotY = 54, plotW = 272, plotH = 136;
    s_canvas->fillRect(plotX, plotY, plotW, plotH, COL_CARD_BG);

    // Dynamic Y range in mmol_tenths (default 2.0..16.0 mmol/L = 20..160 tenths, expands if scans exceed)
    uint16_t yMin = 20U;  // 2.0 mmol/L
    uint16_t yMax = 160U; // 16.0 mmol/L
    if(n > 0U && maxMmol + 15U > yMax) {
        yMax = (uint16_t)(((maxMmol + 25U) / 20U) * 20U);
    }
    if(n > 0U && minMmol < yMin + 5U) {
        yMin = (minMmol > 10U) ? (uint16_t)(minMmol - 10U) : 0U;
    }
    uint16_t ySpan = (yMax > yMin) ? (uint16_t)(yMax - yMin) : 140U;

    auto valToY = [&](uint16_t mmolTenths) -> int {
        if(mmolTenths <= yMin) return plotY + plotH - 2;
        if(mmolTenths >= yMax) return plotY + 2;
        return plotY + plotH - 2 - (int)(((uint32_t)(mmolTenths - yMin) * (uint32_t)(plotH - 4)) / ySpan);
    };

    // Shaded target range band (3.9..10.0 mmol/L = 39..100 tenths)
    int yTop10 = valToY(100U);
    int yBot39 = valToY(39U);
    if(yBot39 > yTop10) {
        s_canvas->fillRect(plotX + 1, yTop10, plotW - 2, yBot39 - yTop10, COL_TARGET_BAND);
    }

    // Horizontal grid lines & Y-axis labels (4, 7, 10, 14 mmol/L)
    const uint16_t gridVals[4] = { 40U, 70U, 100U, 140U };
    for(size_t g = 0; g < 4U; g++) {
        uint16_t gv = gridVals[g];
        if(gv < yMin || gv > yMax) continue;
        int gy = valToY(gv);
        for(int x = plotX; x < plotX + plotW; x += 4) {
            s_canvas->drawPixel(x, gy, COL_GRID);
        }
        char lbl[8];
        snprintf(lbl, sizeof(lbl), "%u", (unsigned)(gv / 10U));
        s_canvas->setTextColor(COL_MUTED);
        s_canvas->setCursor(plotX - (int)strlen(lbl) * 6 - 4, gy - 3);
        s_canvas->print(lbl);
    }
    s_canvas->drawRect(plotX, plotY, plotW, plotH, COL_GRID);

    if(n == 0U) {
        s_canvas->setTextSize(2);
        s_canvas->setTextColor(COL_MUTED);
        s_canvas->setCursor(plotX + 36, plotY + 52);
        s_canvas->print("No scans logged");
        s_canvas->setTextSize(1);
        s_canvas->setCursor(plotX + 24, plotY + 80);
        s_canvas->print("Each valid scan plots time, mmol/L & S/N");
    } else {
        // Compute X span in sensor minutes
        uint16_t xEndAge = maxAge;
        uint16_t xStartAge = minAge;
        if(winMin > 0U) {
            xStartAge = (xEndAge > winMin) ? (uint16_t)(xEndAge - winMin) : 0U;
        } else if(xEndAge == xStartAge) {
            xStartAge = (xEndAge > 60U) ? (uint16_t)(xEndAge - 60U) : 0U;
        }
        uint32_t xSpan = (xEndAge > xStartAge) ? (uint32_t)(xEndAge - xStartAge) : 60U;

        int prevX = -1, prevY = -1;
        for(size_t i = 0; i < n; i++) {
            uint16_t a = pts[i]->age_minutes;
            uint32_t rel = (a > xStartAge) ? (uint32_t)(a - xStartAge) : 0U;
            if(rel > xSpan) rel = xSpan;
            int px = plotX + 6 + (int)((rel * (uint32_t)(plotW - 12)) / xSpan);
            int py = valToY(pts[i]->mmol_tenths);

            if(prevX >= 0) {
                s_canvas->drawLine(prevX, prevY, px, py, COL_CYAN);
                s_canvas->drawLine(prevX, prevY + 1, px, py + 1, COL_CYAN);
            }
            uint16_t dotCol = COL_GREEN;
            if(pts[i]->mmol_tenths < 39U) dotCol = COL_RED;
            else if(pts[i]->mmol_tenths > 100U) dotCol = COL_AMBER;
            s_canvas->fillCircle(px, py, 3, dotCol);
            prevX = px;
            prevY = py;
        }

        // X-axis time labels (left = window start, right = latest scan age)
        s_canvas->setTextSize(1);
        s_canvas->setTextColor(COL_MUTED);
        char leftLbl[24], rightLbl[24];
        snprintf(leftLbl, sizeof(leftLbl), "-%luh%02lum",
                 (unsigned long)(xSpan / 60U), (unsigned long)(xSpan % 60U));
        snprintf(rightLbl, sizeof(rightLbl), "Age %ud %uh (now)",
                 (unsigned)(xEndAge / 1440U), (unsigned)((xEndAge % 1440U) / 60U));
        s_canvas->setCursor(plotX, plotY + plotH + 4);
        s_canvas->print(leftLbl);
        s_canvas->setCursor(plotX + plotW - (int)strlen(rightLbl) * 6, plotY + plotH + 4);
        s_canvas->print(rightLbl);
    }

    // Bottom navigation & zoom bar (y = 206..238)
    s_canvas->fillRoundRect(12, 206, 144, 28, 6, COL_HEADER_BG);
    s_canvas->setTextSize(1);
    s_canvas->setTextColor(COL_CYAN);
    s_canvas->setCursor(22, 216);
    s_canvas->print("ZOOM: ");
    s_canvas->setTextColor(COL_WHITE);
    s_canvas->print(kZoomLabels[s_zoom_idx]);
    s_canvas->setTextColor(COL_MUTED);
    s_canvas->print(" (tap left)");

    s_canvas->fillRoundRect(164, 206, 144, 28, 6, COL_HEADER_BG);
    s_canvas->setTextColor(COL_WHITE);
    s_canvas->setCursor(176, 216);
    s_canvas->print("NEXT PAGE (tap right)");
}

static bool s_popup_active = false;
static unsigned long s_popup_until_ms = 0U;
static char s_popup_title[32] = "";
static char s_popup_line1[44] = "";
static char s_popup_line2[44] = "";
static char s_popup_line3[44] = "";

static void showPopup(const char* title, const char* l1, const char* l2, const char* l3, uint32_t duration_ms = 4000U) {
    snprintf(s_popup_title, sizeof(s_popup_title), "%s", title ? title : "");
    snprintf(s_popup_line1, sizeof(s_popup_line1), "%s", l1 ? l1 : "");
    snprintf(s_popup_line2, sizeof(s_popup_line2), "%s", l2 ? l2 : "");
    snprintf(s_popup_line3, sizeof(s_popup_line3), "%s", l3 ? l3 : "");
    s_popup_active = true;
    s_popup_until_ms = millis() + duration_ms;
}

static void drawPopupOverlay() {
    if(!s_popup_active) return;
    s_canvas->fillRoundRect(18, 44, 284, 152, 10, COL_AMBER);
    s_canvas->fillRoundRect(21, 47, 278, 146, 8, COL_BG);
    s_canvas->fillRoundRect(21, 47, 278, 32, 8, COL_HEADER_BG);

    s_canvas->setTextSize(2);
    s_canvas->setTextColor(COL_AMBER);
    s_canvas->setCursor(34, 55);
    s_canvas->print(s_popup_title);

    s_canvas->setTextSize(1);
    s_canvas->setTextColor(COL_WHITE);
    s_canvas->setCursor(34, 92);
    s_canvas->print(s_popup_line1);
    s_canvas->setCursor(34, 110);
    s_canvas->print(s_popup_line2);
    s_canvas->setCursor(34, 128);
    s_canvas->print(s_popup_line3);

    s_canvas->setTextColor(COL_CYAN);
    s_canvas->setCursor(66, 166);
    s_canvas->print("[ Tap anywhere to dismiss ]");
}

// ─── DIRECT LIBRE 2 BLE STREAMING STATE (0xFDE3 / 0xF001 / 0xF002) ──
static bool s_ble_enabled = false;
static bool s_ble_stack_inited = false;
static bool s_ble_connected = false;
static bool s_ble_do_connect = false;
static volatile bool s_ble_scanning = false;
static uint8_t s_ble_target_mac[6] = {0};
static bool s_ble_has_mac = false;
static uint32_t s_ble_unlock_code = 42U;
static uint16_t s_ble_unlock_count = 0U;
static LcVendorConfig s_ble_cal_cfg;
static bool s_ble_cal_valid = false;
static char s_ble_status[44] = "Standby (NFC Read-Only)";
static uint32_t s_ble_packets_ok = 0U;

static uint8_t s_ble_frag_buf[LL_BLE_RAW_PACKET_LEN];
static size_t s_ble_frag_len = 0U;
static volatile bool s_ble_packet_ready = false;
static unsigned long s_ble_last_frag_ms = 0U;
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

class Libre2ClientCallbacks : public BLEClientCallbacks {
    void onConnect(BLEClient* pclient) override {
        (void)pclient;
        s_ble_connected = true;
        snprintf(s_ble_status, sizeof(s_ble_status), "Connected (authenticating)");
    }
    void onDisconnect(BLEClient* pclient) override {
        (void)pclient;
        s_ble_connected = false;
        snprintf(s_ble_status, sizeof(s_ble_status), "Disconnected (will retry)");
    }
};

class Libre2AdvertisedCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        if(!s_ble_enabled) return;
        bool match = false;
        std::string addrObj = advertisedDevice.getAddress().toString();
        const char* adv_addr = addrObj.c_str();
        static unsigned long s_last_adv_log = 0;
        if(millis() - s_last_adv_log > 2000U) {
            s_last_adv_log = millis();
            std::string svcObj = advertisedDevice.haveServiceUUID() ? advertisedDevice.getServiceUUID().toString() : "-";
            Serial.printf("BLE_ADV_SEEN|addr=%s|rssi=%d|name=%s|svc=%s\n",
                adv_addr,
                advertisedDevice.getRSSI(),
                advertisedDevice.haveName() ? advertisedDevice.getName().c_str() : "-",
                svcObj.c_str());
        }
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
            Serial.printf("BLE_MATCH|addr=%s|rssi=%d\n", advertisedDevice.getAddress().toString().c_str(), advertisedDevice.getRSSI());
            BLEDevice::getScan()->stop();
            s_ble_scanning = false;
            if(s_ble_adv_dev) delete s_ble_adv_dev;
            s_ble_adv_dev = new BLEAdvertisedDevice(advertisedDevice);
            s_ble_do_connect = true;
        }
    }
};

static bool connectAndLoginLibre2Ble() {
    if(!s_ble_adv_dev || !compState.has_scan) return false;
    if(!s_ble_client) {
        s_ble_client = BLEDevice::createClient();
        s_ble_client->setClientCallbacks(new Libre2ClientCallbacks());
    }
    Serial.printf("BLE_CONNECTING|addr=%s|heap=%u\n", s_ble_adv_dev->getAddress().toString().c_str(), (unsigned)ESP.getFreeHeap());
    if(!s_ble_client->connect(s_ble_adv_dev)) {
        Serial.println("BLE_CONNECT_FAIL");
        snprintf(s_ble_status, sizeof(s_ble_status), "BLE connect failed");
        return false;
    }
    Serial.printf("BLE_CONNECTED_GATT|heap=%u\n", (unsigned)ESP.getFreeHeap());
    BLERemoteService* pSvc = s_ble_client->getService(BLEUUID(LL_BLE_SERVICE_UUID));
    if(!pSvc) {
        snprintf(s_ble_status, sizeof(s_ble_status), "No 0xFDE3 service on sensor");
        s_ble_client->disconnect();
        return false;
    }
    BLERemoteCharacteristic* pLogin = pSvc->getCharacteristic(BLEUUID(LL_BLE_CHAR_LOGIN_UUID));
    BLERemoteCharacteristic* pData  = pSvc->getCharacteristic(BLEUUID(LL_BLE_CHAR_DATA_UUID));
    if(!pLogin || !pData) {
        snprintf(s_ble_status, sizeof(s_ble_status), "Missing F001/F002 chars");
        s_ble_client->disconnect();
        return false;
    }

    uint8_t login12[LL_BLE_LOGIN_PAYLOAD_LEN];
    ll_libre2_build_ble_login_payload(
        compState.capture.uid,
        compState.capture.patch,
        s_ble_unlock_code,
        s_ble_unlock_count,
        login12);
    pData->registerForNotify(onLibre2BleNotify, true, true);
    BLERemoteDescriptor* pDescData = pData->getDescriptor(BLEUUID((uint16_t)0x2902));
    if(pDescData) {
        uint8_t cccdOn[2] = { 0x01, 0x00 };
        pDescData->writeValue(cccdOn, 2, true);
        Serial.println("BLE_CCCD_F002_WRITTEN");
    }
    pLogin->registerForNotify([](BLERemoteCharacteristic* c, uint8_t* d, size_t l, bool n) {
        (void)c; (void)n;
        Serial.printf("BLE_F001_NOTIFY|len=%u|b0=%02X\n", (unsigned)l, l > 0 ? d[0] : 0);
    }, true, true);
    BLERemoteDescriptor* pDescLogin = pLogin->getDescriptor(BLEUUID((uint16_t)0x2902));
    if(pDescLogin) {
        uint8_t cccdOn[2] = { 0x01, 0x00 };
        pDescLogin->writeValue(cccdOn, 2, true);
    }
    pLogin->writeValue(login12, sizeof(login12), true);
    Serial.printf("BLE_LOGIN_SENT|unlock_cnt=%u|heap=%u\n", (unsigned)s_ble_unlock_count, (unsigned)ESP.getFreeHeap());
    snprintf(s_ble_status, sizeof(s_ble_status), "Streaming 1m BLE active");
    return true;
}

static void saveBleBondToSd() {
    if(!s_sd_mounted || !s_ble_has_mac || !compState.has_scan) return;
    char uid_hex[17], patch_hex[13], mac_hex[13];
    if(!lc_hex(compState.capture.uid, 8U, uid_hex, sizeof(uid_hex)) ||
       !lc_hex(compState.capture.patch, 6U, patch_hex, sizeof(patch_hex)) ||
       !lc_hex(s_ble_target_mac, 6U, mac_hex, sizeof(mac_hex))) return;
    FILE* f = fopen(BLE_BOND_PATH, "wb");
    if(!f) return;
    fprintf(f, "LL2_BLE|%s|%s|%s|%lu\n", uid_hex, patch_hex, mac_hex, (unsigned long)s_ble_unlock_code);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

static void saveLastFrameToSd(const uint8_t uid[8], const uint8_t patch[6], const uint8_t fram[LC_FRAM_SIZE]) {
    if(!s_sd_mounted) return;
    static char line[LL_BRIDGE_LINE_MAX];
    if(ll_bridge_format_frame(uid, patch, fram, line, sizeof(line)) == 0U) return;
    FILE* f = fopen(LAST_SCAN_PATH, "wb");
    if(!f) return;
    fwrite(line, 1, strlen(line), f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

static void renderDisplay();
// ─── SCREEN SLEEP / WAKE & QMI8658 IMU (BUMP / RAISE / TAP TO WAKE) ─────────
#define SCREEN_TIMEOUT_MS 45000UL
static bool s_screen_asleep = false;
static unsigned long s_last_activity_ms = 0;
static uint8_t s_qmi_addr = 0;
static float s_mag_filtered = 1.0f;
static bool s_wrist_down = true;
static unsigned long s_last_imu_poll_ms = 0;

static void setBacklightPercent(uint8_t pct) {
    if(pct > 100U) pct = 100U;
    uint32_t duty = ((uint32_t)pct * 255U) / 100U;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void wakeScreen(const char* reason) {
    s_last_activity_ms = millis();
    if(s_screen_asleep) {
        s_screen_asleep = false;
        setBacklightPercent(100U);
        Serial.printf("SCREEN_WAKE|reason=%s\n", reason ? reason : "activity");
        renderDisplay();
    }
}

static bool qmiWriteReg(uint8_t addr, uint8_t reg, uint8_t val) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission(true) == 0;
}

static bool qmiReadRegs(uint8_t addr, uint8_t reg, uint8_t* out, size_t len) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if(Wire.endTransmission(true) != 0) return false;
    if(Wire.requestFrom((int)addr, (int)len) != (int)len) return false;
    for(size_t i = 0; i < len; i++) out[i] = (uint8_t)Wire.read();
    return true;
}

static void initQmi8658Imu() {
    const uint8_t addrs[2] = { 0x6B, 0x6A };
    for(int i = 0; i < 2; i++) {
        uint8_t who = 0;
        if(qmiReadRegs(addrs[i], 0x00, &who, 1) && who == 0x05) {
            s_qmi_addr = addrs[i];
            break;
        }
    }
    if(!s_qmi_addr) {
        Serial.println("IMU|QMI8658 not found");
        return;
    }
    // CTRL1=0x60 (address auto-increment + little-endian), CTRL2=0x25 (8G, 250Hz), CTRL7=0x01 (enable accel)
    qmiWriteReg(s_qmi_addr, 0x02, 0x60);
    qmiWriteReg(s_qmi_addr, 0x03, 0x25);
    qmiWriteReg(s_qmi_addr, 0x08, 0x01);
    Serial.printf("IMU|QMI8658 ready at 0x%02X (bump/raise-to-wake active)\n", s_qmi_addr);
}

static void pollImuAndSleepTimeout() {
    unsigned long now = millis();
    if(!s_screen_asleep && (now - s_last_activity_ms > SCREEN_TIMEOUT_MS)) {
        s_screen_asleep = true;
        setBacklightPercent(0U);
        Serial.println("SCREEN_SLEEP|timeout=45s");
    }

    if(!s_qmi_addr || (now - s_last_imu_poll_ms < 40U)) return;
    s_last_imu_poll_ms = now;

    uint8_t raw[6];
    if(!qmiReadRegs(s_qmi_addr, 0x35, raw, 6)) return;
    int16_t ax_raw = (int16_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8));
    int16_t ay_raw = (int16_t)((uint16_t)raw[2] | ((uint16_t)raw[3] << 8));
    int16_t az_raw = (int16_t)((uint16_t)raw[4] | ((uint16_t)raw[5] << 8));

    // 8G range => 4096 LSB/g
    float ax = (float)ax_raw / 4096.0f;
    float ay = (float)ay_raw / 4096.0f;
    float az = (float)az_raw / 4096.0f;
    float mag = sqrtf(ax * ax + ay * ay + az * az);
    s_mag_filtered = 0.55f * s_mag_filtered + 0.45f * mag;
    float dev = fabsf(s_mag_filtered - 1.0f);

    bool is_viewing = (az > 0.30f && fabsf(ay) < 0.85f && fabsf(ax) < 0.85f);
    bool is_down    = (az < 0.15f || fabsf(ay) > 0.70f);

    if(s_screen_asleep) {
        if(dev > 0.10f) {
            wakeScreen("bump");
        } else if(s_wrist_down && is_viewing) {
            s_wrist_down = false;
            wakeScreen("raise");
        }
    } else {
        if(dev > 0.16f) {
            s_last_activity_ms = now;
        }
        if(is_down) s_wrist_down = true;
        else if(is_viewing) s_wrist_down = false;
    }
}

static void renderDisplay();

static void triggerNfcBlePairingAndStartStream() {
    if(!compState.has_scan) {
        snprintf(s_ble_status, sizeof(s_ble_status), "Scan sensor via NFC first");
        showPopup(
            "NFC Scan Required",
            "1. Scan your Libre 2 sensor via NFC first",
            "   to read its UID, patch info & cal data.",
            "2. Then tap PAIR BLE to bond & stream 1m.",
            4500U);
        return;
    }

    uint8_t plain[LC_FRAM_SIZE];
    lc_integrity_transform(compState.capture.uid, compState.capture.patch, compState.capture.fram, plain);
    s_ble_cal_valid = lc_vendor_config_from_fram(
        plain, LC_FACTORY_DIABLE_T2, LC_FACTORY_DIABLE_T1, LC_VENDOR_SLOPE_TABLE, &s_ble_cal_cfg);

    snprintf(s_ble_status, sizeof(s_ble_status), "Sending NFC 0x1E unlock...");
    renderDisplay();

    LlNfcHal hal = { .transceive = uartPassthroughTransceive, .ctx = &Serial1 };
    uint8_t mac_be[6] = {0};
    s_ble_unlock_count = 0U;
    if(ll_libre2_nfc_enable_streaming(&hal, compState.capture.uid, compState.capture.patch, s_ble_unlock_code, mac_be)) {
        memcpy(s_ble_target_mac, mac_be, 6U);
        s_ble_has_mac = true;
    }

    if(!s_ble_stack_inited) {
        snprintf(s_ble_status, sizeof(s_ble_status), "Starting BLE Central...");
        renderDisplay();
        BLEDevice::init("LibreLibre-S3");
        BLEScan* pScan = BLEDevice::getScan();
        pScan->setAdvertisedDeviceCallbacks(new Libre2AdvertisedCallbacks(), true);
        pScan->setActiveScan(true);
        pScan->setInterval(100);
        pScan->setWindow(100);
        s_ble_stack_inited = true;
    }
    s_ble_enabled = true;
    s_ble_last_scan_ms = millis();
    if(s_ble_has_mac) {
        snprintf(s_ble_status, sizeof(s_ble_status), "Paired MAC %02X:%02X:%02X:%02X:%02X:%02X",
                 s_ble_target_mac[0], s_ble_target_mac[1], s_ble_target_mac[2],
                 s_ble_target_mac[3], s_ble_target_mac[4], s_ble_target_mac[5]);
        showPopup(
            "BLE Pairing Sent",
            "NFC 0x1E unlock succeeded.",
            "Connecting to sensor over BLE (0xFDE3)",
            "for 1-minute streaming...",
            3000U);
    } else {
        snprintf(s_ble_status, sizeof(s_ble_status), "Listening for ABB/FDE3 BLE...");
        showPopup(
            "BLE Scanner Active",
            "Listening for sensor BLE advertisement.",
            "Hold sensor near NFC coil during PAIR",
            "if switching bond from phone.",
            3500U);
    }
}

static void stopDirectBleStream() {
    s_ble_enabled = false;
    s_ble_do_connect = false;
    if(s_ble_client && s_ble_connected) {
        s_ble_client->disconnect();
    }
    s_ble_connected = false;
    snprintf(s_ble_status, sizeof(s_ble_status), "Standby (NFC Read-Only)");
}

static void renderBlePage() {
    s_canvas->fillRoundRect(12, 42, 296, 154, 8, COL_CARD_BG);
    s_canvas->setTextSize(2);
    s_canvas->setTextColor(COL_CYAN);
    s_canvas->setCursor(24, 52);
    s_canvas->print("Direct Sensor BLE");

    s_canvas->setTextSize(1);
    s_canvas->setTextColor(s_ble_connected ? COL_GREEN : (s_ble_enabled ? COL_AMBER : COL_MUTED));
    s_canvas->setCursor(24, 78);
    s_canvas->print("State: ");
    s_canvas->print(s_ble_status);

    s_canvas->setTextColor(COL_WHITE);
    s_canvas->setCursor(24, 96);
    s_canvas->print("Target S/N: ");
    s_canvas->print((compState.has_scan && compState.serial_valid) ? compState.serial : "none (scan NFC first)");

    s_canvas->setCursor(24, 114);
    if(s_ble_has_mac) {
        char macLine[40];
        snprintf(macLine, sizeof(macLine), "Sensor MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                 s_ble_target_mac[0], s_ble_target_mac[1], s_ble_target_mac[2],
                 s_ble_target_mac[3], s_ble_target_mac[4], s_ble_target_mac[5]);
        s_canvas->print(macLine);
    } else {
        s_canvas->print("Sensor MAC: awaiting 0xA1 0x1E NFC unlock");
    }

    s_canvas->setCursor(24, 132);
    char pktLine[44];
    snprintf(pktLine, sizeof(pktLine), "1-min BLE packets decoded: %lu", (unsigned long)s_ble_packets_ok);
    s_canvas->print(pktLine);

    s_canvas->setTextColor(COL_AMBER);
    s_canvas->setCursor(24, 152);
    s_canvas->print("Note: Direct BLE bonds sensor to ESP32");
    s_canvas->setCursor(24, 166);
    s_canvas->print("(replaces phone BLE until phone rescans)");

    // Bottom buttons: Left = PAIR / STOP BLE, Right = NEXT PAGE
    s_canvas->fillRoundRect(12, 206, 144, 28, 6, s_ble_enabled ? COL_RED : COL_HEADER_BG);
    s_canvas->setTextColor(COL_WHITE);
    s_canvas->setCursor(20, 216);
    s_canvas->print(s_ble_enabled ? "STOP BLE (tap left)" : "PAIR BLE (tap left)");

    s_canvas->fillRoundRect(164, 206, 144, 28, 6, COL_HEADER_BG);
    s_canvas->setTextColor(COL_WHITE);
    s_canvas->setCursor(176, 216);
    s_canvas->print("MAIN PAGE (tap right)");
}

// ─── 320x240 COLOR HUD RENDERING ──────────────────────────
static void renderDisplayBand() {
    if(!s_canvas) return;
    s_canvas->fillScreen(COL_BG);
    s_canvas->setTextWrap(false);

    // Top header bar (320x30)
    s_canvas->fillRect(0, 0, LCD_W, 30, COL_HEADER_BG);
    s_canvas->setTextSize(2);
    s_canvas->setTextColor(COL_CYAN);
    s_canvas->setCursor(10, 7);
    s_canvas->print("LibreLibre S3");

    if(currentPage == PAGE_BLE) {
        s_canvas->setTextSize(1);
        s_canvas->setTextColor(COL_WHITE);
        s_canvas->setCursor(220, 11);
        s_canvas->print("Page 5/5");
        renderBlePage();
        drawPopupOverlay();
        flushCanvas();
        return;
    }
    if(currentPage == PAGE_GRAPH) {
        s_canvas->setTextSize(1);
        s_canvas->setTextColor(COL_WHITE);
        char cntBuf[24];
        snprintf(cntBuf, sizeof(cntBuf), "%u pts | Page 2/5", (unsigned)s_history.count);
        s_canvas->setCursor(206, 11);
        s_canvas->print(cntBuf);
        renderGraphPage();
        drawPopupOverlay();
        flushCanvas();
        return;
    }

    if(!compState.has_scan) {
        s_canvas->setTextSize(1);
        s_canvas->setTextColor(COL_MUTED);
        s_canvas->setCursor(196, 11);
        s_canvas->print("Offline (Radios OFF)");

        s_canvas->fillRoundRect(12, 44, 296, 152, 8, COL_CARD_BG);
        if(currentPage == PAGE_MAIN) {
            s_canvas->setTextSize(2);
            s_canvas->setTextColor(COL_WHITE);
            s_canvas->setCursor(28, 62);
            s_canvas->print("Waiting for scan...");

            s_canvas->setTextSize(1);
            s_canvas->setTextColor(COL_MUTED);
            s_canvas->setCursor(28, 98);
            s_canvas->print("UART1: RX=GPIO16  TX=GPIO17 (115200)");
            s_canvas->setCursor(28, 116);
            s_canvas->print("USB-C Serial & UART1 active (Air-gapped)");
            s_canvas->setCursor(28, 134);
            s_canvas->print(saveStatus);
            s_canvas->setTextColor(COL_CYAN);
            s_canvas->setCursor(28, 166);
            s_canvas->print("Page 1/5: Tap screen or BOOT -> Graph");
        } else if(currentPage == PAGE_DIAG) {
            s_canvas->setTextSize(2);
            s_canvas->setTextColor(COL_AMBER);
            s_canvas->setCursor(28, 60);
            s_canvas->print("Sensor Diagnostics");

            s_canvas->setTextSize(2);
            s_canvas->setTextColor(COL_MUTED);
            s_canvas->setCursor(28, 94);
            s_canvas->print("No scan captured yet");

            s_canvas->setTextSize(1);
            s_canvas->setCursor(28, 130);
            s_canvas->print("Scan a Libre 2 sensor to view CRC,");
            s_canvas->setCursor(28, 146);
            s_canvas->print("sensor state, age, and quality flags.");
            s_canvas->setTextColor(COL_CYAN);
            s_canvas->setCursor(28, 166);
            s_canvas->print("Page 3/5: Tap screen or BOOT -> UID");
        } else {
            s_canvas->setTextSize(2);
            s_canvas->setTextColor(COL_CYAN);
            s_canvas->setCursor(28, 60);
            s_canvas->print("UID & SD Storage");

            s_canvas->setTextSize(2);
            s_canvas->setTextColor(COL_MUTED);
            s_canvas->setCursor(28, 94);
            s_canvas->print("UID: --");

            s_canvas->setTextSize(1);
            s_canvas->setTextColor(COL_WHITE);
            s_canvas->setCursor(28, 130);
            s_canvas->print(saveStatus);
            s_canvas->setTextColor(COL_CYAN);
            s_canvas->setCursor(28, 166);
            s_canvas->print("Page 4/5: Tap screen or BOOT -> BLE");
        }
        drawPopupOverlay();
        flushCanvas();
        return;
    }

    uint32_t elapsedSec = (millis() - lastScanMillis) / 1000U;
    char hdrRight[32];
    if(elapsedSec < 60U) {
        snprintf(hdrRight, sizeof(hdrRight), "%s | %lus", lastTransport, (unsigned long)elapsedSec);
    } else {
        snprintf(hdrRight, sizeof(hdrRight), "%s | %lum", lastTransport, (unsigned long)(elapsedSec / 60U));
    }
    s_canvas->setTextSize(1);
    s_canvas->setTextColor(COL_WHITE);
    s_canvas->setCursor(220, 11);
    s_canvas->print(hdrRight);

    if(currentPage == PAGE_MAIN) {
        s_canvas->fillRoundRect(12, 40, 296, 92, 8, COL_CARD_BG);
        if(compState.vendor_chain.available) {
            s_canvas->setTextSize(4);
            s_canvas->setTextColor(COL_GREEN);
            s_canvas->setCursor(24, 56);
            s_canvas->print(compState.primary_line);

            char mgLine[32];
            snprintf(mgLine, sizeof(mgLine), "(%lu mg/dL)", (unsigned long)compState.vendor_chain.mg_dl);
            s_canvas->setTextSize(1);
            s_canvas->setTextColor(COL_MUTED);
            s_canvas->setCursor(28, 106);
            s_canvas->print(mgLine);
        } else {
            s_canvas->setTextSize(3);
            s_canvas->setTextColor(COL_RED);
            s_canvas->setCursor(24, 54);
            s_canvas->print("Rescan sensor");

            s_canvas->setTextSize(1);
            s_canvas->setTextColor(COL_AMBER);
            s_canvas->setCursor(24, 96);
            s_canvas->print(compState.diag_line1);
            s_canvas->setCursor(24, 110);
            s_canvas->print("No usable value");
        }

        s_canvas->setTextSize(2);
        s_canvas->setTextColor(COL_WHITE);
        s_canvas->setCursor(16, 144);
        s_canvas->print(compState.vendor_chain.available ? compState.fresh_life_line : "Freshness unknown");

        s_canvas->setTextSize(2);
        s_canvas->setTextColor(COL_CYAN);
        s_canvas->setCursor(16, 170);
        s_canvas->print(compState.serial_line);

        s_canvas->setTextSize(1);
        s_canvas->setTextColor(strstr(saveStatus, "Saved") ? COL_GREEN : COL_AMBER);
        s_canvas->setCursor(16, 198);
        s_canvas->print(saveStatus);

        s_canvas->setTextColor(COL_MUTED);
        s_canvas->setCursor(16, 220);
        s_canvas->print("Page 1/5 (Tap -> Graph)          Scan #");
        s_canvas->print((unsigned long)scanCount);
    } else if(currentPage == PAGE_DIAG) {
        s_canvas->fillRoundRect(12, 42, 296, 160, 8, COL_CARD_BG);
        s_canvas->setTextSize(2);
        s_canvas->setTextColor(COL_AMBER);
        s_canvas->setCursor(24, 54);
        s_canvas->print("Sensor Diagnostics");

        s_canvas->setTextSize(2);
        s_canvas->setTextColor(COL_WHITE);
        s_canvas->setCursor(24, 84);
        s_canvas->print(compState.diag_line1);
        s_canvas->setCursor(24, 110);
        s_canvas->print(compState.diag_line2);
        s_canvas->setCursor(24, 136);
        s_canvas->print(compState.diag_line3);

        s_canvas->setTextSize(1);
        s_canvas->setTextColor(COL_MUTED);
        s_canvas->setCursor(24, 170);
        s_canvas->print(saveStatus);
        s_canvas->setCursor(16, 220);
        s_canvas->print("Page 3/5: Tap / BOOT -> Full Wire UID");
    } else {
        s_canvas->fillRoundRect(12, 42, 296, 160, 8, COL_CARD_BG);
        s_canvas->setTextSize(2);
        s_canvas->setTextColor(COL_CYAN);
        s_canvas->setCursor(24, 54);
        s_canvas->print("UID wire LSB-first");

        s_canvas->setTextSize(2);
        s_canvas->setTextColor(COL_WHITE);
        s_canvas->setCursor(24, 88);
        s_canvas->print(compState.uid_hex);

        s_canvas->setTextColor(COL_GREEN);
        s_canvas->setCursor(24, 120);
        s_canvas->print(compState.serial_line);

        s_canvas->setTextSize(1);
        s_canvas->setTextColor(COL_MUTED);
        s_canvas->setCursor(24, 154);
        s_canvas->print("Serial derived from UID + family nibble");
        if(savedFileName[0]) {
            s_canvas->setCursor(24, 172);
            s_canvas->print(savedFileName);
        }
        s_canvas->setCursor(16, 220);
        s_canvas->print("Page 4/5: Tap / BOOT -> Direct BLE");
    }

    drawPopupOverlay();
    flushCanvas();
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
    Serial1.print(reply);
}

static int uartPassthroughTransceive(
    void* ctx,
    const uint8_t* tx_buf,
    size_t tx_len,
    uint8_t* rx_buf,
    size_t rx_cap,
    size_t* rx_len_out) {
    Stream* io = (Stream*)ctx;
    char cmdHex[16];
    if(!lc_hex(tx_buf, tx_len, cmdHex, sizeof(cmdHex))) return -1;
    while(io->available()) io->read();
    io->print("TX|");
    io->print(cmdHex);
    io->print("\n");

    char line[128];
    size_t pos = 0;
    unsigned long t0 = millis();
    while(millis() - t0 < 350U) {
        while(io->available()) {
            int ch = io->read();
            if(ch == '\n' || ch == '\r') {
                if(pos > 0) {
                    line[pos] = 0;
                    if(strncmp(line, "RX|0|", 5) == 0) {
                        const char* hex = line + 5;
                        size_t hlen = strlen(hex);
                        if((hlen % 2U) != 0U || (hlen / 2U) > rx_cap) return 2;
                        for(size_t i = 0; i < hlen; i += 2U) {
                            char c1 = hex[i], c2 = hex[i + 1U];
                            int v1 = (c1 >= '0' && c1 <= '9') ? (c1 - '0') : (c1 >= 'A' && c1 <= 'F') ? (c1 - 'A' + 10) : -1;
                            int v2 = (c2 >= '0' && c2 <= '9') ? (c2 - '0') : (c2 >= 'A' && c2 <= 'F') ? (c2 - 'A' + 10) : -1;
                            if(v1 < 0 || v2 < 0) return 2;
                            rx_buf[i / 2U] = (uint8_t)((v1 << 4) | v2);
                        }
                        *rx_len_out = hlen / 2U;
                        return 0;
                    }
                    return 3;
                }
            } else if(pos + 1U < sizeof(line)) {
                line[pos++] = (char)ch;
            }
        }
        delay(1);
    }
    return 1;
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
                    bool isNew = ll_history_add(
                        &s_history,
                        compState.serial,
                        fseries.points[k].sensor_minute,
                        fseries.points[k].mmol_tenths,
                        fseries.points[k].mg_dl,
                        bSec);
                    if(isNew && s_history.count > 0U) {
                        appendHistoryToSd(&s_history.points[s_history.count - 1U]);
                    }
                }
            }
        }
    }
    uint32_t bootSec = (uint32_t)(millis() / 1000U);
    compState.capture.timestamp = bootSec;
    saveCaptureToSd(&compState.capture);
    saveLastFrameToSd(uid, patch, fram);

    if(compState.vendor_chain.available && compState.serial_valid) {
        bool isNewPoint = ll_history_add(
            &s_history,
            compState.serial,
            compState.integrity.sensor.age_minutes,
            (uint16_t)compState.vendor_chain.mmol_tenths,
            (uint16_t)compState.vendor_chain.mg_dl,
            bootSec);
        if(isNewPoint && s_history.count > 0U) {
            appendHistoryToSd(&s_history.points[s_history.count - 1U]);
        }
    }

    lastScanMillis = millis();
    scanCount++;
    snprintf(lastTransport, sizeof(lastTransport), "%s", source);
    wakeScreen("scan");
    currentPage = PAGE_MAIN;
    renderDisplay();
    notifyResultSummary(source);
}

static void runMasterNfcScan(Stream& rfStream, const char* sourceName) {
    LlNfcHal hal = { .transceive = uartPassthroughTransceive, .ctx = &rfStream };
    static LcCapture cap;
    LlReaderStatus st = ll_nfc_acquire_sensor(&hal, (uint32_t)(millis() / 1000U), &cap);
    if(st == LlReaderOk) {
        processCompletedScan(cap.uid, cap.patch, cap.fram, sourceName);
    } else if(st != LlReaderNoTag) {
        char err[64];
        snprintf(err, sizeof(err), "SCAN_ERR|src=%s|status=%d\n", sourceName, (int)st);
        Serial.print(err);
    }
}

static void handleIncomingLine(const char* line, const char* source) {
    if(strncmp(line, "LL2_BLE|", 8) == 0) {
        // Format: LL2_BLE|<16-hex-uid>|<12-hex-patch>|<12-hex-mac>|<unlock_code>
        char buf[96];
        snprintf(buf, sizeof(buf), "%s", line + 8);
        char* t_uid = strtok(buf, "|\r\n");
        char* t_pat = strtok(NULL, "|\r\n");
        char* t_mac = strtok(NULL, "|\r\n");
        char* t_cod = strtok(NULL, "|\r\n");
        if(t_uid && t_pat && t_mac && strlen(t_mac) == 12U) {
            for(size_t i = 0; i < 6U; i++) {
                char c1 = t_mac[i * 2U], c2 = t_mac[i * 2U + 1U];
                int v1 = (c1 >= '0' && c1 <= '9') ? (c1 - '0') : (c1 >= 'A' && c1 <= 'F') ? (c1 - 'A' + 10) : (c1 >= 'a' && c1 <= 'f') ? (c1 - 'a' + 10) : 0;
                int v2 = (c2 >= '0' && c2 <= '9') ? (c2 - '0') : (c2 >= 'A' && c2 <= 'F') ? (c2 - 'A' + 10) : (c2 >= 'a' && c2 <= 'f') ? (c2 - 'a' + 10) : 0;
                s_ble_target_mac[i] = (uint8_t)((v1 << 4) | v2);
            }
            s_ble_has_mac = true;
            s_ble_unlock_code = t_cod ? (uint32_t)strtoul(t_cod, NULL, 10) : 42U;
            s_ble_unlock_count = 0U;
            if(!s_ble_stack_inited) {
                BLEDevice::init("LibreLibre-S3");
                BLEScan* pScan = BLEDevice::getScan();
                pScan->setAdvertisedDeviceCallbacks(new Libre2AdvertisedCallbacks(), true);
                pScan->setActiveScan(true);
                pScan->setInterval(100);
                pScan->setWindow(100);
                s_ble_stack_inited = true;
            }
            s_ble_enabled = true;
            snprintf(s_ble_status, sizeof(s_ble_status), "Paired MAC %02X:%02X:%02X:%02X:%02X:%02X",
                     s_ble_target_mac[0], s_ble_target_mac[1], s_ble_target_mac[2],
                     s_ble_target_mac[3], s_ble_target_mac[4], s_ble_target_mac[5]);
            showPopup("BLE Pair Received",
                      "Received sensor MAC & unlock key.",
                      s_ble_status,
                      "Connecting to sensor over BLE...", 3500U);
            renderDisplay();
            saveBleBondToSd();
            Serial.println("OK|LL2_BLE_ARMED");
        }
        return;
    }
    if(strcmp(line, "BLE_PAIR") == 0 || strcmp(line, "ble_pair") == 0) {
        triggerNfcBlePairingAndStartStream();
        currentPage = PAGE_BLE;
        renderDisplay();
        Serial.println("OK|BLE_PAIR_STARTED");
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
    if(strcmp(line, "SCAN") == 0 || strcmp(line, "scan") == 0) {
        runMasterNfcScan(Serial1, "RF-M");
        return;
    }
    if(strcmp(line, "PING") == 0 || strcmp(line, "ping") == 0) {
        Serial.println("PONG|LibreLibre-ESP32-WaveshareS3");
        Serial1.println("PONG|LibreLibre-ESP32-WaveshareS3");
        return;
    }
    static uint8_t uid[8], patch[6], fram[LC_FRAM_SIZE];
    LlBridgeParseStatus st = ll_bridge_parse_line(line, uid, patch, fram);
    if(st == LlBridgeOk) {
        processCompletedScan(uid, patch, fram, source);
    } else if(st != LlBridgeBadPrefix) {
        char err[64];
        snprintf(err, sizeof(err), "ERR|src=%s|code=%d\n", source, (int)st);
        Serial.print(err);
        Serial1.print(err);
    }
}

static void pollStream(Stream& stream, char* buf, size_t capacity, size_t* len, const char* source) {
    while(stream.available() > 0) {
        int ch = stream.read();
        if(ch < 0) break;
        if(ch == '\n' || ch == '\r') {
            if(*len > 0) {
                buf[*len] = 0;
                handleIncomingLine(buf, source);
                *len = 0;
            }
        } else if(*len + 1U < capacity) {
            buf[(*len)++] = (char)ch;
        } else {
            *len = 0;
        }
    }
}

void setup() {
    pinMode(WS_BOOT_BTN, INPUT_PULLUP);
    Serial.begin(115200);
    Serial1.begin(FLIPPER_BAUD, SERIAL_8N1, FLIPPER_UART_RX, FLIPPER_UART_TX);

    memset(&compState, 0, sizeof(compState));
    ll_history_init(&s_history);
    initLcdAndSpi2();
    initMicroSd();
    initTouchCst816();
    initQmi8658Imu();
    s_last_activity_ms = millis();
    if(s_sd_mounted) {
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
                    snprintf(lastTransport, sizeof(lastTransport), "SD");
                    snprintf(saveStatus, sizeof(saveStatus), "Restored from SD (%u pts)", (unsigned)s_history.count);
                }
            }
            fclose(lf);
        }
        FILE* bf = fopen(BLE_BOND_PATH, "rb");
        if(bf) {
            if(fgets(rline, sizeof(rline), bf)) {
                size_t l = strlen(rline);
                while(l > 0 && (rline[l - 1] == '\n' || rline[l - 1] == '\r')) rline[--l] = 0;
                handleIncomingLine(rline, "SD");
            }
            fclose(bf);
        }
    }
    renderDisplay();

    Serial.println("READY|LibreLibre-ESP32-WaveshareS3|LCD=320x240|SD=SPI2_CS41|UART_RX=16|UART_TX=17");
}

void loop() {
    pollStream(Serial, usbBuf, sizeof(usbBuf), &usbLen, "USB");
    pollStream(Serial1, uartBuf, sizeof(uartBuf), &uartLen, "UART");

    bool bootPressed = (digitalRead(WS_BOOT_BTN) == LOW);
    uint16_t tapX = 160, tapY = 120;
    bool tapped = pollTouchTap(&tapX, &tapY);

    if(s_popup_active && (int32_t)(millis() - s_popup_until_ms) >= 0) {
        s_popup_active = false;
        renderDisplay();
    }

    pollImuAndSleepTimeout();

    if(tapped && (millis() - lastNavMs > 220U)) {
        lastNavMs = millis();
        s_last_activity_ms = millis();
        if(s_screen_asleep) {
            wakeScreen("tap");
        } else if(s_popup_active) {
            s_popup_active = false;
            renderDisplay();
        } else if(currentPage == PAGE_GRAPH) {
            // Top-right zoom pills (y < 54, x >= 136): jump directly to tapped zoom window
            if(tapY < 54U && tapX >= 136U) {
                size_t idx = (tapX - 136U) / 36U;
                if(idx < 5U) s_zoom_idx = idx;
                if(s_history.count == 0U) {
                    showPopup("No Scans Logged Yet",
                              "Zoom window set, but 0 scans are stored.",
                              "Scan a Libre 2 sensor first to plot",
                              "readings on this graph.", 3000U);
                }
                renderDisplay();
            } else if(tapX < 160U) {
                s_zoom_idx = (s_zoom_idx + 1U) % 5U;
                if(s_history.count == 0U) {
                    showPopup("No Scans Logged Yet",
                              "Zoom window set, but 0 scans are stored.",
                              "Scan a Libre 2 sensor first to plot",
                              "readings on this graph.", 3000U);
                }
                renderDisplay();
            } else {
                // Right half advances to next page
                currentPage = (ScreenPage)(((int)currentPage + 1) % PAGE_COUNT);
                renderDisplay();
            }
        } else if(currentPage == PAGE_BLE) {
            if(tapX < 160U) {
                if(s_ble_enabled) stopDirectBleStream();
                else triggerNfcBlePairingAndStartStream();
                renderDisplay();
            } else {
                currentPage = PAGE_MAIN;
                renderDisplay();
            }
        } else {
            currentPage = (ScreenPage)(((int)currentPage + 1) % PAGE_COUNT);
            renderDisplay();
        }
    } else if(bootPressed && !bootPrev && (millis() - lastNavMs > 220U)) {
        lastNavMs = millis();
        s_last_activity_ms = millis();
        if(s_screen_asleep) {
            wakeScreen("boot_btn");
        } else {
            currentPage = (ScreenPage)(((int)currentPage + 1) % PAGE_COUNT);
            renderDisplay();
        }
    }
    bootPrev = bootPressed;

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
                appendHistoryToSd(&s_history.points[s_history.count - 1U]);
            }
            lastScanMillis = millis();
            scanCount++;
            snprintf(lastTransport, sizeof(lastTransport), "BLE-S");
            wakeScreen("ble_stream");
            renderDisplay();
            notifyResultSummary("BLE-S");
        }
    }

    if(s_ble_enabled) {
        if(s_ble_do_connect) {
            s_ble_do_connect = false;
            connectAndLoginLibre2Ble();
            renderDisplay();
        } else if(!s_ble_connected && !s_ble_scanning && compState.has_scan && (millis() - s_ble_last_scan_ms > 1000U)) {
            s_ble_last_scan_ms = millis();
            s_ble_scanning = true;
            BLEScan* pScan = BLEDevice::getScan();
            pScan->clearResults();
            if(!pScan->start(5, [](BLEScanResults results) {
                (void)results;
                s_ble_scanning = false;
                Serial.printf("BLE_SCAN_DONE|heap=%u\n", (unsigned)ESP.getFreeHeap());
            }, false)) {
                s_ble_scanning = false;
            }
        }
    }

    static unsigned long lastRefresh = 0;
    if(!s_screen_asleep && compState.has_scan && (millis() - lastRefresh >= 10000U)) {
        lastRefresh = millis();
        renderDisplay();
    }

    delay(5);
}
