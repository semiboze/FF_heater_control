// Run with Node.js: node test/ui_status_test.cjs
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const html = fs.readFileSync(path.join(__dirname, '../docs/index.html'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const elements = new Map();
for (const match of html.matchAll(/id="([^"]+)"/g)) {
    elements.set(match[1], { innerText: '', value: '', style: {}, addEventListener() {} });
}
const buttons = [{ disabled: false }, { disabled: false }];
let timerSequence = 0;
const pendingTimers = new Map();
const context = vm.createContext({
    document: {
        getElementById: id => elements.get(id),
        querySelectorAll: () => buttons,
        addEventListener() {}
    },
    window: { addEventListener() {} },
    TextEncoder, TextDecoder, Date, console,
    alert: () => { throw new Error('Command feedback must not block the UI'); },
    confirm: () => true,
    setTimeout: (callback, delay) => {
        const id = ++timerSequence;
        pendingTimers.set(id, { callback, delay });
        return id;
    },
    clearTimeout: id => pendingTimers.delete(id),
    setInterval: () => 1,
    clearInterval: () => {}
});
vm.runInContext(script, context);
const run = code => vm.runInContext(code, context);
const value = id => elements.get(id).innerText;
const packet = fields => context.handleStatusUpdate({ target: { value: new TextEncoder().encode(fields.join(',')) } });
const flushWrites = async () => { for (let i = 0; i < 8; i++) await Promise.resolve(); };
const acknowledge = async command => { await flushWrites(); packet(['ACK', command]); };
const normal = ['R', '10', '30', '1', '2', '600', '60', '10', '15', '5', '0', '1', '1', '0', '-1', '1', '0', '0'];
packet(normal);
assert.equal(value('rTemp'), '10.0');
assert.equal(value('dTemp'), '30.0');
assert.equal(value('controlMode'), '通常制御');
assert.equal(value('remainTime'), '10分 0秒');
assert.equal(value('fallbackRemain'), '--');
assert.equal(value('cooldownRemain'), '待機なし');

const roomBad = [...normal];
roomBad[1] = 'nan'; roomBad[11] = '0'; roomBad[13] = '1'; roomBad[14] = '1800';
packet(roomBad);
assert.equal(value('rTemp'), '--');
assert.match(value('roomSensorStatus'), /取得不可/);
assert.match(value('espnowStatus'), /センサー異常/);
assert.equal(value('fallbackRemain'), '30分 0秒');

const ductBad = [...normal];
ductBad[2] = 'nan'; ductBad[12] = '0'; ductBad[13] = '2'; ductBad[4] = '3'; ductBad[15] = '3';
packet(ductBad);
assert.equal(value('dTemp'), '--');
assert.equal(value('heaterState'), '点火未確認');
assert.equal(value('ignitionAttempts'), '3 / 3回');
assert(!value('controlMode').includes('点火未確認'));
ductBad[4] = '2';
packet(ductBad);
assert.equal(value('heaterState'), '運転中(ON)');

const stopped = [...roomBad];
stopped[2] = 'nan'; stopped[12] = '0'; stopped[13] = '3'; stopped[16] = '3'; stopped[3] = '0';
packet(stopped);
assert.match(value('stopReason'), /両センサー/);
assert.equal(value('remainTime'), '--');
for (const [reason, pattern] of [['6', /30分経過/], ['7', /OFF設定温度/]]) {
    stopped[16] = reason;
    packet(stopped);
    assert.match(value('stopReason'), pattern);
}

const stale = [...normal]; stale[10] = '180'; stale[17] = '599';
packet(stale);
assert.match(value('espnowStatus'), /途絶/);
assert.equal(value('rTemp'), '--');
assert.equal(value('dTemp'), '--');
assert.equal(value('cooldownRemain'), '9分 59秒');

packet(normal.slice(0, 10));
assert.equal(value('rTemp'), '10.0');
assert.equal(value('dataAge'), '--');
assert.equal(value('fallbackRemain'), '--');
assert.equal(value('ignitionAttempts'), '--');
assert.equal(value('controlMode'), '詳細情報なし');

const malformed = [...normal];
malformed[1] = 'NaN'; malformed[2] = 'Infinity'; malformed[5] = ''; malformed[7] = 'bad'; malformed[8] = 'NaN'; malformed[9] = '';
packet(malformed);
for (const el of elements.values()) assert(!String(el.innerText).includes('NaN'));
assert.equal(value('remainTime'), '--');

run('clearLiveStatus()');
assert.match(value('heaterState'), /不明/);
assert.match(value('autoState'), /不明/);
for (const id of ['rTemp', 'dTemp', 'dataAge', 'remainTime', 'controlMode', 'fallbackRemain', 'ignitionAttempts', 'cooldownRemain', 'stopReason']) assert.equal(value(id), '--');
assert(buttons.every(button => button.disabled));

for (const [id, val] of [['setDur', '60'], ['setOn', '10'], ['setOff', '15'], ['setDuct', '5']]) elements.get(id).value = val;
(async () => {
    run('isSettingsDirty = true; cmdCharacteristic = null; bleDevice = null;');
    assert.equal(await run('saveSettings()'), false);
    assert.equal(run('isSettingsDirty'), true);
    await run('startAutoMode()');
    assert.equal(pendingTimers.size, 0);

    context.writeFailure = async () => { throw new Error('disconnected'); };
    run('bleDevice = {gatt: {connected: true}}; cmdCharacteristic = {writeValue: writeFailure};');
    assert.equal(await run('saveSettings()'), false);
    assert.equal(run('isSettingsDirty'), true);
    assert.equal(pendingTimers.size, 0);

    const writes = [];
    context.writeSuccess = async bytes => writes.push(new TextDecoder().decode(bytes));
    run('cmdCharacteristic = {writeValue: writeSuccess};');
    const acceptedSave = run('saveSettings()');
    assert.equal(run('isSettingsDirty'), true);
    assert.match(value('commandFeedback'), /応答を確認/);
    await acknowledge('S,60,10,15,5');
    assert.equal(await acceptedSave, true);
    assert.equal(run('isSettingsDirty'), false);
    assert.equal(writes.at(-1), 'S,60,10,15,5');
    assert.equal(pendingTimers.size, 0);

    run('isSettingsDirty = true;');
    const rejectedSave = run('saveSettings()');
    await flushWrites();
    packet(['ERR', 'S,60,10,15,5']);
    assert.equal(await rejectedSave, false);
    assert.equal(run('isSettingsDirty'), true);
    assert.match(value('commandFeedback'), /受け付けません/);

    const rejectedStart = run('startAutoMode()');
    await flushWrites();
    packet(['ERR', 'S,60,10,15,5']);
    await rejectedStart;
    assert.equal(pendingTimers.size, 0, 'Rejected settings must not schedule A,1');

    const editedSave = run('saveSettings()');
    elements.get('setOn').value = '11';
    await acknowledge('S,60,10,15,5');
    assert.equal(await editedSave, true);
    assert.equal(run('isSettingsDirty'), true);
    elements.get('setOn').value = '10';

    const legacySave = run('saveSettings()');
    await acknowledge('60,10,15,5');
    assert.equal(await legacySave, true);

    const waiting = run("sendCommand('A,1')");
    await flushWrites();
    const writeCount = writes.length;
    assert.equal(await run("sendCommand('B,2')"), false, 'A second non-stop command must not overlap the first');
    assert.equal(writes.length, writeCount);
    await acknowledge('B,2');
    assert.equal(run('pendingCommand.command'), 'A,1', 'Unrelated ACK must not accept the pending command');
    await acknowledge('A,1');
    assert.equal(await waiting, true);

    const timedOut = run("sendCommand('B,0')");
    await flushWrites();
    const timer = [...pendingTimers.values()][0];
    assert.equal(timer.delay, 5000);
    timer.callback();
    assert.equal(await timedOut, false);
    assert.match(value('commandFeedback'), /応答がありません/);

    const disconnected = run('saveSettings()');
    run('clearLiveStatus()');
    assert.equal(await disconnected, false);
    assert.match(value('commandFeedback'), /接続が切れた/);

    elements.get('setDur').value = '71583';
    const beforeInvalid = writes.length;
    assert.equal(await run('saveSettings()'), false);
    assert.equal(writes.length, beforeInvalid);
    elements.get('setDur').value = '71582';
    const maximumDuration = run('saveSettings()');
    await acknowledge('S,71582,10,15,5');
    assert.equal(await maximumDuration, true);

    elements.get('setDur').value = '60';
    run('isSettingsDirty = true;');
    const interruptedStart = run('startAutoMode()');
    await flushWrites();
    const manualOff = run("sendCommand('B,1')");
    await interruptedStart;
    assert.equal(run('isSettingsDirty'), true);
    await acknowledge('S,60,10,15,5');
    assert.equal(run('pendingCommand.command'), 'B,1', 'An old settings ACK must not accept the stop');
    await acknowledge('B,1');
    assert.equal(await manualOff, true);
    assert.equal(pendingTimers.size, 0, 'Preempted settings must not schedule automatic start');

    const inflightWrites = [];
    let completeSettingsWrite;
    context.writeSlowSettings = bytes => {
        const command = new TextDecoder().decode(bytes);
        inflightWrites.push(command);
        return command.startsWith('S,') ? new Promise(resolve => { completeSettingsWrite = resolve; }) : Promise.resolve();
    };
    run('cmdCharacteristic = {writeValue: writeSlowSettings};');
    const inflightStart = run('startAutoMode()');
    await flushWrites();
    assert.deepEqual(inflightWrites, ['S,60,10,15,5']);
    const forceStop = run("sendCommand('A,0')");
    await inflightStart;
    await flushWrites();
    assert.deepEqual(inflightWrites, ['S,60,10,15,5'], 'STOP must wait for an in-flight GATT write');
    packet(['ACK', 'S,60,10,15,5']);
    assert.equal(run('pendingCommand.command'), 'A,0');
    completeSettingsWrite();
    await flushWrites();
    assert.deepEqual(inflightWrites, ['S,60,10,15,5', 'A,0']);
    await acknowledge('A,0');
    assert.equal(await forceStop, true);
    assert.equal(pendingTimers.size, 0);
    assert(!inflightWrites.includes('A,1'));

    run('cmdCharacteristic = {writeValue: writeSuccess};');
    const startBeforeContinuation = run('startAutoMode()');
    await flushWrites();
    const writesBeforeStop = writes.length;
    packet(['ACK', 'S,60,10,15,5']);
    const stopBeforeStart = run("sendCommand('B,STOP')");
    await startBeforeContinuation;
    await acknowledge('B,STOP');
    assert.equal(await stopBeforeStart, true);
    assert.equal(pendingTimers.size, 0);
    assert.deepEqual(writes.slice(writesBeforeStop), ['B,STOP'], 'STOP after settings ACK must prevent the awaiting start continuation from sending A,1');

    const firstStop = run("sendCommand('B,1')");
    const repeatedStop = run("sendCommand('B,1')");
    await acknowledge('B,1');
    assert.equal(await firstStop, true);
    assert.equal(await repeatedStop, true);

    console.log('PASS: sensor/fallback/status parsing, BLE clearing, ACK/ERR and legacy ACK, STOP preemption and GATT serialization, timeout, dirty settings and duration limits.');
})().catch(error => { console.error(error); process.exitCode = 1; });
