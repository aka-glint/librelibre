# Current Status and Engineering Notes

Updated 2026-10-17. This file tracks what is verified in the current code, the complete instruction-recovered native runtime calibration closure (`libcalibrate.so` `0x68a74..0x86908`), and which early reverse-engineering theories were corrected along the way.

## 1. Current builds (`dist/`)

The application code is compiled and verified against all major Flipper Zero firmwares (`target 7`):

- **Unleashed (`unlshd-094e`, API 88.19)**: `dist/LibreLibre-unleashed.fap` (`dist/LibreLibre.fap`), 69,656 bytes, MD5 `4171cb07391fc3bc0f61a66da9c8bc5c`, SHA-256 `bfd09ec07d94a6a720f9f9df1563cf6ce4b62ec781d41a6a9b0b2881d40aaf79` (flashed and verified live on Flipper Spark)
- **Official Firmware (OFW 1.5.1, API 88.2)**: `dist/LibreLibre-device.fap`, 69,664 bytes, MD5 `a1c22520dd38c8fdeb57acba9b48e203`, SHA-256 `59888c7ac7ed1c744b5418cb55a7bb0069b2efd535d435549619f5bbe70ed4ad`
- **Momentum (`mntm-012`, API 87.1)**: `dist/LibreLibre-momentum.fap`, 69,648 bytes, MD5 `b945b486175117f21001c3ae451efdc4`, SHA-256 `e74494da64028fa94772d3d9ec6d0939c7eece8b2ea65a72bb50b417146c2718`

What the app runs on a scan:
- Calls `lc_vendor_current_trend` and `lc_vendor_extract_fram_series`, executing the complete instruction-recovered native runtime pipeline (`libcalibrate.so` `0x68a74..0x86908`):
  1. Windowed thermistor temperature smoothing (`0x85534`: radius 3 on trend, radius 1 on history) and 5-point trend ADC moving average (`0x86908`).
  2. Runtime factory parameter block (`VMA 0xa1900`, `0x68a74 -> 0x8b26f`) producing `c18c` and `c188` (`G[i] = sig[i] / c18c + c188`, `6,516 / 6,516 = 100.00%` bit-exact).
  3. Exact sample minute `cg_id = ((raw_age & 0xF) == plain[26]) ? raw_age : (raw_age - 1)` (`0x8ac84`) and history de-pack phase `phase = ((cg_id % 15) <= 1) ? 1 : 0` (`0x881e0`).
  4. 3-window OLS + midpoint history smoother (`0x71f50..0x7308c`).
  5. 4-horizon physiological-lag rate projections (`0x7d8b2`, `0x75950..0x79550`) over trend lags `{0, 2, 4, 6, 7, 12, 15}` and history lags `{0, 15, 30}`.
  6. Bi-exponential sensor-age baseline drift (`0x81fc0`) and power-6 rational display curve + `-4.0` offset (`0x765ec, 0x7717a`).
- Verified **68 / 68 (100.00%) exact** on cold current, **68 / 68 (100.00%) exact** on warm current, and **2,108 / 2,108 (100.00%) exact** on 32-slot history series against the native Oracle with zero empirical fitting.
- Live hardware verification on Flipper Spark (`2026-10-17T22:08:58`, `age = 6234 min`): read `5.4 mmol/L` (`98.47 mg/dL = 5.47 mmol/L`) vs `5.6 mmol/L` on LibreLink while falling `-1.0 mg/dL/min`.
- Deployed live across Flipper Zero and CircuitMess Artemis Watch (`aka-glint/librelibre-esp32-artemis`), and synced to Waveshare ESP32-S3-Touch-LCD-2 and Heltec WiFi LoRa 32 V3.

## 2. Side-by-side comparison notes

Across five user-reported side-by-side comparisons in the 9.3 to 9.9 mmol/L reference range, the difference (`Flipper - Reference`) averaged **-0.340 mmol/L** (sample SD **0.114 mmol/L**, range **-0.5 to -0.2 mmol/L**).

Important context on those numbers:
- Those five pairs were typed side-by-side in a narrow glucose band (9.3 to 9.9 mmol/L) on one sensor. They are descriptive numbers, not a full-range accuracy spec.
- Earlier comparisons included a `5.1` vs `5.5` mmol/L pair (`-0.4`) and a larger `7.6` vs `8.9` mmol/L (`-1.3`) disagreement. All reported pairs are kept on the record; the `-1.3` pair is not thrown out as an "outlier" because independent scan-to-phone timestamp binding was not recorded for it.
- No offset or gain tweak has been fitted to these pairs.

## 3. Corrections to earlier investigation notes

While tracing the calibration math, several early hypotheses turned out to be wrong once tested against real bytes. They are listed here so nobody wastes time chasing them again:

1. **Trend ring vs history ring**: An early build read from the 32-slot history ring at offset 124 instead of the 16-slot trend ring at offset 28, which caused an overshoot (`8.4` vs `7.3 mmol/L`) when glucose dipped. Also, offset `316` in FRAM is the 16-bit sensor age in minutes, not a measurement record. Both public fixtures and live EU2 sensors use the exact same ring layout.
2. **Per-sensor coefficients (`i2`) matter hugely**: Public test fixtures use `i2 = 866` (`t1 = 0.1072988, t2 = 1.5`). A live sensor used `i2 = 551` (`t1 = 0.0742513, t2 = 1.25`). Feeding one sensor's record through another sensor's `i2` coefficients shifts the output by over **3.0 mmol/L** (`47 mg/dL`). Always extract the config from the scanned FRAM.
3. **Double precision is required upstream**: While the centered least-squares regression tail is safe in 32-bit `float`, the upstream thermistor log-cubic (`log`) and temperature gain (`pow(1.045, 32.5 - T)`) stages use IEEE-754 `double`, and uFBT passes `-fsingle-precision-constant` by default. All double constants and the 1023-entry `t1`/`t2` tables must use explicit `L` suffixes cast to `double` (`#define D(x) ((double)x##L)`), verified against the compiled ARM `.rodata` section.
4. **What remains unported**: The C code ports the closed per-record calibration chain. It does not port the vendor library's stateful `MESS` (13,664 B) / `COMP` (32 B) / `ATTE` (320 B) multi-sample history filter. Static interpreter probes and small warm-vs-cold tests on two old captures do not bound the full behavior of that stateful layer across all glucose rates of change. Closing that remaining gap requires full def-use recovery of the stateful consumer against capture-bound oracle runs.
