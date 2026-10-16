# LibreLibre offline glucose feasibility

## Outcome

**A public factory-coefficient/temperature formula exists and is implemented as an offline research reference. It is not the complete current Abbott algorithm, and the available sensor data must not produce a glucose reading.** No app integration, deployment, device/phone commands, sensor actions, activation, streaming change, lifetime change or dosing guidance was performed. Project source changes are confined to `glucose-research/`.

The actual capture decodes, with all three section CRCs passing, to **status 5 (`shutdown`)**, sensor age **21,348 minutes**, stored maximum life **20,628 minutes**, and a newest trend record with **hasError=true** and quality bits **0x100 (SENSOR_SIGNAL_LOW in DiaBLE)**. The age exceeds stored maximum life by **720 minutes**. This is a rejection case, not a usable active-sensor calibration fixture. CRC success is not sensor health, authenticity, currentness or glucose validity.

Four schema-2 capture files contain one identical decoded payload. They do not establish four independent scans, a live advancing sensor, or a fresh measurement. The two older schema-1 files are deliberately rejected by the existing strict decoder; they are not silently accepted as additional evidence.

## Evidence and scope

- DiaBLE revision: `e6a909c88faeada49f461d30834174cd95db4042` (MIT).
- Juggluco revision: `a983788a17a35f8e76d9da5c125996b648b4f27a` (GPL-3.0-or-later source snapshots, no Juggluco code incorporated into application).
- The new snapshots, hashes and exact raw URLs are in `source-manifest.json`, `additional-source-manifest.json`, `juggluco-extra-manifest.json`; all 14 reference files are hash-bound in `PROVENANCE.json`.
- Existing pinned DiaBLE `../offline/reference/Libre.swift` supplies parsing and CRC. Its SHA-256 is recorded in `../offline/provenance.json`.
- Real decoded fixture SHA-256: `425ce554893adcc51ba40e027ac53239c4673abaadc809b050a8e46585dfa522`, exactly 344 bytes. Private UID is never emitted by the new audit.
- Native target: Irish LibreLink 2.13.1 / versionCode 11596, ARM64 `libDataProcessing.so`, SHA-256 `5ebf0458983ed9467a1bee6089b3f508d9dfb483f6b8f37e4033fc7b5d8d6bc7`.
- Prior M710s evidence reports were read and copied locally as `reference/native-{NATIVE,PARSER}-FINDINGS.md`. No new decompilation or target-library execution was required: public source resolved the missing historical formula. Read-only `readelf` on M710s supplied `evidence/native-elf.txt` (SHA-256 `30574b879cea9030bae520c1bbc7d43c3ac06467aee7a8fcea59e372f9e1e7d8`).

### Source map

| Question | Pinned evidence |
|---|---|
| Sensor classification | `../offline/reference/Libre.swift:8-29,46-57`; observed patch profile `C5 09 30 01 00 00`, historical EU2/gen1 candidate, not a universal product proof |
| Sensor status enum | `reference/diable-Sensor.swift:77-98` |
| Status, age, trend ring and raw fields | `../offline/reference/Libre.swift:116-145` |
| Maximum life and factory coefficients | `../offline/reference/Libre.swift:179-194` |
| Bit reader and all-three CRC ranges | `../offline/reference/Libre.swift:247-261,315-328` |
| Formula call chain | `reference/diable-Sensor.swift:133-136` -> `reference/diable-Glucose.swift:165-454`; `reference/diable-MainDelegate.swift:270-294,336-338` uses factoryTrend |
| Formula limitation | **`reference/diable-README.md:19-23`: algorithm derived from old LibreLink 2.3; later smoothing and trend projection not understood** |
| Dangerous implicit fallback to avoid | `reference/diable-Glucose.swift:133-145,167-169`: initializer sets raw/10 and guard failure returns that raw object. Our implementation raises instead and never exports it as glucose. |
| Juggluco binary requirement | `reference/juggluco-README.md:26-33`: `libcalibrat2.so` and/or **`libcalibrate.so`**, not `calibration.so`, must be extracted from architecture-specific APK directories |
| Native loading | `reference/juggluco-libre2.cpp:227-303`: dlopen, exported JNI methods or JNI_OnLoad with emulated VM; `:834-940` initialization |
| Native scan inputs/state | `reference/juggluco-libre2.cpp:306,370-421`: parser constant, product-family query, timestamps, configurations, UID/patch/scan, composite/attenuation/measurement state, Out objects, token |
| JNI compatibility layer | `reference/juggluco-jnisub.cpp:162-168,556-558,639 onward` builds substitute JNI/VM interfaces; not a self-contained scalar formula |
| Separate optional user calibration | `reference/juggluco-calibrateValue.hpp:25-37`, `reference/juggluco-calculate.cpp:23-63`: time-weighted a*value+b over already computed sensor values, not factory ADC conversion |

