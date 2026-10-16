# Contributing and Reporting Issues

If you run into a bug or test LibreLibre on another sensor or firmware version, issues and pull requests are welcome.

## Keeping personal data out of issues

**Never upload your `cap_*.json` files, your sensor's 16-hex UID, or your 11-character sensor serial to a public GitHub issue.** Those files contain unique hardware identifiers and raw sensor memory.

If you are reporting a bug, include:
- Your Flipper firmware name and version (for example, Official 1.5.1, Unleashed 094, or Momentum 012).
-Which `.fap` file you installed (or how you built it).
- Your sensor type and region (for example, EU Libre 2).
- What showed up on the screen (and on the Left diagnostic page, such as the `Q` flags or status state, without your UID/serial).

If you are comparing numbers against LibreLink or Juggluco, note both readings and whether your glucose was steady or moving fast when you scanned.

## Code rules for pull requests

1. **Read-only RF policy stays locked.** Do not add any NFC write, activation, reset, or custom command outside the existing 3-command read allowlist in `protocol.c`.
2. **Fail closed.** If a scan is partial, fails CRC, comes from an unsupported sensor state, or has error/quality flags on the newest trend record, the screen must show `Rescan` rather than falling back to an older slot.
3. **No arbitrary offsets or fudge factors.** Do not add hardcoded constants to make the output match a few readings on one sensor. Any change to `vendor_chain.c` has to be backed by verifiable math and tested across multiple coefficient families.
4. **Run the tests before submitting:**
   ```sh
   sh tests/run.sh
   python3 -m unittest discover -s offline/tests -v
   ```
