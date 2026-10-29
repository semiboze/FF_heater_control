#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "HeaterControl.h"
#include "SensorPacket.h"
#include "SensorConfig.h"
#include "EspNowWire.h"
#include "EspNowKeys.h"

using ffheater::ReceivedSensor;
struct ReceivedCommand { char text[80]; };
QueueHandle_t commandQueue = nullptr;
struct ConfigResult { ffheater::ConfigFrame frame; uint8_t mac[6]; };
QueueHandle_t radioQueue = nullptr;
// The former hard-coded peer MAC was not actually checked against received data.
// Deliberately fail closed: operator must register the verified sensor STA MAC.
uint8_t sensorMac[6] = {};
uint8_t nowRoot[16] = {};
bool nowKeyReady = false;
std::atomic<bool> nowPeerReady{false};
Preferences nowPrefs;
struct RadioRx {
    uint8_t sourceMac[6];
    uint8_t bytes[sizeof(ffnow::SampleFrame)];
    uint8_t size;
    uint32_t receivedAt;
};
struct RadioSession {
    uint64_t sensorNonce = 0, parentNonce = 0;
    uint32_t createdAt = 0;
    bool pending = false;
} radioSession;
uint64_t configSensorNonce = 0, configParentNonce = 0;


#define SERVICE_UUID     "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_STATUS_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CHAR_HISTORY_UUID "e93c15d4-f791-4bb1-9daf-88ae6f946be1"
#define CHAR_SENSOR_CONFIG_UUID "fd0c7757-0e61-45c5-9123-950f6c285c01"
#define CHAR_CMD_UUID    "8c772be5-e0d0-4251-bb5c-1f6874eb7310"
BLEServer* pServer = nullptr;
BLECharacteristic* pStatusChar = nullptr;
BLECharacteristic* pHistoryChar = nullptr;
std::atomic<bool> deviceConnected{false};
BLECharacteristic* pSensorConfigChar = nullptr;
ffheater::ConfigFrame pendingConfig{};
bool configPending = false;
uint32_t configCreatedAt = 0;
uint32_t configNextId = 0;
uint8_t configTargetMac[6] = {};
String configMessage = "CFG,UNKNOWN";

void configNotify(const String& message) {
    configMessage = message;
    if (!pSensorConfigChar) return;
    pSensorConfigChar->setValue(message.c_str());
    if (deviceConnected.load()) pSensorConfigChar->notify();
}

void configFail(const char* reason) {
    const uint32_t requestId = pendingConfig.requestId;
    configPending = false;
    configNotify("CFG,FAIL," + String(requestId) + "," + reason);
}


extern ffheater::Controller controller;
struct HistoryPoint { uint32_t at; int16_t room, duct; };
HistoryPoint history[61], snapshot[61];
uint8_t historyHead = 0, historyCount = 0, snapshotCount = 0, snapshotIndex = 0;
uint16_t snapshotId = 0;
uint32_t snapshotAt = 0, lastHistoryAt = 0, lastHistorySend = 0;
bool historyStarted = false, historySending = false;
constexpr int16_t HISTORY_MISSING = INT16_MIN;
int16_t historyValue(float value, bool valid) {
    if (!valid || !std::isfinite(value) || value < -3276.7f || value > 3276.7f) return HISTORY_MISSING;
    return static_cast<int16_t>(std::lround(value * 10));
}
void sampleHistory(uint32_t now) {
    if (historyStarted && now - lastHistoryAt < 60000UL) return;
    historyStarted = true; lastHistoryAt = now;
    history[historyHead] = {now, historyValue(controller.room.temperature, controller.roomValid(now)),
        historyValue(controller.duct.temperature, controller.ductValid(now))};
    historyHead = (historyHead + 1) % 61;
    if (historyCount < 61) ++historyCount;
}
void beginHistory(uint32_t now) {
    snapshotAt = now; snapshotCount = 0; snapshotIndex = 0; ++snapshotId;
    for (uint8_t i = 0; i < historyCount; ++i) {
        const auto &point = history[(historyHead + 61 - historyCount + i) % 61];
        if (now - point.at <= 3600000UL) snapshot[snapshotCount++] = point;
    }
    historySending = true; lastHistorySend = now - 100;
}
void sendHistory(uint32_t now) {
    if (!deviceConnected.load()) { historySending = false; return; }
    if (!historySending || now - lastHistorySend < 100) return;
    lastHistorySend = now;
    // Fixed 12-byte little-endian frame, within the default BLE MTU.
    uint8_t frame[12] = {1, uint8_t(snapshotId), uint8_t(snapshotId >> 8), snapshotIndex, snapshotCount};
    uint16_t age = 0; int16_t room = HISTORY_MISSING, duct = HISTORY_MISSING;
    if (snapshotIndex < snapshotCount) {
        const auto &point = snapshot[snapshotIndex];
        age = uint16_t((snapshotAt - point.at) / 1000); room = point.room; duct = point.duct;
    }
    frame[5] = uint8_t(age); frame[6] = uint8_t(age >> 8);
    frame[7] = uint8_t(room); frame[8] = uint8_t(uint16_t(room) >> 8);
    frame[9] = uint8_t(duct); frame[10] = uint8_t(uint16_t(duct) >> 8);
    frame[11] = 0;
    pHistoryChar->setValue(frame, sizeof(frame)); pHistoryChar->notify();
    if (snapshotCount == 0 || ++snapshotIndex >= snapshotCount) historySending = false;
}

