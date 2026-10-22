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

using ffheater::ReceivedSensor;
struct ReceivedCommand { char text[80]; };
QueueHandle_t sensorQueue = nullptr;
QueueHandle_t commandQueue = nullptr;
const uint8_t SENDER_MAC[] = {0xEC, 0x61, 0x60, 0x93, 0xf8, 0x14};

#define SERVICE_UUID     "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_STATUS_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CHAR_HISTORY_UUID "e93c15d4-f791-4bb1-9daf-88ae6f946be1"
#define CHAR_CMD_UUID    "8c772be5-e0d0-4251-bb5c-1f6874eb7310"
BLEServer* pServer = nullptr;
BLECharacteristic* pStatusChar = nullptr;
BLECharacteristic* pHistoryChar = nullptr;
std::atomic<bool> deviceConnected{false};
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
        digitalWrite(PIN_ON, LOW);
        buttons[BTN_ON].high = false;
    }
    auto& pulse = buttons[button];
    if (pulse.high) return;
    digitalWrite(pulse.pin, HIGH);
    pulse.high = true;
    pulse.startedAt = millis();
    startPattern(static_cast<LedPatternType>(button));
    Serial.println(pulse.label);
}

void updateButtonPulses() {
    for (auto& pulse : buttons) {
        if (pulse.high && millis() - pulse.startedAt >= 200) {
            digitalWrite(pulse.pin, LOW);
            pulse.high = false;
        }
    }
}

void OnDataRecv(const uint8_t* mac, const uint8_t* bytes, int len) {
    if (!sensorQueue) return;
    ReceivedSensor packet{};
    if (!ffheater::decodeSensorPacket(mac, bytes, len, millis(), packet)) return;
    xQueueSend(sensorQueue, &packet, 0);
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
        bool accepted = true;
        if (value == "H,1") {
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

void setup() {
    Serial.begin(115200);
    if (PIN_LED >= 0) pinMode(PIN_LED, OUTPUT);
    writeLed(false);
    for (auto& pulse : buttons) {
        pinMode(pulse.pin, OUTPUT);
        digitalWrite(pulse.pin, LOW);
    }
    sensorQueue = xQueueCreate(4, sizeof(ReceivedSensor));
    commandQueue = xQueueCreate(8, sizeof(ReceivedCommand));
    if (!sensorQueue || !commandQueue) {
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

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    Serial.printf("FF heater receiver: 20261016-rx1, STA MAC=%s, channel=%d\n",
        WiFi.macAddress().c_str(), WiFi.channel());
    if (esp_now_init() == ESP_OK) {
        esp_now_peer_info_t peer{};
        memcpy(peer.peer_addr, SENDER_MAC, 6);
        peer.channel = 1;
        peer.encrypt = false;
        esp_now_add_peer(&peer);
        esp_now_register_recv_cb(OnDataRecv);
    } else Serial.println("ESP-NOW init failed");

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
    ReceivedSensor packet{};
    while (xQueueReceive(sensorQueue, &packet, 0) == pdTRUE) {
        Serial.printf("Sensor RX %02X:%02X:%02X:%02X:%02X:%02X room=%.1f duct=%.1f\n",
            packet.sourceMac[0], packet.sourceMac[1], packet.sourceMac[2],
            packet.sourceMac[3], packet.sourceMac[4], packet.sourceMac[5],
            packet.data.roomTemp, packet.data.ductTemp);
        controller.receive(packet.data.roomTemp, packet.data.ductTemp, packet.receivedAt);
    }

    const uint32_t now = millis();
    ffheater::Command effect = controller.tick(now);
    handleCommands(now, effect);
    mergeEffect(controller.tick(now), effect);
    // Apply once: a same-iteration OFF always wins over ON.
    if (effect == ffheater::Command::OFF) triggerButton(BTN_OFF);
    else if (effect == ffheater::Command::ON) triggerButton(BTN_ON);

    const bool cooldown = controller.cooldownActive(now);
    if (cooldown != persistedCooldown) {
        prefs.putBool("autooffcool", cooldown);
        persistedCooldown = cooldown;
    }
    updateLedPattern();
    sampleHistory(now);
    handleBLENotify(now);
    sendHistory(now);
    delay(1);
}
