#pragma once
// ESP-NOW protocol v2: encrypted unicast + one-use parent challenge.
// Plain v1 SensorData/ConfigFrame packets MUST NOT be accepted.
#include <cstdint>
#include <cstddef>
#include <cstring>
#include "SensorConfig.h"

namespace ffnow {
constexpr uint32_t MAGIC = 0x31484646UL; // FFH1 on the wire, little-endian
constexpr uint8_t VERSION = 2;
enum FrameKind : uint8_t { HELLO = 1, CHALLENGE = 2, SAMPLE = 3, CONFIG = 4, REPORT = 5 };
#pragma pack(push, 1)
struct HelloFrame {
    uint32_t magic = MAGIC;
    uint8_t version = VERSION;
    uint8_t kind = HELLO;
    uint64_t sensorNonce = 0;
};
struct ChallengeFrame {
    uint32_t magic = MAGIC;
    uint8_t version = VERSION;
    uint8_t kind = CHALLENGE;
    uint64_t sensorNonce = 0;
    uint64_t parentNonce = 0;
};
struct SampleFrame {
    uint32_t magic = MAGIC;
    uint8_t version = VERSION;
    uint8_t kind = SAMPLE;
    uint64_t sensorNonce = 0;
    uint64_t parentNonce = 0;
    uint8_t sensorData[24] = {};
};
struct ConfigSecureFrame {
    uint32_t magic = MAGIC;
    uint8_t version = VERSION;
    uint8_t kind = CONFIG;
    uint64_t sensorNonce = 0;
    uint64_t parentNonce = 0;
    ffheater::ConfigFrame config{};
};
#pragma pack(pop)
static_assert(sizeof(HelloFrame) == 14, "HelloFrame size");
static_assert(sizeof(ChallengeFrame) == 22, "ChallengeFrame size");
static_assert(sizeof(SampleFrame) == 46, "SampleFrame size");
static_assert(sizeof(ConfigSecureFrame) == 38, "ConfigSecureFrame size");

template <typename Frame>
inline bool parse(const uint8_t* bytes, int length, Frame& frame, FrameKind kind) {
    if (!bytes || length != int(sizeof(Frame))) return false;
    std::memcpy(&frame, bytes, sizeof(frame));
    return frame.magic == MAGIC && frame.version == VERSION && frame.kind == kind;
}
inline bool matchesChallenge(uint64_t incomingSensor, uint64_t incomingParent,
                             uint64_t expectedSensor, uint64_t expectedParent) {
    return incomingSensor && incomingParent && incomingSensor == expectedSensor &&
           incomingParent == expectedParent;
}
inline bool validUnicastMac(const uint8_t* mac) {
    if (!mac || (mac[0] & 1)) return false;
    const uint8_t empty[6] = {};
    return std::memcmp(mac, empty, 6) != 0;
}
inline bool allowedMac(const uint8_t* actual, const uint8_t* expected) {
    return actual && expected && std::memcmp(actual, expected, 6) == 0;
}
inline int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
inline bool parseHex(const char* input, size_t count, uint8_t* out) {
    if (!input || !out || std::strlen(input) != count * 2) return false;
    for (size_t i = 0; i < count; ++i) {
        int h = hexNibble(input[i*2]), l = hexNibble(input[i*2+1]);
        if (h < 0 || l < 0) return false;
        out[i] = uint8_t((h << 4) | l);
    }
    return true;
}
inline bool parseMac(const char* input, uint8_t* out) {
    if (!input || !out || std::strlen(input) != 17) return false;
    for (int i = 0; i < 6; ++i) {
        if (i && input[i*3-1] != ':') return false;
        int h = hexNibble(input[i*3]), l = hexNibble(input[i*3+1]);
        if (h < 0 || l < 0) return false;
        out[i] = uint8_t((h << 4) | l);
    }
    // Exclude multicast, broadcast and all-zero source addresses.
    return validUnicastMac(out);
}
} // namespace ffnow