bool oldDeviceConnected = false;

#ifdef TARGET_ESP32_S3
constexpr int PIN_ON = 1, PIN_OFF = 2, PIN_UP = 3, PIN_DOWN = 4;
#else
constexpr int PIN_ON = 25, PIN_OFF = 32, PIN_UP = 26, PIN_DOWN = 27;
#endif
enum ButtonType { BTN_ON, BTN_OFF, BTN_UP, BTN_DOWN };
struct ButtonPulse {
    int pin;
    const char* label;
    uint32_t startedAt;
    bool high;
};
ButtonPulse buttons[] = {{PIN_ON, "電源ON", 0, false}, {PIN_OFF, "電源OFF", 0, false},
                         {PIN_UP, "UP", 0, false}, {PIN_DOWN, "DOWN", 0, false}};

Preferences prefs;
ffheater::Controller controller;
bool persistedCooldown = false;

// USB-only, interactive test mode. It deliberately shares the production
// Controller instance: only its clock and sensor source are replaced. A reset
// is required to return to normal operation, so an operator cannot silently
// leave ESP-NOW control disabled after a test.
struct Simulation {
    bool active = false;
    uint32_t now = 0;
} simulation;

uint32_t controlNow() { return simulation.active ? simulation.now : millis(); }

void printControlTime(uint32_t now) {
    if (!simulation.active) {
        Serial.printf("[REAL %lu ms]", static_cast<unsigned long>(now));
        return;
    }
    const uint32_t seconds = now / 1000UL;
    Serial.printf("[SIM %02lu:%02lu:%02lu]", static_cast<unsigned long>(seconds / 3600UL),
        static_cast<unsigned long>((seconds / 60UL) % 60UL),
        static_cast<unsigned long>(seconds % 60UL));
}

void logGpio(const char* action, const ButtonPulse& pulse) {
    printControlTime(controlNow());
    Serial.printf(" GPIO%d %s: %s\n", pulse.pin, action, pulse.label);
}

#ifdef TARGET_ESP32_S3
// GPIO2 is the OFF output on S3: never drive LED patterns on that same pin.
constexpr int PIN_LED = -1;
#else
constexpr int PIN_LED = 2;
#endif
void writeLed(bool high) { if (PIN_LED >= 0) digitalWrite(PIN_LED, high ? HIGH : LOW); }
enum LedPatternType { PATTERN_ON, PATTERN_OFF, PATTERN_UP, PATTERN_DOWN,
    PATTERN_AUTO, PATTERN_SETTING, PATTERN_BLE_CONN, PATTERN_EMERGENCY,
    PATTERN_BLE_DISCONN, PATTERN_COUNT };
const char* const patterns[] = {"o-o", "OO", "o-o", "O-o-O", "O-O-O", "o-O-o-O",
    "O-O", "o-o-o-O-O-O-o-o-o--o-o-o-O-O-O-o-o-o", "o-o-o-o-O"};
const char* activeSequence = "";
size_t currentStep = 0;
uint32_t lastChangeTime = 0;
bool patternRunning = false;

void startPattern(LedPatternType type) {
    if (type >= PATTERN_COUNT) return;
    activeSequence = patterns[type];
    currentStep = 0;
    patternRunning = true;
    lastChangeTime = millis();
}

void updateLedPattern() {
    if (!patternRunning) return;
    const char step = activeSequence[currentStep];
    if (step == '\0') {
        writeLed(false);
        patternRunning = false;
        return;
    }
    const uint32_t duration = step == 'O' ? 600 : 200;
    if (millis() - lastChangeTime >= duration) {
        lastChangeTime = millis();
        writeLed(step == 'O' || step == 'o');
        ++currentStep;
    }
}

