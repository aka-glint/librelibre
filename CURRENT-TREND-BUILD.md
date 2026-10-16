# Current-trend app integration and strict build

## Delivered
- LibreLibre app evaluator now calls `lc_vendor_current_trend`, never the history helper `lc_vendor_estimate`.
- Result requires `vendor_chain.available`; factory/history numeric fallback removed. One prominent one-decimal mmol/L number, same-capture freshness/remaining life, derived serial, save state and navigation; no banners.
- Unavailable current result (including empty/error/quality newest) shows a plain Rescan prompt, not an older sample. Left opens diagnostics, then UID details; OK saves and Right rescans. Error paths do not label a nonexistent result current.
- Admission requires complete transcript/capture, supported profile, all three CRCs, parsed bounded indexes/footer, active state, known nonzero life with age strictly below maximum, and clear newest quality/error flags. Historical factory mathematical rejection does not veto legitimate vendor output.
- Trend next-write index is 0..15, zero wraps to slot 15 (matches the existing parser); 16+ rejects. Newest sample minute is sensor age at offset 316, not ADC/record ID. Current value must bind this same age; freshness and remaining life use this same capture. Final current representation is bounded before integer conversion, without fitting.

## Verified execution
All commands were run locally from `.`; no hardware or deployment.

- `sh tests/run.sh > logs/current-trend-tests.log 2>&1`: **exit 0**, unpiped. Full suite, including existing history numerical seam tests unchanged.
- New `tests/test_current_trend_app.py`: **8 tests pass**, normal and ASan/UBSan. Actual app evaluator + draw, not a mirrored evaluator. Covers forbidden history call/factory draw, history mutation invariance, newest mutation changes number, empty/error/quality/flags/index rejection, full integrity/active/lifetime gates (including age==max), factory-only fallback refusal, vendor reference-temperature acceptance despite historical formula rejection, and same-capture life/freshness.
- Existing draw/classification/completion harnesses now execute real vendor evaluation rather than silently drawing factory estimates or stubbing availability. Historical pure numerical tests were not changed.
- Regression observed RED before app changes: evaluator was still calling history; unavailable paths lacked Rescan. During integration full suite caught the obsolete completion stub and obsolete primary-page diagnostics expectations; corrected harness wiring/navigation, not bypassed tests.
- Font check: 5,616 strings, numeric 0.0..555.5 mmol/L, maximum 128 px; widest derived serial row 111 px.
- Clean build then strict build, both **exit 0**:
  ```sh
  UFBT_HOME=.ufbt-unleashed FBT_TOOLCHAIN_PATH=.uid-toolchain ufbt STRICT_FAP_IMPORT_CHECK=1
  ```
- Build APPCHK: **Target 7, API 88.19**. Independently parsed actual ELF `.fapmeta`: API minor=19, major=88, target=7.
- `python3 tests/verify_factory_build.py > logs/current-trend-arm-verification.json`: **exit 0**. Actual ARM table bytes bit-exact, SDK imports checked, double log/pow/floor and double AEABI helpers statically linked. AppState heap 5280 bytes; manifest stack 4096; compiler finish_scan estimate 600 bytes, draw 248 bytes (not runtime high-water evidence).

## Artifact
- Built file: `dist/libre_capture.fap` (menu name LibreLibre).
- Size: **56788 bytes**.
- SHA-256: `881fae09cfec1520147ab708d460840ae9bda3bc9273e1efd001d4ee7215206b`.
- Read-only content-addressed snapshot: `artifacts/current-trend-881fae09cfec1520147ab708d460840ae9bda3bc9273e1efd001d4ee7215206b/LibreLibre.fap`, with `sources.tar.gz`, this report, test/build/ARM evidence and SHA256SUMS. See snapshot manifest for source archive size/hash.

## Numerical limits / open issue
The **current unsmoothed mismatch remains unresolved**. Per the supplied latest M710s LIVE-FRAM-LAYOUT evidence: trend c0 fails closed; c1 is approximately 141.78 mg/dL versus oracle 132 mg/dL. That is not explained or fixed by this app integration. No tuning/fitting or coefficient/layout reinterpretation was performed. Offset 316 is sensor age, layout is the same on both sensor sets, and sample IDs are computed, not ADC. Missing vendor smoothing/admission/state remains an omitted layer, not a proven causal explanation of the residual.

The full suite's old rawin1/rawin2 90.219894/73.207632 history comparisons are **historical arithmetic seams only**, not current-value validation. Multi-family parameters are exercised, but coefficient dependence is not an explanation for the current discrepancy. Current selector aggregate reports 13 usable / 13 rejected of 26 local input entries (the test traverses two directories; not asserted unique captures or full app admission).

The referenced external M710s report/verify21 was supplied as context, not rerun here; it is not present in this local reader tree. Actual local tests/build evidence above is retained. No physical display, NFC reception, SD, runtime memory, clinical correctness or device launch claim.

## Files changed
`app.c`, `application.fam`, `vendor_chain.c`, `vendor_chain.h`, `tests/run.sh`, `tests/test_current_trend_app.py` (new), `tests/test_freshness_ui.py`, `tests/test_status_ui.py`, `tests/test_serial_ui.py`, `tests/test_scan_classification.py`, `tests/test_scan_feedback.py`. Build products and this report/evidence added locally. No hardware state changed.
