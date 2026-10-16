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

// Version 1 stays compatible with the sensor node. Invalid temperatures are NaN.
struct SensorData {
    uint8_t version;
    float roomTemp;
    float roomHum;
    float ductTemp;
    float ductHum;
    float batteryVoltage;
};
static_assert(sizeof(SensorData) == 24, "Sensor packet layout changed");
struct ReceivedSensor { SensorData data; uint32_t receivedAt; };
struct ReceivedCommand { char text[80]; };
QueueHandle_t sensorQueue = nullptr;
QueueHandle_t commandQueue = nullptr;
const uint8_t SENDER_MAC[] = {0xEC, 0x61, 0x60, 0x93, 0xf8, 0x14};

#define SERVICE_UUID     "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_STATUS_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CHAR_CMD_UUID    "8c772be5-e0d0-4251-bb5c-1f6874eb7310"
BLEServer* pServer = nullptr;
BLECharacteristic* pStatusChar = nullptr;
std::atomic<bool> deviceConnected{false};
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
    if (!sensorQueue || len != sizeof(SensorData) || memcmp(mac, SENDER_MAC, 6) != 0) return;
    ReceivedSensor packet{};
    memcpy(&packet.data, bytes, sizeof(packet.data));
    if (packet.data.version != 1) return;
    packet.receivedAt = millis();
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
        if (value == "A,0" || value == "B,1" || value == "B,STOP") {
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
    while (xQueueReceive(sensorQueue, &packet, 0) == pdTRUE)
        controller.receive(packet.data.roomTemp, packet.data.ductTemp, packet.receivedAt);

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
    handleBLENotify(now);
    delay(1);
}
