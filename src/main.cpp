#include <Arduino.h>
#include <Preferences.h>
#include <DHT.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ==================== BLE UUID設定 ====================
#define SERVICE_UUID           "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_STATUS_UUID       "beb5483e-36e1-4688-b7f5-ea07361b26a8" // Notify用 (状態送信)
#define CHAR_CMD_UUID          "8c772be5-e0d0-4251-bb5c-1f6874eb7310" // Write用 (コマンド受信)

BLEServer* pServer = NULL;
BLECharacteristic* pStatusChar = NULL;
bool deviceConnected = false;

// ==================== ピン・ボタン定義 ====================
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
    {25, "電源ON",   0, false, 0, 0},
    {32, "電源OFF",  0, false, 0, 0},
    {26, "温度上げ", 0, false, 0, 0},
    {27, "温度下げ", 0, false, 0, 0}
};
const int BUTTON_COUNT = sizeof(buttons) / sizeof(ButtonPulse);

const int PIN_DHT_ROOM = 13; 
const int PIN_DHT_DUCT = 14; 
DHT dhtRoom(PIN_DHT_ROOM, DHT11);
DHT dhtDuct(PIN_DHT_DUCT, DHT11);

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

// ==================== LED点滅パターン定義 ====================
// 点滅間隔(ms)
#define BLINK_INTERVAL_MS 200

// 各ボタン・操作ごとの回数定義
#define PATTERN_ON_COUNT      1  // 0.2秒間点灯(1回点滅/長め)
#define PATTERN_OFF_COUNT     5  // 1秒間点灯(0.2s * 5回=1秒) ※回数で表現
#define PATTERN_UP_COUNT      2  // 2回点滅
#define PATTERN_DOWN_COUNT    3  // 3回点滅
#define PATTERN_SETTING_COUNT 4  // 4回点滅

// ==================== LED制御用変数 ====================
const int PIN_LED = 2;
int remainingBlinks = 0;
unsigned long nextBlinkTime = 0;
bool ledState = false;

// ボタンの種類とパターンを紐付け
void triggerLedPattern(int count) {
    remainingBlinks = count * 2; // ON/OFFの切り替え回数
    ledState = false;
    nextBlinkTime = millis();
}
// ==================== 関数定義 ====================
void triggerButton(ButtonType btn) {
    if (buttons[btn].pressCount > 0) return;
    if (btn == BTN_ON || btn == BTN_OFF) {
        buttons[btn].pressCount = 2;
    } else {
        buttons[btn].pressCount = 1;
    }
    buttons[btn].nextPressTime = millis(); 
}

void updateButtonPulses() {
    unsigned long currentMillis = millis();
    for (int i = 0; i < BUTTON_COUNT; i++) {
        if (buttons[i].isPinHigh && currentMillis >= buttons[i].turnOffTime) {
            digitalWrite(buttons[i].pin, LOW);
            buttons[i].isPinHigh = false;
            buttons[i].nextPressTime = currentMillis + 1000;
        }
        if (!buttons[i].isPinHigh && buttons[i].pressCount > 0 && currentMillis >= buttons[i].nextPressTime) {
            digitalWrite(buttons[i].pin, HIGH);
            buttons[i].isPinHigh = true;
            buttons[i].turnOffTime = currentMillis + 500; 
            buttons[i].pressCount--;                      
        }
    }
}

// ==================== BLEコールバック処理 ====================
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        deviceConnected = true;
        Serial.println("BLE: スマホと接続完了");
    }
    void onDisconnect(BLEServer* pServer) {
        deviceConnected = false;
        Serial.println("BLE: 切断検知。アドバタイズ再開");
        BLEDevice::startAdvertising();
    }
};

class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pChar) {
        String value = pChar->getValue().c_str();
        if (value.length() == 0) return;

        Serial.print("BLE受信: "); Serial.println(value);

        // 1. 受信確認(ACK)をスマホへ即座に送信
        pStatusChar->setValue(("ACK," + value.substring(2)).c_str());
        pStatusChar->notify();

        // 2. パターン分岐とLED点滅、機能実行
        if (value.startsWith("B,")) {
            int btnIdx = value.substring(2).toInt();
            if (btnIdx >= 0 && btnIdx < BUTTON_COUNT) {
                triggerButton((ButtonType)btnIdx);
                
                // ボタンごとのLEDパターン選択
                if (btnIdx == 0) triggerLedPattern(PATTERN_ON_COUNT);
                else if (btnIdx == 1) triggerLedPattern(PATTERN_OFF_COUNT);
                else if (btnIdx == 2) triggerLedPattern(PATTERN_UP_COUNT);
                else if (btnIdx == 3) triggerLedPattern(PATTERN_DOWN_COUNT);
            }
        }
        else if (value.startsWith("A,")) {
            int mode = value.substring(2).toInt();
            if (mode == 1) {
                autoModeActive = true;
                autoModeStartTime = millis();
                currentHeaterState = HEATER_OFF;
                // 必要ならここで triggerLedPattern(パターン); を呼ぶ
            } else {
                autoModeActive = false;
                if (currentHeaterState != HEATER_OFF) {
                    triggerButton(BTN_OFF);
                    currentHeaterState = HEATER_OFF;
                }
            }
        }
        else if (value.startsWith("S,")) {
            int dur; float onT, offT, ductT;
            if (sscanf(value.c_str(), "S,%d,%f,%f,%f", &dur, &onT, &offT, &ductT) == 4) {
                autoModeMinutes = dur;
                targetOnTemp = onT;
                targetOffTemp = offT;
                ductThreshTemp = ductT;
                
                prefs.putInt("duration", autoModeMinutes);
                prefs.putFloat("ontemp", targetOnTemp);
                prefs.putFloat("offtemp", targetOffTemp);
                prefs.putFloat("ductthresh", ductThreshTemp);
                
                // 設定保存成功時のLEDパターン
                triggerLedPattern(PATTERN_SETTING_COUNT);
                Serial.println("設定値を不揮発メモリへ保存完了");
            }
        }
    }
};

