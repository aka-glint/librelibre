# User Guide

LibreLibre is an offline NFC tap reader for Flipper Zero. You tap a standard European Libre 2 sensor, and the Flipper decodes the FRAM and shows the newest glucose reading, sample freshness, and remaining sensor life on the screen.

It does not replace LibreLink or Juggluco. Think of it as an independent check and a backup reader that works with zero phone or network dependencies.

## Compatibility

1. **Sensor**: Standard European FreeStyle Libre 2 (`0xC5` / `0x9D` family). Libre 2+, Libre 3, and US sensors are not supported.
2. **Flipper Zero**: Any standard Flipper Zero (target `f7`) with a working microSD card.
3. **Firmware**: Official (Stock) Firmware, Unleashed, and Momentum are all supported.
4. **Units**: `mmol/L`.

The app is not locked to a specific sensor. It reads the factory calibration fields (`i1` through `i6`, `t1`, `t2`) straight out of whatever sensor you scan.

## Installation

Each Flipper firmware line has its own API version number, and the Flipper refuses to open a `.fap` built for a different API major. Grab the file from `dist/` that matches your Flipper:

- **Unleashed (`unlshd-094e`, API 88.19)**: `dist/LibreLibre-unleashed.fap` (also saved as `dist/LibreLibre.fap`)
  SHA-256: `bfd09ec07d94a6a720f9f9df1563cf6ce4b62ec781d41a6a9b0b2881d40aaf79` (MD5: `4171cb07391fc3bc0f61a66da9c8bc5c`)
- **Official / Stock Firmware (OFW 1.5.1, API 88.2)**: `dist/LibreLibre-device.fap`
  SHA-256: `59888c7ac7ed1c744b5418cb55a7bb0069b2efd535d435549619f5bbe70ed4ad` (MD5: `a1c22520dd38c8fdeb57acba9b48e203`)
- **Momentum (`mntm-012`, API 87.1)**: `dist/LibreLibre-momentum.fap`
  SHA-256: `e74494da64028fa94772d3d9ec6d0939c7eece8b2ea65a72bb50b417146c2718` (MD5: `b945b486175117f21001c3ae451efdc4`)

How to put it on the Flipper:

1. Connect your Flipper to qFlipper (or put the microSD card in your computer).
2. Copy the `.fap` file for your firmware into `SD Card/apps/NFC/LibreLibre.fap` (`/ext/apps/NFC/LibreLibre.fap`).
3. If you had an older test build in `apps/NFC/`, delete or move the old one out of `apps/` so you only have one `LibreLibre` entry in your menu.
4. On the Flipper, open **Apps -> NFC -> LibreLibre**.

You do not need to flash new firmware onto your Flipper to run this. Just pick the matching `.fap` build (or compile one against your firmware's SDK using the instructions in [BUILD-AND-TEST.md](BUILD-AND-TEST.md)).

## Scanning a sensor

1. Open **LibreLibre**. It starts scanning immediately without making you press OK first. The LED pulses blue while it looks for a tag.
2. Hold the back of the Flipper steady against the sensor.
3. Keep it still for a moment while it reads all 43 FRAM blocks (15 read frames).
4. When the read finishes, the Flipper chimes, writes the capture to the SD card, and shows the result screen.

If you pull the Flipper away mid-read, it will catch the short frame count (`fram_length < 344`) and tell you to rescan instead of showing partial data.

## Reading the result screen

- **Glucose reading (`mmol/L`)**: The large number at the top. This comes from the newest 1-minute trend slot in FRAM (offset 28, 16 slots), never from the 15-minute history ring.
- **`Fresh 0m=now; Left Xd Yh`**:
  - `Fresh` tells you how old the newest trend sample is on the sensor's own minute clock.
  - `Left` is the sensor's maximum lifetime minus its current age (`max_life - age` in minutes), shown in days and hours when over 24 hours remain, or hours and minutes under 24 hours.
- **`Serial 3M...`**: The 11-character sensor serial unpacked from the 8-byte ISO15693 UID and patch family nibble. It is derived from the UID, not stored as ASCII text in FRAM.
- **`Saved + verified on SD`**: The app writes the capture JSON to `/ext/apps_data/libre_research/`, syncs the file, and reads every byte back to make sure the SD write did not corrupt.

## Controls

| Button | What it does |
|---|---|
| **Right** | Start a new scan from the result screen |
| **Left** | Cycle through the main result screen, raw sensor diagnostics (`Age`, `Q` flags, CRC status), and the full 16-hex wire UID |
| **OK** | Retry saving the capture to SD (or start a scan if you previously cancelled to the idle screen) |
| **Back** | Cancel an active scan, or exit the app from the result/idle screen |

## Troubleshooting

### The screen says `Rescan the sensor` / `No usable value`

That means the app refused to show a number for that scan. There are two common reasons:
1. **Positioning / partial read**: The Flipper moved before all 344 bytes finished transferring. Hold the back of the Flipper flat against the sensor and press **Right** to scan again.
2. **Sensor record flag**: The NFC read completed and all three CRCs passed, but the sensor's newest trend slot has `has_error` or nonzero `quality_bits` set (for example, during warm-up, sensor shutdown, or a noisy minute on the sensor). Press **Left** to inspect the diagnostic page (`Age`, `Q` hex, status state). The app will never hide a flagged newest record by showing you an older slot instead.

### The app says API mismatch when launching

Your Flipper firmware API version does not match the `.fap` file you copied. Check which firmware you are running (Official, Unleashed, or Momentum) and grab the matching `.fap` from `dist/`, or run `ufbt` against your firmware version (see [BUILD-AND-TEST.md](BUILD-AND-TEST.md)).

### How close is the reading to LibreLink?

See [LIMITATIONS.md](LIMITATIONS.md). LibreLibre runs the complete instruction-recovered native runtime calibration pipeline (`libcalibrate.so` `0x68a74..0x86908`, verified **68 / 68 (100.00%)** on cold current, **68 / 68 (100.00%)** on warm current, and **2,108 / 2,108 (100.00%)** on 32-slot history series). Because a phone running LibreLink receives 1-minute BLE packets continuously (keeping the warm temporal blend `0x80d2c` active), a single cold NFC tap can still differ by `0.1 to 0.2 mmol/L` during fast glucose changes. Do not use the app on its own to dose insulin.

## Keeping your capture files private

Every completed scan writes a JSON file under `/ext/apps_data/libre_research/cap_<UID>_<tick>_<seq>.json`. Those files contain your sensor's unique hardware UID, derived serial, local timestamps, and raw 344-byte FRAM. Keep them for your own logs or offline debugging, and do not post them publicly.