## Implemented parser: raw diagnostics only

`raw_parser.py` requires exactly 344 decrypted bytes, exact-six patch info, historical C5/9D + `30 01` standard EU2 profile, matching footer profile, **all three CRCs**, and bounded trend/history indexes. This scope is intentionally narrower than every family the upstream supports.

It extracts status, little-endian sensor age, maximum life, raw trend/history ring indexes, sixteen newest-first six-byte trend records, and i1..i6 factory fields. It does **not** parse history measurements or invent timestamps.

For trend record index i:

- slot = `(trend_index - 1 - i) mod 16`, offset = `28 + 6*slot`;
- raw ADC: bits 0..13;
- quality: lower nine of bits 14..24; remaining two bits preserved separately;
- hasError: bit 25;
- raw temperature: bits 26..37 shifted left two;
- temperature adjustment: bits 38..46 shifted left two, negated by bit 47;
- sensor-relative minute = age-i, or null before sensor start. `within_sensor_age` is only this index check, not a validity flag.

Factory extraction: i1/i2 are header byte2 bits 0/3 with widths 3/10. At footer byte336: i3 is eight bits with sign bit33, i4 starts bit8 width14, i5 starts bit40 width12 <<2, i6 starts bit52 width12 <<2. Actual coefficients are source-consistent, but their presence alone does not validate conversion.

The newest real record is source-consistent with raw ADC 14, raw temperature 9636, adjustment -16, quality 256 and hasError set. These are **raw research diagnostics, not mg/dL or mmol/L**. They remain absent from any Flipper glucose UI. The offline audit suppresses raw ADC and all numerical glucose candidates.

## Smallest reproducible public formula path

`factory_reference.py` implements the exact pinned tables and scalar equation from DiaBLE `factoryGlucose`. `reference-oracle.cpp` is a separately expressed C++ numerical oracle with literal upstream t1/t2 arrays. Both derive from the **same public source**, not independent medical reference data. Swift itself was unavailable on this host; no claim of executing upstream Swift is made.

Given raw ADC G, raw temperature Traw, signed adjustment A, coefficients and tables:

1. `R = Traw * 72500 / (A + i6) - 1000`.
2. `L = ln(R)`.
3. `temperature = 1 / (0.00000005283566*L^3 + 0.0000007061775*L^2 + 0.0001964561*L + 0.0009180023) - 273.15`.
4. `g1 = 65 * (G-i3) / (i4-i3)`; `g2 = 1.045^(32.5-temperature)`.
5. `candidate = (g1*g2-t1[i2-1]) / t2[i2-1]`, rounded as upstream for accepted positive values.

The upstream formula does not use i1/i5; they are preserved, not guessed into a new equation. Both tables have 1,023 entries. Exact double literals occupy **16,368 bytes** before code/other data. Do not replace the irregular tables with a guessed exponential or repeated pattern. This source-derived scalar route needs no Android shared library, network or sensor reconfiguration. It is the smallest evidenced **historical factory conversion candidate**, not a reproduction of LibreLink 2.13.1's complete calibrated/filtering/attenuation/projection algorithm.

The Python reference verifies the source SHA before parsing tables; rejects missing/out-of-range coefficients, invalid arithmetic inputs, pre-start samples, hasError/nonzero/unknown quality, and nonpositive/nonfinite candidates; never falls back to raw/10. Every returned numerical candidate still explicitly says `glucose_available=false` and `clinical_validity_claimed=false`. It is deliberately not called by `audit.py` or the application. There is no current-reading interface or treatment path.

For a genuine future on-device port, the compact equation/tables could be reimplemented in portable C in isolation, with double-vs-float and rounding differential tests, actual SDK math imports/linkage, stack/heap/latency measurements, and independently paired reference fixtures. **Those resource/performance/clinical gates are not demonstrated by this Python/C++ host artifact.**

## Native path and portability constraints

The native path remains the route to the current proprietary complete algorithm, not a discovered open scalar implementation. Juggluco's loading shim and shared-library requirements are visible; a public wrapper does not make the binary algorithm portable or remove licensing/redistribution constraints.

Measured **LibreLink ARM64 binary**, not measurements of absent Juggluco-named libraries:

| Resource | Static observation |
|---|---:|
| File bytes | 5,486,936 |
| Sum of PT_LOAD memory sizes | 9,236,240 |
| Sum of writable PT_LOAD memory sizes | 3,779,776 |
| Architecture | ELF64 AArch64 DYN |
| Dynamic dependencies | liblog.so, libc.so, libm.so, libdl.so |
| Previously recovered input state lengths | composite 32; attenuation 320; measurement 13,664 bytes; total 14,016 |

