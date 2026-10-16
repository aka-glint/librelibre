# Experimental offline EU Libre2 FRAM integrity

**Research byte integrity only. No glucose output, physiology, clinical validity, or treatment use.** Runs offline using Python standard library; no sensor/device, NFC, app, native proprietary library, network, or external dependency is used by the CLI.

## Run

From this directory:

```sh
python libre2_offline.py ../captures/cap_011b01aa_00.json --output-dir results/new-run
python -m unittest discover -s tests -v
```

The output directory must be new; its parent must already exist. Existing files/directories and symlinks are never overwritten. The directory is created 0700; `raw.bin`, `decrypted.bin` (344 bytes each) and `report.json` are 0600 from creation. Inputs are read-only and unchanged. Keep all captures and results private: decrypted FRAM may contain sensitive data even though no physiological interpretation is performed. UID is not printed or put in the report. Binary bytes go only to private files. Stdout is a small experimental/no-glucose JSON summary.

Exit codes: **0** validated/gated capture and all three decrypted CRCs pass; **3** validated/gated capture but one or more decrypted CRCs fail (private outputs still preserved); **2** invalid JSON/schema/transcript, unsupported family, or file/output error. Error JSON is deliberately generic to avoid leaking input or UID. argparse usage errors also exit 2.

## Accepted capture profile

Schema `libre-nfc-research-capture`, integer version 2, `capture_kind=conservative_raw_fram`, `complete=true`, transport error integer 0, FRAM expected/actual length 344, patch length 6, no glucose. Hex is strict unspaced even-length hexadecimal. Duplicate JSON keys, nonfinite constants, mistyped booleans/integers, truncated FRAM, and mismatched declared lengths fail closed. Descriptive timestamps/app metadata are not relied on for integrity or freshness.

UID must be eight bytes in exact `ISO15693 inventory wire LSB-first` order; bytes 6/7 must be 07/E0 (TI). **Do not reverse wire UID.** Family is recomputed rather than trusting capture claims. Only patch marker C5/9D, region byte 01, and historical generation nibble `(high=3, low<9) OR (high=7, low<4)` are accepted. This is a conservative historical EU2/gen1 **candidate** gate, not verified product identification. C6/7F/2C/2B, other families/regions, and later generations are rejected; no Libre2+ or US14day fallback exists.

Transcript validation intentionally accepts only the current conservative capture profile: exactly one inventory `260100` response (flags, DSFID, same UID), patch `02A107` response (flags, exact patch), and 15 ordered read-multiple commands `0223` covering blocks 0..42 in groups of three with the last group one block. Every response has success flags byte 00, zero transport error, exact recorded/expected length, and concatenates to the exact FRAM. A different legitimate transcript batching profile is rejected rather than guessed. SDK link CRC was removed; this validates recorded bytes and cross-field consistency, **not original RF/link CRC, authentication, read freshness, or an atomic stable snapshot**.

## Exact algorithm and integrity

MIT public DiaBLE revision `e6a909c88faeada49f461d30834174cd95db4042`, verified byte-for-byte; see `provenance.json` and `reference/`. `prepareVariables` uses little-endian pairs UID[4:6], [2:4], [0:2], UInt16 wrap, and constant 241A XOR 14C6. `processCrypto` is the exact shifts/XOR chain with A0C5/6860. The EU2 path takes little-endian patch[4:6] XOR 0044 and emits four little-endian XOR key words for each of 43 blocks. XOR is reversible; roundtrip alone is not proof, so tests also compare a separately written C oracle and actual CRCs.

CRC: init FFFF, reflected 8408, final 16-bit reversal; equivalent to upstream MSB-first polynomial1021 consuming each byte LSB-first. `123456789` -> 89F6. No link-CRC substitution or final XOR. Stored CRC words are little-endian. Half-open ranges:

| Section | Stored | CRC data |
|---|---|---|
| header | [0,2) | [2,24) |
| body | [24,26) | [26,320) |
| footer | [320,322) | [322,344) |

Raw and decrypted checks are both reported. Only all three **decrypted** CRCs passing gives exit0. No heuristic skips decryption or forces success. CRCs detect byte corruption, not malicious forgery or clinical validity.

## Verification limits

See `TEST-RESULTS.md`, `evidence/tdd-*-{red,green}.log`, and private `results/verification.json`. Swift is not installed; a small separate C oracle (compiled with warnings-as-errors) was used instead. It independently expresses the same pinned public algorithm, not a second independently sourced algorithm. The four real files have identical encrypted/decrypted bytes; do **not** count them as independent sensor scans. No device, app, capture, protocol, or build files were modified.

## License

Python adaptation and C oracle retain DiaBLE copyright and MIT license in `LICENSE.md` / `reference/LICENSE.md`. The reference code cites earlier LibreTools work; this implementation's verified provenance is specifically the pinned DiaBLE files, not an unverified claim about earlier origins.
