# LibreLibre

Hold your Flipper Zero against a standard European FreeStyle Libre 2 sensor and get a glucose reading, sensor age, and remaining life on the screen. No phone, no internet connection, no account, and no server in the loop.

I built this because I wanted an independent, offline reader I could use as an alternative check and a backup when my phone is dead, out of reach, or acting up. It is **not a replacement for LibreLink** (or Juggluco). A phone gives you continuous Bluetooth readings and alarms while you sleep. This is a manual tap reader you can keep in your pocket, inspect yourself, and run without trusting a black box.

## What it does

When you open the app, it starts scanning right away. Hold the back of the Flipper against the sensor and it will:

1. Read the 344-byte FRAM over ISO15693 NFC (read-only commands only).
2. Decrypt the payload and check all three section CRCs (header, body, footer).
3. Pull the sensor's own factory calibration values out of the FRAM and run the newest trend record through the recovered temperature and calibration math.
4. Show the glucose reading in **mmol/L**, how fresh the sample is on the sensor's clock, how many days or hours are left on the sensor, and the sensor's derived serial number.
5. Save the raw capture as JSON on the SD card (`/ext/apps_data/libre_research/`) and read it back to confirm the write succeeded.

If the scan is incomplete, the CRCs fail, the sensor is expired or shut down, or the newest trend record has a sensor error flag set, the app does not guess or fall back to an older reading. It just tells you to **Rescan**.

## Install it on your Flipper

You do not need custom firmware to use this. The C code uses standard Flipper SDK calls and works on Official (Stock) Firmware, Unleashed, and Momentum.

Because the Flipper app loader checks the firmware API number stamped into every `.fap` file, I built and included a binary for each firmware line in `dist/`. Pick the one that matches what your Flipper is running:

| Your Flipper Firmware | File to download | Target / API | Size | SHA-256 |
|---|---|---|---|---|
| **Official / Stock Firmware (OFW 1.5.1)** | [`dist/LibreLibre-ofw.fap`](dist/LibreLibre-ofw.fap) (or [`dist/LibreLibre.fap`](dist/LibreLibre.fap)) | Target 7, API 88.2 | 57,736 B | `4ceb9b0c7aa0629cf704c675ca7664aa6cd9e27101d05a1627e73ea9cae6b5dd` |
| **Unleashed (`unlshd-094`)** | [`dist/LibreLibre-unleashed.fap`](dist/LibreLibre-unleashed.fap) | Target 7, API 88.19 | 57,744 B | `e0005aae1260760898675a539b2e643b695615a9b066e6f54016fc95ddf0054c` |
| **Momentum (`mntm-012`)** | [`dist/LibreLibre-momentum.fap`](dist/LibreLibre-momentum.fap) | Target 7, API 87.1 | 57,744 B | `c8853fb840e1cad45f52e9e87111aad8153debf69afe026c3403c75279ac930c` |

### Steps

