"""Static safety regression checks, NOT simulated NFC/device execution."""
from pathlib import Path
import re
root = Path(__file__).resolve().parents[1]
app = (root / "app.c").read_text()
assert app.count("iso15693_3_poller_send_frame(") == 1
assert "nfc_poller_start_ex(app->poller, poller_callback, app)" in app
assert not re.search(r"\bnfc_poller_start\s*\(", app)
assert not re.search(r"\biso15693_3_poller_(activate|read_block|read_blocks|write_block)\s*\(", app)
assert "if(!lc_command_allowed(command, command_length))" in app
assert "FSOM_CREATE_NEW" in app and "FSOM_CREATE_ALWAYS" not in app
assert "storage_file_sync(file)" in app
assert "memcmp(json, verified, length) == 0" in app
assert "if(created && !saved) storage_common_remove(storage, path)" in app
assert "app->attempts >=" not in app and "SCAN_SOFT_LIMIT_MS" not in app
assert "nfc_poller_stop(app->poller)" in app
assert app.index("nfc_poller_stop(app->poller)") < app.index("nfc_poller_free(app->poller)")
assert app.index("gui_remove_view_port(gui, app->viewport)") < app.index("view_port_free(app->viewport)")
for resource in ("nfc_free(app->nfc)", "bit_buffer_free(app->rx)", "bit_buffer_free(app->tx)", "furi_message_queue_free(app->keys)", "furi_mutex_free(app->mutex)", "furi_record_close(RECORD_GUI)", "furi_record_close(RECORD_STORAGE)"):
    assert resource in app
for callback in ("draw", "input", "poller_callback"):
    body = app.split("static " + ("NfcCommand " if callback == "poller_callback" else "void ") + callback + "(", 1)[1].split("\n}\n", 1)[0]
    assert "storage_" not in body
    assert "furi_delay" not in body
print("PASS static audit: sole RF allowlist; extended poller prevents implicit memory reads")
assert "bits % 8" not in app
assert app.count("lc_transaction_cancel(&app->gate)") == 2
assert "app->cancel" not in app
send = app.split("static Iso15693_3Error send_read(", 1)[1].split("\n}\n", 1)[0]
assert send.index("furi_mutex_acquire") < send.index("lc_transaction_commit") < send.index("furi_mutex_release") < send.index("iso15693_3_poller_send_frame")
for before in app.split("lc_transaction_cancel(&app->gate)")[:-1]:
    assert before.rfind("furi_mutex_acquire(app->mutex") > before.rfind("furi_mutex_release(app->mutex")
print("PASS static audit: persistent inventory / single custom query; no write/activate/escalation path")
print("PASS static audit: commit/cancel share short mutex; SDK I/O runs unlocked")
print("PASS static audit: exclusive file create; sync + exact readback; cleanup ownership")
print("PASS static audit: stop/join before free; GUI unregister; records/buffers released")
print("LIMITATION: static checks do not prove physical runtime, RF behaviour or SD operation")
assert 'furi_hal_speaker_' not in app
assert 'furi_record_close(RECORD_NOTIFICATION)' in app
assert 'furi_record_open(RECORD_NOTIFICATION)' in app
for callback in ('draw', 'input', 'poller_callback'):
    kind = 'NfcCommand' if callback == 'poller_callback' else 'void'
    body = app.split(f'static {kind} {callback}(', 1)[1].split('\n}\n', 1)[0]
    assert 'notification_message' not in body and 'save_capture(' not in body
stop = app.split('static void stop_scan(', 1)[1].split('\n}\n', 1)[0]
assert stop.index('sequence_blink_stop') < stop.index('nfc_poller_stop')
assert stop.index('sequence_reset_rgb') < stop.index('nfc_poller_stop')
main = app.split('int32_t libre_capture_app(', 1)[1]
assert main.count('finish_scan(app)') == 1
assert 'else if(app->page == PageResult) save_capture(app)' in main
cancel = main.split('if(app->page == PageScan) {', 1)[1].split('} else running', 1)[0]
assert 'stop_scan(app)' in cancel and 'save_capture' not in cancel and 'finish_scan' not in cancel
print('PASS feedback audit: main-thread autosave/manual retry; no storage/notification in callbacks; notification record closed; blink cleared before SDK join')
