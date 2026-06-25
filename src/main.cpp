#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h> 
#include <Preferences.h>
#include <DHT.h>
#include "secret.h" // 【追加】外部化したWi-Fi情報の読み込み

// ==================== 【要設定】Wi-Fi接続先の複数登録 ====================
WiFiMulti wifiMulti;

// ==================== ピンアサイン（無印ESP32 38PIN準拠） ====================
const int PIN_PC_BTN1 = 25; 
const int PIN_PC_BTN2 = 26; 
const int PIN_PC_BTN3 = 27; 
const int PIN_PC_BTN4 = 32; 

const int PIN_DHT_ROOM = 13; 
const int PIN_DHT_DUCT = 14; 

// ==================== センサー初期化 ====================
DHT dhtRoom(PIN_DHT_ROOM, DHT11);
DHT dhtDuct(PIN_DHT_DUCT, DHT11);

// ==================== Webサーバー設定 ====================
WebServer server(80);

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

struct ButtonPulse {
    int pin;
    unsigned long turnOffTime;
    bool isActive;
};
ButtonPulse buttons[4] = {
    {PIN_PC_BTN1, 0, false},
    {PIN_PC_BTN2, 0, false},
    {PIN_PC_BTN3, 0, false},
    {PIN_PC_BTN4, 0, false}
};

float currentRoomTemp = 0.0;
float currentDuctTemp = 0.0;

// ==================== 関数定義 ====================

void triggerButton(int buttonIndex, unsigned long durationMs = 500) {
    if (buttonIndex < 0 || buttonIndex >= 4) return;
    digitalWrite(buttons[buttonIndex].pin, HIGH);
    buttons[buttonIndex].turnOffTime = millis() + durationMs;
    buttons[buttonIndex].isActive = true;
}

void updateButtonPulses() {
    unsigned long currentMillis = millis();
    for (int i = 0; i < 4; i++) {
        if (buttons[i].isActive && currentMillis >= buttons[i].turnOffTime) {
            digitalWrite(buttons[i].pin, LOW);
            buttons[i].isActive = false;
        }
    }
}

