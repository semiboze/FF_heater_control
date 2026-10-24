// node test/sensor_config_ui_test.cjs
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const html = fs.readFileSync(path.join(__dirname, '../docs/index.html'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const elements = new Map();
for (const m of html.matchAll(/id="([^"]+)"/g)) elements.set(m[1], {
    id: m[1], innerText: '', textContent: '', value: '', style: {},
    addEventListener() {}, removeEventListener() {}, setAttribute(k, v) { this[k] = v; }
});
const document = {
    getElementById(id) {
        assert(elements.has(id), 'Missing UI element ' + id);
        return elements.get(id);
    },
    querySelectorAll: () => [],
    addEventListener() {},
    visibilityState: 'visible'
};
const window = { addEventListener() {}, devicePixelRatio: 1 };
const context = vm.createContext({
    document, window, TextDecoder, TextEncoder, console, Date,
    setInterval: () => 1, setTimeout: () => 1,
    clearInterval() {}, clearTimeout() {},
    confirm: () => true, alert() { throw new Error('No blocking alert'); }
});
vm.runInContext(script, context);
const run = expression => vm.runInContext(expression, context);
const receive = data => run('receiveSensorConfig')({
    target: { value: new TextEncoder().encode(data) }
});
const display = id => elements.get(id).textContent;
receive('CFG,DONE,1,0,0,60');
assert.equal(run('sensorApplied.room'), 0);
const report = ['R', '18.4', '42.1', '0', '0', '0', '60', '10', '15', '5',
    '0', '1', '1', '0', '-1', '0', '0', '0'].join(',');
run('handleStatusUpdate')({target: {value: new TextEncoder().encode(report)}});
assert.equal(display('sensorRoomRaw'), '18.4℃');
assert.equal(display('sensorRoomPreview'), '18.4℃');
run("stepSensorOffset('room', 1)");
assert.equal(display('sensorRoomOffset'), '+0.1℃');
assert.equal(display('sensorRoomPreview'), '18.5℃');
run("stepSensorOffset('room', -1)");
assert.equal(display('sensorRoomOffset'), '0.0℃');
for (let i = 0; i < 100; i++) run("stepSensorOffset('duct', -1)");
assert.equal(run('sensorDraft.duct'), -30);
assert.equal(display('sensorDuctPreview'), '39.1℃');
run("switchPanel('sensor')");
assert.equal(elements.get('heaterPanel').hidden, true);
assert.equal(elements.get('sensorPanel').hidden, false);
run("switchPanel('heater')");
assert.equal(elements.get('heaterPanel').hidden, false);
receive('CFG,WAIT,2');
assert.equal(run('sensorRequestPending'), true);
receive('CFG,FAIL,2,TIMEOUT');
assert.equal(run('sensorRequestPending'), false);
console.log('PASS: sensor config tab, centering data, 0.1°C steps, bounds, pending/failure');
