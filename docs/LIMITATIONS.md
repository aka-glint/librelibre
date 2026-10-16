# How the Math Works and Known Limitations

I want anyone using or poking around this project to know what the code is doing under the hood, what is verified, and what is still unfinished.

## Why this exists

LibreLibre is an independent NFC tap reader, display, and direct Bluetooth companion for standard European FreeStyle Libre 2 sensors. It is not a certified medical device.

If your phone dies or you want to see what the sensor is reporting on your own hardware (a Flipper Zero, a CircuitMess Artemis watch, a Waveshare S3 touchscreen, or a Heltec V3 OLED) without going through a phone app or cloud account, this gives you a direct read and a live 1-minute Bluetooth stream.

## What is implemented across Flipper Zero and ESP32

When LibreLibre reads the 344-byte FRAM from a standard European Libre 2 sensor (`0xC5` / `0x9D` family), it runs these stages in C (`integrity.c`, `vendor_chain.c`, `vendor_tables.c`, `lifecycle.c`, `libre2_ble.c`):

1. **Transport and family check**: Verifies all 15 read frames completed (`344/344` bytes, zero transport errors), checks the Texas Instruments UID prefix (`E0 07`), and checks the EU2 patch family and generation bytes.
2. **EU2 XOR decryption and 3-section CRC16**: Decrypts the 43 blocks using the UID and patch info, then checks the stored 16-bit CRCs for the header (`[0..24)`), body (`[24..320)`), and footer (`[320..344)`).
3. **Sensor state and lifetime gate**: Checks that `status_code == 3` (active), `max_life > 0`, and `age < max_life`. Sensors in warm-up, expired, or shutdown states are rejected before any number is shown.
4. **Per-sensor calibration extraction**: Extracts the sensor's own calibration fields from its FRAM header and footer (`i1` 3-bit slope index, `i2` 10-bit table index into the 1023-entry IEEE-754 double `t1`/`t2` tables, `i3` ADC offset, `i4` ADC span, `i5`, and `i6` thermistor reference).
5. **Newest trend record decode, 48-point FRAM series extraction, and quality gate**: Selects the newest slot in the 16-slot 1-minute trend ring at offset 28 (`(plain[26] + 15) % 16`), unpacks the 14-bit raw ADC, 12-bit temperature code, signed temperature adjustment, error bit, and 9-bit quality mask, and extracts up to 48 historical + trend points (`8` hours) for the on-device 2D graph. If the newest record is empty or carries any error/quality flag, the app stops and shows `Rescan`.
6. **Temperature and factory conversion (`lc_vendor_current_trend`)**:
   - Scales raw ADC by `max(0, ADC - i3) * (65.0 / (i4 - i3))`.
   - Converts thermistor resistance to Celsius via the 4-term Steinhart-Hart log-cubic polynomial (`0xdb4c`) and `- 273.15`.
   - Applies temperature gain compensation `pow(1.045, 32.5 - T)` (`0xe6f4`).
   - Applies the reciprocal-linear factory calibration transform (`0xc36c`) using the sensor's `t1`, `t2`, and `slope[i1]`.
   - Rounds to integer `mg/dL` (`40..500` clamp) and `mmol/L` tenths (`floor(mg_dl / 18.0 * 10.0 + 0.5)`).
7. **Direct 1-minute BLE decryption and calibration (`libre2_ble.c`)**:
   - Verifies our C `ll_libre2_build_ble_login_payload()` and `lc_integrity_ble_enable_cmd()` bit-for-bit against the vendor `dpGetStreamingUnlockPayload` and `dpGetEnableStreamingPayload` vectors.
   - Decrypts incoming 46-byte `0xF002` BLE notifications, verifies the packet CRC16, and runs the newest raw BLE sample (`bytes[0..6]`) through the same per-sensor factory calibration config extracted from FRAM.

## Where the numbers differ from LibreLink (and why)

LibreLibre runs the closed per-record calibration chain above, but it **does not run the vendor library's stateful multi-sample history filter** (the 13,664-byte `MESS` / `COMP` / `ATTE` state container used by the Android library across consecutive scans).

What that means in practice from my own side-by-side checks against LibreLink/Juggluco:

- **Mid-range (9.3 to 9.9 mmol/L reference, five paired scans)**: LibreLibre read `9.1 to 9.4 mmol/L` vs `9.3 to 9.9 mmol/L` on LibreLink (difference `-0.5 to -0.2 mmol/L`, mean **-0.34 mmol/L**, sample SD `0.114 mmol/L`).
- **Low-range (4.6 to 5.4 mmol/L reference, five paired scans)**:
  - `5.0 mmol/L` LibreLibre vs `5.4 mmol/L` LibreLink (`-0.4 mmol/L`)
  - `4.9 mmol/L` LibreLibre vs `5.4 mmol/L` LibreLink (`-0.5 mmol/L`)
  - `4.0 mmol/L` LibreLibre vs `4.6 mmol/L` LibreLink (`-0.6 mmol/L`)
  - `3.9` and `4.0 mmol/L` LibreLibre vs `4.6 mmol/L` LibreLink (`-0.7` and `-0.6 mmol/L`)
  Across the five low-range pairs, the difference is `-0.7 to -0.4 mmol/L` (mean **-0.56 mmol/L**, median `-0.6 mmol/L`).
- **Fast glucose swing**: On an earlier scan taken during a rapid change, the gap was larger (`7.6` vs `8.9 mmol/L`, `-1.3 mmol/L`).
- **What the low-range pairs tell us**: If the gap were a simple multiplicative gain or slope error, the `-0.34 mmol/L` difference at `9.5 mmol/L` would shrink in half to roughly `-0.17 mmol/L` at `4.6 mmol/L`. Instead, the low-range difference stays around `-0.4 to -0.7 mmol/L` (matching the `-8.66 mg/dL` = `-0.48 mmol/L` same-byte C-minus-warm-oracle gap measured in `NUMERICAL-CLOSURE-AUDIT.md`). That points to an additive baseline/background term or the omitted multi-sample dynamic filter rather than a simple slope multiplier.
- **No fudge factor is applied**: I deliberately did not add `+0.4` or `+0.5 mmol/L` to the code. Every sensor carries its own calibration indices in FRAM (swapping two real sensors' `i2` configs changes the output by over `3.0 mmol/L`), and hardcoding a flat offset from one sensor would be bad engineering.

## Other hard limits

- **Read-only by default**: Standard NFC scans only send the 17 read commands (`26 01 00`, `02 A1 07`, `02 23`). Step 18 (`02 A1 07 1E` BLE pairing) only runs if you explicitly toggle `BLE Pair: ON` (`Up` on the Flipper scan screen) to pair an ESP32 watch/companion.
- **No clinical validation**: Comparing two apps checks how close the math is, not what your actual blood glucose is on a lab analyzer. Do not use this project on its own for insulin dosing or treatment decisions.