void handleRoot() {
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>";
    html += "<style>body{font-family:sans-serif;background:#f0f2f5;color:#333;padding:10px;} .card{background:#fff;padding:15px;margin-bottom:15px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1);} h2{margin-top:0;} .btn{display:inline-block;padding:10px 20px;background:#007bff;color:#fff;border:none;border-radius:4px;cursor:pointer;text-decoration:none;margin:5px;} .btn-danger{background:#dc3545;} input[type='number']{width:70px;padding:5px;font-size:16px;} select{padding:5px;font-size:16px;margin-left:5px;border-radius:4px;border:1px solid #ccc; background:#fff;}</style>";
    html += "<title>FF Heater Remote</title></head><body>";
    
    html += "<div class='card'><h2>手動リモコン操作</h2>";
    html += "<a href='/trigger?btn=1' class='btn'>ボタン1 (ON)</a>";
    html += "<a href='/trigger?btn=2' class='btn'>ボタン2 (OFF)</a>";
    html += "<a href='/trigger?btn=3' class='btn'>ボタン3 (UP)</a>";
    html += "<a href='/trigger?btn=4' class='btn'>ボタン4 (DOWN)</a>";
    html += "</div>";
    
    html += "<div class='card'><h2>現在のステータス</h2>";
    html += "<p>接続中のSSID: <strong>" + WiFi.SSID() + "</strong></p>"; 
    html += "<p>室内温度: <strong>" + String(currentRoomTemp, 1) + " ℃</strong></p>";
    html += "<p>ダクト温度: <strong>" + String(currentDuctTemp, 1) + " ℃</strong></p>";
    html += "<p>自動制御モード: <strong>" + String(autoModeActive ? "有効（稼働中）" : "無効（停止中）") + "</strong></p>";
    if (autoModeActive) {
        long remaining = (autoModeMinutes * 60) - ((millis() - autoModeStartTime) / 1000);
        if (remaining < 0) remaining = 0;
        html += "<p>残り時間: <strong>" + String(remaining / 60) + "分 " + String(remaining % 60) + "秒</strong></p>";
    }
    String stateStr[] = {"停止中(OFF)", "点火確認中", "運転中(ON)"};
    html += "<p>ヒーター仮想状態: <strong>" + stateStr[currentHeaterState] + "</strong></p></div>";
    
    // 【設定変更フォーム】確実なセレクト連動MMIに完全リプレース
    html += "<div class='card'><h2>自動制御設定</h2><form action='/save' method='POST'>";
    
    // 1. タイマー時間 (1時間単位の選択肢 + 自由入力)
    html += "<p>タイマー時間: <input type='number' id='idx_duration' name='duration' value='" + String(autoModeMinutes) + "'> 分 ";
    html += "<select onchange=\"document.getElementById('idx_duration').value=this.value; this.selectedIndex=0;\">";
    html += "<option value='' disabled selected>選択...</option>";
    html += "<option value='60'>1時間</option><option value='120'>2時間</option><option value='180'>3時間</option><option value='240'>4時間</option><option value='300'>5時間</option><option value='360'>6時間</option>";
    html += "</select></p>";
    
    // 2. ONトリガー温度 (5℃〜30℃まで1℃刻みの選択肢 + 自由入力)
    html += "<p>ONトリガー温度: <input type='number' step='0.1' id='idx_ontemp' name='ontemp' value='" + String(targetOnTemp, 1) + "'> ℃ ";
    html += "<select onchange=\"document.getElementById('idx_ontemp').value=this.value; this.selectedIndex=0;\">";
    html += "<option value='' disabled selected>選択...</option>";
    for (int t = 5; t <= 30; t++) { html += "<option value='" + String(t) + "'>" + String(t) + "℃</option>"; }
    html += "</select> (天井)</p>";
    
    // 3. OFFトリガー温度 (5℃〜30℃まで1℃刻みの選択肢 + 自由入力)
    html += "<p>OFFトリガー温度: <input type='number' step='0.1' id='idx_offtemp' name='offtemp' value='" + String(targetOffTemp, 1) + "'> ℃ ";
    html += "<select onchange=\"document.getElementById('idx_offtemp').value=this.value; this.selectedIndex=0;\">";
    html += "<option value='' disabled selected>選択...</option>";
    for (int t = 5; t <= 30; t++) { html += "<option value='" + String(t) + "'>" + String(t) + "℃</option>"; }
    html += "</select> (天井)</p>";
    
    // 4. 点火判定温度上昇値 (2℃〜15℃まで1℃刻みの選択肢 + 自由入力)
    html += "<p>点火判定温度上昇値: <input type='number' step='0.1' id='idx_ductthresh' name='ductthresh' value='" + String(ductThreshTemp, 1) + "'> ℃ ";
    html += "<select onchange=\"document.getElementById('idx_ductthresh').value=this.value; this.selectedIndex=0;\">";
    html += "<option value='' disabled selected>選択...</option>";
    for (int t = 2; t <= 15; t++) { html += "<option value='" + String(t) + "'>" + String(t) + "℃</option>"; }
    html += "</select> (ダクト)</p>";
    
    html += "<input type='submit' class='btn' value='設定を保存して更新'></form>";
    
    if (!autoModeActive) {
        html += "<a href='/toggleAuto?mode=on' class='btn'>自動制御を開始</a>";
    } else {
        html += "<a href='/toggleAuto?mode=off' class='btn btn-danger'>自動制御を強制停止</a>";
    }
    html += "</div>";
    
    html += "<script>setInterval(function(){ location.reload(); }, 5000);</script>"; 
    html += "</body></html>";
    server.send(200, "text/html", html);
}

