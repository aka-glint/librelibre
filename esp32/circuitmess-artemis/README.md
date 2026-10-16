# LibreLibre ESP32 — CircuitMess Artemis Watch

Standalone LibreLibre firmware for the **CircuitMess Artemis** watch (`ESP32-S3-MINI-1`, 4 MB Flash, 2 MB PSRAM, 128x128 `ST7735S` SPI2 color display, `LSM6DS3TR-C` IMU at `0x6A`, 4 physical buttons, SPIFFS storage, and optional direct Libre 2 BLE client).

## Hardware Auto-Detection (`EFUSE_RD_USR_DATA1_REG`)
Reads hardware revision from `EFUSE_BLK3` at boot (`Rev 0/1` vs `Rev 2/3`):
- **Rev 2/3 (`HWRev=3`):** `TFT_SCK=36`, `TFT_MOSI=35`, `TFT_DC=48`, `TFT_RST=34`, `LED_BL=33` (inverted PWM), `I2C_SDA=40`, `I2C_SCL=41` (`LSM6DS3TR-C` @ `0x6A`), `BTN_UP=1`, `BTN_DOWN=2`, `BTN_SELECT=3`, `BTN_ALT=21`.
- **Rev 0/1:** `TFT_SCK=41`, `TFT_MOSI=40`, `TFT_DC=39`, `TFT_RST=38`, `LED_BL=37` (inverted PWM), `I2C_SDA=35`, `I2C_SCL=36`, `BTN_UP=9`, `BTN_DOWN=10`, `BTN_SELECT=8`, `BTN_ALT=21`.

## Controls
- **UP / DOWN:** Cycle 5 pages (`1/5 Main`, `2/5 Graph`, `3/5 Diagnostics`, `4/5 UID & SPIFFS`, `5/5 Direct Sensor BLE`).
- **SELECT:**
  - On `Page 2/5 (Graph)`: Cycle zoom window (`15m`, `1h`, `3h`, `8h`, `24h`, `ALL`).
  - On `Page 5/5 (BLE)`: Toggle `PAIR BLE` / `STOP BLE`.
  - On `Page 1/3/4`: Jump directly to `Page 2/5 (Graph)`.
- **ALT (Back):** Jump between `Page 1/5 (Main)` and `Page 2/5 (Graph)` (or dismiss modal popup).
- **Screen Wake (45s timeout):** Wakes on any button press, physical bump (`> 0.12g` on `LSM6DS3TR-C`), wrist raise, or incoming scan/BLE stream packet.
