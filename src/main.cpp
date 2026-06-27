#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h> 
#include <WebServer.h>
#include <ESPmDNS.h> 
#include <Preferences.h>
#include <DHT.h>
#include "secret.h" 

// ==================== Wi-Fi設定 ====================
WiFiMulti wifiMulti; 

// ==================== ピン・ボタン定義 ====================
enum ButtonType {
    BTN_ON,
    BTN_OFF,
    BTN_UP,
    BTN_DOWN
};

// 30PIN開発ボード準拠のピンアサイン
struct ButtonPulse {
    int pin;
    String label;
    unsigned long turnOffTime;
    bool isPinHigh;
    int pressCount;
    unsigned long nextPressTime;
};

// ここを書き換えるだけでピンやWeb画面の名称変更が可能
ButtonPulse buttons[] = {
    {25, "電源ON",   0, false, 0, 0}, // BTN_ON
    {32, "電源OFF",  0, false, 0, 0}, // BTN_OFF
    {26, "温度上げ", 0, false, 0, 0}, // BTN_UP
    {27, "温度下げ", 0, false, 0, 0}  // BTN_DOWN
};
const int BUTTON_COUNT = sizeof(buttons) / sizeof(ButtonPulse);

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

float currentRoomTemp = 0.0;
float currentDuctTemp = 0.0;

// ==================== 関数定義 ====================

// 指定されたボタンのパルス生成シーケンスを開始する（ノンブロッキング）
void triggerButton(ButtonType btn) {
    if (buttons[btn].pressCount > 0) return; // すでに処理中なら無視
    
    // ON/OFFなら2回、それ以外（上げ下げ）なら1回押す仕様を自動判別
    if (btn == BTN_ON || btn == BTN_OFF) {
        buttons[btn].pressCount = 2;
    } else {
        buttons[btn].pressCount = 1;
    }
    buttons[btn].nextPressTime = millis(); // 即座に1回目を開始
}

// loop内で毎サイクル呼び出され、パルスタイミングと連打間隔（1秒）を制御
void updateButtonPulses() {
    unsigned long currentMillis = millis();
    
    for (int i = 0; i < BUTTON_COUNT; i++) {
        // ピンが現在HIGHで、通電時間を過ぎたらLOWに戻す
        if (buttons[i].isPinHigh && currentMillis >= buttons[i].turnOffTime) {
            digitalWrite(buttons[i].pin, LOW);
            buttons[i].isPinHigh = false;
            
            // 次回押すまでのインターバル（1秒 = 1000ms）をセット
            buttons[i].nextPressTime = currentMillis + 1000;
        }
        
        // まだ押す回数が残っており、インターバル時間を経過していればピンをHIGHにする
        if (!buttons[i].isPinHigh && buttons[i].pressCount > 0 && currentMillis >= buttons[i].nextPressTime) {
            digitalWrite(buttons[i].pin, HIGH);
            buttons[i].isPinHigh = true;
            buttons[i].turnOffTime = currentMillis + 500; // パルス幅 500ms
            buttons[i].pressCount--;                      // 残り回数を減らす
        }
    }
}

// void handleRoot() {
//     String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>";
//     html += "<style>body{font-family:sans-serif;background:#f0f2f5;color:#333;padding:10px;} .card{background:#fff;padding:15px;margin-bottom:15px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1);} h2{margin-top:0;} .btn{display:inline-block;padding:10px 20px;background:#007bff;color:#fff;border:none;border-radius:4px;cursor:pointer;text-decoration:none;margin:5px;} .btn-danger{background:#dc3545;} input[type='number']{width:70px;padding:5px;font-size:16px;} select{padding:5px;font-size:16px;margin-left:5px;border-radius:4px;border:1px solid #ccc; background:#fff;}</style>";
//     html += "<title>FF Heater Remote</title></head><body>";
    
