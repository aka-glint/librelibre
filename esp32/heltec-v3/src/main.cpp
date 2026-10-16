/*
 * LibreLibre ESP32 Companion — Heltec WiFi LoRa 32 V3 (ESP32-S3)
 *
 * Receives raw 344-byte EU Libre 2 NFC captures over GPIO UART (from Flipper Zero
 * pins 13/14) or USB Serial (CP2102) using the CRC-checked LL2|... frame format,
 * Strict offline / air-gapped build: WiFi and BLE radios are NOT initialized.
 *
 * Runs the exact same clean-room C calibration, integrity, sensor-status, and
 * lifecycle pipeline as the Flipper Zero app (vendor_chain.c, integrity.c,
 * factory.c, lifecycle.c, protocol.c) and renders the result on the Heltec V3
 * 128x64 SSD1306 OLED.
 *
 * Verified Heltec V3 Pin Map:
 *   OLED I2C: SDA=17, SCL=18, RST=21, Addr=0x3C (SSD1306 128x64)
 *   Vext (OLED power): GPIO 36 -> pull LOW before OLED init
 *   Built-in white LED: GPIO 35
 *   PRG / USER button: GPIO 0 (active LOW; cycles Result -> Diagnostics -> UID)
 *   Flipper GPIO UART (Serial1): RX = GPIO 47 (to Flipper Pin 13 TX),
 *                                TX = GPIO 48 (to Flipper Pin 14 RX), 115200 8N1
 *
 * License: MIT
 */

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "bridge.h"
#include "nfc_io.h"

// ─── HELTEC V3 HARDWARE PINS ──────────────────────────────
#define OLED_SDA       17
#define OLED_SCL       18
#define OLED_RST       21
#define OLED_ADDR      0x3C
#define PIN_VEXT       36
#define PIN_LED        35
#define PIN_PRG        0

// Safe free GPIOs on Heltec V3 for Flipper Zero GPIO UART connection:
// Connect Flipper Pin 13 (TX) -> Heltec GPIO 47 (RX)
// Connect Flipper Pin 14 (RX) -> Heltec GPIO 48 (TX)
// Connect Flipper Pin 8 (GND) -> Heltec GND
#define FLIPPER_UART_RX 47
#define FLIPPER_UART_TX 48
#define FLIPPER_BAUD    115200


Adafruit_SSD1306 display(128, 64, &Wire, OLED_RST);

enum ScreenPage { PAGE_MAIN = 0, PAGE_DIAG = 1, PAGE_UID = 2 };
static ScreenPage currentPage = PAGE_MAIN;
static LlCompanionState compState;
static uint32_t lastScanMillis = 0;
static uint32_t scanCount = 0;
static char lastTransport[16] = "none";

// Line accumulators for USB Serial, Flipper GPIO Serial1, and BLE NUS
static char usbBuf[LL_BRIDGE_LINE_MAX];
static size_t usbLen = 0;

static char uartBuf[LL_BRIDGE_LINE_MAX];
static size_t uartLen = 0;


// Button debounce
static bool prgPrev = false;
static unsigned long lastPrgMs = 0;

static void renderDisplay() {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextWrap(false);

    if(!compState.has_scan) {
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.println("LibreLibre ESP32");
        display.setCursor(0, 14);
        display.println("Waiting for scan...");
        display.setCursor(0, 28);
        display.println("UART: RX=47 TX=48");
        display.setCursor(0, 40);
        display.println("Radios: OFF (offline)");
        display.setCursor(0, 54);
        display.println("USB / UART ready");
        display.display();
        return;
    }

    uint32_t elapsedSec = (millis() - lastScanMillis) / 1000U;
    char headerRight[16];
    if(elapsedSec < 60U) {
        snprintf(headerRight, sizeof(headerRight), "%lus", (unsigned long)elapsedSec);
    } else {
        snprintf(headerRight, sizeof(headerRight), "%lum", (unsigned long)(elapsedSec / 60U));
    }

    if(currentPage == PAGE_MAIN) {
        // Top right: time since scan received + transport tag
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.print(lastTransport);
        display.setCursor(96, 0);
        display.print(headerRight);

        // Prominent glucose reading (or Rescan)
        if(compState.vendor_chain.available) {
            display.setTextSize(2);
            display.setCursor(0, 12);
            display.println(compState.primary_line);
        } else {
            display.setTextSize(1);
            display.setCursor(0, 14);
            display.println(compState.diag_line1);
            display.setCursor(0, 24);
            display.println("No usable value");
        }

        display.setTextSize(1);
        display.setCursor(0, 33);
        if(compState.vendor_chain.available) {
            const char* fl = compState.fresh_life_line;
            if(strncmp(fl, "Fresh ", 6) == 0) fl += 6; // "0m=now; Left 12d 18h" (20 chars = 120px <= 128px)
            display.println(fl);
        } else {
            display.println("Rescan the sensor");
        }
        display.setCursor(0, 44);
        display.println(compState.serial_line);
        display.setCursor(0, 54);
        display.print("PRG:info  #");
        display.print((unsigned long)scanCount);
    } else if(currentPage == PAGE_DIAG) {
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.println("Diagnostics");
        display.setCursor(0, 12);
        display.println(compState.diag_line1);
        display.setCursor(0, 22);
        display.println(compState.diag_line2);
        display.setCursor(0, 32);
        display.println(compState.diag_line3);
        display.setCursor(0, 44);
        display.println(compState.serial_line);
        display.setCursor(0, 54);
        display.println("PRG: next -> UID");
    } else {
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.println("UID wire LSB-first");
        display.setCursor(0, 14);
        display.println(compState.uid_hex);
        display.setCursor(0, 28);
        display.println(compState.serial_line);
        display.setCursor(0, 40);
        display.println("Serial derived UID");
        display.setCursor(0, 54);
        display.println("PRG: next -> Main");
    }

    display.display();
}

