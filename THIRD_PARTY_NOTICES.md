# Third-Party Notices and Attribution

LibreLibre's original Flipper Zero application code, offline utilities, and test harnesses are released under the [MIT License](LICENSE). Several components adapt or include material from permissive open-source projects as detailed below.

## 1. DiaBLE (`gui-dos/DiaBLE`) - MIT License

- **Upstream repository:** https://github.com/gui-dos/DiaBLE
- **Pinned revision:** `e6a909c88faeada49f461d30834174cd95db4042`
- **Used in:** `integrity.c`, `factory.c`, `protocol.c` (derived serial bit-packing), `offline/libre2_offline.py`, `offline/reference/`, and `glucose-research/reference/diable-*`.
- **License:** MIT License, Copyright (c) 2026 Guido Soranzio. Full text in [INTEGRITY-LICENSE.md](INTEGRITY-LICENSE.md) and [offline/LICENSE.md](offline/LICENSE.md).

## 2. u8g2 Font Metrics Reference (`tests/font-reference/`) - BSD-2-Clause / Font Notices

- **Upstream source:** Extracted from `DarkFlippers/unleashed-firmware` commit `2f62b3ab` (`lib/u8g2/`) for host-side pixel-width layout verification only (not linked into the Flipper `.fap` binary, which uses the firmware's built-in SDK symbols).
- **Copyright & Notices:**
  - u8g2 library code (`u8g2_font.c`, `u8x8_8x8.c`, `u8g2.h`, `u8x8.h`): Copyright (c) 2016, olikraus@gmail.com, All rights reserved (2-clause BSD license).
  - Extracted font glyph arrays (`fonts.c`): Copyright (c) 1984, 1987 Adobe Systems Incorporated; Copyright (c) 1988, 1991 Digital Equipment Corporation; Copyright sahwar 2009.

## 3. Flipper DateTime Host Test Reference (`tests/datetime-reference/`)

- **Upstream source:** `DarkFlippers/unleashed-firmware` commit `2f62b3ab` (`lib/datetime/datetime.c`, `datetime.h`), used only in `tests/test_timestamps.py` to verify host-side calendar conversion parity.

## 4. Clean-Room Numerical Chain (`vendor_chain.c`, `vendor_tables.c`)

- **Provenance:** Clean-room C re-expression of mathematical stages (ADC scaling, thermistor Steinhart-Hart log-cubic temperature conversion, reciprocal-linear factory calibration, and centered least-squares regression) from interoperability analysis. No proprietary vendor binary, decompiled source code, or GPL code is included in this repository or linked into the `.fap` binary.
