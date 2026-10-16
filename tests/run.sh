#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p logs
ulimit -c 0
gcc -std=c11 -Wall -Wextra -Werror -pedantic -I. tests/test_protocol.c protocol.c capture.c -o tests/host_tests
./tests/host_tests
gcc -std=c11 -Wall -Wextra -Werror -pedantic -g -fsanitize=address,undefined -fno-omit-frame-pointer -I. tests/test_protocol.c protocol.c capture.c -o tests/host_tests_san
ASAN_OPTIONS=detect_leaks=1 ./tests/host_tests_san
gcc -std=c11 -Wall -Wextra -Werror -pedantic -I. tests/test_serial.c protocol.c capture.c -o tests/host_serial
./tests/host_serial
gcc -std=c11 -Wall -Wextra -Werror -pedantic -g -fsanitize=address,undefined -fno-omit-frame-pointer -I. tests/test_serial.c protocol.c capture.c -o tests/host_serial_san
ASAN_OPTIONS=detect_leaks=1 ./tests/host_serial_san
python3 tests/test_send_boundary.py
SANITIZE=1 python3 tests/test_send_boundary.py
python3 tests/test_raw.py
python3 tests/test_app_flow.py
SANITIZE=1 python3 tests/test_app_flow.py
python3 tests/test_scan_persistence.py
python3 tests/test_scan_feedback.py
SANITIZE=1 python3 tests/test_scan_feedback.py
python3 tests/test_autostart.py
SANITIZE=1 python3 tests/test_autostart.py
python3 tests/test_scan_classification.py
SANITIZE=1 python3 tests/test_scan_classification.py
python3 tests/test_save_feedback.py
SANITIZE=1 python3 tests/test_save_feedback.py
python3 tests/audit_source.py
python3 tests/test_integrity.py
SANITIZE=1 python3 tests/test_integrity.py
python3 tests/test_sensor_status.py
SANITIZE=1 python3 tests/test_sensor_status.py
python3 tests/test_status_ui.py
SANITIZE=1 python3 tests/test_status_ui.py
python3 tests/test_serial_ui.py
SANITIZE=1 python3 tests/test_serial_ui.py
python3 tests/test_factory.py
SANITIZE=1 python3 tests/test_factory.py
python3 tests/test_timestamps.py
python3 tests/test_lifecycle.py
python3 tests/test_freshness_ui.py
python3 tests/test_current_trend_app.py
SANITIZE=1 python3 tests/test_current_trend_app.py
python3 tests/test_font_layout.py
# RECOVERED vendor raw-to-estimator chain (bounded port). Double and float paths,
# record decode, config extraction, temperature, regression, host differential.
python3 tests/test_vendor_chain.py
python3 tests/test_vendor_chain_differential.py
# REQUIRED multi-family differential: fixture family AND live family on the same
# records. A fixture-only pass must never again stand in for the general case.
python3 tests/test_vendor_chain_family_differential.py

rm -f tests/host_tests tests/host_tests_san tests/host_serial tests/host_serial_san
