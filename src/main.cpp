#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h> // ESP-NOW安定化用
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ==================== データ構造 ====================
typedef struct {
    uint8_t version;
    float roomTemp;
    float roomHum;
    float ductTemp;
    float ductHum;
    float batteryVoltage;
} SensorData;

SensorData receivedData;
unsigned long lastReceivedTime = 0; 
const unsigned long TIMEOUT_THRESHOLD = 15000; 

// ==================== BLE UUID設定 ====================
#define SERVICE_UUID           "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_STATUS_UUID       "beb5483e-36e1-4688-b7f5-ea07361b26a8" 
#define CHAR_CMD_UUID          "8c772be5-e0d0-4251-bb5c-1f6874eb7310" 

BLEServer* pServer = NULL;
BLECharacteristic* pStatusChar = NULL;
bool deviceConnected = false;
bool oldDeviceConnected = false; 

// ==================== ピン・ボタン定義 ====================
#ifdef TARGET_ESP32_S3
  #define PIN_ON    1   
  #define PIN_OFF   2
  #define PIN_UP    3
  #define PIN_DOWN  4
#else
  #define PIN_ON    25
  #define PIN_OFF   32
  #define PIN_UP    26
  #define PIN_DOWN  27
#endif

enum ButtonType { BTN_ON, BTN_OFF, BTN_UP, BTN_DOWN };

struct ButtonPulse {
    int pin;
    String label;
    unsigned long turnOffTime;
    bool isPinHigh;
    int pressCount;
    unsigned long nextPressTime;
};

ButtonPulse buttons[] = {
    {PIN_ON,   "電源ON",   0, false, 0, 0},
    {PIN_OFF,  "電源OFF",  0, false, 0, 0},
    {PIN_UP,   "UP",       0, false, 0, 0},
    {PIN_DOWN, "DOWN",     0, false, 0, 0}
};

const int BUTTON_COUNT = sizeof(buttons) / sizeof(ButtonPulse);

// ==================== グローバル制御変数 ====================
Preferences prefs;
float targetOnTemp = 10.0;     
float targetOffTemp = 15.0;    
float ductThreshTemp = 5.0;    
int autoModeMinutes = 60;      

bool autoModeActive = false;
unsigned long autoModeStartTime = 0;

enum HeaterState { HEATER_OFF, HEATER_IGNITING, HEATER_ON };
HeaterState currentHeaterState = HEATER_OFF;

unsigned long ignitionStartTime = 0;
float ignitionStartDuctTemp = 0.0;
const unsigned long IGNITION_TIMEOUT_MS = 300000; 

float currentRoomTemp = 0.0;
float currentDuctTemp = 0.0;

// ==================== LED制御 ====================
const int PIN_LED = 2;

struct LedPattern { const char* sequence; };

enum LedPatternType {
    PATTERN_ON, PATTERN_OFF, PATTERN_UP, PATTERN_DOWN,
    PATTERN_AUTO, PATTERN_SETTING, PATTERN_BLE_CONN,
    PATTERN_EMERGENCY, PATTERN_BLE_DISCONN, PATTERN_COUNT
};

const LedPattern patterns[] = {
    {"o-o"},         // PATTERN_ON
    {"OO"},          // PATTERN_OFF
    {"o-o"},         // PATTERN_UP
    {"O-o-O"},       // PATTERN_DOWN
    {"O-O-O"},       // PATTERN_AUTO
    {"o-O-o-O"},     // PATTERN_SETTING
    {"O-O"},         // PATTERN_BLE_CONN
    {"o-o-o-O-O-O-o-o-o--o-o-o-O-O-O-o-o-o"}, // PATTERN_EMERGENCY
    {"o-o-o-o-O"}    // PATTERN_BLE_DISCONN
};

int currentStep = 0;
const char* activeSequence = "";
unsigned long lastChangeTime = 0;
bool isPatternRunning = false;

// ==================== 関数定義 ====================
void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
    memcpy((void*)&receivedData, incomingData, sizeof(receivedData));
    currentRoomTemp = receivedData.roomTemp;
    currentDuctTemp = receivedData.ductTemp;
    lastReceivedTime = millis();
}

void startPattern(LedPatternType patternType) {
    if (patternType < 0 || patternType >= PATTERN_COUNT) return;
    activeSequence = patterns[patternType].sequence;
    currentStep = 0;
    isPatternRunning = true;
    lastChangeTime = 0;
}

void triggerButton(ButtonType btnType) {
    if (buttons[btnType].pressCount > 0) return;
    buttons[btnType].pressCount = 1;
    buttons[btnType].nextPressTime = millis(); 
    
    switch (btnType) {
        case BTN_ON:   startPattern(PATTERN_ON);   break;
        case BTN_OFF:  startPattern(PATTERN_OFF);  break;
        case BTN_UP:   startPattern(PATTERN_UP);   break;
        case BTN_DOWN: startPattern(PATTERN_DOWN); break;
    }
}