void triggerButton(ButtonType button) {
    if (button == BTN_OFF) {
        // A stop also cancels any ON pulse already in progress.
        auto& onPulse = buttons[BTN_ON];
        if (onPulse.high) {
            digitalWrite(PIN_ON, LOW);
            onPulse.high = false;
            logGpio("LOW (cancelled)", onPulse);
        }
    }
    auto& pulse = buttons[button];
    if (pulse.high) return;
    digitalWrite(pulse.pin, HIGH);
    pulse.high = true;
    pulse.startedAt = millis();
    logGpio("HIGH", pulse);
    startPattern(static_cast<LedPatternType>(button));
}

void updateButtonPulses() {
    for (auto& pulse : buttons) {
        if (pulse.high && millis() - pulse.startedAt >= 200) {
            digitalWrite(pulse.pin, LOW);
            pulse.high = false;
            logGpio("LOW (200ms pulse complete)", pulse);
        }
    }
}

void OnDataRecv(const uint8_t* mac, const uint8_t* bytes, int len) {
    // Wi-Fi callback: short MAC/length check + queue only. Never auto-add peers.
    if (!nowPeerReady || !radioQueue || !ffnow::allowedMac(mac, sensorMac) || !bytes ||
        len <= 0 || len > int(sizeof(RadioRx::bytes))) return;
    RadioRx rx{};
    memcpy(rx.sourceMac, mac, 6);
    rx.size = uint8_t(len);
    memcpy(rx.bytes, bytes, len);
    rx.receivedAt = millis();
    xQueueSend(radioQueue, &rx, 0);
}

class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer*) override { deviceConnected.store(true); }
    void onDisconnect(BLEServer*) override { deviceConnected.store(false); }
};

class MyCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* characteristic) override {
        const auto value = characteristic->getValue();
        if (value.empty() || value.size() >= sizeof(ReceivedCommand::text)) return;
        ReceivedCommand command{};
        memcpy(command.text, value.data(), value.size());
        if (commandQueue) xQueueSend(commandQueue, &command, 0);
    }
};

bool validSettings(const ffheater::Settings& settings) {
    return settings.durationMinutes > 0 && settings.durationMinutes <= UINT32_MAX / 60000UL
        && std::isfinite(settings.onTemperature) && std::isfinite(settings.offTemperature)
        && settings.onTemperature < settings.offTemperature
        && std::isfinite(settings.ductRise) && settings.ductRise > 0;
}

void acknowledge(const char* command, bool accepted) {
    if (!deviceConnected.load()) return;
    String response = accepted ? "ACK," : "ERR,";
    response += command;
    pStatusChar->setValue(response.c_str());
    pStatusChar->notify();
}

void mergeEffect(ffheater::Command incoming, ffheater::Command& effect) {
    if (incoming == ffheater::Command::OFF || effect == ffheater::Command::NONE) effect = incoming;
}

