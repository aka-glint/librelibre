# How the Math Works and Known Limitations

I want anyone using or poking around this project to know what the code is doing under the hood, what is verified, and what is still unfinished.

## Why this exists

LibreLibre is an independent NFC tap reader and a second check. It is not a replacement for LibreLink or Juggluco, and it is not a certified medical device.

If your phone dies or you want to see what the sensor is reporting on its own hardware without going through a phone app or cloud account, this gives you a direct read.

## What is implemented on the Flipper

When the Flipper reads the 344-byte FRAM from a standard European Libre 2 sensor (`0xC5` / `0x9D` family), it runs these stages in C (`integrity.c`, `vendor_chain.c`, `vendor_tables.c`, `lifecycle.c`):

1. **Transport and family check**: Verifies all 15 read frames completed (`344/344` bytes, zero transport errors), checks the Texas Instruments UID prefix (`E0 07`), and checks the EU2 patch family and generation bytes.
2. **EU2 XOR decryption and 3-section CRC16**: Decrypts the 43 blocks using the UID and patch info, then checks the stored 16-bit CRCs for the header (`[0..24)`), body (`[24..320)`), and footer (`[320..344)`).
3. **Sensor state and lifetime gate**: Checks that `status_code == 3` (active), `max_life > 0`, and `age < max_life`. Sensors in warm-up, expired, or shutdown states are rejected before any number is shown.
4. **Per-sensor calibration extraction**: Extracts the sensor's own calibration fields from its FRAM header and footer (`i1` 3-bit slope index, `i2` 10-bit table index into the 1023-entry IEEE-754 double `t1`/`t2` tables, `i3` ADC offset, `i4` ADC span, `i5`, and `i6` thermistor reference).
5. **Newest trend record decode and quality gate**: Selects the newest slot in the 16-slot 1-minute trend ring at offset 28 (`(plain[26] + 15) % 16`), unpacks the 14-bit raw ADC, 12-bit temperature code, signed temperature adjustment, error bit, and 9-bit quality mask. If the record is empty or carries any error/quality flag, the app stops and shows `Rescan the sensor`.
6. **Temperature and factory conversion (`lc_vendor_current_trend`)**:
   - Scales raw ADC by `max(0, ADC - i3) * (65.0 / (i4 - i3))`.
   - Converts thermistor resistance to Celsius via the 4-term Steinhart-Hart log-cubic polynomial (`0xdb4c`) and `- 273.15`.
   - Applies temperature gain compensation `pow(1.045, 32.5 - T)` (`0xe6f4`).
   - Applies the reciprocal-linear factory calibration transform (`0xc36c`) using the sensor's `t1`, `t2`, and `slope[i1]`.
   - Rounds to integer `mg/dL` (`40..500` clamp) and `mmol/L` tenths (`floor(mg_dl / 18.0 * 10.0 + 0.5)`).

## Where the numbers differ from LibreLink (and why)

The Flipper port runs the closed per-record calibration chain above, but it **does not run the vendor library's stateful multi-sample history filter** (the 13,664-byte `MESS` / `COMP` / `ATTE` state container used by the Android library across consecutive scans).

What that means in practice:

- In my own side-by-side comparisons in the 9.3 to 9.9 mmol/L range (five paired readings), the Flipper read an average of **-0.34 mmol/L** relative to LibreLink/Juggluco (sample SD **0.114 mmol/L**, range -0.5 to -0.2 mmol/L).
- On an earlier scan taken during a faster glucose change, the gap was larger (`7.6` on the Flipper vs `8.9` on the phone).
- Because the five steady pairs sit in a narrow band (9.3 to 9.9 mmol/L) on a single sensor, they do not prove whether the remaining gap across the full 2 to 25 mmol/L range is a flat offset, a slight ratio difference, or lag from the omitted multi-sample filter.
- **No fudge factor is applied.** I deliberately did not add `+0.34 mmol/L` to the code. Every sensor carries its own calibration indices in FRAM (swapping two real sensors' `i2` configs changes the output by over `3.0 mmol/L`), and hardcoding an offset from a few scans on one sensor would be dishonest engineering.

## Other hard limits

- **Tap-only**: You only get a number when you physically scan the sensor. There are no background Bluetooth readings and no alarms.
- **No wall-clock authentication**: Freshness (`Fresh 0m=now`) and remaining life (`Left Xd Yh`) come from the sensor's own minute counters in FRAM. The small `HH:MM` clock in the corner is just the Flipper's local RTC at scan time.
- **No clinical validation**: Comparing two apps checks how close the math is, not what your actual blood glucose is on a lab analyzer. Do not use this app for insulin dosing or treatment decisions.