void updateButtonPulses() {
    unsigned long currentMillis = millis();
    for (int i = 0; i < BUTTON_COUNT; i++) {
        if (buttons[i].isPinHigh && currentMillis >= buttons[i].turnOffTime) {
            digitalWrite(buttons[i].pin, LOW);
            buttons[i].isPinHigh = false;
            buttons[i].nextPressTime = currentMillis + 500;
        }
        if (!buttons[i].isPinHigh && buttons[i].pressCount > 0 && currentMillis >= buttons[i].nextPressTime) {
            digitalWrite(buttons[i].pin, HIGH);
            buttons[i].isPinHigh = true;
            buttons[i].turnOffTime = currentMillis + 200; // すべて200ms
            buttons[i].pressCount--;                      
        }
    }
}

void updateLedPattern() {
    if (!isPatternRunning) return;
    unsigned long now = millis();
    char cmd = activeSequence[currentStep];

    if (cmd == '\0') {
        digitalWrite(PIN_LED, LOW);
        isPatternRunning = false;
        return;
    }

    unsigned long duration = (cmd == 'O') ? 600 : 200;
    if (now - lastChangeTime >= duration) {
        lastChangeTime = now;
        digitalWrite(PIN_LED, LOW);
        if (cmd == 'O' || cmd == 'o') { digitalWrite(PIN_LED, HIGH); }
        currentStep++;
    }
}

// ==================== BLE クラス ====================
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        deviceConnected = true;
        Serial.println("BLE: スマホと接続完了");
        startPattern(PATTERN_BLE_CONN);
    }
    void onDisconnect(BLEServer* pServer) {
        deviceConnected = false;
        startPattern(PATTERN_BLE_DISCONN); 
        Serial.println("BLE: 切断検知。");
    }
};

class MyCallbacks: public BLECharacteristicCallbacks {
void onWrite(BLECharacteristic *pChar) {
        String value = pChar->getValue().c_str();
        if (value.length() == 0) return;
        Serial.print("BLE受信: "); Serial.println(value);

        pStatusChar->setValue(("ACK," + value.substring(2)).c_str());
        pStatusChar->notify();

        if (value.startsWith("B,")) {
            if (value == "B,STOP") {
                startPattern(PATTERN_EMERGENCY);
                triggerButton(BTN_OFF);
                Serial.println("緊急停止コマンド受信");
            } else {
                int btnIdx = value.substring(2).toInt();
                if (btnIdx >= 0 && btnIdx < BUTTON_COUNT) {
                    triggerButton((ButtonType)btnIdx);
                    startPattern((LedPatternType)btnIdx);
                }
            }
        }
        else if (value.startsWith("A,")) {
            int mode = value.substring(2).toInt();
            if (mode == 1) {
                // ★修正: 稼働中の再開時はリセットせず状態を維持する
                if (!autoModeActive) { currentHeaterState = HEATER_OFF; }
                autoModeActive = true;
                autoModeStartTime = millis();
                startPattern(PATTERN_AUTO);
            } else {
                autoModeActive = false;
                if (currentHeaterState != HEATER_OFF) {
                    triggerButton(BTN_OFF);
                    currentHeaterState = HEATER_OFF;
                    startPattern(PATTERN_OFF);
                }
            }
        }
        else if (value.startsWith("S,")) {
            // ★修正: sscanfのバグを回避して確実に文字列を分割・保存する
            int c1 = value.indexOf(',');
            int c2 = value.indexOf(',', c1 + 1);
            int c3 = value.indexOf(',', c2 + 1);
            int c4 = value.indexOf(',', c3 + 1);

            if (c1 > 0 && c2 > 0 && c3 > 0 && c4 > 0) {
                autoModeMinutes = value.substring(c1 + 1, c2).toInt();
                targetOnTemp = value.substring(c2 + 1, c3).toFloat();
                targetOffTemp = value.substring(c3 + 1, c4).toFloat();
                ductThreshTemp = value.substring(c4 + 1).toFloat();
                
                prefs.putInt("duration", autoModeMinutes);
                prefs.putFloat("ontemp", targetOnTemp);
                prefs.putFloat("offtemp", targetOffTemp);
                prefs.putFloat("ductthresh", ductThreshTemp);
                
                startPattern(PATTERN_SETTING);
                Serial.println("設定値を不揮発メモリへ保存完了");
            }
        }
    }
};

