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
#ifdef TARGET_ESP32_S3
  // ESP32-S3用のピン定義
  #define PIN_ON    1   // 例: S3で使えるピンに変更
  #define PIN_OFF   2
  #define PIN_UP    3
  #define PIN_DOWN  4
#else
  // 元のESP32用のピン定義
  #define PIN_ON    25
  #define PIN_OFF   26
  #define PIN_UP    27
  #define PIN_DOWN  14
#endif

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

// 初期化時に上記のマクロ判定ピンを配列に渡す
ButtonPulse buttons[] = {
    {PIN_ON,   "電源ON",   0, false, 0, 0},
    {PIN_OFF,  "電源OFF",  0, false, 0, 0},
    {PIN_UP,   "UP",       0, false, 0, 0},
    {PIN_DOWN, "DOWN",     0, false, 0, 0}
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
// #define BLINK_INTERVAL_MS 200

// ==================== LED制御用変数 ====================
const int PIN_LED = 2;

// --- LEDパターン管理 ---
struct LedPattern {
    const char* sequence;
};

enum LedPatternType {
    PATTERN_ON,
    PATTERN_OFF,
    PATTERN_UP,
    PATTERN_DOWN,
    PATTERN_AUTO,
    PATTERN_SETTING,
    PATTERN_BLE_CONN,
    PATTERN_EMERGENCY,
    PATTERN_BLE_DISCONN, // 追加したいパターンもここに追加可能
    PATTERN_COUNT        // パターンの総数を自動取得
};
// 8つのパターンを定義（ここを書き換えればいつでも変更可能）
const LedPattern patterns[] = {
    {"o-o"},         // PATTERN_ON
    {"OO"},       // PATTERN_OFF
    {"o-o"},     // PATTERN_UP
    {"O-o-O"},     // PATTERN_DOWN
    {"O-O-O"},       // PATTERN_AUTO
    {"o-O-o-O"},     // PATTERN_SETTING
    {"O-O"},       // PATTERN_BLE_CONN
    {"o-o-o-O-O-O-o-o-o--o-o-o-O-O-O-o-o-o"},     // PATTERN_EMERGENCY
    {"o-o-o-o-O"}    // PATTERN_BLE_DISCONN (追加分)
};

// 状態管理変数
int currentStep = 0;
const char* activeSequence = "";
unsigned long lastChangeTime = 0;
bool isPatternRunning = false;
const int LED_PIN = 2; // お使いのボードのLEDピン番号に合わせてください


// 引数の型を int ではなく LedPatternType に変更
void startPattern(LedPatternType patternType) {
    // 範囲外チェック（安全対策）
    if (patternType < 0 || patternType >= PATTERN_COUNT) return;
    
    activeSequence = patterns[patternType].sequence;
    currentStep = 0;
    isPatternRunning = true;
    lastChangeTime = 0;
}

// ==================== 関数定義 ====================
void triggerButton(ButtonType btnType) {
    if (buttons[btnType].pressCount > 0) return;
    if (btnType == BTN_ON || btnType == BTN_OFF) {
        buttons[btnType].pressCount = 2;
    } else {
        buttons[btnType].pressCount = 1;
    }
    buttons[btnType].nextPressTime = millis(); 
    // LEDパターン呼び出しの追加
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
        // 例：BLE接続が確立したとき (setup内やイベントハンドラ内)
        startPattern(PATTERN_BLE_CONN); // BLE_CONN パターン開始
    }
    void onDisconnect(BLEServer* pServer) {
        deviceConnected = false;
        startPattern(PATTERN_BLE_DISCONN); // ★ここが切断時のトリガー
        Serial.println("BLE: 切断検知。アドバタイズ再開");
        delay(500);
        pServer->getAdvertising()->start();
    }
};

class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pChar) {
        String value = pChar->getValue().c_str();
        if (value.length() == 0) return;

        Serial.print("BLE受信: "); Serial.println(value);

        // 1. 受信確認(ACK)
        pStatusChar->setValue(("ACK," + value.substring(2)).c_str());
        pStatusChar->notify();

        // 2. パターン分岐とLED点滅、機能実行
        if (value.startsWith("B,")) {
            int btnIdx = value.substring(2).toInt();
            if (btnIdx >= 0 && btnIdx < BUTTON_COUNT) {
                triggerButton((ButtonType)btnIdx);
                startPattern((LedPatternType)btnIdx); // ★ここに追加：0:ON, 1:OFF, 2:UP, 3:DOWN がそのまま対応
            }
            // もし「B,STOP」のような独自のSTOPコマンドを作るならここに書く
            else if (value == "B,STOP") {
                startPattern(PATTERN_EMERGENCY); // ★EMERGENCYパターン
            }
        }
        else if (value.startsWith("A,")) {
            int mode = value.substring(2).toInt();
            if (mode == 1) {
                autoModeActive = true;
                autoModeStartTime = millis();
                currentHeaterState = HEATER_OFF;
                startPattern(PATTERN_AUTO); // ★AUTO開始パターン
            } else {
                autoModeActive = false;
                if (currentHeaterState != HEATER_OFF) {
                    triggerButton(BTN_OFF);
                    currentHeaterState = HEATER_OFF;
                    startPattern(PATTERN_OFF); // ★OFFパターン
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
                
                startPattern(PATTERN_SETTING); // ★設定保存完了パターン
                Serial.println("設定値を不揮発メモリへ保存完了");
            }
        }
    }
};
void updateLedPattern() {
    if (!isPatternRunning) return;

    unsigned long now = millis();
    char cmd = activeSequence[currentStep];

    if (cmd == '\0') {
        digitalWrite(LED_PIN, LOW);
        isPatternRunning = false;
        return;
    }

    unsigned long duration = (cmd == 'O') ? 700 : 300;
    
    if (now - lastChangeTime >= duration) {
        lastChangeTime = now;
        if (cmd == '-') { digitalWrite(LED_PIN, LOW); }
        else { digitalWrite(LED_PIN, HIGH); } // O または o
        
        // 消灯処理: 光る要素(O,o)の直後に必ず消灯時間を挟むロジック
        if (cmd != '-') {
            // 次のステップで消灯させるために少し待つ処理が必要な場合は調整
        }
        currentStep++;
    }
}

// ==================== 初期設定 ====================
void setup() {
    Serial.begin(115200);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW); // 初期状態は消灯

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
    // 追加：LED点滅処理を常に呼び出す
    updateLedPattern();

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