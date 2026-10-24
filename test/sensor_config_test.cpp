#include "SensorConfig.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>

int main() {
    using namespace ffheater;
    static_assert(sizeof(ConfigFrame) == 16, "Unexpected sensor config frame");
    ConfigFrame f{};
    f.op = CONFIG_SET;
    f.requestId = 12345;
    f.roomTenths = -30;
    f.ductTenths = 30;
    for (uint16_t sleep : {uint16_t(15), uint16_t(30), uint16_t(34), uint16_t(60)}) {
        f.sleepSeconds = sleep;
        assert(validConfig(f));
        ConfigFrame received{};
        assert(decodeConfig(reinterpret_cast<const uint8_t*>(&f), sizeof(f), received));
        assert(received.requestId == f.requestId);
        assert(received.roomTenths == -30 && received.ductTenths == 30);
        assert(received.sleepSeconds == sleep);
    }
    f.sleepSeconds = 45;
    assert(!validConfig(f));
    f.sleepSeconds = 60;
    f.roomTenths = -31;
    assert(!validConfig(f));
    f.roomTenths = 31;
    assert(!validConfig(f));
    f.roomTenths = 0;
    f.ductTenths = -31;
    assert(!validConfig(f));
    f.ductTenths = 0;
    ConfigFrame received{};
    auto* bytes = reinterpret_cast<uint8_t*>(&f);
    assert(!decodeConfig(bytes, sizeof(f)-1, received));
    assert(!decodeConfig(nullptr, sizeof(f), received));
    const uint16_t magic = f.magic;
    f.magic = 0;
    assert(!decodeConfig(bytes, sizeof(f), received));
    f.magic = magic;
    f.version = 2;
    assert(!decodeConfig(bytes, sizeof(f), received));
    std::cout << "PASS: ESP-NOW config wire format, valid ranges and rejection checks\n";
}
