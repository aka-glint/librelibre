# Bluetooth Streaming and NFC Capture on Flipper Zero and ESP32

A standard European FreeStyle Libre 2 sensor has two radios inside it:

1. **ISO15693 NFC (13.56 MHz)**: Passive tag memory (`344` bytes of encrypted FRAM). It contains the sensor's factory calibration values (`i1..i6`), a 16-slot 1-minute trend ring (`15` minutes), and a 32-slot 15-minute history ring (`8` hours).
2. **Bluetooth Low Energy (`0xFDE3` service)**: Wakes up once every 60 seconds, connects to its paired reader, accepts a 12-byte encrypted login on characteristic `0xF001`, and pushes a 46-byte encrypted 1-minute reading over characteristic `0xF002`.

By default, every NFC scan in LibreLibre is strictly read-only (`17` ISO15693 commands). A read-only scan leaves your phone's Bluetooth connection completely untouched.

If you want to stream 1-minute Bluetooth readings directly to an ESP32 watch or display (without a phone in the middle), you need two things from NFC first:

1. One full 344-byte FRAM capture so the ESP32 has that sensor's factory calibration parameters (`i1..i6`) and 8-hour history seed.
2. Optional **Step 18 (`02 A1 07 1E`)**, the ISO15693 BLE enable command, which writes a 32-bit unlock code (`enable_time`) into the sensor and returns the sensor's 6-byte Bluetooth MAC address.

Here are the two ways to do that: capturing NFC with a Flipper Zero, or wiring an ISO15693 NFC module straight to an ESP32.

---

## Why the NFC `patch_info` bytes matter for Bluetooth

Every time you scan a Libre 2 sensor over NFC, command #2 (`02 A1 07`) returns 6 bytes of `patch_info` (for example `C5 09 30 01 XX XX`).

On European Libre 2 sensors, **bytes 4 and 5 of `patch_info` change on every single NFC scan**. Those two bytes are XORed with `0x44` (`t = ((info[5] << 8) | info[4]) ^ 0x44`) and used in two places:

1. Decrypting the 344-byte FRAM read during that specific NFC scan.
2. Building the 12-byte BLE login payload written to `0xF001` (`ll_libre2_build_ble_login_payload()` in `libre2_ble.c`).

The sensor only updates its stored Bluetooth key when Step 18 (`02 A1 07 1E`) runs during an NFC scan. That means:

- A normal 17-command read-only NFC scan changes `patch_info[4..5]` for that FRAM read, but does **not** change the sensor's Bluetooth key.
- An 18-command BLE-pairing NFC scan locks that specific scan's 6-byte `patch_info` and `enable_time` into the sensor's Bluetooth state.
- Your ESP32 BLE client **must** build its 12-byte `0xF001` login payload using the exact 6-byte `patch_info` and `enable_time` from the scan that ran Step 18. If your phone later scans the sensor with Bluetooth pairing enabled, the sensor's active key moves back to the phone until you pair again.

---

## Path 1: Capture NFC with Flipper Zero and bridge to ESP32

This is the zero-soldering setup if you already carry a Flipper Zero and an ESP32 watch or board (`esp32/circuitmess-artemis` or `esp32/waveshare-s3-lcd2`).

### Step 1: Scan on the Flipper

