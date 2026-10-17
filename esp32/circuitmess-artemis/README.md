# LibreLibre ESP32 — CircuitMess Artemis Watch

Standalone LibreLibre firmware for the **CircuitMess Artemis 2** watch (`ESP32-S3-MINI-1`, 4 MB Flash, 2 MB PSRAM, 128x128 `ST7735S` SPI2 color display, `LSM6DS3TR-C` IMU at `0x6A`, `PCF8563` RTC at `0x51`, 4 physical buttons, SPIFFS storage, and dual-mode Bluetooth engine).

## Highlights & Features

1. **Dual Bluetooth Operation Mode**:
   - **Direct Libre 2 BLE Client (`0xFDE3`)**: Connects directly to FreeStyle Libre 2 sensors, authenticating with cryptographic handshake and streaming 1-minute calibrated glucose readings independently without a phone.
   - **Juggluco / Phone GATT Server Companion**: Advertises as `Artemis Watch` exposing the Nordic UART Service (`6E400001-B5A3-F393-E0A9-E50E24DCCA9E`). Receives live glucose notifications (`GB({"t":"notify",...})`) and syncs RTC clock (`setTime(...)`) directly from the Juggluco or Gadgetbridge Android app.
   - **Bluetooth Radio OFF**: Completely shuts down the 2.4 GHz radio when not needed to maximize battery life.

2. **Slide 2 Phone Notifications & Full Gadgetbridge Integration**:
   - **Full-Screen Reader**: Word-wrapped high-contrast viewer on Slide 2 with app badges (WhatsApp, SMS, Phone, Email, System).
   - **Bidirectional Dismissal**: Pressing **SELECT** dismisses notifications locally and transmits `DISMISS` back to Android to clear them from the phone status bar.
   - **Incoming Call Handling**: Full-screen call card with caller name, number, audible ringer, LED strobe, and watch-side call rejection (`SELECT` -> `REJECT`).
   - **Find My Watch & Music Banners**: Responds to phone locator pings with 4s alternating chime and strobing LEDs; displays music track and artist banners.

3. **Dual-Engine Flashlight Mode**:
   - Long-press **ALT** for **>1.5 seconds** (or Settings row 18) triggers 100% white screen floodlight + all 6 perimeter flight deck LEDs.
   - Press **any button** to exit instantly; auto shuts off after 3 minutes.

4. **Medical Emergency SOS Beacon Mode**:
   - Long-press **SELECT** for **>3.0 seconds** (or Settings row 19) activates Morse code `... --- ...` distress pulses on LEDs + 2800 Hz buzzer.
   - Displays full-screen **Type 1 Diabetic emergency card** with live glucose reading, rescue guidance, and stored ICE contact. Dispatches an emergency distress packet to Gadgetbridge on phone.
   - Press **any button** to silence and exit immediately.

5. **Smart Low-Battery & Power Saver Mode**:
   - Clamps CPU to 80 MHz, caps backlight to 20%, enforces 5s sleep timeout, and suspends decorative pulses.
   - Arms automatically when battery $\le$ 15% (or manual toggle in Settings row 15).
   - Preserves continuous CGM BLE monitoring and urgent hypo/hyper alarms with 100% reliability.

6. **Time in Range (TIR) & 24h Stats Dashboard**:
   - 7th zoom preset on Slide 3 calculates and displays 24-hour TIR % (3.9–10.0 mmol/L), Low % (<3.9), High % (>10.0), and average glucose.

7. **Night / Do-Not-Disturb (DND) Mode & Retro Hourly Chime**:
   - Settings row 16 disables raise-to-wake in bed, mutes routine notification popups/chimes, and caps wake brightness to 15% (urgent alarms remain 100% armed).
   - Settings row 17 enables a retro digital watch double-pip chime on the hour (`:00`).

8. **Custom ICE Emergency Contact Storage**:
   - Non-volatile storage of emergency contact name and phone number in `/spiffs/ice.cfg`.
   - Configurable via serial command `SET_ICE|<Name>|<Phone>`.

9. **Glanceable Onboard RGB Status LED**:
   - Hardware LEDC PWM with 100% intensity when awake and continuous 30% ambient glow during screen sleep.
   - Green (target glucose), Amber (warning), Red (urgent hypo/hyper), Cyan (Juggluco), Blue (Direct Libre 2).

---

## Screen Navigation (6 Pages)

- **Slide 1 / Pg 1: Cyberpunk Home HUD**: Date, big Cyberpunk RTC clock, `mmol/L` reading, vector trend arrow, battery % / charge status, dual-mode BLE dot indicator, `SAV` / `Zz` status pills, and toggleable Kiroshi `GLITCH FX`.
- **Slide 2 / Pg 2: Phone Notifications**: Focused word-wrapped notification reader with app badge, sender title, message body, timestamp, and bidirectional dismissal.
- **Slide 3 / Pg 3: 2D Glucose Graph & TIR**: 48-point history graph with target band (3.9–10.0 mmol/L) across 7 cycleable zoom windows (`15m`, `30m`, `1h`, `3h`, `6h`, `12h`, and `TIR` stats card).
- **Slide 4 / Pg 4: Bluetooth Status**: Direct Libre 2 BLE connection/stream state, Juggluco Nordic UART Service GATT status, and packet counters.
- **Slide 5 / Pg 5: Diagnostics & Telemetry**: Battery voltage, charge controller state, USB detection, sensor CRC/integrity, and stored history points.
- **Slide 6 / Pg 6: Settings Menu**: 23-row sliding window menu with vertical scrollbar.

---

## Controls

