# Code Layout and Research Notes

If you just want to install and use the app, read the [README](../README.md) and [User Guide](USER-GUIDE.md). This page is a map of the source files for anyone reading the code.

## Source files

- `app.c`: Main Flipper application loop, GUI viewport draw/input callbacks, persistent ISO15693 polling, LED/chime feedback, and verified SD card save/readback.
- `protocol.c` / `protocol.h`: Strict RF command allowlist (`26 01 00`, `02 A1 07`, and `02 23` blocks `0..42`), TI UID check, EU2 family gate, hex helpers, and 5-bit UID-to-serial unpacking (`lc_serial_from_uid`).
- `transaction.c` / `transaction.h`: Thread-safe commit/cancel gate between the UI thread and the NFC poller worker.
- `capture.c` / `capture.h`: Transcript verification and JSON serializer for `/ext/apps_data/libre_research/cap_*.json`.
- `integrity.c` / `integrity.h`: EU2 FRAM XOR decryption, 3-section CRC16 (`0x8408` reflected, bit-reversed output), and sensor metadata parser (`status_code`, `age_minutes`, `max_life_minutes`, ring indices, newest trend slot flags).
- `vendor_chain.c` / `vendor_chain.h` / `vendor_tables.c` / `vendor_tables.h`: Complete instruction-recovered native runtime calibration pipeline (`libcalibrate.so` `0x68a74..0x86908`, verified **68/68** cold current, **68/68** warm current, and **2,108/2,108** history series against the native Oracle):
  - `lc_vendor_current_trend()`: Evaluates current glucose (`mg/dL` and `mmol/L` tenths) from a decrypted 344-byte FRAM.
  - `lc_vendor_extract_fram_series()`: Extracts the 47-point (`31` committed history + `16` trend) chronological graph series from a 344-byte FRAM.
  - `lc_vendor_calibrate_ble_packet()`: Evaluates 1-minute 46-byte Libre 2 BLE stream packets across all 7 trend lags `{0, 2, 4, 6, 7, 12, 15}` and 3 history lags `{0, 15, 30}` using the exact same runtime factory coefficients (`0x8b26f`), windowed temperature smoothing (`0x85534`), 5-point trend ADC average (`0x86908`), 4-horizon rate projections (`0x7d8b2`), bi-exponential sensor-age drift (`0x81fc0`), and low-glucose rational curve (`0x765ec, 0x7717a`).
- `lc_fram_decode.c` / `lc_fram_decode.h`: Standalone raw 344-byte FRAM ring unpacker implementing `cg_id = ((raw_age & 0xF) == plain[26]) ? raw_age : (raw_age - 1)` (`0x8ac84`) and `phase = ((cg_id % 15) <= 1) ? 1 : 0` (`0x881e0`).
- `lc_live_pipeline.c` / `lc_live_pipeline.h`: Standalone C reference implementation of the working-series producer (`0x86908 / 0x85534 / 0x8543a`), runtime factory slope/offset (`0x8b26f`), 3-window OLS + midpoint history smoother (`0x71f50..0x7308c`), 4-horizon current/history estimators (`0x75950..0x79550`), warm temporal blend (`0x80d2c`), and display curve (`0x765ec, 0x7717a`).
- `graph_bridge.c` / `graph_bridge.h`: Bridges `lc_vendor_extract_fram_series()` into the 2D graph points array on Flipper and ESP32.
- `esp32/circuitmess-artemis/`: Complete CircuitMess Artemis Watch firmware (`src/main.cpp`, `src/libre2_ble.c`, `tools/artemis_companion.html`), documented in [ARTEMIS-WATCH.md](ARTEMIS-WATCH.md).
- `factory.c` / `factory.h`: Shared 1023-entry IEEE-754 double `t1`/`t2` calibration tables and historical factory formula helper (retained for the SD JSON log and differential tests; never used as a screen fallback).
- `lifecycle.c` / `lifecycle.h`: Sensor-clock freshness and remaining sensor lifetime formatting.
- `offline/`: Standalone Python CLI (`libre2_offline.py`) for decrypting and checking saved `cap_*.json` files on a computer.
- `glucose-research/`: Reference parsers and notes on how the printed sensor serial is derived from the UID ([`SERIAL-IN-FRAM.md`](../glucose-research/SERIAL-IN-FRAM.md)).

## Engineering notes

- [CURRENT-TREND-BUILD.md](../CURRENT-TREND-BUILD.md): Details on wiring `lc_vendor_current_trend` into `app.c` and removing historical fallback paths.
- [CURRENT-STATUS-AND-CORRECTIONS.md](../CURRENT-STATUS-AND-CORRECTIONS.md): Summary of what is verified, what earlier hypotheses were ruled out or corrected during reverse engineering, and what remains open in the stateful smoothing layer.
