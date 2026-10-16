# Code Layout and Research Notes

If you just want to install and use the app, read the [README](../README.md) and [User Guide](USER-GUIDE.md). This page is a map of the source files for anyone reading the code.

## Source files

- `app.c`: Main Flipper application loop, GUI viewport draw/input callbacks, persistent ISO15693 polling, LED/chime feedback, and verified SD card save/readback.
- `protocol.c` / `protocol.h`: Strict RF command allowlist (`26 01 00`, `02 A1 07`, and `02 23` blocks `0..42`), TI UID check, EU2 family gate, hex helpers, and 5-bit UID-to-serial unpacking (`lc_serial_from_uid`).
- `transaction.c` / `transaction.h`: Thread-safe commit/cancel gate between the UI thread and the NFC poller worker.
- `capture.c` / `capture.h`: Transcript verification and JSON serializer for `/ext/apps_data/libre_research/cap_*.json`.
- `integrity.c` / `integrity.h`: EU2 FRAM XOR decryption, 3-section CRC16 (`0x8408` reflected, bit-reversed output), and sensor metadata parser (`status_code`, `age_minutes`, `max_life_minutes`, ring indices, newest trend slot flags).
- `vendor_chain.c` / `vendor_chain.h` / `vendor_tables.c` / `vendor_tables.h`: Recovered per-record calibration chain (`lc_vendor_current_trend`), thermistor log-cubic temperature conversion, temperature gain, reciprocal-linear calibration transform, and centered least-squares regression helper.
- `factory.c` / `factory.h`: Shared 1023-entry IEEE-754 double `t1`/`t2` calibration tables and historical factory formula helper (retained for the SD JSON log and differential tests; never used as a screen fallback).
- `lifecycle.c` / `lifecycle.h`: Sensor-clock freshness and remaining sensor lifetime formatting.
- `offline/`: Standalone Python CLI (`libre2_offline.py`) for decrypting and checking saved `cap_*.json` files on a computer.
- `glucose-research/`: Reference parsers and notes on how the printed sensor serial is derived from the UID ([`SERIAL-IN-FRAM.md`](../glucose-research/SERIAL-IN-FRAM.md)).

## Engineering notes

- [CURRENT-TREND-BUILD.md](../CURRENT-TREND-BUILD.md): Details on wiring `lc_vendor_current_trend` into `app.c` and removing historical fallback paths.
- [CURRENT-STATUS-AND-CORRECTIONS.md](../CURRENT-STATUS-AND-CORRECTIONS.md): Summary of what is verified, what earlier hypotheses were ruled out or corrected during reverse engineering, and what remains open in the stateful smoothing layer.