void handleCommands(uint32_t now, ffheater::Command& effect) {
    ReceivedCommand received{};
    while (xQueueReceive(commandQueue, &received, 0) == pdTRUE) {
        const String value(received.text);
        // Simulation is intentionally USB-only. BLE commands must not alter a
        // test sequence or cause an unexpected button pulse during a test.
        if (simulation.active) {
            acknowledge(received.text, false);
            continue;
        }
        bool accepted = true;
        if (value == "C,GET" || value == "C,RESET" || value.startsWith("C,SET,")) {
            accepted = nowPeerReady && !configPending;
            if (accepted) {
                ffheater::ConfigFrame next{};
                next.op = value == "C,GET" ? ffheater::CONFIG_GET : ffheater::CONFIG_SET;
                if (value.startsWith("C,SET,")) {
                    int room = 0, duct = 0, sleep = 0;
                    char trailing = 0;
                    const int matched = sscanf(received.text, "C,SET,%d,%d,%d%c",
                                               &room, &duct, &sleep, &trailing);
                    accepted = matched == 3 && room >= -30 && room <= 30 &&
                               duct >= -30 && duct <= 30 && sleep >= 0 && sleep <= 65535;
                    if (accepted) {
                        next.roomTenths = static_cast<int16_t>(room);
                        next.ductTenths = static_cast<int16_t>(duct);
                        next.sleepSeconds = static_cast<uint16_t>(sleep);
                        accepted = ffheater::validConfig(next);
                    }
                } else if (value == "C,RESET") {
                    next.roomTenths = 0; next.ductTenths = 0; next.sleepSeconds = 60;
                }
                if (accepted && next.op == ffheater::CONFIG_SET &&
                    (controller.active() || controller.state() != ffheater::HeaterState::OFF))
                    accepted = false;
                if (accepted) {
                    ++configNextId;
                    if (configNextId == 0) ++configNextId;
                    next.requestId = configNextId;
                    pendingConfig = next;
                    configPending = true;
                    configCreatedAt = now;
                    configNotify("CFG,WAIT," + String(next.requestId));
                }
            }
        } else if (value == "H,1") {
            beginHistory(now);
        } else if (value == "A,0" || value == "B,1" || value == "B,STOP") {
            mergeEffect(controller.stopByUser(now), effect);
        } else if (value == "A,1") {
            // A fault/stop and an already queued start must not cancel out.
            if (effect == ffheater::Command::OFF) accepted = false;
            else {
                controller.startAuto(now);
                startPattern(PATTERN_AUTO);
            }
        } else if (value == "B,0") {
            if (effect == ffheater::Command::OFF || buttons[BTN_OFF].high) accepted = false;
            else {
                controller.recordManualOn(now);
                mergeEffect(ffheater::Command::ON, effect);
            }
        } else if (value == "B,2" || value == "B,3") {
            triggerButton(value == "B,2" ? BTN_UP : BTN_DOWN);
        } else if (value.startsWith("S,")) {
            ffheater::Settings settings;
            unsigned long duration = 0;
            char extra = 0;
            const int fields = sscanf(received.text, "S,%lu,%f,%f,%f%c", &duration,
                &settings.onTemperature, &settings.offTemperature, &settings.ductRise, &extra);
            settings.durationMinutes = duration;
            accepted = fields == 4 && validSettings(settings);
            if (accepted) {
                controller.settings = settings;
                prefs.putInt("duration", settings.durationMinutes);
                prefs.putFloat("ontemp", settings.onTemperature);
                prefs.putFloat("offtemp", settings.offTemperature);
                prefs.putFloat("ductthresh", settings.ductRise);
                startPattern(PATTERN_SETTING);
            }
        } else accepted = false;
        acknowledge(received.text, accepted);
    }
}

void handleBLEConnection() {
    const bool connected = deviceConnected.load();
    if (connected == oldDeviceConnected) return;
    startPattern(connected ? PATTERN_BLE_CONN : PATTERN_BLE_DISCONN);
    if (!connected) pServer->getAdvertising()->start();
    oldDeviceConnected = connected;
}

uint32_t lastNotifyTime = 0;
void handleBLENotify(uint32_t now) {
    if (!deviceConnected.load() || now - lastNotifyTime < 2000) return;
    lastNotifyTime = now;
    const auto& settings = controller.settings;
    // R fields 0..10 are unchanged; fields 11..17 add sensor/fallback details.
    char message[224];
    snprintf(message, sizeof(message),
        "R,%.1f,%.1f,%u,%u,%lu,%lu,%.1f,%.1f,%.1f,%lu,%u,%u,%u,%ld,%u,%u,%lu",
        controller.roomValid(now) ? controller.room.temperature : NAN,
        controller.ductValid(now) ? controller.duct.temperature : NAN,
        controller.active() ? 1U : 0U, static_cast<unsigned>(controller.state()),
        static_cast<unsigned long>(controller.autoRemainingSeconds(now)),
        static_cast<unsigned long>(settings.durationMinutes),
        settings.onTemperature, settings.offTemperature, settings.ductRise,
        static_cast<unsigned long>(controller.dataAgeSeconds(now)),
        controller.roomValid(now) ? 1U : 0U, controller.ductValid(now) ? 1U : 0U,
        static_cast<unsigned>(controller.mode(now)),
        static_cast<long>(controller.fallbackRemainingSeconds(now)),
        static_cast<unsigned>(controller.ignitionAttempts()),
        static_cast<unsigned>(controller.stopReason()),
        static_cast<unsigned long>(controller.cooldownRemainingSeconds(now)));
    pStatusChar->setValue(message);
    pStatusChar->notify();
}