These are static segment sizes and helper-level length checks, **not measured peak runtime RAM, heap, stack, first-scan bootstrap state or universal state schemas**. Native state contents, initialization/version behavior, required configuration/token semantics and quality/actionability remain unresolved. Null handling in a JNI wrapper is not proof that an empty-state scan produces a legitimate reading.

Flipper's official hardware specification documents a 32-bit Cortex-M4 at 64 MHz, 1,024 KB shared flash and 256 KB shared SRAM: <https://docs.flipper.net/zero/development/hardware/tech-specs>. The existing ARM64 Android DSO cannot load directly on that MCU: incompatible instruction set/ABI, JNI/Android dependencies, and writable segment sizes already exceeding total SRAM. Putting it on SD does not solve working-memory or execution compatibility. Juggluco lists armv7 and x86/x86_64 variants too, but none is shown to target Cortex-M4/Thumb/Furi; no Juggluco DSO was downloaded, measured or executed in this pass. Any host-native bridge would violate standalone Flipper independence and would still need legitimate state/bootstrap and reference validation.

## What tests actually prove

Run from `.`:

```sh
python3 -m unittest discover -s glucose-research -p 'test_*.py' -v
python3 -m unittest discover -s offline/tests -v
python3 glucose-research/audit.py captures/cap_011*.json
```

The audit pattern deliberately selects the four existing schema-2 fixtures, not the older schema-1 files. Explicit unsupported files cause fail-closed rejection, not subset success. The audit has no sensor/device/network I/O and no calibrated-candidate call.

New behavior followed red -> green cycles: absent parser, early-record negative time, absent coefficient extraction, absent formula, invalid input/quality acceptance, absent audit, and nonpositive formula candidate. Persisted cycle logs are under `evidence/tdd-*`; the initial parser/early-record failures are retained in this task's tool transcript rather than a saved log. Subsequent differential tests extend coverage without claiming that already implemented behavior initially failed.

- New suite: real metadata/coefficient fixture, all-three corruption rejection, malformed length/profile/index rejection, reserved status handling, pre-start index handling, upstream readBits-vs-packed raw field comparison, no-fallback error paths, real-capture abstention.
- Formula differential: **all 1,023 table indexes** on explicitly synthetic positive vectors, signed adjustments and signed i3, checking temperature, unrounded numerical candidate and rounding against the separately compiled same-source C++ oracle.
- Synthetic example evaluates to a numerical candidate of 134; **this is constructed test data and not Connor's glucose**.
- Existing offline decoder regression: **9 tests pass**, including actual-capture decryption/C oracle and integrity.
- `evidence/actual-capture-audit.json` is a real executed audit of the four complete captures and contains **no glucose candidate**.
- No physical sensor scan, app display, on-device parser, RF freshness behavior or clinical accuracy was tested here. No FAP was built or deployed in this pass.

## Concrete blockers and stopping point

1. **Current capture is unusable for glucose**: shutdown + beyond stored life + error/SIGNAL_LOW. Do not clear flags, alter life, search older valid-looking slots and label them current, or infer freshness from CRC.
2. **No independent authorized paired reference** is present with these fixtures. No numerical comparison against LibreLink/reader output for the same sample and timestamp was possible. Same-source C++ agreement is software translation evidence only.
3. **No independently established sample time/currentness**: sequential full-memory reads are not atomic, four identical payloads do not prove live updates, and the device RTC is labeled plausibility-only. `sample_age_seconds` stays null and `freshness` stays unestablished. Upstream's synthetic date from lastReadingDate must not be mistaken for a sample timestamp.
4. **Historical formula != current full algorithm**: pinned DiaBLE explicitly documents unknown later smoothing/trend projection. The newer native algorithm's attenuation/actionability/state/bootstrap have not been reconstructed.
5. **No MCU qualification**: mathematical source availability does not establish SDK linkage, numeric fidelity, resource limits, error UI, stale-cache behavior or emergency reliability on Flipper.

The appropriate next engineering fixture is a complete, coherent, read-only capture from an already active supported sensor with legitimate paired reference value/error/status and timing evidence, obtained through an authorized existing reader workflow. It is an external data dependency, not permission to activate, pair, extend, reset, write or suppress an error. This pass stops at a tested offline parser + historical formula reference + explicit abstention; **LibreLibre is not yet a validated emergency glucose display**.

## Issues encountered

No local `<research-workspace>` directory existed; the known M710s path was reachable and the reports were retrieved read-only. An initial guessed Juggluco `LICENSE` URL returned 404; the repository root showed `LICENSE.txt`, which was retrieved. An initial broad audit glob correctly failed on the two older schema-1 files; the reproducible command now explicitly selects schema-2 fixtures. Swift was not installed; the numerical cross-check uses C++ and is labeled accordingly. No missing clinical reference was fabricated.
