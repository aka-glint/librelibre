# CircuitMess Artemis Watch Guide (`esp32/circuitmess-artemis`)

Turns the **CircuitMess Artemis** watch (`ESP32-S3-MINI-1`, 4 MB Flash, 2 MB PSRAM, `128x128` ST7735S color TFT, `LSM6DS3TR-C` IMU at `0x6A`, `PCF8563` RTC at `0x51`, 6 perimeter Flight-Deck LEDs, piezo buzzer, and onboard RGB LED) into a standalone FreeStyle Libre 2 receiver, alarm watch, and phone notification companion.

---

## 1. Native Runtime Sensor Calibration (`0x68a74..0x86908`)

Both 344-byte NFC FRAM scans (`lc_vendor_current_trend`, `lc_vendor_extract_fram_series`) and 1-minute 46-byte Libre 2 BLE stream packets (`lc_vendor_calibrate_ble_packet` in `src/vendor_chain.c`, called by `ll_libre2_evaluate_ble_packet` in `src/libre2_ble.c`) execute Abbott's instruction-recovered runtime pipeline:
1. **Thermistor & Windowed Temperature Compensation (`0x853b8..0x85534`):** 4-term Steinhart-Hart log-cubic `T_raw`, averaged across radius `3` on Trend (`|lag_i - lag_j| <= 3` across the 7 BLE trend lags `{0, 2, 4, 6, 7, 12, 15}`) and radius `1` on History (`{0, 15, 30}`), followed by `pow(1.045, 32.5 - T_gain)`.
2. **5-Point Trend ADC Moving Average (`0x86908`):** Radius-2 moving average across non-rejected trend ADC samples before `(raw - i3) * 65.0f * 1000.0f / (i4 - i3)`.
3. **Runtime Factory Slope & Offset (`0x68a74 -> 0x8b26f`, VMA `0xa1900`):** Parameter block `p = { -965071, 10500544, -1469174, 13193833 }` producing `c18c` and `c188` (`G[i] = sig[i] / c18c + c188`).
4. **Exact Sample Minute & History De-Pack Phase (`0x8ac84`, `0x881e0`):** `cg_id = ((raw_age & 0xF) == plain[26]) ? raw_age : (raw_age - 1)` and `phase = ((cg_id % 15) <= 1) ? 1 : 0`.
5. **3-Window OLS + Midpoint History Smoother (`0x71f50..0x7308c`):** Backward `(-2,-1,0)`, centered `(-1,0,+1)`, and forward `(0,+1,+2)` 3-point OLS fits plus midpoint `0.5 * (v_back + v_fwd)`.
6. **4-Horizon Rate Projections (`0x7d8b2`, `0x75950..0x79550`):** Projects `P_hist (0, 15, 30)`, `P0 (0, 6, 15)`, `P1 (0, 2, 7)`, and `P2 (0, 4, 12)` with weights `[AE990[mod15], 91, 91, 91, 10]`, adds bi-exponential sensor-age drift (`0x81fc0`), applies the power-6 rational display curve + `-4.0` offset (`0x765ec, 0x7717a`), and rounds to `mg/dL` and `mmol/L` tenths.

---

## 2. Tri-Mode Bluetooth Architecture (`BleMode`)

Switchable on the watch (`Settings -> BT MODE` or `Slide 4 -> SELECT`) or over Serial/BLE (`BT_MODE|0..2`):
1. **`DIRECT` (`BLE_MODE_DIRECT = 0`) — Direct FreeStyle Libre 2 BLE Client (`0xFDE3`):**
   - Connects directly to standard EU Libre 2 sensors every 60 seconds without a phone.
   - Uses `7.5–10 ms` connection interval (`esp_ble_gap_set_prefer_conn_params(6, 8, 0, 400)`), `+9 dBm` TX power, non-blocking CCCD write (`0x2902` -> `0xF001` 12-byte login payload), and `s_ble_client->getServices()` cache refresh across reconnects.
   - Persists bond credentials (`UID`, `patch_info`, `MAC`, `unlock_code`, and monotonic `unlock_count`) to `/spiffs/ble_bond.txt`.