// USB serial configuration is local-only. Secret and MAC are never taken over BLE.
String nowSerialLine;
bool resetArmed = false;
uint32_t resetArmedAt = 0;
void printMac(const uint8_t* mac) {
    for (int i = 0; i < 6; ++i) {
        if (i) Serial.print(':');
        if (mac[i] < 16) Serial.print('0');
        Serial.print(mac[i], HEX);
    }
}
bool heaterStoppedForKeyChange() {
    return !controller.active() && controller.state() == ffheater::HeaterState::OFF &&
           !buttons[BTN_ON].high;
}
void newNowKey() {
    if (!heaterStoppedForKeyChange()) { Serial.println("ERROR: stop heater first"); return; }
    uint8_t next[16] = {};
    ffnow::randomBytes(next, sizeof(next));
    if (!ffnow::saveKey(nowPrefs, next)) { Serial.println("ERROR: NVS save failed"); return; }
    ffnow::printKeyOnce(next);
    memcpy(nowRoot, next, sizeof(next));
    nowKeyReady = true;
    if (nowPeerReady) { nowPeerReady = false; esp_now_deinit(); }
    Serial.println("Saved. Enter NOW APPLY to restart. Update sensor key too.");
    memset(next, 0, sizeof(next));
}
void handleNowCommand(const String& command) {
    if (command == "NOW STATUS") {
        Serial.print("ESP-NOW key: "); Serial.println(nowKeyReady ? "SET" : "NOT SET");
        Serial.print("Sensor MAC: ");
        if (nowPrefs.getBytesLength("sensor") == 6) { printMac(sensorMac); Serial.println(); }
        else Serial.println("NOT SET");
        Serial.print("Encrypted peer: "); Serial.println(nowPeerReady ? "READY" : "INACTIVE");
    } else if (command == "NOW INIT") {
        if (nowKeyReady) { Serial.println("Already initialized. Use NOW RESET for new key."); return; }
        newNowKey();
    } else if (command == "NOW RESET") {
        if (!heaterStoppedForKeyChange()) { Serial.println("ERROR: heater not stopped"); return; }
        resetArmed = true; resetArmedAt = millis();
        Serial.println("Confirm within 20s: NOW CONFIRM RESET (old key will stop working)");
    } else if (command == "NOW CONFIRM RESET") {
        if (!resetArmed || millis() - resetArmedAt > 20000) {
            Serial.println("ERROR: confirmation expired"); return;
        }
        resetArmed = false;
        newNowKey();
    } else if (command.startsWith("NOW SENSOR ")) {
        if (!heaterStoppedForKeyChange()) { Serial.println("ERROR: stop heater first"); return; }
        uint8_t mac[6] = {};
        if (!ffnow::parseMac(command.c_str() + 11, mac)) {
            Serial.println("ERROR: enter MAC as AA:BB:CC:DD:EE:FF"); return;
        }
        if (nowPrefs.putBytes("sensor", mac, 6) != 6) {
            Serial.println("ERROR: NVS MAC save failed"); return;
        }
        if (nowPeerReady) { nowPeerReady = false; esp_now_deinit(); }
        memcpy(sensorMac, mac, 6);
        Serial.println("Sensor MAC saved. Enter NOW APPLY to restart.");
    } else if (command == "NOW APPLY") {
        if (!heaterStoppedForKeyChange()) { Serial.println("ERROR: heater not stopped"); return; }
        Serial.println("Restarting controller..."); Serial.flush(); delay(100); ESP.restart();
    } else if (command.length()) Serial.println("Commands: NOW STATUS / NOW INIT / NOW RESET / NOW CONFIRM RESET / NOW SENSOR <MAC> / NOW APPLY");
}

const char* heaterStateName(ffheater::HeaterState state) {
    switch (state) {
        case ffheater::HeaterState::OFF: return "OFF";
        case ffheater::HeaterState::IGNITING: return "IGNITING";
        case ffheater::HeaterState::ON: return "ON";
        case ffheater::HeaterState::UNCONFIRMED: return "UNCONFIRMED";
    }
    return "UNKNOWN";
}

const char* stopReasonName(ffheater::StopReason reason) {
    switch (reason) {
        case ffheater::StopReason::NONE: return "NONE";
        case ffheater::StopReason::USER: return "USER";
        case ffheater::StopReason::TIMER: return "TIMER";
        case ffheater::StopReason::BOTH_SENSORS: return "BOTH_SENSORS";
        case ffheater::StopReason::COMM_TIMEOUT: return "COMM_TIMEOUT";
        case ffheater::StopReason::IGNITION_FAILED: return "IGNITION_FAILED";
        case ffheater::StopReason::ROOM_LIMIT: return "ROOM_LIMIT";
        case ffheater::StopReason::TEMPERATURE: return "TEMPERATURE";
    }
    return "UNKNOWN";
}

void printSimulationStatus() {
    printControlTime(simulation.now);
    Serial.printf(" state=%s auto=%s room=%.1f(%s) duct=%.1f(%s) age=%lus attempts=%u stop=%s\n",
        heaterStateName(controller.state()), controller.active() ? "ON" : "OFF",
        controller.room.temperature, controller.roomValid(simulation.now) ? "valid" : "invalid",
        controller.duct.temperature, controller.ductValid(simulation.now) ? "valid" : "invalid",
        static_cast<unsigned long>(controller.dataAgeSeconds(simulation.now)),
        static_cast<unsigned>(controller.ignitionAttempts()), stopReasonName(controller.stopReason()));
}

