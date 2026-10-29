#pragma once
// Native ESP32 only. No secret is embedded in the public firmware.
#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>
#include <mbedtls/md.h>
#include "EspNowWire.h"
namespace ffnow {
constexpr size_t KEY_BYTES = 16;
inline bool loadKey(Preferences& prefs, uint8_t* key) {
    return prefs.getBytesLength("root") == KEY_BYTES &&
        prefs.getBytes("root", key, KEY_BYTES) == KEY_BYTES;
}
inline bool saveKey(Preferences& prefs, const uint8_t* key) {
    if (prefs.putBytes("root", key, KEY_BYTES) != KEY_BYTES) return false;
    uint8_t readback[KEY_BYTES] = {};
    return loadKey(prefs, readback) && std::memcmp(readback, key, KEY_BYTES) == 0;
}
inline void randomBytes(uint8_t* bytes, size_t len) { esp_fill_random(bytes, len); }
inline uint64_t nonce() {
    uint64_t value = 0;
    do { randomBytes(reinterpret_cast<uint8_t*>(&value), sizeof(value)); } while (!value);
    return value;
}
inline void printKeyOnce(const uint8_t* bytes) {
    // static const char* HEX = "0123456789ABCDEF";
    static const char* hexDigits = "0123456789ABCDEF";
    char output[KEY_BYTES*2+1] = {};
    for (size_t i = 0; i < KEY_BYTES; ++i) {
        // output[2*i] = HEX[bytes[i] >> 4]; output[2*i+1] = HEX[bytes[i] & 15];
        output[2*i] = hexDigits[bytes[i] >> 4]; output[2*i+1] = hexDigits[bytes[i] & 15];
    }
    Serial.print("ESP-NOW ONE-TIME KEY: "); Serial.println(output);
    std::memset(output, 0, sizeof(output));
}
inline bool deriveKeys(const uint8_t* root, uint8_t* pmk, uint8_t* lmk) {
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) return false;
    uint8_t digest[32] = {};
    const char* labels[2] = {"FFH-ESPNOW-PMK-v1", "FFH-ESPNOW-LMK-v1"};
    uint8_t* out[2] = {pmk, lmk};
    for (int i = 0; i < 2; ++i) {
        if (mbedtls_md_hmac(md, root, KEY_BYTES,
            reinterpret_cast<const unsigned char*>(labels[i]), std::strlen(labels[i]), digest) != 0)
            return false;
        std::memcpy(out[i], digest, KEY_BYTES);
    }
    std::memset(digest, 0, sizeof(digest));
    return true;
}
} // namespace ffnow