//     html += "<div class='card'><h2>手動リモコン操作</h2>";
//     // 構造体配列から動的にボタンを生成（直しやすい構成）
//     for (int i = 0; i < BUTTON_COUNT; i++) {
//         html += "<a href='/trigger?btn=" + String(i) + "' class='btn'>" + buttons[i].label + "</a>";
//     }
//     html += "</div>";
    
//     html += "<div class='card'><h2>現在のステータス</h2>";
//     html += "<p>接続中のSSID: <strong>" + WiFi.SSID() + "</strong></p>"; 
//     html += "<p>室内温度: <strong>" + String(currentRoomTemp, 1) + " ℃</strong></p>";
//     html += "<p>ダクト温度: <strong>" + String(currentDuctTemp, 1) + " ℃</strong></p>";
//     html += "<p>自動制御モード: <strong>" + String(autoModeActive ? "有効（稼働中）" : "無効（停止中）") + "</strong></p>";
//     if (autoModeActive) {
//         long remaining = (autoModeMinutes * 60) - ((millis() - autoModeStartTime) / 1000);
//         if (remaining < 0) remaining = 0;
//         html += "<p>残り時間: <strong>" + String(remaining / 60) + "分 " + String(remaining % 60) + "秒</strong></p>";
//     }
//     String stateStr[] = {"停止中(OFF)", "点火確認中", "運転中(ON)"};
//     html += "<p>ヒーター仮想状態: <strong>" + stateStr[currentHeaterState] + "</strong></p></div>";
    
//     html += "<div class='card'><h2>自動制御設定</h2><form action='/save' method='POST'>";
    
//     html += "<p>タイマー時間: <input type='number' id='idx_duration' name='duration' value='" + String(autoModeMinutes) + "'> 分 ";
//     html += "<select onchange=\"if(this.value){document.getElementById('idx_duration').value=this.value;}; this.selectedIndex=0;\">";
//     html += "<option value='' disabled selected>選択...</option>";
//     html += "<option value='60'>1時間</option><option value='120'>2時間</option><option value='180'>3時間</option><option value='240'>4時間</option><option value='300'>5時間</option><option value='360'>6時間</option><option value='420'>7時間</option><option value='480'>8時間</option><option value='540'>9時間</option><option value='600'>10時間</option><option value='660'>11時間</option><option value='720'>12時間</option>";
//     html += "</select></p>";
    
//     html += "<p>ONトリガー温度: <input type='number' step='0.1' id='idx_ontemp' name='ontemp' value='" + String(targetOnTemp, 1) + "'> ℃ ";
//     html += "<select onchange=\"if(this.value){document.getElementById('idx_ontemp').value=this.value;}; this.selectedIndex=0;\">";
//     html += "<option value='' disabled selected>選択...</option>";
//     for (int t = 5; t <= 30; t++) { html += "<option value='" + String(t) + "'>" + String(t) + "℃</option>"; }
//     html += "</select> (天井)</p>";
    
//     html += "<p>OFFトリガー温度: <input type='number' step='0.1' id='idx_offtemp' name='offtemp' value='" + String(targetOffTemp, 1) + "'> ℃ ";
//     html += "<select onchange=\"if(this.value){document.getElementById('idx_offtemp').value=this.value;}; this.selectedIndex=0;\">";
//     html += "<option value='' disabled selected>選択...</option>";
//     for (int t = 5; t <= 30; t++) { html += "<option value='" + String(t) + "'>" + String(t) + "℃</option>"; }
//     html += "</select> (天井)</p>";
    
//     html += "<p>点火判定温度上昇値: <input type='number' step='0.1' id='idx_ductthresh' name='ductthresh' value='" + String(ductThreshTemp, 1) + "'> ℃ ";
//     html += "<select onchange=\"if(this.value){document.getElementById('idx_ductthresh').value=this.value;}; this.selectedIndex=0;\">";
//     html += "<option value='' disabled selected>選択...</option>";
//     for (int t = 2; t <= 15; t++) { html += "<option value='" + String(t) + "'>" + String(t) + "℃</option>"; }
//     html += "</select> (ダクト)</p>";
    
