# LibreLibre

Read a standard European FreeStyle Libre 2 sensor over NFC with a Flipper Zero, or stream live 1-minute Bluetooth readings and alarms straight to an ESP32-S3 watch or display. No phone, no internet connection, no account, and no cloud server in the loop.

I built this because I wanted an independent, offline reader and wrist companion I could inspect myself, use when my phone is dead or out of reach, and run without trusting a black box.

## Hardware targets in this repo

All four targets share the same C decryption, 3-section CRC16, per-sensor factory calibration, 48-point FRAM history extractor, and Libre 2 BLE decryption core (`integrity.c`, `vendor_chain.c`, `vendor_tables.c`, `factory.c`, `lifecycle.c`, `protocol.c`, `scan_history.c`):

| Target | Directory | Hardware | What it does |
|---|---|---|---|
| **Flipper Zero** | Root (`/`) + [`dist/`](dist/) | Flipper Zero (OFW, Unleashed, Momentum) | 17-command read-only NFC scan, optional Step-18 (`02 A1 07 1E`) BLE pairing (`Up` key), 2D glucose graph (`15m`/`1h`/`3h`/`8h`), SD JSON capture save, and USB/UART bridge output. |
| **CircuitMess Artemis Watch** | [`esp32/circuitmess-artemis/`](esp32/circuitmess-artemis/) | ESP32-S3-MINI-1, 128x128 ST7735S, LSM6DS3TR-C IMU, PCF8563 RTC, 6x Flight-Deck LEDs, Piezo Buzzer | 7-page dark-mode cyberpunk watch (`CP2077` HUD + Kiroshi `GLITCH FX` toggle), direct 1-minute Libre 2 BLE streaming (`0xFDE3`), flight-deck LED alerts (`18 ms` update blip, `3s` flash at `<=4.2` or `>=13.0`), latching piezo audible alarms (`<=3.6` or `>=16.0` with 15-minute snooze on button ack), raise-to-wake, and SPIFFS persistence. |
| **Waveshare S3 Touch-LCD-2** | [`esp32/waveshare-s3-lcd2/`](esp32/waveshare-s3-lcd2/) | ESP32-S3, 240x320 ST7789T3 IPS, CST816D capacitive touch, microSD | 5-page dark HUD, touch + button controls, zoomable 24h graph, direct 1-minute Libre 2 BLE streaming (`0xFDE3`), and verified microSD history/capture storage. |
| **Heltec WiFi LoRa 32 V3** | [`esp32/heltec-v3/`](esp32/heltec-v3/) | ESP32-S3, 128x64 SSD1306 OLED | 100% air-gapped offline companion display and graph node (Bluetooth stack stripped at compile time). Receives `LL2|` frames over GPIO UART or USB Serial. |

---

## 1. Flipper Zero app

When you open the Flipper app, it starts scanning right away. Hold the back of the Flipper against the sensor and it will:

1. Read the 344-byte FRAM over ISO15693 NFC (`17` read-only commands by default).
2. If you pressed **Up** before scanning (`BLE Pair: ON`), send optional Step 18 (`02 A1 07 1E`) to pair the sensor's Bluetooth to your ESP32 watch/display and read back the sensor's 6-byte BLE MAC.
3. Decrypt the payload and check all three section CRCs (header, body, footer).
4. Pull the sensor's own factory calibration values out of FRAM, calculate the current glucose reading in **mmol/L**, and extract all 48 historical + trend points (`8` hours) for the 2D graph.
5. Save the raw capture as JSON on the SD card (`/ext/apps_data/libre_research/`) and output the `LL2|` frame over GPIO UART (`Pin 13 TX / Pin 14 RX` at `115200 8N1`).

### Install on Flipper Zero

The C code uses standard Flipper SDK calls and works on Official (Stock) Firmware, Unleashed, and Momentum. Pick the `.fap` in `dist/` that matches your firmware:

| Your Flipper Firmware | File to download | Target / API |
|---|---|---|
| **Official / Stock Firmware (OFW 1.5.1)** | [`dist/LibreLibre-ofw.fap`](dist/LibreLibre-ofw.fap) | Target 7, API 88.2 |
| **Unleashed (`unlshd-094`)** | [`dist/LibreLibre-unleashed.fap`](dist/LibreLibre-unleashed.fap) | Target 7, API 88.19 |
| **Momentum (`mntm-012`)** | [`dist/LibreLibre-momentum.fap`](dist/LibreLibre-momentum.fap) | Target 7, API 87.1 |

Copy the matching file onto your Flipper's SD card as **`apps/NFC/LibreLibre.fap`**, then open **Apps -> NFC -> LibreLibre**.

**Buttons on the Flipper:**
- **Up (on scan screen)**: Toggle `BLE Pair: OFF / ON` (controls whether Step 18 `02 A1 07 1E` runs during the scan).
- **OK (on result screen)**: Open the 2D glucose graph (`15m`, `1h`, `3h`, `8h` zoom with **Up/Down**).
- **Left**: Cycle between the main reading, sensor diagnostics (`Age`, `Q` bits, CRC state), and the 16-hex wire UID + paired BLE MAC.
- **Right**: Start a new scan.

