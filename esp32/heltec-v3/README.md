# LibreLibre ESP32 (Heltec WiFi LoRa 32 V3)

This is the ESP32-S3 build of LibreLibre for the **Heltec WiFi LoRa 32 V3** (ESP32-S3 + built-in 128x64 SSD1306 OLED).

It runs the exact same C calibration, EU2 XOR decryption, 3-section CRC16, sensor status, and remaining-life pipeline (`vendor_chain.c`, `integrity.c`, `factory.c`, `lifecycle.c`, `protocol.c`, `nfc_io.c`) as the Flipper Zero app, and renders the result on the Heltec's 128x64 OLED.

## How it works

The ESP32 owns the full 17-command ISO15693 acquisition state machine (`src/nfc_io.c`), decryption, CRC checks, calibration math, and OLED UI. While waiting on a dedicated SPI ISO15693 transceiver breakout (such as a PN5180 or ST25R3916), you can feed raw sensor captures into the Heltec in three ways:

1. **Flipper Zero GPIO UART (`Serial1`, 115200 8N1)**:
   Wire the Flipper's expansion UART pins to the Heltec V3's free GPIOs:
   - Flipper Pin 13 (`TX`) -> Heltec **GPIO 47** (`RX`)
   - Flipper Pin 14 (`RX`) -> Heltec **GPIO 48** (`TX`)
   - Flipper Pin 8 (`GND`) -> Heltec **GND**
   Whenever the Flipper finishes scanning a sensor, it transmits the CRC16-protected `LL2|...` frame out Pin 13 and the Heltec immediately decodes and displays the reading.
2. **USB Serial (`Serial` via CP2102, 115200 8N1)**:
   Send any saved `cap_*.json` file or a synthetic EU2 scan over USB using `tools/send_capture.py`.
3. **Master-polled raw RF transceive (`SCAN`)**:
   Send `SCAN\n` to trigger `ll_nfc_acquire_sensor(&hal, ...)`, which steps through Inventory (`26 01 00`), Patch Info (`02 A1 07`), and all 15 Read-Multiple-Blocks commands (`02 23`) over the `LlNfcHal` transceive interface.

## Screen layout (128x64 OLED)

 Adafruit GFX's 5x7 font is fixed-width (6px per character, 21 characters per 128px line). Every row is sized so nothing wraps or spills onto the line below (`display.setTextWrap(false)`):

- **Top row**: Transport source (`USB`, `UART`, `RF-M`) on the left, time since scan (`0s`, `1m`) on the right.
- **Main value (`X.Y mmol/L`)**: 2x font size (`y = 12`).
- **Freshness and remaining life (`0m=now; Left 12d 18h`)**: 20 characters (`120px`), single line (`y = 33`), computed from the sensor's internal minute counters.
- **Sensor serial (`Serial 3M...`)**: 18 characters (`108px`), derived from the 8-byte ISO15693 UID and patch family nibble (`y = 44`).
- **Bottom row (`PRG:info  #N`)**: Navigation prompt and scan counter (`y = 54`).

Pressing the **PRG / USER** button (GPIO 0) cycles between:
1. **Main screen**: Glucose reading, freshness + remaining life, derived serial.
2. **Diagnostics screen**: CRC status, sensor state (`active`, `shutdown`, `warming up`), remaining life, and `Age/MaxLife Q` flags.
3. **UID screen**: Full 16-hex wire UID (LSB-first) and derived serial.

## Prebuilt Heltec V3 firmware (`dist/`)

Compiled with PlatformIO (`espressif32`, `board = heltec_wifi_lora_32_V3`, `framework = arduino`):

| File | Size | SHA-256 |
|---|---|---|
| [`dist/firmware.bin`](dist/firmware.bin) | 330,576 B | `f4939249f246959fad9c8ec0c50f1b9eb917d4ad15859c2148b0d8816d19f637` |
| [`dist/bootloader.bin`](dist/bootloader.bin) | 15,104 B | `1776e4dd896a69d0a5c2e79957b0e2a88aa4129b1381d6478683515a1f6af343` |
| [`dist/partitions.bin`](dist/partitions.bin) | 3,072 B | `1d9cca96de0fe07ad7fc0648b9878ddecd9ce565e38b589ad20fea698ed4c80c` |

## Building, testing, and flashing

```sh
# Run host + ASan/UBSan bridge, state-machine, and math verification tests:
sh tests/run.sh

# Build Heltec V3 firmware with PlatformIO:
pio run

# Flash to a connected Heltec V3 over USB:
pio run -t upload
```

## Testing the UI over USB without a sensor

```sh
python3 tools/send_capture.py --synthetic --port /dev/ttyUSB0
```

## License

MIT License. See [LICENSE](LICENSE), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), and [INTEGRITY-LICENSE.md](INTEGRITY-LICENSE.md).
