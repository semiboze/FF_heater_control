#include "SensorPacket.h"
#include "HeaterControl.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>

int main() {
    const uint8_t historicalPeer[] = {0xEC, 0x61, 0x60, 0x93, 0xF8, 0x14};
    const uint8_t actualSensor[] = {0x24, 0x6F, 0x28, 0x01, 0x02, 0x03};
    ffheater::SensorData data = {1, 8.0f, 40.0f, 20.0f, 30.0f, 3.7f};
    const auto* bytes = reinterpret_cast<const uint8_t*>(&data);
    ffheater::ReceivedSensor packet{};
    assert(ffheater::decodeSensorPacket(historicalPeer, bytes, sizeof(data), 0, packet));
    assert(ffheater::decodeSensorPacket(actualSensor, bytes, sizeof(data), 60000, packet));
    assert(std::memcmp(packet.sourceMac, actualSensor, sizeof(actualSensor)) == 0);
    assert(packet.receivedAt == 60000);
    assert(packet.data.roomTemp == 8.0f && packet.data.ductTemp == 20.0f);

    // Test the actual packet decoder followed by the production recovery gate.
    ffheater::Controller controller;
    controller.receive(packet.data.roomTemp, packet.data.ductTemp, packet.receivedAt);
    assert(!controller.roomValid(60000));
    assert(ffheater::decodeSensorPacket(actualSensor, bytes, sizeof(data), 120000, packet));
    controller.receive(packet.data.roomTemp, packet.data.ductTemp, packet.receivedAt);
    assert(controller.roomValid(120000) && controller.ductValid(120000));
    assert(controller.room.temperature == 8.0f && controller.duct.temperature == 20.0f);

    // A sensor failure is a valid packet, not a communication failure.
    data.roomTemp = NAN;
    assert(ffheater::decodeSensorPacket(actualSensor, bytes, sizeof(data), 180000, packet));
    controller.receive(packet.data.roomTemp, packet.data.ductTemp, packet.receivedAt);
    assert(controller.communicationValid(180000));
    assert(!controller.roomValid(180000) && controller.ductValid(180000));
    data.ductTemp = NAN;
    assert(ffheater::decodeSensorPacket(actualSensor, bytes, sizeof(data), 240000, packet));
    assert(std::isnan(packet.data.roomTemp) && std::isnan(packet.data.ductTemp));

    assert(!ffheater::decodeSensorPacket(actualSensor, bytes, sizeof(data) - 1, 0, packet));
    assert(!ffheater::decodeSensorPacket(actualSensor, bytes, sizeof(data) + 1, 0, packet));
    assert(!ffheater::decodeSensorPacket(actualSensor, bytes, -1, 0, packet));
    assert(!ffheater::decodeSensorPacket(nullptr, bytes, sizeof(data), 0, packet));
    assert(!ffheater::decodeSensorPacket(actualSensor, nullptr, sizeof(data), 0, packet));
    data.version = 2;
    assert(!ffheater::decodeSensorPacket(actualSensor, bytes, sizeof(data), 0, packet));
    std::cout << "PASS: receiver compatibility, arbitrary sender MAC, wire format, recovery and NaN\n";
}
