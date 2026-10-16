# Executed offline integrity verification

## Outcome

Working CLI: `offline/libre2_offline.py`.

All four available schema2 capture files validated, passed the recomputed historical EU2/gen1 candidate gate, and decrypted to **header PASS, body PASS, footer PASS**. Each CLI execution exited **0**, no stderr. Raw/encrypted bytes failed all three CRCs, as expected for these encrypted inputs. **No glucose was calculated or reported; this is byte integrity, not clinical validity.**

All four encrypted FRAMs are identical, as are their decrypted outputs. They must not be claimed as four independent scans: repeated saving of the same in-memory capture remains possible. Sequential-read snapshot stability and scan freshness are not established.

| Section | Raw stored | Raw computed | Raw result | Decrypted stored/computed | Decrypted result |
|---|---|---|---|---|---|
| header | e5ec | b326 | FAIL | 2a98 / 2a98 | PASS |
| body | fdd8 | 9932 | FAIL | c53c / c53c | PASS |
| footer | 7cf8 | 1f91 | FAIL | b786 / b786 | PASS |

Raw SHA256: `44f971640f66472ec6e22dd84f795c47703a11c2a733cd373cb00e4e95906212`.

Decrypted SHA256: `425ce554893adcc51ba40e027ac53239c4673abaadc809b050a8e46585dfa522`.

The SHA256 of each input JSON before/after execution matched; inputs were preserved. UID was never printed or recorded in reports. Each output directory is 0700; every report and binary is 0600, verified by actual stat calls. Binary lengths are exactly 344 bytes.

## Exact private outputs

Root: `offline/results/`.

Each of these directories contains `report.json`, `raw.bin`, `decrypted.bin`:

- `cap_011b01aa_00/`
- `cap_011b995e_00/`
- `cap_011cba6d_00/`
- `cap_011cc322_00/`

Full verification record: `offline/results/verification.json` (0600).

Example exact report: `offline/results/cap_011cc322_00/report.json`.

Example exact decrypted bytes: `offline/results/cap_011cc322_00/decrypted.bin`.

## Real automated tests

Command executed: `python -m unittest discover -s tests -v`.

Final actual output: **Ran 9 tests in 0.655s - OK. No skips.** Log: `offline/evidence/final-tests.log`.

Coverage:

- Reference CRC known vector `123456789 -> 89f6`.
- Explicit UInt16 overflow and wire-UID byte order fixtures.
- 100 deterministic synthetic FRAM cases: Python decrypt byte-for-byte matches separately expressed C oracle, and XOR roundtrip recovers original bytes.
- Strict schema2/completeness/length/type/hex/transcript validation with 22 malformed/inconsistent mutations.
- Conservative family gate including C5/9D acceptance, generation bounds, rejection of later/other products and non-EU region, reversed UID rejection.
- Synthetic correct three-section CRC construction, nine isolated boundary/stored/data bit corruptions detected, wrong FRAM length rejection.
- End-to-end CLI tests: synthetic integrity failure exit3 with private files; synthetic integrity pass exit0; input preservation; UID not leaked; directory/file permissions; refused overwrite; rejected duplicate JSON keys without creating output.
- Actual private capture automated validation/decrypt/three CRC checks, C-reference comparison, and input hash preservation.
- 350 deterministic payload lengths (0..349): production reflected CRC + reversal matches direct pinned MSB-first polynomial1021 bit reference.

Strict vertical TDD logs show expected missing-feature FAIL followed by full suite GREEN for each production slice: `evidence/tdd-02-{red,green}.log` wrapping;03 decrypt/C comparison;04 capture/transcript;05 family;06 three sections;07 CLI. Existing CRC slice01 logs were preserved. Final extra tests verify already-existing implementation/reference equivalence, without new production behavior.

## Source provenance / independent comparison limits

All four downloaded reference files were re-fetched from exact pinned raw URLs and compared byte-for-byte successfully. Local SHA256 values match `provenance.json`. Revision: `e6a909c88faeada49f461d30834174cd95db4042`. License: upstream MIT, Copyright (c) 2026 Guido Soranzio; full notices retained in `LICENSE.md` and `reference/LICENSE.md`.

Swift executable was checked and is **not installed**. No large compiler was installed. Available `/usr/bin/cc` compiled `tests/reference.c` with `-std=c99 -Wall -Wextra -Werror`. The C oracle independently expresses the pinned public word/shift/XOR algorithm, shares no Python implementation code, and matched all 100 synthetic cases **and each of the four real decrypted files** byte-for-byte. This is implementation differential evidence, **not** execution of the original Swift application or an independently sourced algorithm.

## Scope and limitations

Only `offline/` files and new private outputs under `offline/results/` were modified/created. No app, build, protocol, capture input, or device modifications/access. No proprietary native libraries. No activation, streaming unlock, BLE work, glucose calculations, physiological interpretation, or treatment inference. CRC consistency does not prove freshness, atomicity, authenticity, or clinical validity. Product identification remains explicitly a candidate, not independently verified.
