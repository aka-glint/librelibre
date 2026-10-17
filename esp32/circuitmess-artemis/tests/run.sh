#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
gcc -std=c11 -Wall -Wextra -Werror -pedantic -g -fsanitize=address,undefined \
    -Isrc tests/test_bridge.c src/bridge.c src/nfc_io.c src/libre2_ble.c src/scan_history.c src/protocol.c src/capture.c \
    src/integrity.c src/factory.c src/vendor_chain.c src/vendor_tables.c \
    src/lifecycle.c -lm -o tests/test_bridge_runner
./tests/test_bridge_runner
rm -f tests/test_bridge_runner
python3 tools/send_capture.py --synthetic --dry-run > /dev/null
echo "PASS tools/send_capture.py synthetic dry-run"
