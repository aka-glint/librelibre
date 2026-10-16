# Current Status and Engineering Notes

Updated 2026-10-16. This file tracks what is verified in the current code, what we learned from side-by-side testing, and which early reverse-engineering theories were corrected along the way.

## 1. Current builds (`dist/`)

The application code is compiled and verified against all three major Flipper Zero firmwares (`target 7`):

- **Official Firmware (OFW 1.5.1, API 88.2)**: `dist/LibreLibre-ofw.fap` (`dist/LibreLibre.fap`), 56,756 bytes, SHA-256 `4ceb9b0c7aa0629cf704c675ca7664aa6cd9e27101d05a1627e73ea9cae6b5dd`
- **Unleashed (`unlshd-094`, API 88.19)**: `dist/LibreLibre-unleashed.fap`, 56,748 bytes, SHA-256 `e0005aae1260760898675a539b2e643b695615a9b066e6f54016fc95ddf0054c`
- **Momentum (`mntm-012`, API 87.1)**: `dist/LibreLibre-momentum.fap`, 56,756 bytes, SHA-256 `c8853fb840e1cad45f52e9e87111aad8153debf69afe026c3403c75279ac930c`

What the app runs on a scan:
- Calls `lc_vendor_current_trend` on the newest 1-minute trend record (offset 28, 16 slots, indexed by `(plain[26] + 15) % 16`).
- Uses the scanned sensor's own calibration fields (`i1..i6`, `t1`, `t2`) from FRAM header/footer.
- Checks completeness (344 bytes), EU2 profile, all 3 section CRCs, active sensor state (`status == 3`), valid remaining life (`0 < age < max_life`), and zero error/quality flags on the newest trend record.
- Displays the single `mmol/L` reading, sensor-clock freshness (`Fresh 0m=now`), remaining sensor life (`Left Xd Yh`), derived 11-character serial, and SD save verification.
- Shows `Rescan the sensor` whenever any check fails. There is no fallback to the 15-minute history ring or to older trend slots.

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