2. **`JUGGLUCO` (`BLE_MODE_JUGGLUCO = 1`) — Phone Companion GATT Server (Nordic UART Service):**
   - Advertises as `Bangle.js Artemis` with NUS GATT service `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` (`RX = 6E400002-...`, `TX = 6E400003-...`).
   - Enforces strict `\r\n` (CRLF) termination on TX (`sendJugglucoTx`) so the Bangle.js Gadgetbridge driver never truncates JSON closing braces.
   - Parses Gadgetbridge / Juggluco packets (`parseGadgetbridgeLine`):
     - **Glucose via Weather (`t:"weather"`):** Parses glucose and trend arrows from `"loc"` / `"txt"` or reconstructs `mmol/L` (`hi.lo`) / `mg/dL` (`hi*10 + lo`) from Juggluco's weather encoding, plus polls `{"t":"weather"}` every 60s.
     - **Direct CGM (`t:"cgm"`):** Parses `"val"` and `"trend"`.
     - **Notifications (`t:"notify"` / `t:"notify-"`):** Stores up to 12 phone notifications (`Slide 2`), shows word-wrapped color-coded popups (WhatsApp Emerald, Signal Blue, Telegram Cyan, SMS/Messages, Gmail/Mail Amber, Instagram/Discord Purple, Phone Red), and supports magic configuration notifications (`#ICE:<Name>|<Phone>` and `#CFG:bright=..`).
     - **Incoming Calls (`t:"call"`):** Displays caller name + number with 30s ringing alert and **SELECT to Reject Call** (`{"t":"call","n":"REJECT"}`).
     - **Time & Timezone Sync:** Handles `setTime(<epoch>)`, `setTimeZone(<hours>)`, and `GB({"t":"setTime",...})`, writing directly to the hardware `PCF8563` I2C RTC (`0x51`).
     - **Find My Watch (`t:"find"`):** Triggers audio/visual locator beacon until any button is pressed.
3. **`OFF` (`BLE_MODE_OFF = 2`) — Radio Disabled:**
   - Shuts down BLE scanning/advertising for maximum battery life.

---

## 3. 6-Slide Cyberpunk HUD & 24-Row Settings Menu (`128x128 ST7735S`)

### Slide Layout (`ScreenPage`)
- **Slide 1 (`PAGE_HOME = 0`) — Cyberpunk Home HUD:**
  - Top bar: Formatted date (`6` selectable formats), DND crescent moon icon, Power Saver `[E]` pill, calibrated battery percentage (`XX%` / `+XX%`), and BLE mode status dot.
  - Center: Large `HH:MM` RTC clock with shadow + `6` selectable color palettes (`CP2077`, `CYAN`, `MAGENTA`, `MATRIX`, `AMBER`, `CRIMSON`) and optional Kiroshi `GLITCH FX` scanline tear.
  - Bottom HUD: Current `mmol/L` reading, reading age & link status (`BLE-S`, `BLE-J`, `USB`, `FLS`), and boxed vector trend arrow (`computeTrendDir` over last 5–18 sensor minutes).
- **Slide 2 (`PAGE_NOTIFS = 1`) — Phone Notifications Inbox:**
  - Browse up to `12` saved phone notifications (`[1/N]`), app color accent bar, timestamp, sender/title, 5-line word-wrapped body, `UP/DOWN` to scroll, `SELECT` to dismiss.
- **Slide 3 (`PAGE_GRAPH = 2`) — Glucose History Graph & TIR Stats Card:**
  - `SELECT` cycles through `7` views: `15m`, `1h`, `3h`, `8h`, `24h`, `ALL` 2D pixel glucose graphs plus the **Time-In-Range (`TIR`) Stats Card** (`% IN RANGE 3.9–10.0`, `% ABOVE 10.0`, `% BELOW 3.9`, progress bars, 24h mean glucose, and sample count).
- **Slide 4 (`PAGE_BLE = 3`) — Bluetooth Management:**
  - Displays live mode details (`Direct Sensor BLE` MAC/status/packet count, `Juggluco / Phone` NUS GATT connection/packet count, or `Bluetooth Radio OFF`). `SELECT` toggles pairing/mode.
- **Slide 5 (`PAGE_DIAG = 4`) — Hardware & Sensor Diagnostics:**
  - Live battery voltage (`X.XX V`), `%`, `[CHG]` / `[USB]` state, `BT` mode, `RGB LED` state, sensor CRC status, sensor state, `Age/MaxLife`, history point count, and SPIFFS status.