static void notifyResultSummary(const char* source) {
    char reply[192];
    if(compState.vendor_chain.available) {
        snprintf(reply, sizeof(reply),
                 "OK|src=%s|mmol=%lu.%lu|mgdl=%lu|serial=%s|age=%u|life=%u\n",
                 source,
                 (unsigned long)(compState.vendor_chain.mmol_tenths / 10U),
                 (unsigned long)(compState.vendor_chain.mmol_tenths % 10U),
                 (unsigned long)compState.vendor_chain.mg_dl,
                 compState.serial_valid ? compState.serial : "NA",
                 compState.integrity.sensor.age_minutes,
                 compState.integrity.sensor.max_life_minutes);
    } else {
        snprintf(reply, sizeof(reply),
                 "RESCAN|src=%s|diag=%s|serial=%s|age=%u|life=%u\n",
                 source,
                 compState.diag_line1,
                 compState.serial_valid ? compState.serial : "NA",
                 compState.integrity.sensor.age_minutes,
                 compState.integrity.sensor.max_life_minutes);
    }
    Serial.print(reply);
    Serial1.print(reply);
}


// ─── ISO15693 RAW TRANSCEIVE BACKEND (UART PASSTHROUGH / FUTURE SPI NFC) ──
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
    return 1; // timeout
}

static void runMasterNfcScan(Stream& rfStream, const char* sourceName) {
    LlNfcHal hal = { .transceive = uartPassthroughTransceive, .ctx = &rfStream };
    static LcCapture cap;
    LlReaderStatus st = ll_nfc_acquire_sensor(&hal, (uint32_t)(millis() / 1000U), &cap);
    if(st == LlReaderOk) {
        digitalWrite(PIN_LED, HIGH);
        ll_companion_evaluate(cap.uid, cap.patch, cap.fram, &compState);
        lastScanMillis = millis();
        scanCount++;
        snprintf(lastTransport, sizeof(lastTransport), "%s", sourceName);
        currentPage = PAGE_MAIN;
        renderDisplay();
        notifyResultSummary(sourceName);
        digitalWrite(PIN_LED, LOW);
    } else if(st != LlReaderNoTag) {
        char err[64];
        snprintf(err, sizeof(err), "SCAN_ERR|src=%s|status=%d\n", sourceName, (int)st);
        Serial.print(err);
    }
}

static void handleIncomingLine(const char* line, const char* source) {
    if( strcmp(line, "SCAN") == 0 || strcmp(line, "scan") == 0 ) {
        runMasterNfcScan(Serial1, "RF-M");
        return;
    }
    if( strcmp(line, "PING") == 0 || strcmp(line, "ping") == 0 ) {
        Serial.println("PONG|LibreLibre-ESP32-HeltecV3");
        Serial1.println("PONG|LibreLibre-ESP32-HeltecV3");
        return;
    }
    static uint8_t uid[8], patch[6], fram[LC_FRAM_SIZE];
    LlBridgeParseStatus st = ll_bridge_parse_line(line, uid, patch, fram);
    if(st == LlBridgeOk) {
        digitalWrite(PIN_LED, HIGH);
        ll_companion_evaluate(uid, patch, fram, &compState);
        lastScanMillis = millis();
        scanCount++;
        snprintf(lastTransport, sizeof(lastTransport), "%s", source);
        currentPage = PAGE_MAIN;
        renderDisplay();
        notifyResultSummary(source);
        digitalWrite(PIN_LED, LOW);
    } else if(st != LlBridgeBadPrefix) {
        char err[64];
        snprintf(err, sizeof(err), "ERR|src=%s|code=%d\n", source, (int)st);
        Serial.print(err);
        Serial1.print(err);
    }
}

static void initHeltecDisplay() {
    // Heltec V3 critical init order: Wire(17,18) -> Vext(36) LOW -> RST(21) -> begin(0x3C)
    Wire.begin(OLED_SDA, OLED_SCL);
    pinMode(PIN_VEXT, OUTPUT);
    digitalWrite(PIN_VEXT, LOW);
    delay(50);
    if(!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        Serial.println("ERR|OLED init failed");
        return;
    }
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.display();
}

void setup() {
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);
    pinMode(PIN_PRG, INPUT_PULLUP);

    Serial.begin(115200);
    Serial1.begin(FLIPPER_BAUD, SERIAL_8N1, FLIPPER_UART_RX, FLIPPER_UART_TX);

    memset(&compState, 0, sizeof(compState));
    initHeltecDisplay();
    renderDisplay();

    Serial.println("READY|LibreLibre-ESP32-HeltecV3|UART_RX=47|UART_TX=48|BAUD=115200");
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
            *len = 0; // bounded overflow reset
        }
    }
}

void loop() {
    pollStream(Serial, usbBuf, sizeof(usbBuf), &usbLen, "USB");
    pollStream(Serial1, uartBuf, sizeof(uartBuf), &uartLen, "UART");


    // PRG button cycles Main -> Diagnostics -> UID
    bool pressed = (digitalRead(PIN_PRG) == LOW);
    if(pressed && !prgPrev && (millis() - lastPrgMs > 250U)) {
        lastPrgMs = millis();
        if(compState.has_scan) {
            currentPage = (ScreenPage)(((int)currentPage + 1) % 3);
            renderDisplay();
        }
    }
    prgPrev = pressed;

    // Refresh elapsed time header every 10 seconds when a scan is displayed
    static unsigned long lastRefresh = 0;
    if(compState.has_scan && (millis() - lastRefresh >= 10000U)) {
        lastRefresh = millis();
        renderDisplay();
    }

    delay(5);
}