1. Grab the `.fap` for your firmware from the table above.
2. Open qFlipper (or put your Flipper's microSD card in a reader) and copy that file onto the SD card as **`apps/NFC/LibreLibre.fap`**.
3. On the Flipper, go to **Apps -> NFC -> LibreLibre**.
4. Hold the Flipper steady against your sensor until it chimes.

More detail on scanning, buttons, and troubleshooting is in [docs/USER-GUIDE.md](docs/USER-GUIDE.md).

## What you see on the screen

- **Top right (`HH:MM` or `--:--`)**: The Flipper's clock when the tag was read.
- **Big number (`X.Y mmol/L`)**: The glucose value calculated from the newest 1-minute trend slot, using that sensor's own calibration fields.
- **`Fresh 0m=now; Left Xd Yh`**: Freshness comes straight from the sensor's internal minute counter, not wall-clock math. Remaining life is `max_life - age` from the sensor's FRAM.
- **`Serial 3M...`**: The 11-character sensor serial derived from the tag UID and family nibble, so you can confirm which sensor you scanned.
- **Bottom line**: Confirms the capture was written and verified on the SD card.

**Buttons on the result screen:**
- **Right**: Scan again.
- **Left**: Flip between the main reading, raw sensor diagnostics (`Age`, `Q` bits, CRC state), and the full 16-hex wire UID.
- **OK**: Retry saving to the SD card if the card failed on the first try.
- **Back**: Cancel an active scan or exit the app.

## What this is and what it isn't (Limitations)

I want to be completely straight about what works and where the gaps are:

1. **Standard European Libre 2 only.** It is built and tested against standard EU Libre 2 sensors (`0xC5` / `0x9D` family). It does **not** support Libre 2+, Libre 3, or US sensors. Those use different security and protocols.
2. **It is an alternative check, not a replacement for your CGM app.** There is no Bluetooth streaming, no background tracking, and no high/low alarms.
3. **The number can sit slightly below LibreLink.** The Flipper runs the closed per-sensor factory calibration and temperature chain on the newest trend record, but it skips the vendor library's 13.6 KB multi-sample history state blob. On my own side-by-side checks in the 9.3 to 9.9 mmol/L range, the Flipper read about 0.3 to 0.4 mmol/L lower than LibreLink/Juggluco on average (mean -0.34 mmol/L, SD 0.11 across five paired scans, plus a larger gap during a fast swing). I did not slap a fake offset onto the code to hide that gap. Every sensor carries its own factory coefficients in FRAM, and forcing a flat fudge factor from one sensor would be bad engineering.
4. **Strict read-only.** The RF allowlist permits only three ISO15693 read commands: inventory (`26 01 00`), patch info (`02 A1 07`), and bounded 43-block FRAM reads (`02 23`). There is no code in this repo to activate a sensor, reset it, extend its life, or alter its Bluetooth state. Scanned sensors keep working normally with LibreLink and Juggluco.
5. **Not medical advice or a certified medical device.** I built this for learning, verification, and having a tool that works when nothing else is around. Do not use it on its own to dose insulin.
6. **Keep your SD captures private.** The JSON files saved in `/ext/apps_data/libre_research/` contain your sensor's UID, derived serial, and raw FRAM bytes. Do not upload your `cap_*.json` files to public GitHub issues.

Full write-up on the math and known limits: [docs/LIMITATIONS.md](docs/LIMITATIONS.md).

## ESP32 / Heltec V3 version and GPIO UART output

After every completed scan, the Flipper also outputs a CRC16-checked ASCII frame (`LL2|<16-hex-uid>|<12-hex-patch>|<688-hex-fram>|<4-hex-crc16>\r\n`) at `115200 8N1` on GPIO Pin 13 (`TX`) / Pin 14 (`RX`).

The standalone ESP32-S3 port for the **Heltec WiFi LoRa 32 V3** (128x64 SSD1306 OLED) runs the exact same C decryption, CRC16, sensor status, and calibration chain (`vendor_chain.c`, `integrity.c`, `factory.c`, `lifecycle.c`, `protocol.c`, `nfc_io.c`) on the ESP32 and renders the same screen layout on the Heltec OLED (over GPIO UART from the Flipper, USB Serial, or BLE NUS).

## Technical details and building from source

If you just want to use the app, you can stop reading here. If you want to see how the FRAM is parsed, how the calibration math works, or how to compile it yourself:

- [User guide and troubleshooting](docs/USER-GUIDE.md)
- [How the math works and where the limits are](docs/LIMITATIONS.md)
- [Building from source and running the test suite](docs/BUILD-AND-TEST.md)
- [Code layout and research notes](docs/RESEARCH.md)
- [Current status and engineering corrections](CURRENT-STATUS-AND-CORRECTIONS.md)
- [Offline Python FRAM decoder (`offline/`)](offline/README.md)
- [How the serial number is packed in the UID (`glucose-research/SERIAL-IN-FRAM.md`)](glucose-research/SERIAL-IN-FRAM.md)

## License

MIT License. See [LICENSE](LICENSE), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), and [INTEGRITY-LICENSE.md](INTEGRITY-LICENSE.md) for attribution to Guido Soranzio's DiaBLE project (MIT) for the EU2 FRAM XOR decrypt, CRC16, and calibration table structure.