- **Slide 6 (`PAGE_SETTINGS = 5`) — 24-Row Scrollable Settings Menu (`/spiffs/settings.cfg`):**
  1. `0: BT MODE` (`DIRECT` / `JUGGLUCO` / `OFF`)
  2. `1: RGB LED` (`ON` / `OFF` — onboard LEDC PWM status indicator on `GPIO 14/12/13`, dims to 30% in sleep)
  3. `2: RAISE WAKE` (`ON` / `OFF` — `LSM6DS3TR-C` raise-to-wake with 5s glance limit & immediate 250 ms wrist-lower sleep)
  4. `3: DECK PULSE` (`OFF`, `5 MIN`, `10 MIN`, `15 MIN`, `30 MIN`, `60 MIN` — periodic sequential sweep across the 6 perimeter flight-deck LEDs)
  5. `4: NOTIF LIGHT` (`ON` / `OFF`)
  6. `5: NOTIF SOUND` (`ON` / `OFF`)
  7. `6: WARN LOW` (`3.5 .. 5.5` mmol/L or `OFF`, default `4.2`)
  8. `7: WARN HIGH` (`9.0 .. 18.0` mmol/L or `OFF`, default `13.0`)
  9. `8: ALARM LOW` (`2.8 .. 4.0` mmol/L or `OFF`, default `3.6`)
  10. `9: ALARM HIGH` (`13.0 .. 22.0` mmol/L or `OFF`, default `16.0`)
  11. `10: ALM SOUND` (`ON` / `OFF`)
  12. `11: TIMEOUT` (`5 SEC`, `10 SEC`, `15 SEC`, `30 SEC`, `1 MIN`, `2 MIN`, `NEVER`)
  13. `12: BRIGHTNESS` (`10%`, `25%`, `45%`, `70%`, `100%`)
  14. `13: HOME COLOR` (`CP2077`, `CYAN`, `MAGENTA`, `MATRIX`, `AMBER`, `CRIMSON`)
  15. `14: GLITCH FX` (`ON` / `OFF`)
  16. `15: PWR SAVER` (`OFF`, `AUTO` `<=15%`, `ON`)
  17. `16: NIGHT DND` (`ON` / `OFF` — suppresses raise-to-wake and routine notification chimes/lights during `23:00..07:00`)
  18. `17: HOURLY CHM` (`ON` / `OFF` — retro double-pip chime on `:00`)
  19. `18: FLASHLIGHT` (`TOGGLE` — all 6 perimeter LEDs + 100% white screen floodlight; also **1.5s long-press on ALT/BACK**)
  20. `19: SOS BEACON` (`TEST` — Morse `S-O-S` optical/audio beacon + medical emergency overlay + `/spiffs/ice.cfg` emergency contact; also **3.0s long-press on SELECT**)
  21. `20: SET HOUR` (`00..23` -> `PCF8563` RTC)
  22. `21: SET MIN` (`00..59` -> `PCF8563` RTC)
  23. `22: SET DATE` (`01..31` -> `PCF8563` RTC)
  24. `23: DATE FMT` (`DD MON`, `DD/MM/YY`, `MM/DD/YY`, `DD.MM.YY`, `ISO 8601`, `DAY D/M`)

---

## 4. Companion Dashboard (`tools/artemis_companion.html`) & USB/BLE Protocol

Open `esp32/circuitmess-artemis/tools/artemis_companion.html` in any Chromium browser (Chrome, Edge, Brave) to connect to the watch over **Web Bluetooth (`Bangle.js Artemis` NUS)** or **Web Serial (`115200`)**:
- **Live Telemetry:** Real-time glucose, battery `%`, BLE mode, backlight `%`, DND/Power Saver status, and ICE contact.
- **Libre 2 Sensor Bridge:** Drag-and-drop any Flipper Zero `cap_*.json` capture to compute the 344-byte FRAM CRC16 and push both the 8-hour history curve (`LL2|...`) and BLE pairing bond (`LL2_BLE|...`) straight to the watch.
- **Remote Controls & Serial/NUS Commands:**
  - `STATUS` -> returns full watch state
  - `BLE_DIAG` -> returns direct BLE bond & connection state
  - `BT_MODE|0..2` -> sets `DIRECT (0)`, `JUGGLUCO (1)`, or `OFF (2)`
  - `SET_BL|10..100` / `BRIGHT_IDX|0..4` -> sets backlight brightness
  - `TIME|YYYY|MM|DD|WD|HH|MM|SS` -> sets hardware `PCF8563` RTC
  - `DATE_FMT|0..5` -> sets date format on Home HUD
  - `SET_ICE|<Name>|<Phone>` -> saves emergency contact to `/spiffs/ice.cfg`
  - `FLASHLIGHT|0..1` -> toggles flashlight floodlight
  - `SOS|0..1` -> toggles Morse SOS + medical rescue overlay
  - `NIGHT_DND|0..1`, `PWR_SAVER|0..2`, `HOURLY_CHM|0..1`, `RAISE_WAKE|0..1`, `RGB_LED|0..1`, `DECK_PULSE|0..5`, `NOTIF_LIGHT|0..1`, `NOTIF_SOUND|0..1`, `ALM_SOUND|0..1`
  - `TEST_NOTIF|<App>|<Title>|<Body>`, `DISMISS_NOTIF|<id>`, `CLEAR_NOTIFS`
  - `TEST_ALARM|<mmol_tenths>`, `ACK_ALARM`
