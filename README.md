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
- **6-Slide Cyberpunk HUD** (`10-bit` LEDC PWM backlight on `GPIO 33`, `6` selectable color themes: `CP2077`, `CYAN`, `MAGENTA`, `MATRIX`, `AMBER`, `CRIMSON`):
  - `Slide 1 (PAGE_HOME)`: Cyberpunk watchface (formatted date in `6` selectable styles, DND crescent moon & Power Saver `[E]` indicators, battery `%`, `HH:MM` RTC clock, `mmol/L` reading, reading age/source badge `BLE-S` / `BLE-J` / `USB` / `FLS`, boxed vector trend arrow, and optional Kiroshi `GLITCH FX` scanline tear).
  - `Slide 2 (PAGE_NOTIFS)`: Phone notification inbox (`12` stored notifications with app color accents, sender/title, 5-line word-wrapped body, `UP/DOWN` scroll, `SELECT` to dismiss) plus incoming call overlay (`SELECT` to reject call).
  - `Slide 3 (PAGE_GRAPH)`: Zoomable 2D pixel glucose graph (`15m`, `1h`, `3h`, `8h`, `24h`, `ALL`) plus **Time-In-Range (`TIR`) Stats Card** (`% IN RANGE 3.9–10.0`, `% ABOVE 10.0`, `% BELOW 3.9`, progress bars, 24h mean glucose, and sample count).
  - `Slide 4 (PAGE_BLE)`: Tri-mode Bluetooth management (`DIRECT` Libre 2 MAC/status/1m packet count, `JUGGLUCO` / `Bangle.js Artemis` NUS GATT connection/packet count, or `OFF`).
  - `Slide 5 (PAGE_DIAG)`: Hardware & sensor diagnostics (`Bat: X.XX V`, `%`, `[CHG]` / `[USB]`, `BT` mode, `RGB LED`, sensor `CRC`, status, `Age/MaxLife`, history point count, and SPIFFS status).
  - `Slide 6 (PAGE_SETTINGS)`: 24-row scrollable Settings menu (`/spiffs/settings.cfg`): `BT MODE`, `RGB LED`, `RAISE WAKE`, `DECK PULSE`, `NOTIF LIGHT`, `NOTIF SOUND`, `WARN LOW`, `WARN HIGH`, `ALARM LOW`, `ALARM HIGH`, `ALM SOUND`, `TIMEOUT`, `BRIGHTNESS`, `HOME COLOR`, `GLITCH FX`, `PWR SAVER`, `NIGHT DND`, `HOURLY CHM`, `FLASHLIGHT`, `SOS BEACON`, `SET HOUR`, `SET MIN`, `SET DATE`, and `DATE FMT`.
- **Quick Hardware Shortcuts & Emergency Modes**:
  - **Hold `ALT/BACK` (`>= 1.5s`)**: Toggle **Flashlight Mode** (all 6 perimeter LEDs + 100% white screen floodlight).
  - **Hold `SELECT` (`>= 3.0s`)**: Toggle **Medical SOS Beacon + ICE Overlay** (Morse `S-O-S` optical/audio beacon + current glucose + rescue instructions + `/spiffs/ice.cfg` emergency contact).
  - **Web Bluetooth & USB Companion (`tools/artemis_companion.html`, `tools/flipper_bridge_sync.py`)**: Browser dashboard over Web Bluetooth NUS or Web Serial for live telemetry, drag-and-drop Flipper `cap_*.json` sensor bridging (`LL2` + `LL2_BLE`), ICE contact editing, and full watch configuration.
- **Flight-Deck LEDs (`GPIO 17, 18, 43, 44, 45, 46`) and Piezo Alarms (`GPIO 47` / `GPIO 11`)**:
  - **Normal update**: 1 subtle 18 ms blip on the flight-deck LEDs when a new value arrives.
  - **Warning (`<= 4.2` or `>= 13.0` mmol/L default)**: 3 seconds of 4 Hz flashing across all 6 flight-deck lights.
  - **Urgent (`<= 3.6` or `>= 16.0` mmol/L default)**: Wakes the screen, displays the `!! URGENT LOW !!` / `!! URGENT HIGH !!` modal, flashes all 6 flight-deck lights, and sounds the two-tone piezo alarm until acknowledged by pressing any button (which pauses audio for 15 minutes while keeping LED alerts active).
- **Persistence across reboots**: Every 1-minute `BLE-S` or `BLE-J` reading is appended to `/spiffs/scans.csv` and the monotonic `unlock_count` is saved to `/spiffs/ble_bond.txt`. On reboot, the watch restores the newest reading from `/spiffs/scans.csv` and automatically reconnects on the next 60s sensor wake.

---

## What this is and what it isn't (Limitations)

1. **Standard European Libre 2 only.** Built and tested against standard EU Libre 2 sensors (`0xC5` / `0x9D` family). Does not support Libre 2+, Libre 3, or US sensors.
2. **Calibration matches Abbott's runtime-selected native calculation (`libcalibrate.so` `0x68a74..0x86908`).** Both 344-byte NFC FRAM scans (`lc_vendor_current_trend` and `lc_vendor_extract_fram_series`) and 46-byte 1-minute BLE stream packets (`lc_vendor_calibrate_ble_packet`) execute the full recovered runtime pipeline:
   - Windowed thermistor temperature smoothing (`0x85534`, radius 3 on trend / radius 1 on history) and 5-point trend ADC moving average (`0x86908`).
   - Runtime factory parameter block (`VMA 0xa1900`, `0x68a74 -> 0x8b26f`) producing `c18c` and `c188`.
   - Exact sample minute `cg_id = ((raw_age & 0xF) == plain[26]) ? raw_age : (raw_age - 1)` (`0x8ac84`) and history de-pack phase `phase = ((cg_id % 15) <= 1) ? 1 : 0` (`0x881e0`).
   - 3-window OLS + midpoint history smoother (`0x71f50..0x7308c`).
   - 4-horizon physiological-lag rate projections (`0x7d8b2`, `0x75950..0x79550`) over trend lags `{0, 2, 4, 6, 7, 12, 15}` and history lags `{0, 15, 30}` (the exact 10 samples carried in every 46-byte Libre 2 BLE packet).
   - Bi-exponential sensor-age drift (`0x81fc0`) and power-6 rational display curve + `-4.0` offset (`0x765ec, 0x7717a`).
   - Verified **68 / 68 (100.00%) exact** on cold current, **68 / 68 (100.00%) exact** on warm current, and **2,108 / 2,108 (100.00%) exact** on 32-slot history series against the native Oracle with zero empirical fitting. A cold single-tap NFC scan does not carry prior 1-minute BLE warm state (`0x80d2c`), which can differ by `0.1–0.2 mmol/L` during fast glucose swings.
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
