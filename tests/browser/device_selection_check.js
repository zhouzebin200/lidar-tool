const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

const source = fs.readFileSync(require('node:path').join(__dirname, '../../web/app.js'), 'utf8');
let deviceClick;
const nodes = {
  deviceCount: {textContent: ''},
  deviceList: {innerHTML: '', addEventListener(_type, callback) { deviceClick = callback; }, querySelectorAll() { return []; }},
  activeModel: {textContent: ''},
  socketState: {className: '', innerHTML: ''},
  pointCount: {textContent: ''},
  emptyState: {classList: {toggle() {}, remove() {}}},
};
const state = {selectedDevice: null, activeDevice: null, points: 0, frameCounter: 0};
const context = {state, $: id => nodes[id], performance: {now: () => 100}, rendererReady: false};
const frameCode = source.slice(source.indexOf('function uploadFrame('), source.indexOf('function render(schedule'));
const deviceCode = source.slice(source.indexOf('function renderDevices('), source.indexOf('function syncNetworkMode('));
vm.runInNewContext(frameCode + deviceCode, context);

context.renderDevices([
  {id: 10, state: 'online', vendor: 'Hesai', model: 'PandarXT', ip: '192.168.1.10', packets: 2, confidence: 1},
  {id: 64, state: 'online', vendor: 'Hesai', model: 'Pandar64', ip: '192.168.1.64', packets: 2, confidence: 1},
]);
assert.match(nodes.deviceList.innerHTML, /data-device-id="64"/);
assert.equal(state.selectedDevice, 10);

function frame(deviceId) {
  const data = Buffer.alloc(28);
  data.writeUInt32LE(0x31564c55, 0);
  data.writeUInt32LE(deviceId, 8);
  return data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength);
}
const card = {querySelector: () => ({textContent: 'Pandar64'})};
const button = {dataset: {deviceId: '64'}, closest: () => card};
deviceClick({target: {closest: () => button}});
assert.equal(state.selectedDevice, 64);
context.uploadFrame(frame(10));
assert.equal(state.frameCounter, 0);
context.uploadFrame(frame(64));
assert.equal(state.frameCounter, 1);
assert.equal(state.activeDevice, 64);
console.log('DEVICE_SELECTION_CHECK PASSED');