void applySimulationCommand(ffheater::Command command) {
    if (command == ffheater::Command::NONE) return;
    printControlTime(simulation.now);
    Serial.printf(" control decision: %s\n", command == ffheater::Command::ON ? "ON" : "OFF");
    triggerButton(command == ffheater::Command::ON ? BTN_ON : BTN_OFF);
}

void tickSimulation() { applySimulationCommand(controller.tick(simulation.now)); }

void startSimulation() {
    if (controller.active() || controller.state() != ffheater::HeaterState::OFF ||
        buttons[BTN_ON].high || buttons[BTN_OFF].high || configPending) {
        Serial.println("ERROR: stop normal control and wait for button pulses/configuration first.");
        return;
    }
    const ffheater::Settings settings = controller.settings;
    controller = ffheater::Controller{};
    controller.settings = settings;
    simulation.active = true;
    simulation.now = 0;
    Serial.println("SIMULATION ENABLED: ESP-NOW measurements are ignored by control until reboot.");
    Serial.println("Enter SIM TEMP <room> <duct> twice, then SIM AUTO START. Use nan for a failed sensor.");
    printSimulationStatus();
}

bool parseTwoTemperatures(const String& command, float& room, float& duct) {
    char extra = 0;
    return sscanf(command.c_str(), "SIM TEMP %f %f %c", &room, &duct, &extra) == 2 &&
        (!std::isfinite(room) || (room >= -100.0f && room <= 100.0f)) &&
        (!std::isfinite(duct) || (duct >= -100.0f && duct <= 300.0f));
}

void handleSimulationCommand(const String& command) {
    if (command == "SIM HELP") {
        Serial.println("SIM START | SIM TEMP <room> <duct> | SIM AUTO START | SIM AUTO STOP");
        Serial.println("SIM ADVANCE <seconds> (no packet: tests communication timeout) | SIM STATUS | SIM RESTART");
        return;
    }
    if (command == "SIM START") { startSimulation(); return; }
    if (!simulation.active) {
        Serial.println("Simulation is off. Enter SIM START first (SIM HELP for commands).");
        return;
    }
    if (command == "SIM STATUS") { printSimulationStatus(); return; }
    if (command == "SIM RESTART") {
        Serial.println("Restarting: normal ESP-NOW control will resume after boot.");
        Serial.flush(); delay(100); ESP.restart();
        return;
    }
    if (command == "SIM AUTO START") {
        controller.startAuto(simulation.now);
        tickSimulation();
        printSimulationStatus();
        return;
    }
    if (command == "SIM AUTO STOP") {
        applySimulationCommand(controller.stopByUser(simulation.now));
        printSimulationStatus();
        return;
    }
    float room = NAN, duct = NAN;
    if (parseTwoTemperatures(command, room, duct)) {
        controller.receive(room, duct, simulation.now);
        printControlTime(simulation.now);
        Serial.printf(" injected room=%.1f duct=%.1f\n", room, duct);
        tickSimulation();
        printSimulationStatus();
        return;
    }
    unsigned long seconds = 0;
    char extra = 0;
    if (sscanf(command.c_str(), "SIM ADVANCE %lu %c", &seconds, &extra) == 1 && seconds <= 86400UL) {
        // Tick once per virtual second. This preserves exact 3-minute timeout,
        // 5-minute retry, and timer boundaries while deliberately generating no
        // artificial sensor packets.
        for (unsigned long i = 0; i < seconds; ++i) {
            simulation.now += 1000UL;
            tickSimulation();
        }
        printSimulationStatus();
        return;
    }
    Serial.println("Invalid SIM command. Enter SIM HELP.");
}

