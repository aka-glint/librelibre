# LibreLibre ESP32 (Waveshare ESP32-S3-Touch-LCD-2)

This is the full-color 320x240 LCD + microSD + capacitive touch build of LibreLibre for the **Waveshare ESP32-S3-Touch-LCD-2** (ESP32-S3R8, 16MB Flash, 8MB Octal PSRAM, ST7789 320x240 display, CST816D touch, and shared SPI2 microSD card slot).

It runs the exact same C calibration, EU2 XOR decryption, 3-section CRC16, sensor status, remaining-life, and verified SD JSON capture pipeline (`vendor_chain.c`, `integrity.c`, `factory.c`, `lifecycle.c`, `protocol.c`, `capture.c`, `nfc_io.c`) as the Flipper Zero app.

## What the Waveshare build adds over the Heltec V3 build

1. **Full 320x240 16-bit color HUD (`ST7789` @ 40 MHz SPI2 DMA)**:
   - Displays the `mmol/L` reading in large 4x green type (`24x32` px per character) alongside `(mg/dL)`, full `Fresh 0m=now; Left 12d 18h`, `Serial 3M...`, and SD verification status on one screen with zero crowding.
   - Color-coded status cards: Green for valid active readings, Red/Amber for `Rescan sensor` / flagged records.
2. **Verified microSD JSON capture storage (`/sdcard/libre_research/`)**:
   - Shares `SPI2_HOST` cleanly between the ST7789 LCD (`CS = GPIO 45`) and the microSD slot (`CS = GPIO 41`, `MISO = GPIO 40`) using `sdspi_host_init_device()`.
   - Writes the exact same schema-2 `cap_<UID>_<tick>_<seq>.json` file as the Flipper app, flushes with `fsync()`, reopens the file, and verifies byte-for-byte `memcmp` readback before reporting `Saved + verified on SD`.
   - If no microSD card is inserted, live scanning and display continue normally (`SD absent (live display only)`).
3. **Capacitive touch (`CST816D` on I2C `SDA=48, SCL=47, INT=46`)**:
   - Tap anywhere on the screen (or press the `BOOT` button on GPIO 0) to cycle through:
     - **Page 1 (Main)**: Large `X.Y mmol/L` + `(mg/dL)` card, `Fresh 0m=now; Left Xd Yh`, `Serial 3M...`, and SD save status.
     - **Page 2 (Diagnostics)**: CRC status, sensor state (`active`, `shutdown`, `warming up`), remaining life, and `Age/MaxLife Q` flags.
     - **Page 3 (UID & File)**: Full 16-hex wire UID (LSB-first), derived serial, and the saved SD filename (`cap_...json`).

## Verified Waveshare ESP32-S3-Touch-LCD-2 Pin Map

| Peripheral | Signal | GPIO | Notes |
|---|---|---|---|
| **ST7789 LCD (`SPI2_HOST`)** | `SCLK` / `MOSI` / `CS` / `DC` / `BL` | `39` / `38` / `45` / `42` / `1` | `RST = NC` (GPIO 40 is SD MISO, never drive as LCD reset); Backlight PWM on GPIO 1 |
| **MicroSD (`SPI2_HOST`)** | `SCLK` / `MOSI` / `MISO` / `CS` | `39` / `38` / `40` / `41` | Attached via `sdspi_host_init_device()` on shared `SPI2_HOST` |
| **CST816D Touch (`I2C0`)** | `SDA` / `SCL` / `INT` | `48` / `47` / `46` | Address `0x15`; auto-sleep (`0xFE`) disabled; explicit STOP between reg write and 6-byte read |
| **External / Flipper UART (`Serial1`)** | `RX` / `TX` | `16` / `17` | `115200 8N1` (connect Flipper Pin 13 TX -> GPIO 16, Pin 14 RX -> GPIO 17, GND -> GND) |
| **USB-C (`HWCDC`)** | Native USB Serial/JTAG | `19` / `20` | Accepts `LL2|...` frames, `PING`, and `SCAN` commands |
| **WiFi / BLE Radios** | Disabled (Air-gapped) | — | Neither WiFi nor BLE is initialized |

## Prebuilt Waveshare S3 firmware (`dist/`)

Compiled with PlatformIO (`espressif32`, `board = esp32-s3-devkitc-1`, `framework = arduino`, 16MB Flash, Octal PSRAM):

| File | Size | SHA-256 |
|---|---|---|
| [`dist/firmware.bin`](dist/firmware.bin) | 429,008 B | `cf8b18b5222b6e54da96bf588961221a74a24150182d4effbcef1c300a2c90f0` |
| [`dist/bootloader.bin`](dist/bootloader.bin) | 15,104 B | `2a71d69b471e20c2bac7fb469f3c6a807b3ebee780e348e5889db0da849ca363` |
| [`dist/partitions.bin`](dist/partitions.bin) | 3,072 B | `1d9cca96de0fe07ad7fc0648b9878ddecd9ce565e38b589ad20fea698ed4c80c` |

## Building, testing, and flashing

> **Note before flashing your Waveshare watch board:** Your Waveshare ESP32-S3-Touch-LCD-2 is also your Artemis watch hardware (`~/artemis2-6` locked known-good, `~/artemis2-7` active dev). Only flash this onto the Waveshare when you want to run LibreLibre on it, or keep `~/artemis2-6` ready to restore.

```sh
# Run host + ASan/UBSan bridge, state-machine, and math verification tests:
sh tests/run.sh

# Build firmware with PlatformIO:
pio run

# Flash to a connected Waveshare ESP32-S3-Touch-LCD-2 over USB (/dev/ttyACM0):
pio run -t upload
```

## Testing over USB without a sensor

```sh
python3 tools/send_capture.py --synthetic --port /dev/ttyACM0
```

## License

MIT License. See [LICENSE](LICENSE), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), and [INTEGRITY-LICENSE.md](INTEGRITY-LICENSE.md).
