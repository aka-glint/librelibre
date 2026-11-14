"""Compile the real reply sender and credential decoders with host transport shims."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'src/main.cpp').read_text()
reply = source[source.index('static void sendJugglucoTx(const char* fmt, ...) {'):source.index('static float parseGlucoseValue')]
parsers = source[source.index('        auto hexBytes ='):source.index('        if(!hexBytes(t_uid')]
program = r'''
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <cassert>
struct SerialShim {
    std::string output;
    void print(const char* s) { output += s; }
    void println(const char* s) { output += s; output += "\n"; }
} Serial;
struct Characteristic {
    std::string output, chunk;
    void setValue(uint8_t* data, size_t len) { assert(len <= 20); chunk.assign((char*)data, len); }
    void notify() { output += chunk; }
} characteristic;
bool s_juggluco_connected = true;
Characteristic* s_pTxCharacteristic = &characteristic;
void delay(int) {}
''' + reply + '\nint main() {\n' + parsers + r'''
    uint8_t value[6];
    assert(hexBytes("aAbB00112233", value, 6));
    assert(value[0] == 0xaa && value[5] == 0x33);
    assert(!hexBytes("ZZ0000112233", value, 6));
    assert(!hexBytes("123", value, 6));
    assert(!hexBytes(nullptr, value, 6));
    assert(decimal("4294967295", UINT32_MAX));
    assert(!decimal("4294967296", UINT32_MAX));
    assert(decimal("65535", UINT16_MAX));
    assert(!decimal("65536", UINT16_MAX));
    assert(!decimal("-1", UINT16_MAX));
    assert(!decimal("12junk", UINT16_MAX));
    assert(!decimal("", UINT16_MAX));
    std::string payload(220, 'x');
    sendJugglucoTx("%s\n", payload.c_str());
    assert(Serial.output == payload + "\r\n");
    assert(characteristic.output == Serial.output);
    Serial.output.clear(); characteristic.output.clear();
    s_juggluco_connected = false;
    sendJugglucoTx("{\"t\":\"cfg\"}\n");
    assert(Serial.output == "{\"t\":\"cfg\"}\r\n");
    assert(characteristic.output.empty());
    Serial.output.clear();
    sendJugglucoTx("%s", std::string(600, 'x').c_str());
    assert(Serial.output == "ERR|REPLY_TOO_LONG\n");
}
'''
with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / 'test.cpp'
    cpp.write_text(program)
    binary = Path(directory) / 'test'
    subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS firmware reply fragmentation, USB without BLE, overflow and credential decoding')
