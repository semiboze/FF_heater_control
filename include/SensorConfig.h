#pragma once
#include <cstdint>
#include <cstring>

namespace ffheater {
// Separate protocol: do not change the legacy 24-byte SensorData payload.
constexpr uint16_t CONFIG_MAGIC = 0x5343;
constexpr uint8_t CONFIG_VERSION = 1;
enum ConfigOp : uint8_t { CONFIG_GET = 1, CONFIG_SET = 2, CONFIG_REPORT = 3 };
enum ConfigStatus : uint8_t { CONFIG_OK = 0, CONFIG_BAD_VALUE = 1, CONFIG_SAVE_ERROR = 2 };
#pragma pack(push, 1)
struct ConfigFrame {
    uint16_t magic = CONFIG_MAGIC;
    uint8_t version = CONFIG_VERSION;
    uint8_t op = CONFIG_GET;
    uint32_t requestId = 0;
    int16_t roomTenths = 0;
    int16_t ductTenths = 0;
    uint16_t sleepSeconds = 60;
    uint8_t status = CONFIG_OK;
    uint8_t reserved = 0;
};
#pragma pack(pop)
static_assert(sizeof(ConfigFrame) == 16, "ConfigFrame wire format must be 16 bytes");

inline bool supportedSleep(uint16_t seconds) {
    return seconds == 15 || seconds == 30 || seconds == 34 || seconds == 60;
}
inline bool validConfig(const ConfigFrame& frame) {
    return frame.roomTenths >= -30 && frame.roomTenths <= 30 &&
           frame.ductTenths >= -30 && frame.ductTenths <= 30 &&
           supportedSleep(frame.sleepSeconds);
}
inline bool decodeConfig(const uint8_t* bytes, int length, ConfigFrame& frame) {
    if (!bytes || length != static_cast<int>(sizeof(frame))) return false;
    std::memcpy(&frame, bytes, sizeof(frame));
    return frame.magic == CONFIG_MAGIC && frame.version == CONFIG_VERSION &&
           (frame.op == CONFIG_GET || frame.op == CONFIG_SET || frame.op == CONFIG_REPORT);
}
} // namespace ffheater