---

## 2. How Bluetooth streaming and NFC work on ESP32

A Libre 2 sensor streams a 46-byte encrypted reading over Bluetooth Low Energy (`0xFDE3`) once every 60 seconds, but it needs an initial NFC scan for two things:

1. Reading the 344-byte FRAM once so the ESP32 has that sensor's factory calibration values (`i1..i6`) and 8-hour history graph.
2. Running Step 18 (`02 A1 07 1E`) over NFC to set the sensor's Bluetooth `enable_time` and learn its 6-byte BLE MAC. Because bytes 4..5 of the sensor's `02 A1 07` `patch_info` response change on every NFC scan and are mixed into the 12-byte BLE `0xF001` login payload (`t = ((info[5] << 8) | info[4]) ^ 0x44`), the ESP32 must use the exact 6-byte `patch_info` from the scan that ran Step 18.

You can feed NFC scans into the ESP32 targets in two ways:

1. **Capture NFC on the Flipper Zero and bridge to ESP32**: Toggle `BLE Pair: ON` (`Up` key) on the Flipper, scan the sensor, and pass the `LL2|<uid>|<patch>|<fram>|<crc16>` and `LL2_BLE|<uid>|<patch>|<mac>|<code>|<cnt>` lines to the ESP32 over GPIO UART or USB Serial.
2. **Wire an ISO15693 NFC reader (`PN5180`, `ST25R3916`, or `TRF7970A`) directly to the ESP32**: All three ESP32 targets include `src/nfc_io.c`, which implements the full 17+1 command ISO15693 state machine natively on ESP32 through a single `LlNfcTransceiveFn` callback.

Full walkthrough of both setups, the BLE crypto, and the four ESP32 Bluedroid link-layer traps we solved: **[docs/BLE-AND-NFC.md](docs/BLE-AND-NFC.md)**.

---

## 3. CircuitMess Artemis Watch (`esp32/circuitmess-artemis`)

Turns the CircuitMess Artemis 2 watch (`ESP32-S3-MINI-1`, 128x128 ST7735S) into a standalone wrist receiver and alarm watch:

- **Dual-Mode Bluetooth Engine**:
  - **Direct Libre 2 BLE Client (`0xFDE3`)**: Connects directly to FreeStyle Libre 2 sensors, authenticating with cryptographic handshake and streaming 1-minute calibrated glucose readings independently without a phone.
  - **Juggluco / Phone GATT Server Companion**: Advertises as `Artemis Watch` exposing the Nordic UART Service (`6E400001-B5A3-F393-E0A9-E50E24DCCA9E`). Receives live glucose notifications (`GB({"t":"notify",...})`) and syncs RTC clock (`setTime(...)`) directly from the Juggluco or Gadgetbridge Android app.
  - **Bluetooth Radio OFF**: Completely shuts down the 2.4 GHz radio when not needed to maximize battery life.
  - Switchable at any time via **Settings -> BT MODE** or quick-toggle on `Page 6/7 (BLE)`.
- **Battery Fuel Gauge & Charge Detection**:
  - Calibrated ADC with 2.5V precision voltage reference (`GPIO 4`) sampling the 1:4 battery divider (`GPIO 5`).
  - Hardware USB 5V detection (`GPIO 42`) and active-low charge controller status (`GPIO 7`).
  - Displays live percentage (`XX%` / `+XX%`) on Home HUD, header bar, and `Page 4/7 (Diagnostics)`.
- **Glanceable Onboard RGB Status LED (`GPIO 14, 12, 13`)**:
  - Active-low glanceable indicator: Green (in target / charged), Amber (warning / charging), Red (urgent alarm), Blue (Libre 2 connected), Cyan (Juggluco connected).
- **Dynamic Power Management**:
  - Dynamic CPU throttling down to **80 MHz** during screen-off sleep while keeping FreeRTOS, BLE radio, and I2C sensors alive. Wakes to **240 MHz** on button press or wrist raise.