//     html += "<input type='submit' class='btn' value='設定を保存して更新'></form>";
    
//     if (!autoModeActive) {
//         html += "<a href='/toggleAuto?mode=on' class='btn'>自動制御を開始</a>";
//     } else {
//         html += "<a href='/toggleAuto?mode=off' class='btn btn-danger'>自動制御を強制停止</a>";
//     }
//     html += "</div>";
    
//     html += "<script>setInterval(function(){ location.reload(); }, 5000);</script>"; 
//     html += "</body></html>";
//     server.send(200, "text/html", html);
// }
void handleRoot() {
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>";
    html += "<style>body{font-family:sans-serif;background:#f0f2f5;color:#333;padding:10px;} .card{background:#fff;padding:15px;margin-bottom:15px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1);} h2{margin-top:0;} .btn{display:inline-block;padding:10px 20px;background:#007bff;color:#fff;border:none;border-radius:4px;cursor:pointer;text-decoration:none;margin:5px;} .btn-danger{background:#dc3545;} input[type='number']{width:70px;padding:5px;font-size:16px;} select{padding:5px;font-size:16px;margin-left:5px;border-radius:4px;border:1px solid #ccc; background:#fff;}</style>";
    html += "<title>FF Heater Remote</title></head><body>";
    
    html += "<div class='card'><h2>手動リモコン操作</h2>";
    for (int i = 0; i < BUTTON_COUNT; i++) {
        html += "<a href='/trigger?btn=" + String(i) + "' class='btn'>" + buttons[i].label + "</a>";
    }
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
    
    html += "<div class='card'><h2>自動制御設定</h2><form action='/save' method='POST'>";
    
    html += "<p>タイマー時間: <input type='number' id='idx_duration' name='duration' value='" + String(autoModeMinutes) + "'> 分 ";
    html += "<select onchange=\"if(this.value){document.getElementById('idx_duration').value=this.value;}; this.selectedIndex=0;\">";
    html += "<option value='' disabled selected>選択...</option>";
    html += "<option value='60'>1時間</option><option value='120'>2時間</option><option value='180'>3時間</option><option value='240'>4時間</option><option value='300'>5時間</option><option value='360'>6時間</option><option value='420'>7時間</option><option value='480'>8時間</option><option value='540'>9時間</option><option value='600'>10時間</option><option value='660'>11時間</option><option value='720'>12時間</option>";
    html += "</select></p>";
    
    html += "<p>ONトリガー温度: <input type='number' step='0.1' id='idx_ontemp' name='ontemp' value='" + String(targetOnTemp, 1) + "'> ℃ ";
    html += "<select onchange=\"if(this.value){document.getElementById('idx_ontemp').value=this.value;}; this.selectedIndex=0;\">";
    html += "<option value='' disabled selected>選択...</option>";
    for (int t = 5; t <= 30; t++) { html += "<option value='" + String(t) + "'>" + String(t) + "℃</option>"; }
    html += "</select> (天井)</p>";
    
    html += "<p>OFFトリガー温度: <input type='number' step='0.1' id='idx_offtemp' name='offtemp' value='" + String(targetOffTemp, 1) + "'> ℃ ";
    html += "<select onchange=\"if(this.value){document.getElementById('idx_offtemp').value=this.value;}; this.selectedIndex=0;\">";
    html += "<option value='' disabled selected>選択...</option>";
    for (int t = 5; t <= 30; t++) { html += "<option value='" + String(t) + "'>" + String(t) + "℃</option>"; }
    html += "</select> (天井)</p>";
    
    html += "<p>点火判定温度上昇値: <input type='number' step='0.1' id='idx_ductthresh' name='ductthresh' value='" + String(ductThreshTemp, 1) + "'> ℃ ";
    html += "<select onchange=\"if(this.value){document.getElementById('idx_ductthresh').value=this.value;}; this.selectedIndex=0;\">";
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
                triggerButton(BTN_OFF); // トグルから明示的なOFFへ変更
                currentHeaterState = HEATER_OFF;
            }
        }
    }
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleTrigger() {
    if (server.hasArg("btn")) {
        int btnIdx = server.arg("btn").toInt();
        if (btnIdx >= 0 && btnIdx < BUTTON_COUNT) {
            triggerButton((ButtonType)btnIdx); 
        }
    }
    server.sendHeader("Location", "/");
    server.send(303);
}

