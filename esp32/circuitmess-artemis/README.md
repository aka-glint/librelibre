# LibreLibre ESP32 — CircuitMess Artemis Watch

Standalone LibreLibre firmware for the **CircuitMess Artemis** watch (`ESP32-S3-MINI-1`, 4 MB Flash, 2 MB PSRAM, 128x128 `ST7735S` SPI2 color display, `LSM6DS3TR-C` IMU at `0x6A`, 4 physical buttons, SPIFFS storage, and optional direct Libre 2 BLE client).

## Hardware Auto-Detection (`EFUSE_RD_USR_DATA1_REG`)
Reads hardware revision from `EFUSE_BLK3` at boot (`Rev 0/1` vs `Rev 2/3`):
- **Rev 2/3 (`HWRev=3`):** `TFT_SCK=36`, `TFT_MOSI=35`, `TFT_DC=48`, `TFT_RST=34`, `LED_BL=33` (inverted PWM), `I2C_SDA=40`, `I2C_SCL=41` (`LSM6DS3TR-C` @ `0x6A`), `BTN_UP=1`, `BTN_DOWN=2`, `BTN_SELECT=3`, `BTN_ALT=21`.
- **Rev 0/1:** `TFT_SCK=41`, `TFT_MOSI=40`, `TFT_DC=39`, `TFT_RST=38`, `LED_BL=37` (inverted PWM), `I2C_SDA=35`, `I2C_SCL=36`, `BTN_UP=9`, `BTN_DOWN=10`, `BTN_SELECT=8`, `BTN_ALT=21`.

## Controls & Features
- **UP / DOWN:** Cycle 6 slides (`1/6 Cyberpunk Home HUD`, `2/6 Phone Notifications`, `3/6 Glucose History Graph & TIR`, `4/6 Bluetooth Management`, `5/6 Hardware Diagnostics`, `6/6 Settings Menu`).
- **Native Runtime Calibration (`0x68a74..0x86908`) & Multi-Horizon BLE Estimator (`lc_vendor_calibrate_ble_packet`):**
  - Evaluates both 344-byte NFC FRAM scans (`lc_vendor_current_trend`, `lc_vendor_extract_fram_series`) and 46-byte 1-minute Libre 2 BLE packets (`lc_vendor_calibrate_ble_packet` across all 7 trend lags `{0, 2, 4, 6, 7, 12, 15}` and 3 history lags `{0, 15, 30}`) using the 100%-verified native runtime factory coefficients (`0x8b26f`), windowed temperature smoothing (`0x85534`), 5-point trend ADC average (`0x86908`), 3-window OLS + midpoint history smoother (`0x71f50..0x7308c`), 4-horizon rate projections (`0x7d8b2`), sensor-age drift (`0x81fc0`), and low-glucose rational curve (`0x765ec, 0x7717a`).
- **Dual-Mode Bluetooth (`DIRECT` Libre 2 `0xFDE3`, `JUGGLUCO` / Gadgetbridge NUS GATT Server, or `OFF`):**
  - Supports direct sensor BLE streaming (`0xFDE3` / `0xF001` / `0xF002`), Gadgetbridge/Juggluco phone notifications + weather/CGM broadcast + time sync (`setTime`), and a Web Bluetooth browser control deck (`tools/artemis_companion.html`).
- **Hardware Peripherals:**
  - `PCF8563` / `BM8563` I2C RTC (`0x51`), calibrated battery fuel gauge (`GPIO 4` 2.5V reference + `GPIO 5` divider + `GPIO 42` USB 5V + `GPIO 7` charge status), LEDC PWM RGB status LED (`GPIO 14/12/13`), 6 perimeter Flight-Deck LEDs (`GPIO 17, 18, 43, 44, 45, 46`), piezo alarm (`GPIO 47/11`), flashlight mode, medical SOS beacon, Night DND, and Power Saver.
- **Screen Wake:** Wakes on button press, wrist raise (`LSM6DS3TR-C` @ `0x6A`, sleeps immediately when wrist lowered for 250 ms), incoming notification/call, or urgent glucose alarm.