1. Open **Apps -> NFC -> LibreLibre** on the Flipper.
2. For a normal read-only check (leaving your phone's Bluetooth alone), just hold the Flipper against the sensor.
3. To pair the sensor's Bluetooth to your ESP32 instead, press **Up** on the scan screen before tapping so the header shows **`BLE Pair: ON`**, then hold the Flipper against the sensor until it chimes.

When `BLE Pair: ON` is enabled, the Flipper runs the normal 17 read commands and then sends Step 18:

```text
02 A1 07 1E <4B enable_time LE> <4B usefulFunction(uid, 0x1e, enable_time)>
```

The sensor replies with its 6-byte Bluetooth MAC (`ble_paired_mac`), which the Flipper saves in `/ext/apps_data/libre_research/cap_<UID>_<tick>.json` alongside `"ble_unlock_code": 42`.

### Step 2: Send the capture and bond to the ESP32

You can pass the capture and BLE bond to the ESP32 in three ways:
- **Web Bluetooth / Web Serial Companion Dashboard (`esp32/circuitmess-artemis/tools/artemis_companion.html`)**: Open the HTML dashboard in Chrome/Edge, connect to the Artemis Watch over Web Bluetooth (`Bangle.js Artemis` NUS) or Web Serial (`115200`), and drag-and-drop your `cap_<UID>_<tick>.json` file into the **Libre 2 Sensor Bridge** card. It computes the 344-byte FRAM CRC16 (`lcIntegrityCrc`) and pushes both `LL2|` and `LL2_BLE|` to the watch automatically.
- **Automated Python USB Bridge (`esp32/circuitmess-artemis/tools/flipper_bridge_sync.py` & `upload_flipper_to_watch.py`)**: Automatically detects the attached Flipper Zero, pulls the newest `cap_*.json` from `/ext/apps_data/libre_research/`, computes the CRC16, and pushes `LL2_BLE|` and `LL2|` to the Artemis Watch over USB.
- **Direct GPIO UART (`115200 8N1` on Flipper Pin 13 `TX` / Pin 14 `RX`) or manual USB Serial (`115200`)**:

1. **Full FRAM frame (`LL2|`)**: Seeds the sensor's factory calibration (`i1..i6`), current reading, and 48-point (`8h`) history graph into flash (`/spiffs/last_frame.txt` and `/spiffs/scans.csv`):
   ```text
   LL2|<16-hex-uid>|<12-hex-patch>|<688-hex-fram>|<4-hex-crc16>
   ```
2. **BLE bond line (`LL2_BLE|`)**: Sets the active pairing `patch_info`, target 6-byte BLE MAC, `enable_time` (`42`), and starting monotonic `unlock_count`, then saves the bond to `/spiffs/ble_bond.txt` (or `/sdcard/libre_research/ble_bond.txt` on Waveshare):
   ```text
   LL2_BLE|<16-hex-uid>|<12-hex-patch>|<12-hex-mac>|<unlock_code>|<unlock_count>
   ```

When each 46-byte encrypted `0xF002` notification arrives every 60 seconds, `ll_libre2_evaluate_ble_packet()` in `libre2_ble.c` decrypts the 46 bytes, verifies the CRC16, unpacks all **7 trend samples** (`lags {0, 2, 4, 6, 7, 12, 15}`) and **3 history samples** (`lags {0, 15, 30}`), and calls `lc_vendor_calibrate_ble_packet()` in `vendor_chain.c` to run the full native runtime calibration pipeline (`0x68a74..0x86908`).

NOTE: On ESP32-S3 boards using the built-in USB-Serial/JTAG controller (`circuitmess-artemis`), the hardware RX FIFO is 64 bytes. When sending a 727-byte `LL2|` line over USB from a script, send it in 16-byte chunks with 15 ms spacing so the hardware FIFO never drops characters.

---

## Path 2: Wire an ISO15693 NFC reader directly to an ESP32

If you want your ESP32 board to scan the sensor directly without a Flipper Zero, all three ESP32 projects (`esp32/circuitmess-artemis`, `esp32/waveshare-s3-lcd2`, `esp32/heltec-v3`) already include the complete ISO15693 master state machine in `src/nfc_io.c` and `src/nfc_io.h`.

### Supported NFC transceiver chips

The Libre 2 tag is an **ISO/IEC 15693 (NFC-V, 13.56 MHz)** tag with Texas Instruments manufacturer code `0x07` (`UID` MSB `E0 07 ...`).

- **Use**: `PN5180` (SPI), `ST25R3916` / `ST25R3911B` (SPI/I2C, the same family inside the Flipper Zero), or `TRF7970A` (SPI).
- **Do NOT use `PN532` or `RC522`**: Those are ISO14443A/MIFARE chips and cannot read ISO15693 / NFC-V Libre sensors.

### How `src/nfc_io.c` works

`src/nfc_io.h` defines a single hardware-agnostic callback for sending one raw ISO15693 frame (without SOF/EOF/CRC16, which your NFC chip's hardware CRC engine handles) and receiving the tag's response bytes (starting with the 1-byte ISO15693 response flags `0x00`):

```c
typedef bool (*LlNfcTransceiveFn)(
    const uint8_t* tx,
    size_t tx_len,
    uint8_t* rx,
    size_t rx_capacity,
    size_t* rx_len_out,
    void* user_ctx);
```

When you pass your SPI/I2C transceiver's callback into `ll_nfc_poll_once(transceive_fn, ctx, &capture)`, `nfc_io.c` runs the exact 17-command read sequence:

1. **Step 0 (`26 01 00`)**: Single-slot Inventory (`flags = 0x26`, command `0x01`, mask length `0x00`). Parses the 8-byte wire UID (`lc_uid_parse`).
2. **Step 1 (`02 A1 07`)**: TI custom Get Patch Info (`flags = 0x02`, command `0xA1`, vendor `0x07`). Parses the 6-byte `patch_info` (`lc_patch_parse`) and checks `lc_family_allowed()` before touching FRAM.
3. **Steps 2..15 (`02 23 <start_block> 02`)**: 14 Read Multiple Blocks commands (`3` blocks = `24` bytes each, blocks `0x00..0x29`).
4. **Step 16 (`02 23 2A 00`)**: Final Read Multiple Blocks command (`1` block = `8` bytes, block `0x2A`), completing all 43 blocks (`344` bytes).

### Adding optional Step 18 (`02 A1 07 1E`) on an ESP32 NFC reader

If the user opts into Bluetooth pairing during an ESP32 NFC scan, send one additional frame right after Step 16 succeeds:

```c
uint8_t ble_cmd[12];
uint32_t unlock_code = 42U;
lc_integrity_ble_enable_cmd(capture.uid, capture.patch, unlock_code, ble_cmd);

uint8_t rx[16];
size_t rx_len = 0;
if(my_nfc_transceive(ble_cmd, sizeof(ble_cmd), rx, sizeof(rx), &rx_len, ctx)) {
    uint8_t paired_mac[6];
    if(lc_integrity_parse_ble_mac(rx, rx_len, paired_mac)) {
        // Save capture.uid, capture.patch, paired_mac, and unlock_code to BLE bond!
    }
}
```

Then pass `(capture.uid, capture.patch, capture.fram)` directly into `processCompletedScan(capture.uid, capture.patch, capture.fram, "NFC")`. The ESP32 immediately decrypts the FRAM, verifies all three CRCs, populates the 8-hour graph, saves the capture to flash/SD, and starts listening for the sensor's 60-second BLE wake.

---

## Four ESP32 BLE traps we hit and solved

If you are modifying the BLE client in `esp32/circuitmess-artemis` or `esp32/waveshare-s3-lcd2`, keep these four hardware/stack traps in mind:

1. **Bluedroid's default 50 ms connection interval is too slow for Libre 2**:
   The sensor drops the link (`reason = 0x13`) if GATT discovery and `0xF001` login take longer than roughly `1.15 seconds`. ESP-IDF Bluedroid defaults to a `50 ms` (`40 * 1.25 ms`) connection interval, which makes 21 GATT discovery round-trips take over `1,050 ms`. Calling `esp_ble_gap_set_prefer_conn_params(*s_ble_adv_dev->getAddress().getNative(), 6, 8, 0, 400)` (`7.5 to 10 ms` interval) and setting `+9 dBm` TX power (`ESP_PWR_LVL_P9`) before `connect()` drops service discovery down to `460 to 670 ms`.
2. **Enable `0x2902` CCCD on `0xF002` before writing the 12-byte login to `0xF001`**:
   On service `0xFDE3`, enable notifications (`{0x01, 0x00}` on descriptor `0x2902`, handle `0x0027`) via non-blocking `esp_ble_gattc_write_char_descr()` first, and then write the 12-byte `ll_libre2_build_ble_login_payload()` to `0xF001` (handle `0x0023`) inside `ESP_GATTC_WRITE_DESCR_EVT`. Never use `BLERemoteDescriptor::writeValue(..., true)`: `BLERemoteDescriptor.cpp` does not release its semaphore if the sensor disconnects mid-write, which hangs the main loop.
3. **Monotonic `unlock_count` replay protection (`b = enable_time + unlock_count`)**:
   The first 4 bytes of the 12-byte `0xF001` login packet are `b = enable_time + unlock_count` (little-endian). The sensor stores the highest `b` it has ever accepted and immediately rejects any login where `b <= last_accepted_b` with `status = 0x85` followed by disconnect `0x13`. Always increment `s_ble_unlock_count` and persist it to flash (`/spiffs/ble_bond.txt`) the moment `0xF001` login succeeds (and bump it by `+5` if a replay rejection ever occurs after a power loss).
4. **`BLEClient::getServices()` cache across reconnects**:
   In `ESP32 BLE Arduino`, `BLEClient::onDisconnect` unregisters `m_gattc_if` without clearing `m_haveServices = false`. Always call `s_ble_client->getServices()` after connecting so Bluedroid's local GATTC cache is populated for the new `gattc_if`.