// ==================== 初期設定 ====================
void setup() {
    Serial.begin(115200);
    
    // 構造体配列から動的にピン初期化
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
    
    WiFi.mode(WIFI_STA);
    registerWiFiNetworksMulti(wifiMulti); 
    
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

// ==================== メメインループ ====================
void loop() {
    // ---- Wi-Fi自動再接続処理（ノンブロッキング） ----
    static unsigned long lastWiFiCheck = 0;
    static bool wasConnected = true;

    if (millis() - lastWiFiCheck >= 5000) { 
        lastWiFiCheck = millis();
        
        if (wifiMulti.run() != WL_CONNECTED) {
            if (wasConnected) {
                Serial.println("Warning: Wi-Fi切断を検知。再接続を試みています...");
                wasConnected = false;
            }
        } else {
            if (!wasConnected) {
                Serial.println("Wi-Fi再接続に成功しました。");
                Serial.print("IPアドレス: ");
                Serial.println(WiFi.localIP());
                
                // MDNS.announce(); の代わり
                MDNS.end();
                if (MDNS.begin("ffheater")) {
                    MDNS.addService("http", "tcp", 80);
                }
                wasConnected = true;
            }
        }
    }
    // -----------------------------------------------

    server.handleClient(); 
    updateButtonPulses();  // パルス生成器の更新駆動
    
    static unsigned long lastSensorRead = 0;
    if (millis() - lastSensorRead >= 2000) {
        lastSensorRead = millis();
        float r = dhtRoom.readTemperature();
        float d = dhtDuct.readTemperature();
        if (!isnan(r)) currentRoomTemp = r;
        if (!isnan(d)) currentDuctTemp = d;
    }
    
    if (autoModeActive) {
        // タイマー満了時の処理
        if (millis() - autoModeStartTime >= ((unsigned long)autoModeMinutes * 60 * 1000)) {
            autoModeActive = false; 
            if (currentHeaterState != HEATER_OFF) {
                triggerButton(BTN_OFF); // 明示的なOFF
                currentHeaterState = HEATER_OFF;
            }
            return;
        }
        
        switch (currentHeaterState) {
            case HEATER_OFF:
                if (currentRoomTemp <= targetOnTemp && currentRoomTemp > 0.0) {
                    triggerButton(BTN_ON); // 明示的なON
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
                    Serial.println("5分経過しても温度上昇なし。不発とみなし再点火試行。");
                    triggerButton(BTN_ON); // 再点火試行
                    ignitionStartTime = millis(); 
                    ignitionStartDuctTemp = currentDuctTemp; 
                }
                break;
                
            case HEATER_ON:
                if (currentRoomTemp >= targetOffTemp) {
                    triggerButton(BTN_OFF); // 明示的なOFF
                    currentHeaterState = HEATER_OFF;
                    Serial.println("目標温度達成。消火します。");
                }
                else {
                    // -----------------------------------------------------------------
                    // 【将来の拡張エリア】
                    // ここに「目標温度に近づいたら、BTN_UP / BTN_DOWN を叩く」自動火力調整
                    // ロジックを今後スムーズに追加可能です。
                    // 例: if(currentRoomTemp > targetOffTemp - 1.0) { triggerButton(BTN_DOWN); }
                    // -----------------------------------------------------------------
                }
                break;
        }
    }
}