void pollNowSerial() {
    while (Serial.available()) {
        char c = char(Serial.read());
        if (c == '\r') continue;
        if (c == '\n') {
            if (nowSerialLine.startsWith("SIM")) handleSimulationCommand(nowSerialLine);
            else handleNowCommand(nowSerialLine);
            nowSerialLine = "";
        } else if (c >= 32 && c <= 126 && nowSerialLine.length() < 90) nowSerialLine += c;
    }
}
void handleRadioPackets() {
    RadioRx rx{};
    while (xQueueReceive(radioQueue, &rx, 0) == pdTRUE) {
        ffnow::HelloFrame hello{};
        if (ffnow::parse(rx.bytes, rx.size, hello, ffnow::HELLO)) {
            if (!hello.sensorNonce) continue;
            radioSession.sensorNonce = hello.sensorNonce;
            radioSession.parentNonce = ffnow::nonce();
            radioSession.createdAt = millis();
            radioSession.pending = true;
            ffnow::ChallengeFrame challenge{};
            challenge.sensorNonce = radioSession.sensorNonce;
            challenge.parentNonce = radioSession.parentNonce;
            esp_now_send(sensorMac, reinterpret_cast<const uint8_t*>(&challenge), sizeof(challenge));
            continue;
        }
        ffnow::SampleFrame sample{};
        if (ffnow::parse(rx.bytes, rx.size, sample, ffnow::SAMPLE)) {
            if (!radioSession.pending || millis() - radioSession.createdAt > 8000 ||
                !ffnow::matchesChallenge(sample.sensorNonce, sample.parentNonce,
                    radioSession.sensorNonce, radioSession.parentNonce)) continue;
            // Consume challenge exactly once, BEFORE decoding temperature.
            radioSession.pending = false;
            ReceivedSensor packet{};
            if (!ffheater::decodeSensorPacket(rx.sourceMac, sample.sensorData,
                sizeof(sample.sensorData), rx.receivedAt, packet)) continue;
            Serial.printf("Encrypted sensor RX room=%.1f duct=%.1f\n",
                packet.data.roomTemp, packet.data.ductTemp);
            if (!simulation.active) {
                controller.receive(packet.data.roomTemp, packet.data.ductTemp, packet.receivedAt);
            } else {
                printControlTime(simulation.now);
                Serial.println(" ESP-NOW measurement ignored by simulation control");
            }
            if (configPending && millis() - configCreatedAt < 600000UL) {
                if (pendingConfig.op == ffheater::CONFIG_SET &&
                    (controller.active() || controller.state() != ffheater::HeaterState::OFF)) {
                    configFail("NOT_OFF");
                } else {
                    ffnow::ConfigSecureFrame request{};
                    request.kind = ffnow::CONFIG;
                    request.sensorNonce = sample.sensorNonce;
                    request.parentNonce = sample.parentNonce;
                    request.config = pendingConfig;
                    const esp_err_t tx = esp_now_send(sensorMac,
                        reinterpret_cast<const uint8_t*>(&request), sizeof(request));
                    if (tx == ESP_OK) {
                        configSensorNonce = sample.sensorNonce;
                        configParentNonce = sample.parentNonce;
                        memcpy(configTargetMac, sensorMac, 6);
                    }
                }
            }
            continue;
        }
        ffnow::ConfigSecureFrame report{};
        if (ffnow::parse(rx.bytes, rx.size, report, ffnow::REPORT)) {
            if (!configPending || report.sensorNonce != configSensorNonce ||
                report.parentNonce != configParentNonce ||
                report.config.requestId != pendingConfig.requestId ||
                !ffnow::allowedMac(rx.sourceMac, configTargetMac) ||
                report.config.magic != ffheater::CONFIG_MAGIC ||
                report.config.version != ffheater::CONFIG_VERSION ||
                report.config.op != ffheater::CONFIG_REPORT) continue;
            const auto& c = report.config;
            if (c.status == ffheater::CONFIG_OK && ffheater::validConfig(c) &&
                (pendingConfig.op == ffheater::CONFIG_GET ||
                (c.roomTenths == pendingConfig.roomTenths &&
                 c.ductTenths == pendingConfig.ductTenths &&
                 c.sleepSeconds == pendingConfig.sleepSeconds))) {
                configPending = false;
                configNotify("CFG,DONE," + String(c.requestId) + "," + String(c.roomTenths) +
                    "," + String(c.ductTenths) + "," + String(c.sleepSeconds));
            } else configFail(c.status == ffheater::CONFIG_SAVE_ERROR ? "SAVE" :
                    c.status == ffheater::CONFIG_BAD_VALUE ? "VALUE" : "MISMATCH");
        }
    }
}
void setup() {
    Serial.begin(115200);
    if (PIN_LED >= 0) pinMode(PIN_LED, OUTPUT);
    writeLed(false);
    for (auto& pulse : buttons) {
        pinMode(pulse.pin, OUTPUT);
        digitalWrite(pulse.pin, LOW);
    }
    commandQueue = xQueueCreate(8, sizeof(ReceivedCommand));
    radioQueue = xQueueCreate(8, sizeof(RadioRx));
    if (!commandQueue || !radioQueue) {
        Serial.println("Queue allocation failed; outputs remain OFF");
        while (true) delay(1000);
    }

    prefs.begin("heater-config", false);
    ffheater::Settings saved;
    saved.durationMinutes = prefs.getInt("duration", 60);
    saved.onTemperature = prefs.getFloat("ontemp", 10.0);
    saved.offTemperature = prefs.getFloat("offtemp", 15.0);
    saved.ductRise = prefs.getFloat("ductthresh", 5.0);
    if (validSettings(saved)) controller.settings = saved;
    persistedCooldown = prefs.getBool("autooffcool", false);
    if (persistedCooldown) controller.restoreCooldown(millis());

    nowPrefs.begin("ffnow-sec", false);
    nowKeyReady = ffnow::loadKey(nowPrefs, nowRoot);
    const bool hasSensorMac = nowPrefs.getBytesLength("sensor") == 6 &&
        nowPrefs.getBytes("sensor", sensorMac, 6) == 6 &&
        ffnow::validUnicastMac(sensorMac);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    Serial.printf("Controller STA MAC=%s, channel=%d\n", WiFi.macAddress().c_str(), WiFi.channel());
    if (nowKeyReady && hasSensorMac && esp_now_init() == ESP_OK) {
        uint8_t pmk[16] = {}, lmk[16] = {};
        const bool derived = ffnow::deriveKeys(nowRoot, pmk, lmk);
        esp_err_t pmkStatus = derived ? esp_now_set_pmk(pmk) : ESP_FAIL;
        esp_now_peer_info_t peer{};
        memcpy(peer.peer_addr, sensorMac, 6);
        peer.channel = 1; peer.ifidx = WIFI_IF_STA; peer.encrypt = true;
        memcpy(peer.lmk, lmk, 16);
        nowPeerReady = pmkStatus == ESP_OK && esp_now_add_peer(&peer) == ESP_OK;
        memset(pmk, 0, sizeof(pmk)); memset(lmk, 0, sizeof(lmk));
        if (nowPeerReady) esp_now_register_recv_cb(OnDataRecv);
    }
    Serial.println(nowPeerReady ? "ESP-NOW v2 encrypted: READY" :
        "ESP-NOW INACTIVE: enter NOW STATUS/INIT/SENSOR via USB serial");

    BLEDevice::init("FF_Heater");
    BLEDevice::setMTU(247);
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    BLEService* service = pServer->createService(SERVICE_UUID);
    pStatusChar = service->createCharacteristic(CHAR_STATUS_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    pStatusChar->addDescriptor(new BLE2902());
    pHistoryChar = service->createCharacteristic(CHAR_HISTORY_UUID, BLECharacteristic::PROPERTY_NOTIFY);
    pHistoryChar->addDescriptor(new BLE2902());
    pSensorConfigChar = service->createCharacteristic(CHAR_SENSOR_CONFIG_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    pSensorConfigChar->addDescriptor(new BLE2902());
    pSensorConfigChar->setValue(configMessage.c_str());
    BLECharacteristic* command = service->createCharacteristic(CHAR_CMD_UUID,
        BLECharacteristic::PROPERTY_WRITE);
    command->setCallbacks(new MyCallbacks());
    service->start();
    BLEAdvertising* advertising = BLEDevice::getAdvertising();
    advertising->addServiceUUID(SERVICE_UUID);
    advertising->setScanResponse(true);
    BLEDevice::startAdvertising();
    Serial.println("BLE Ready: 待機中...");
}

void loop() {
    updateButtonPulses();
    handleBLEConnection();
    pollNowSerial();
    if (nowPeerReady) handleRadioPackets();

    const uint32_t now = controlNow();
    if (configPending && millis() - configCreatedAt >= 600000UL) configFail("TIMEOUT");
    if (!simulation.active) {
        ffheater::Command effect = controller.tick(now);
        handleCommands(now, effect);
        mergeEffect(controller.tick(now), effect);
        // Apply once: a same-iteration OFF always wins over ON.
        if (effect == ffheater::Command::OFF) triggerButton(BTN_OFF);
        else if (effect == ffheater::Command::ON) triggerButton(BTN_ON);
    } else {
        // BLE commands are rejected in handleCommands; call it to drain its
        // queue without permitting a queued command to leak into normal mode.
        ffheater::Command ignored = ffheater::Command::NONE;
        handleCommands(now, ignored);
    }

    if (!simulation.active) {
        const bool cooldown = controller.cooldownActive(now);
        if (cooldown != persistedCooldown) {
            prefs.putBool("autooffcool", cooldown);
            persistedCooldown = cooldown;
        }
    }
    updateLedPattern();
    sampleHistory(now);
    handleBLENotify(now);
    sendHistory(now);
    delay(1);
}
