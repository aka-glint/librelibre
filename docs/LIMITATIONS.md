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
6. **Native runtime calibration and multi-horizon estimator (`lc_vendor_current_trend`, `lc_vendor_extract_fram_series`, `0x68a74..0x86908`)**:
   - Converts thermistor resistance to Celsius via the 4-term Steinhart-Hart log-cubic polynomial (`0x853b8..0x8542a`) and averages valid temperatures over a clipped window (`0x85534`: radius 3 on trend, radius 1 on history).
   - Smooths non-rejected trend ADC values with a 5-point moving average (`0x86908`, radius 2), scales by `(raw - i3) * 65.0f * 1000.0f / (i4 - i3)`, and applies temperature gain `pow(1.045, 32.5 - T_gain)`.
   - Applies the runtime factory parameter block at `0xa1900` (`0x68a74 -> 0x8b26f`) to compute `c18c` and `c188` (`G[i] = sig[i] / c18c + c188`).
   - Determines exact sample minute `cg_id = ((raw_age & 0xF) == plain[26]) ? raw_age : (raw_age - 1)` (`0x8ac84`) and history de-pack shift `phase = ((cg_id % 15) <= 1) ? 1 : 0` (`0x881e0`).
   - Smooths the 32 history records with 3-window OLS fits (`backward`, `centered`, `forward`) plus the midpoint (`0x71f50..0x7308c`).
   - Projects 4 physiological-lag rate horizons (`P_hist` on history lags `0, 15, 30`; `P0` on trend lags `0, 6, 15`; `P1` on trend lags `0, 2, 7`; `P2` on trend lags `0, 4, 12`) and combines them with weights `[AE990[mod15], 91, 91, 91, 10]` (`0x75950..0x765a4`), and evaluates the 3-horizon history series estimator across all 31 committed history slots (`0x77a00..0x79550`).
   - Adds bi-exponential sensor-age drift `corr(id) = (pow(1.0013889074325562, -id) - pow(1.0025640726089478, -id)) * 80.0` (`0x81fc0`), applies the power-6 rational display curve + `-4.0` offset (`0x765ec, 0x7717a`), and rounds to integer `mg/dL` and `mmol/L` tenths.
7. **Direct 1-minute BLE decryption and multi-horizon calibration (`libre2_ble.c`, `lc_vendor_calibrate_ble_packet`)**:
   - Verifies our C `ll_libre2_build_ble_login_payload()` and `lc_integrity_ble_enable_cmd()` bit-for-bit against the vendor `dpGetStreamingUnlockPayload` and `dpGetEnableStreamingPayload` vectors.
   - Decrypts incoming 46-byte `0xF002` BLE notifications, verifies the packet CRC16, and runs all 7 BLE trend samples (`lags {0, 2, 4, 6, 7, 12, 15}`) and 3 BLE history samples (`lags {0, 15, 30}`) through the exact same runtime factory calibration, windowed temperature smoothing, 4-horizon rate projection (`P0, P1, P2, P_hist`), bi-exponential drift, and display curve.

## Verification against the native Oracle (and Cold vs Warm state)

Across all **68 chronological sensor captures / 136 warm–cold replays** against the unmodified native `libcalibrate.so` Oracle:
- **Cold Current Glucose (`0x7717a`):** **68 / 68 (100.00%) exact**
- **Warm Current Glucose (`0x80d2c -> 0x7717a`):** **68 / 68 (100.00%) exact**
- **32-Slot History Glucose Series (`0x7951a`):** **2,108 / 2,108 (100.00%) exact**
- **Why a cold single-tap NFC scan can still differ by 0.1–0.2 mmol/L from a phone during fast changes:** A phone running LibreLink receives a BLE packet every 1 minute (`dt = 1`), keeping `libcalibrate.so`'s warm temporal blend (`0x80d2c`, `K_AE970[1] = 105`, `46.7%` previous state + `53.3%` current estimate) active continuously. A single stateless NFC tap evaluates the cold current estimate (`100%` current estimate), which matched within `0.2 mmol/L` (`5.4 vs 5.6 mmol/L` while dropping `-1.0 mg/dL/min`) on live hardware verification.
- **Zero empirical fitting**: Every constant comes directly from disassembled `libcalibrate.so` instructions and read-only tables (`0xa1900`, `0xae970..0xaec60`).

## Other hard limits

- **Read-only by default**: Standard NFC scans only send the 17 read commands (`26 01 00`, `02 A1 07`, `02 23`). Step 18 (`02 A1 07 1E` BLE pairing) only runs if you explicitly toggle `BLE Pair: ON` (`Up` on the Flipper scan screen) to pair an ESP32 watch/companion.
- **No clinical validation**: Comparing two apps checks how close the math is, not what your actual blood glucose is on a lab analyzer. Do not use this project on its own for insulin dosing or treatment decisions.