// ==================== 初期設定 ====================
void setup() {
    Serial.begin(115200);
    
    for (int i = 0; i < BUTTON_COUNT; i++) {
        pinMode(buttons[i].pin, OUTPUT);
        digitalWrite(buttons[i].pin, LOW);
    }
    
    dhtRoom.begin();
    dhtDuct.begin();
    
    prefs.begin("heater-config", false);
    autoModeMinutes = prefs.getInt("duration", 60);
    targetOnTemp = prefs.getFloat("ontemp", 10.0);
    targetOffTemp = prefs.getFloat("offtemp", 15.0);
    ductThreshTemp = prefs.getFloat("ductthresh", 5.0);
    
    // BLE初期化
    BLEDevice::init("FF_Heater");
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    BLEService *pService = pServer->createService(SERVICE_UUID);

    pStatusChar = pService->createCharacteristic(
                    CHAR_STATUS_UUID,
                    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
                  );
    pStatusChar->addDescriptor(new BLE2902());

    BLECharacteristic *pCmdChar = pService->createCharacteristic(
                                    CHAR_CMD_UUID,
                                    BLECharacteristic::PROPERTY_WRITE
                                  );
    pCmdChar->setCallbacks(new MyCallbacks());

    pService->start();
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    BLEDevice::startAdvertising();
    
    Serial.println("BLE Ready: スマホからの接続待機中...");
}

// ==================== メインループ ====================
void loop() {
    // LED点滅ロジック (非同期)
    if (remainingBlinks > 0 && millis() >= nextBlinkTime) {
        ledState = !ledState;
        digitalWrite(PIN_LED, ledState ? HIGH : LOW);
        remainingBlinks--;
        nextBlinkTime = millis() + BLINK_INTERVAL_MS;
    }
    updateButtonPulses(); 
    
    static unsigned long lastSensorRead = 0;
    if (millis() - lastSensorRead >= 2000) {
        lastSensorRead = millis();
        float r = dhtRoom.readTemperature();
        float d = dhtDuct.readTemperature();
        if (!isnan(r)) currentRoomTemp = r;
        if (!isnan(d)) currentDuctTemp = d;
        
        // BLE接続中のみステータスを送信
        if (deviceConnected) {
            long remaining = 0;
            if (autoModeActive) {
                remaining = (autoModeMinutes * 60) - ((millis() - autoModeStartTime) / 1000);
                if (remaining < 0) remaining = 0;
            }
            
            // CSV形式で送信 "R,室温,ダクト温,自動モード,状態,残り秒数,設定タイマー,ON温,OFF温,ダクト温"
            char statusStr[128];
            snprintf(statusStr, sizeof(statusStr), "R,%.1f,%.1f,%d,%d,%ld,%d,%.1f,%.1f,%.1f",
                     currentRoomTemp, currentDuctTemp, autoModeActive ? 1 : 0, currentHeaterState, remaining,
                     autoModeMinutes, targetOnTemp, targetOffTemp, ductThreshTemp);
            
            pStatusChar->setValue(statusStr);
            pStatusChar->notify();
        }
    }
    
    if (autoModeActive) {
        if (millis() - autoModeStartTime >= ((unsigned long)autoModeMinutes * 60 * 1000)) {
            autoModeActive = false; 
            if (currentHeaterState != HEATER_OFF) {
                triggerButton(BTN_OFF); 
                currentHeaterState = HEATER_OFF;
            }
            return;
        }
        
        switch (currentHeaterState) {
            case HEATER_OFF:
                if (currentRoomTemp <= targetOnTemp && currentRoomTemp > 0.0) {
                    triggerButton(BTN_ON); 
                    ignitionStartTime = millis();
                    ignitionStartDuctTemp = currentDuctTemp; 
                    currentHeaterState = HEATER_IGNITING;
                    Serial.println("点火プロセス開始。");
                }
                break;
                
            case HEATER_IGNITING:
                if (currentDuctTemp >= (ignitionStartDuctTemp + ductThreshTemp)) {
                    currentHeaterState = HEATER_ON;
                    Serial.println("点火成功検知。通常運転モードへ。");
                }
                else if (millis() - ignitionStartTime >= IGNITION_TIMEOUT_MS) {
                    Serial.println("不発判定。再点火試行。");
                    triggerButton(BTN_ON); 
                    ignitionStartTime = millis(); 
                    ignitionStartDuctTemp = currentDuctTemp; 
                }
                break;
                
            case HEATER_ON:
                if (currentRoomTemp >= targetOffTemp) {
                    triggerButton(BTN_OFF); 
                    currentHeaterState = HEATER_OFF;
                    Serial.println("目標温度達成。消火。");
                }
                break;
        }
    }
}