void handleSave() {
    if (server.hasArg("duration")) autoModeMinutes = server.arg("duration").toInt();
    if (server.hasArg("ontemp")) targetOnTemp = server.arg("ontemp").toFloat();
    if (server.hasArg("offtemp")) targetOffTemp = server.arg("offtemp").toFloat();
    if (server.hasArg("ductthresh")) ductThreshTemp = server.arg("ductthresh").toFloat();
    
    prefs.putInt("duration", autoModeMinutes);
    prefs.putFloat("ontemp", targetOnTemp);
    prefs.putFloat("offtemp", targetOffTemp);
    prefs.putFloat("ductthresh", ductThreshTemp);
    
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleToggleAuto() {
    if (server.hasArg("mode")) {
        String mode = server.arg("mode");
        if (mode == "on") {
            autoModeActive = true;
            autoModeStartTime = millis();
            currentHeaterState = HEATER_OFF; 
        } else {
            autoModeActive = false;
            if (currentHeaterState != HEATER_OFF) {
                triggerButton(0); 
                currentHeaterState = HEATER_OFF;
            }
        }
    }
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleTrigger() {
    if (server.hasArg("btn")) {
        int btnNum = server.arg("btn").toInt();
        triggerButton(btnNum - 1); 
    }
    server.sendHeader("Location", "/");
    server.send(303);
}

// ==================== 初期設定 ====================
void setup() {
    Serial.begin(115200);
    
    pinMode(PIN_PC_BTN1, OUTPUT);
    pinMode(PIN_PC_BTN2, OUTPUT);
    pinMode(PIN_PC_BTN3, OUTPUT);
    pinMode(PIN_PC_BTN4, OUTPUT);
    digitalWrite(PIN_PC_BTN1, LOW);
    digitalWrite(PIN_PC_BTN2, LOW);
    digitalWrite(PIN_PC_BTN3, LOW);
    digitalWrite(PIN_PC_BTN4, LOW);
    
    dhtRoom.begin();
    dhtDuct.begin();
    
    prefs.begin("heater-config", false);
    autoModeMinutes = prefs.getInt("duration", 60);
    targetOnTemp = prefs.getFloat("ontemp", 10.0);
    targetOffTemp = prefs.getFloat("offtemp", 15.0);
    ductThreshTemp = prefs.getFloat("ductthresh", 5.0);
    
    WiFi.mode(WIFI_STA);
    registerWiFiNetworks();
    
    Serial.println("Scanning and connecting to available WiFi...");
    
    while (wifiMulti.run() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWi-Fi Connected!");
    Serial.print("Connected SSID: ");
    Serial.println(WiFi.SSID());
    Serial.print("Assigned IP Address: ");
    Serial.println(WiFi.localIP()); 
    
    if (MDNS.begin("ffheater")) {
        Serial.println("mDNS responder started. Access via http://ffheater.local");
    } else {
        Serial.println("Error setting up MDNS responder!");
    }
    
    server.on("/", handleRoot);
    server.on("/save", handleSave);
    server.on("/toggleAuto", handleToggleAuto);
    server.on("/trigger", handleTrigger);
    server.begin();
    
    MDNS.addService("http", "tcp", 80);
}

// ==================== メインループ ====================
void loop() {
    server.handleClient(); 
    updateButtonPulses();  
    
    static unsigned long lastSensorRead = 0;
    if (millis() - lastSensorRead >= 2000) {
        lastSensorRead = millis();
        float r = dhtRoom.readTemperature();
        float d = dhtDuct.readTemperature();
        if (!isnan(r)) currentRoomTemp = r;
        if (!isnan(d)) currentDuctTemp = d;
    }
    
    if (autoModeActive) {
        if (millis() - autoModeStartTime >= ((unsigned long)autoModeMinutes * 60 * 1000)) {
            autoModeActive = false; 
            if (currentHeaterState != HEATER_OFF) {
                triggerButton(0); 
                currentHeaterState = HEATER_OFF;
            }
            return;
        }
        
        switch (currentHeaterState) {
            case HEATER_OFF:
                if (currentRoomTemp <= targetOnTemp && currentRoomTemp > 0.0) {
                    triggerButton(0); 
                    ignitionStartTime = millis();
                    ignitionStartDuctTemp = currentDuctTemp; 
                    currentHeaterState = HEATER_IGNITING;
                    Serial.println("点火プロセス開始。5分間の監視に入ります。");
                }
                break;
                
            case HEATER_IGNITING:
                if (currentDuctTemp >= (ignitionStartDuctTemp + ductThreshTemp)) {
                    currentHeaterState = HEATER_ON;
                    Serial.println("点火成功を検知。通常運転モードへ移行。");
                }
                else if (millis() - ignitionStartTime >= IGNITION_TIMEOUT_MS) {
                    Serial.println("5分経過しても温度上昇なし。不発とみなし再試行します。");
                    triggerButton(0); 
                    ignitionStartTime = millis(); 
                    ignitionStartDuctTemp = currentDuctTemp; 
                }
                break;
                
            case HEATER_ON:
                if (currentRoomTemp >= targetOffTemp) {
                    triggerButton(0); 
                    currentHeaterState = HEATER_OFF;
                    Serial.println("目標温度達成。消火します。");
                }
                break;
        }
    }
}