#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ffheater {

// Preserve the existing ESP-NOW version 1 wire format, including alignment.
struct SensorData {
    uint8_t version;
    float roomTemp;
    float roomHum;
    float ductTemp;
    float ductHum;
    float batteryVoltage;
};
static_assert(sizeof(SensorData) == 24, "Sensor packet layout changed");
static_assert(offsetof(SensorData, roomTemp) == 4, "Room temperature offset changed");
static_assert(offsetof(SensorData, ductTemp) == 12, "Duct temperature offset changed");

struct ReceivedSensor {
    SensorData data;
    uint32_t receivedAt;
    uint8_t sourceMac[6];
};

inline bool decodeSensorPacket(const uint8_t* sourceMac, const uint8_t* bytes,
                              int length, uint32_t receivedAt, ReceivedSensor& packet) {
    if (!sourceMac || !bytes || length != static_cast<int>(sizeof(SensorData))) return false;
    SensorData data;
    std::memcpy(&data, bytes, sizeof(data));
    if (data.version != 1) return false;
    // The original receiver did not bind incoming packets to its peer-list MAC.
    // A historical peer entry must not silently exclude the working sensor node.
    packet.data = data;
    packet.receivedAt = receivedAt;
    std::memcpy(packet.sourceMac, sourceMac, sizeof(packet.sourceMac));
    return true;
}

} // namespace ffheater
