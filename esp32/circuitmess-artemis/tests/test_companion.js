// Exercise the actual page script with browser/transport shims.
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const html = fs.readFileSync(__dirname + '/../tools/artemis_companion.html', 'utf8');
const elements = new Map();
const alerts = [];
const context = vm.createContext({TextEncoder, TextDecoder, console,
  setTimeout: (fn, ms) => setTimeout(fn, Math.min(ms, ms === 10000 ? 100 : 1)), clearTimeout,
  alert: msg => alerts.push(msg), navigator: {},
  document: {getElementById(id) {if (!elements.has(id)) elements.set(id, {value: '', appendChild() {}, addEventListener() {}, classList: {add(){},remove(){}}}); return elements.get(id);}, createElement: () => ({})}
});
vm.runInContext(html.match(/<script>([\s\S]*?)<\/script>/)[1], context);
(async () => {
  await assert.rejects(vm.runInContext("sendCommand('STATUS', true)", context), /not connected/);
  vm.runInContext(`isConnected=true; transportType='usb'; usbWriter={write:async()=>{throw new Error('unplugged')}};
    parsedSensorData={uid_wire_hex:'0011223344556677',patch_info_hex:'001122334455',fram_hex:'00',_cleanMac:'112233445566',_computedCrc:'0000'};`, context);
  await vm.runInContext('pushSensorToWatch()', context);
  assert.match(alerts.pop(), /Setup was not confirmed.*unplugged/);
  vm.runInContext(`usbWriter={write:async()=>{}}`, context);
  await assert.rejects(vm.runInContext("sendAcknowledged('STATUS', 'OK|')", context), /timed out/);
  const reply = vm.runInContext("sendAcknowledged('STATUS', 'OK|')", context);
  vm.runInContext("processIncomingChunk('OK|REA'); processIncomingChunk('DY\\r\\n')", context);
  assert.equal(await reply, 'OK|READY');
  vm.runInContext("parseWatchLine('STATUS|bl_pct=75')", context);
  assert.equal(elements.get('telBL').textContent, '75%');
  vm.runInContext(`let commands=[]; let wire='';
    usbWriter={write:async bytes=>{
      wire+=new TextDecoder().decode(bytes);
      if(wire.endsWith('\\n')) {
        commands.push(wire);
        const ack=wire.startsWith('LL2|') ? 'OK|src=USB' : wire.startsWith('LL2_BLE|') ? 'OK|LL2_BLE_ARMED' : 'OK|BT_MODE=0';
        wire=''; parseWatchLine(ack);
      }
    }}`, context);
  await vm.runInContext('pushSensorToWatch()', context);
  assert.match(alerts.pop(), /credentials accepted/);
  assert.equal(vm.runInContext("commands.map(s=>s.split('|')[0]).join(',')", context), 'LL2,LL2_BLE,BT_MODE');
  console.log('PASS companion failures, ACK timeout, fragmented ACK and USB brightness');
})().catch(error => {console.error(error); process.exitCode=1;});
