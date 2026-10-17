# Building from Source and Running Tests

## Prebuilt `.fap` binaries (`dist/`)

LibreLibre uses standard Flipper Zero SDK APIs (`furi`, `gui`, `canvas`, `nfc`, `iso15693_3_poller`, `storage`, `notification`). There are no custom-firmware-only calls in the source.

Because the Flipper app loader checks the firmware API major version stamped in the `.fap` header, `dist/` includes prebuilt binaries for all three major firmwares. Copy the one that matches your Flipper to `apps/NFC/LibreLibre.fap` on your SD card:

| Firmware | File in `dist/` | Size | Target / API | SHA-256 | MD5 |
|---|---|---|---|---|---|
| **Unleashed (`unlshd-094e`)** | `dist/LibreLibre-unleashed.fap` (`dist/LibreLibre.fap`) | 69,656 B | Target 7, API 88.19 | `bfd09ec07d94a6a720f9f9df1563cf6ce4b62ec781d41a6a9b0b2881d40aaf79` | `4171cb07391fc3bc0f61a66da9c8bc5c` |
| **Official Firmware (OFW 1.5.1)** | `dist/LibreLibre-device.fap` | 69,664 B | Target 7, API 88.2 | `59888c7ac7ed1c744b5418cb55a7bb0069b2efd535d435549619f5bbe70ed4ad` | `a1c22520dd38c8fdeb57acba9b48e203` |
| **Momentum (`mntm-012`)** | `dist/LibreLibre-momentum.fap` | 69,648 B | Target 7, API 87.1 | `e74494da64028fa94772d3d9ec6d0939c7eece8b2ea65a72bb50b417146c2718` | `b945b486175117f21001c3ae451efdc4` |

All three builds pass:
- `STRICT_FAP_IMPORT_CHECK=1` against the target SDK's `api_symbols.csv` (all 70 imported symbols verified).
- Bit-exact IEEE-754 double verification of the 1023-entry `t1` and `t2` calibration tables in `.rodata` (`#define D(x) ((double)x##L)` bypasses uFBT's `-fsingle-precision-constant` flag so double tables are never narrowed through single-precision float).
- Static linking check confirming `log`, `pow`, `floor`, `__aeabi_ddiv`, and `__aeabi_dmul` are linked into the binary.

## Compiling with `ufbt`

If your Flipper is on a newer firmware release and gives an API mismatch error, build a fresh `.fap` in two commands using `ufbt` (`pip install ufbt` or `uv tool install ufbt`):

```sh
# For Official (Stock) Flipper Firmware:
ufbt update --hw-target f7 --channel release
ufbt STRICT_FAP_IMPORT_CHECK=1

# For Unleashed Firmware:
ufbt update --hw-target f7 --index-url=https://up.unleashedflip.com/directory.json --channel release
ufbt STRICT_FAP_IMPORT_CHECK=1

# For Momentum Firmware:
ufbt update --hw-target f7 --index-url=https://up.momentum-fw.dev/firmware/directory.json --channel release
ufbt STRICT_FAP_IMPORT_CHECK=1
```

The built binary lands at `dist/libre_capture.fap`. Copy it to `apps/NFC/LibreLibre.fap` on your Flipper's SD card, or deploy over USB with `ufbt launch`.

## Running the test suite

You only need `gcc` and `python3` on Linux to run the full host and sanitizer test suite:

```sh
sh tests/run.sh
python3 -m unittest discover -s offline/tests -v
```

What `sh tests/run.sh` tests (20 suites, including AddressSanitizer and UndefinedBehaviorSanitizer runs):
- **RF command allowlist (`test_protocol.c`, `audit_source.py`)**: Proves exhaustively across all 16,777,216 3-byte frames and 65,536 read-multiple combinations that only inventory (`26 01 00`), patch info (`02 A1 07`), and bounded 43-block reads (`02 23`) can ever be sent.
- **Cancel and mutex boundary (`test_send_boundary.py`, `test_app_flow.py`)**: Compiles the real `send_read` and `poller_callback` functions from `app.c` against host shims and verifies mutex locking and cancellation behavior.
- **Derived serial unpacking (`test_serial.c`, `test_serial_ui.py`)**: Verifies the 5-bit UID-to-serial packing against published test vectors and checks that `patch[2] >> 4` selects the leading digit (`3M...`).
- **EU2 XOR decrypt and 3-section CRC16 (`test_integrity.py`, `offline/tests/test_decoder.py`)**: Differential-tests C and Python implementations across random and corrupted payloads.
- **Sensor status, freshness, and remaining life (`test_sensor_status.py`, `test_lifecycle.py`, `test_freshness_ui.py`)**: Verifies fail-closed rejection on warm-up, expired, shutdown, CRC failure, and flagged records, plus uint16 overflow protection on `max_life - age`.
- **Screen pixel widths (`test_font_layout.py`)**: Measures every UI string and the full `0.0` to `555.5 mmol/L` numeric range against the real u8g2 font metrics in `tests/font-reference/` to ensure nothing clips past 128 pixels.
- **Calibration math (`test_factory.py`, `test_vendor_chain.py`, `test_vendor_chain_differential.py`, `test_vendor_chain_family_differential.py`)**: Verifies all 1,023 table indices, temperature conversion, least-squares leaf regression, and multi-family coefficient sensitivity.