- **7-Page Dark Cyberpunk UI** (zero pure white `#FFFFFF` anywhere on screen for night readability, plus 10-bit LEDC PWM backlight dimming on `GPIO 33`):
  - `Pg 1/7`: Home watchface (date, `HH:MM` RTC time, battery `%`, `mmol/L` reading, vector trend arrow, `CP2077` Kiroshi HUD theme, and toggleable `GLITCH FX` scanline/chromatic aberration).
  - `Pg 2/7`: Detailed scan card (`mmol/L`, `mg/dL`, freshness, remaining sensor life, serial, transport badge `BLE-S` / `BLE-J` / `USB` / `FLS`).
  - `Pg 3/7`: Zoomable 2D pixel graph (`15m`, `1h`, `3h`, `8h`, `24h`, `ALL`).
  - `Pg 4/7`: Hardware diagnostics (`Bat: X.XX V`, `USB/CHG` status, `BT Mode`, `RGB LED`, sensor `CRC`, status, `Age/MaxLife`, `Q` flags).
  - `Pg 5/7`: 16-hex wire UID + `patch_info`.
  - `Pg 6/7`: Multi-mode Bluetooth status (Direct Libre 2 MAC/streaming vs Juggluco NUS GATT status vs Radio OFF).
  - `Pg 7/7`: Persistent Settings tile (`/spiffs/settings.cfg`): `BT MODE`, `RGB LED`, `RAISE WAKE`, `TIMEOUT`, `BRIGHTNESS`, `HOME COLOR`, `GLITCH FX`, `LED <= LOW`, `LED >= HIGH`, `ALM <= LOW`, and `ALM >= HIGH`.
- **Flight-Deck LEDs (`GPIO 17, 18, 43, 44, 45, 46`) and Piezo Alarms (`GPIO 47` / `GPIO 11`)**:
  - **Normal update**: 1 subtle 18 ms blip on the flight-deck LEDs when a new value arrives.
  - **Warning (`<= 4.2` or `>= 13.0` mmol/L default)**: 3 seconds of 4 Hz flashing across all 6 flight-deck lights.
  - **Urgent (`<= 3.6` or `>= 16.0` mmol/L default)**: Wakes the screen, displays the `!! URGENT LOW !!` / `!! URGENT HIGH !!` modal, flashes all 6 flight-deck lights, and sounds the two-tone piezo alarm until acknowledged by pressing any button (which pauses audio for 15 minutes while keeping LED alerts active).
- **Persistence across reboots**: Every 1-minute `BLE-S` or `BLE-J` reading is appended to `/spiffs/scans.csv` and the monotonic `unlock_count` is saved to `/spiffs/ble_bond.txt`. On reboot, the watch restores the newest reading from `/spiffs/scans.csv` and automatically reconnects on the next 60s sensor wake.

---

## What this is and what it isn't (Limitations)

1. **Standard European Libre 2 only.** Built and tested against standard EU Libre 2 sensors (`0xC5` / `0x9D` family). Does not support Libre 2+, Libre 3, or US sensors.
2. **The number sits slightly below LibreLink.** LibreLibre runs the closed per-sensor factory calibration and temperature chain on each record, but skips the vendor library's 13.6 KB multi-sample history state blob. In my own side-by-side checks:
   - In the **9.3 to 9.9 mmol/L** range (five paired readings), LibreLibre read `9.1 to 9.4 mmol/L` (mean **-0.34 mmol/L**, range `-0.5 to -0.2 mmol/L`).
   - In the **4.6 to 5.4 mmol/L** range (five paired readings: `5.0 vs 5.4`, `4.9 vs 5.4`, `4.0 vs 4.6`, and `3.9 / 4.0 vs 4.6`), LibreLibre read **-0.4 to -0.7 mmol/L** lower (mean **-0.56 mmol/L**).
   I did not slap a fake offset onto the code to hide that gap. Every sensor carries its own factory coefficients in FRAM, and forcing a flat fudge factor from one sensor would be bad engineering.
3. **Not medical advice or a certified medical device.** Built for learning, verification, and having hardware you own and control. Do not use it on its own to dose insulin.
4. **Keep your SD captures private.** The JSON files saved in `/ext/apps_data/libre_research/` contain your sensor's UID, derived serial, and raw FRAM bytes. Do not upload your `cap_*.json` files to public GitHub issues.

## Documentation index

- [Bluetooth streaming and NFC capture guide (`docs/BLE-AND-NFC.md`)](docs/BLE-AND-NFC.md)
- [User guide and troubleshooting (`docs/USER-GUIDE.md`)](docs/USER-GUIDE.md)
- [How the math works and where the limits are (`docs/LIMITATIONS.md`)](docs/LIMITATIONS.md)
- [Building from source and running the test suite (`docs/BUILD-AND-TEST.md`)](docs/BUILD-AND-TEST.md)
- [Code layout and research notes (`docs/RESEARCH.md`)](docs/RESEARCH.md)
- [Current status and engineering corrections (`CURRENT-STATUS-AND-CORRECTIONS.md`)](CURRENT-STATUS-AND-CORRECTIONS.md)
- [Offline Python FRAM decoder (`offline/README.md`)](offline/README.md)
- [How the serial number is packed in the UID (`glucose-research/SERIAL-IN-FRAM.md`)](glucose-research/SERIAL-IN-FRAM.md)

## License

MIT License. See [LICENSE](LICENSE), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), and [INTEGRITY-LICENSE.md](INTEGRITY-LICENSE.md) for attribution to Guido Soranzio's DiaBLE project (MIT) for the EU2 FRAM XOR decrypt, CRC16, and calibration table structure.