- **UP / DOWN**:
  - On standard pages: Cycle through pages.
  - On Slide 2 (Notifications): Browse unread notifications.
  - On Slide 6 (Settings): Scroll up / down rows (0..22).
- **SELECT**:
  - On Slide 2: Dismiss selected notification (clears locally and on phone).
  - On Slide 3: Cycle zoom window / switch to 24h TIR dashboard.
  - On Slide 4: Toggle BLE scan / pairing.
  - On Slide 6: Toggle or cycle the selected setting.
  - **Hold for >3.0s**: Activates Medical Emergency SOS Beacon.
- **ALT (Back)**:
  - Jump between Home HUD and Settings.
  - Dismiss incoming notification popups.
  - **Hold for >1.5s**: Toggles Dual-Engine Flashlight mode.
- **Any Button**:
  - Wakes screen from sleep.
  - Instantly turns off Flashlight mode or silences Medical SOS Beacon.
  - Acknowledges urgent glucose alarms.

---

## Settings Menu (23 Rows)

1. `BT MODE`: Direct (Libre 2) / Juggluco (Phone) / OFF
2. `RGB LED`: ON / OFF (always-on 30% glow during sleep)
3. `RAISE WAKE`: ON / OFF
4. `DECK PULSE`: OFF / 5 MIN / 10 MIN / 15 MIN / 30 MIN / 60 MIN
5. `NOTIF LIGHT`: ON / OFF (perimeter LED pulse on notification)
6. `NOTIF SOUND`: ON / OFF (melodic 2-tone buzzer chime)
7. `WARN LOW`: Threshold cycle (3.5 .. 5.5 mmol/L or OFF)
8. `WARN HIGH`: Threshold cycle (9.0 .. 18.0 mmol/L or OFF)
9. `ALARM LOW`: Threshold cycle (2.8 .. 4.0 mmol/L or OFF)
10. `ALARM HIGH`: Threshold cycle (13.0 .. 22.0 mmol/L or OFF)
11. `ALM SOUND`: ON / OFF (audio mute for latching alarms)
12. `TIMEOUT`: 5s / 10s / 15s / 30s / 1m / 2m / Never
13. `BRIGHTNESS`: 15% / 30% / 50% / 75% / 100%
14. `HOME COLOR`: CP2077 / CYAN / MAGENTA / MATRIX / AMBER / CRIMSON
15. `GLITCH FX`: ON / OFF (Kiroshi HUD scanline tear)
16. `PWR SAVER`: OFF / AUTO (<=15% battery) / ON
17. `NIGHT DND`: ON / OFF (disables raise-wake, mutes routine alerts)
18. `HOURLY CHM`: ON / OFF (retro digital watch double-pip at :00)
19. `FLASHLIGHT`: Toggle flashlight (100% white screen + 6 perimeter LEDs)
20. `SOS BEACON`: Test / activate Medical SOS Beacon
21. `SET HOUR`: Manual RTC hour increment
22. `SET MIN`: Manual RTC minute increment
23. `SET DATE`: Manual RTC date increment

---

## Building and Flashing

```bash
# Build firmware
pio run -d esp32/circuitmess-artemis -e circuitmess_artemis

# Upload over USB
pio run -d esp32/circuitmess-artemis -e circuitmess_artemis -t upload --upload-port /dev/ttyACM0
```

---

## Remote Configuration & ICE Contacts from Android

Artemis supports multiple flexible methods to update settings and ICE (In Case of Emergency) contacts directly from your phone:

### 1. Magic Self-Notification (Zero-Install)
Simply send yourself a message (via SMS, WhatsApp, Signal, Telegram, or Google Keep note) containing one of these tags. Gadgetbridge forwards the notification, and Artemis automatically intercepts it, saves the configuration to SPIFFS, and flashes a confirmation popup:

- **Set ICE Contact**:
  `#ICE: Jane Doe / +447700900123`
  *(Supported delimiters: `/`, `|`, or `,`)*
- **Update Settings**:
  `#CFG: BRIGHT=100 DND=ON SAVER=AUTO CHIME=ON WAKE=ON`
  *(Options: `BRIGHT=10..100`, `DND=ON/OFF`, `SAVER=OFF/AUTO/ON`, `CHIME=ON/OFF`, `WAKE=ON/OFF`, `RGB=ON/OFF`, `THEME=0..5`)*

### 2. Web Bluetooth Companion Dashboard (Recommended)
Open `tools/artemis_companion.html` in Chrome on Android or desktop:
- Connects directly to the watch via Nordic UART Service (`6E400001`).
- Provides an intuitive Cyberpunk control dashboard to configure ICE contacts, backlight brightness, night DND, power saver, alarms, flashlight, and push test notifications in real time.

### 3. Automation Intents (Tasker / MacroDroid / Termux)
Enable **Allow Intent Access** in Gadgetbridge device settings. Any automation app can broadcast BLE UART lines to Artemis:
```bash
# Set ICE contact
am broadcast -a com.banglejs.uart.tx --es line "SET_ICE|Jane Doe|+447700900123"

# Adjust brightness or trigger actions
am broadcast -a com.banglejs.uart.tx --es line "SET_BL|100"
am broadcast -a com.banglejs.uart.tx --es line "CLEAR_NOTIFS"
```

### 4. Native Gadgetbridge JSON Protocol
Artemis parses standard Gadgetbridge / Bangle.js JSON packets:
- `GB({"t":"set_ice","name":"Jane Doe","phone":"+447700900123"})`
- `GB({"t":"set_cfg","bright":100,"dnd":1,"saver":1,"chime":1})`
- `GB({"t":"get_ice"})` and `GB({"t":"get_cfg"})`