// ==================== setup ====================
void setup() {
    Serial.begin(115200);

    // ★修正: ESP-NOW安定化のためチャネルを1に固定
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE); 

    if (esp_now_init() != ESP_OK) { Serial.println("Error ESP-NOW"); }
    
    esp_now_peer_info_t peerInfo = {};
    uint8_t senderMac[] = {0xEC, 0x61, 0x60, 0x93, 0xf8, 0x14};
    memcpy(peerInfo.peer_addr, senderMac, 6);
    peerInfo.channel = 1;
    peerInfo.encrypt = false;
    if (esp_now_add_peer(&peerInfo) != ESP_OK) { Serial.println("Failed peer"); }

    esp_now_register_recv_cb(OnDataRecv);
    
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);
    for (int i = 0; i < BUTTON_COUNT; i++) {
        pinMode(buttons[i].pin, OUTPUT);
        digitalWrite(buttons[i].pin, LOW);
    }
    
    prefs.begin("heater-config", false);
    autoModeMinutes = prefs.getInt("duration", 60);
    targetOnTemp = prefs.getFloat("ontemp", 10.0);
    targetOffTemp = prefs.getFloat("offtemp", 15.0);
    ductThreshTemp = prefs.getFloat("ductthresh", 5.0);
    
    BLEDevice::init("FF_Heater");
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    BLEService *pService = pServer->createService(SERVICE_UUID);
    pStatusChar = pService->createCharacteristic(CHAR_STATUS_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    pStatusChar->addDescriptor(new BLE2902());
    BLECharacteristic *pCmdChar = pService->createCharacteristic(CHAR_CMD_UUID, BLECharacteristic::PROPERTY_WRITE);
    pCmdChar->setCallbacks(new MyCallbacks());

    pService->start();
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    BLEDevice::startAdvertising();
    
    Serial.println("BLE Ready: 待機中...");
}

// ==================== 独立処理モジュール ====================
void handleBLEConnection() {
    if (!deviceConnected && oldDeviceConnected) {
        delay(500); 
        pServer->getAdvertising()->start();
        oldDeviceConnected = deviceConnected;
    }
    if (deviceConnected && !oldDeviceConnected) {
        oldDeviceConnected = deviceConnected;
    }
}

void handleAutoControl() {
    if (!autoModeActive) return;

    unsigned long currentMillis = millis();
    if (currentMillis - autoModeStartTime >= (autoModeMinutes * 60000UL)) {
        autoModeActive = false;
        if (currentHeaterState != HEATER_OFF) {
            triggerButton(BTN_OFF); 
            currentHeaterState = HEATER_OFF;
        }
        return;
    } 

    switch (currentHeaterState) {
        case HEATER_OFF:
            if (currentRoomTemp <= targetOnTemp) {
                triggerButton(BTN_ON);
                currentHeaterState = HEATER_IGNITING;
                ignitionStartTime = currentMillis;
                ignitionStartDuctTemp = currentDuctTemp;
            }
            break;

        case HEATER_IGNITING:
            if (currentMillis - ignitionStartTime >= IGNITION_TIMEOUT_MS) {
                if (currentDuctTemp >= (ignitionStartDuctTemp + ductThreshTemp)) {
                    currentHeaterState = HEATER_ON;
                } else {
                    triggerButton(BTN_ON);
                    ignitionStartTime = currentMillis; 
                }
            }
            break;

        case HEATER_ON:
            if (currentRoomTemp >= targetOffTemp) {
                triggerButton(BTN_OFF);
                currentHeaterState = HEATER_OFF;
            }
            break;
    }
}

unsigned long lastNotifyTime = 0;
void handleBLENotify() {
    if (deviceConnected && (millis() - lastNotifyTime >= 2000)) {
        lastNotifyTime = millis();
        unsigned long dataAgeSeconds = (millis() - lastReceivedTime) / 1000;
        
        int remainSec = 0;
        if (autoModeActive) {
            unsigned long elapsed = millis() - autoModeStartTime;
            unsigned long total = autoModeMinutes * 60000UL;
            if (total > elapsed) remainSec = (total - elapsed) / 1000;
        }

        char notifyMsg[128];
        snprintf(notifyMsg, sizeof(notifyMsg), "R,%.1f,%.1f,%d,%d,%d,%d,%.1f,%.1f,%.1f,%lu", 
                 currentRoomTemp, currentDuctTemp, autoModeActive ? 1 : 0, 
                 currentHeaterState, remainSec, autoModeMinutes, 
                 targetOnTemp, targetOffTemp, ductThreshTemp, dataAgeSeconds);
                 
        pStatusChar->setValue(notifyMsg);
        pStatusChar->notify();
    }
}

void checkFailSafe() {
    if (millis() - lastReceivedTime > TIMEOUT_THRESHOLD) {
        if (currentHeaterState != HEATER_OFF) {
            triggerButton(BTN_OFF);
            currentHeaterState = HEATER_OFF;
            autoModeActive = false;
        }
    }
}

// ==================== メインループ ====================
void loop() {
    updateLedPattern();
    updateButtonPulses(); 
    handleBLEConnection();
    handleAutoControl();
    handleBLENotify(); 
    checkFailSafe();
}