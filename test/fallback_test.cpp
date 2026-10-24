#include "HeaterControl.h"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using ffheater::Command;
using ffheater::ControlMode;
using ffheater::Controller;
using ffheater::HeaterState;
using ffheater::SensorHealth;
using ffheater::StopReason;

namespace {

constexpr uint32_t MINUTE = 60000UL;
const float MISSING = std::numeric_limits<float>::quiet_NaN();

#define CHECK(condition) do { \
    if (!(condition)) throw std::runtime_error( \
        std::string(__func__) + ":" + std::to_string(__LINE__) + " " #condition); \
} while (false)

struct Emission {
    uint32_t time;
    Command command;
};

// Advance the same state machine used by the firmware with a fresh packet each
// minute. Silence tests explicitly omit receive(), rather than accidentally
// testing the communication timeout while exercising a 30-minute deadline.
struct Rig {
    Controller controller;
    uint32_t now;
    std::vector<Emission> emissions;

    Rig(float room = 8.0f, float duct = 8.0f, uint32_t initial = 0)
        : now(initial) {
        controller.settings.durationMinutes = 180;
        controller.receive(room, duct, now);
        now += MINUTE;
        controller.receive(room, duct, now);
    }

    Command tick() {
        const Command result = controller.tick(now);
        if (result != Command::NONE) emissions.push_back({now, result});
        return result;
    }

    Command start() {
        controller.startAuto(now);
        return tick();
    }

    Command step(uint32_t elapsed, float room, float duct) {
        now += elapsed;
        controller.receive(room, duct, now);
        return tick();
    }

    void advance(uint32_t elapsed, float room, float duct) {
        while (elapsed > 0) {
            const uint32_t part = elapsed > MINUTE ? MINUTE : elapsed;
            step(part, room, duct);
            elapsed -= part;
        }
    }

    size_t count(Command command) const {
        size_t result = 0;
        for (const auto& emission : emissions)
            if (emission.command == command) ++result;
        return result;
    }
};

void recovery_requires_two_packets() {
    SensorHealth sensor;
    sensor.receive(8, 0);
    CHECK(!sensor.healthy);
    sensor.receive(9, MINUTE);
    CHECK(sensor.healthy);
    CHECK(sensor.temperature == 9);
    sensor.receive(MISSING, 2 * MINUTE);
    CHECK(!sensor.healthy);
    CHECK(!std::isfinite(sensor.temperature));
    sensor.receive(10, 3 * MINUTE);
    CHECK(!sensor.healthy);
    sensor.receive(11, 4 * MINUTE);
    CHECK(sensor.healthy);
    sensor.receive(std::numeric_limits<float>::infinity(), 5 * MINUTE);
    CHECK(!sensor.healthy);
}

void repeated_ticks_do_not_count_as_sensor_recovery() {
    Controller controller;
    controller.receive(8, 8, 0);
    for (uint32_t now = 1; now < 1000; ++now) {
        CHECK(controller.tick(now) == Command::NONE);
        CHECK(!controller.roomValid(now));
        CHECK(!controller.ductValid(now));
    }
    controller.receive(8, 8, MINUTE);
    CHECK(controller.roomValid(MINUTE));
    CHECK(controller.ductValid(MINUTE));
}

void no_packet_cannot_start_auto() {
    Controller controller;
    controller.startAuto(0);
    CHECK(controller.tick(0) == Command::OFF);
    CHECK(!controller.active());
    CHECK(controller.stopReason() == StopReason::COMM_TIMEOUT);
}

void cooled_duct_substitutes_for_missing_room() {
    Rig rig(MISSING, 10);
    CHECK(rig.start() == Command::ON);
    CHECK(rig.controller.mode(rig.now) == ControlMode::ROOM_FALLBACK);
    CHECK(rig.controller.fallbackRemainingSeconds(rig.now) == 1800);
    CHECK(rig.controller.ignitionAttempts() == 1);
    Rig tooWarm(MISSING, 10.1f);
    CHECK(tooWarm.start() == Command::NONE);
    CHECK(tooWarm.controller.state() == HeaterState::OFF);
}

void room_fallback_stops_at_thirty_minutes_and_repeats_after_ten() {
    Rig rig(MISSING, 8);
    const uint32_t firstOn = rig.now;
    CHECK(rig.start() == Command::ON);
    rig.advance(30 * MINUTE - 1, MISSING, 20);
    CHECK(rig.controller.state() == HeaterState::ON);
    CHECK(rig.count(Command::OFF) == 0);
    CHECK(rig.controller.fallbackRemainingSeconds(rig.now) == 1);
    CHECK(rig.step(1, MISSING, 8) == Command::OFF);
    CHECK(rig.controller.stopReason() == StopReason::ROOM_LIMIT);
    CHECK(rig.now - firstOn == 30 * MINUTE);
    CHECK(rig.controller.active());
    CHECK(rig.controller.cooldownRemainingSeconds(rig.now) == 600);
    rig.advance(10 * MINUTE - 1, MISSING, 8);
    CHECK(rig.controller.state() == HeaterState::OFF);
    CHECK(rig.count(Command::ON) == 1);
    CHECK(rig.step(1, MISSING, 8) == Command::ON);
    CHECK(rig.controller.stopReason() == StopReason::NONE);
    CHECK(rig.controller.fallbackRemainingSeconds(rig.now) == 1800);
    CHECK(rig.controller.ignitionAttempts() == 1);
    rig.advance(30 * MINUTE, MISSING, 20);
    CHECK(rig.controller.state() == HeaterState::OFF);
    CHECK(rig.controller.active());
    CHECK(rig.count(Command::OFF) == 2);
}

void room_failure_while_running_starts_one_deadline() {
    Rig rig;
    CHECK(rig.start() == Command::ON);
    CHECK(rig.step(MINUTE, 8, 20) == Command::NONE);
    CHECK(rig.controller.state() == HeaterState::ON);
    CHECK(rig.step(MINUTE, MISSING, 20) == Command::NONE);
    CHECK(rig.controller.fallbackRemainingSeconds(rig.now) == 1800);
    rig.advance(10 * MINUTE, MISSING, 20);
    CHECK(rig.controller.fallbackRemainingSeconds(rig.now) == 1200);
    rig.advance(20 * MINUTE - 1, MISSING, 20);
    CHECK(rig.controller.state() == HeaterState::ON);
    CHECK(rig.step(1, MISSING, 20) == Command::OFF);
}

void two_normal_room_packets_cancel_deadline_and_new_failure_restarts_it() {
    Rig rig(MISSING, 8);
    CHECK(rig.start() == Command::ON);
    rig.advance(10 * MINUTE, MISSING, 20);
    CHECK(rig.step(MINUTE, 8, 20) == Command::NONE);
    CHECK(!rig.controller.roomValid(rig.now));
    CHECK(rig.controller.fallbackRemainingSeconds(rig.now) == 1140);
    CHECK(rig.step(MINUTE, 8, 20) == Command::NONE);
    CHECK(rig.controller.roomValid(rig.now));
    CHECK(rig.controller.fallbackRemainingSeconds(rig.now) == -1);
    rig.advance(25 * MINUTE, 8, 20);
    CHECK(rig.count(Command::OFF) == 0);
    CHECK(rig.step(MINUTE, MISSING, 20) == Command::NONE);
    CHECK(rig.controller.fallbackRemainingSeconds(rig.now) == 1800);
}

void room_failure_while_off_has_no_deadline_until_ignition() {
    Rig rig(20, 8);
    CHECK(rig.start() == Command::NONE);
    CHECK(rig.controller.fallbackRemainingSeconds(rig.now) == -1);
    CHECK(rig.step(MINUTE, MISSING, 8) == Command::ON);
    CHECK(rig.controller.fallbackRemainingSeconds(rig.now) == 1800);
}

void missing_duct_sends_exactly_three_times_then_room_can_stop() {
    Rig rig(8, MISSING);
    const uint32_t initialOn = rig.now;
    CHECK(rig.start() == Command::ON);
    rig.advance(5 * MINUTE - 1, 8, MISSING);
    CHECK(rig.count(Command::ON) == 1);
    CHECK(rig.step(1, 8, MISSING) == Command::ON);
    CHECK(rig.controller.ignitionAttempts() == 2);
    rig.advance(5 * MINUTE, 8, MISSING);
    CHECK(rig.count(Command::ON) == 3);
    CHECK(rig.emissions[1].time - initialOn == 5 * MINUTE);
    CHECK(rig.emissions[2].time - initialOn == 10 * MINUTE);
    CHECK(rig.controller.state() == HeaterState::UNCONFIRMED);
    rig.advance(20 * MINUTE, 8, MISSING);
    CHECK(rig.count(Command::ON) == 3);
    CHECK(rig.count(Command::OFF) == 0);
    CHECK(rig.controller.active());
    CHECK(rig.step(MINUTE, 15, MISSING) == Command::OFF);
    CHECK(rig.controller.active());
}

void room_off_precedes_a_due_ignition_retry() {
    Rig rig(8, MISSING);
    CHECK(rig.start() == Command::ON);
    rig.advance(5 * MINUTE - 1, 8, MISSING);
    CHECK(rig.step(1, 15, MISSING) == Command::OFF);
    CHECK(rig.count(Command::ON) == 1);
    CHECK(rig.controller.cooldownActive(rig.now));
}

void healthy_duct_without_heat_stops_five_minutes_after_third_on() {
    Rig rig;
    CHECK(rig.start() == Command::ON);
    rig.advance(15 * MINUTE - 1, 8, 8);
    CHECK(rig.count(Command::ON) == 3);
    CHECK(rig.count(Command::OFF) == 0);
    CHECK(rig.step(1, 8, 8) == Command::OFF);
    CHECK(!rig.controller.active());
    CHECK(rig.controller.stopReason() == StopReason::IGNITION_FAILED);
    rig.advance(20 * MINUTE, 8, 8);
    CHECK(rig.count(Command::ON) == 3);
}

void duct_rise_confirms_ignition_and_prevents_retries() {
    Rig rig;
    CHECK(rig.start() == Command::ON);
    CHECK(rig.step(1, 8, 13) == Command::NONE);
    CHECK(rig.controller.state() == HeaterState::ON);
    rig.advance(20 * MINUTE, 8, 13);
    CHECK(rig.count(Command::ON) == 1);
    CHECK(rig.count(Command::OFF) == 0);
}

void recovered_duct_gets_fresh_baseline_without_resetting_attempts() {
    Rig rig(8, MISSING);
    CHECK(rig.start() == Command::ON);
    rig.advance(10 * MINUTE, 8, MISSING);
    CHECK(rig.controller.ignitionAttempts() == 3);
    CHECK(rig.step(MINUTE, 8, 30) == Command::NONE);
    CHECK(!rig.controller.ductValid(rig.now));
    CHECK(rig.step(MINUTE, 8, 30) == Command::NONE);
    CHECK(rig.controller.ductValid(rig.now));
    CHECK(rig.controller.state() != HeaterState::ON);
    CHECK(rig.controller.ignitionAttempts() == 3);
    rig.advance(5 * MINUTE - 1, 8, 30);
    CHECK(rig.controller.active());
    CHECK(rig.count(Command::ON) == 3);
    CHECK(rig.step(1, 8, 30) == Command::OFF);
    CHECK(rig.controller.stopReason() == StopReason::IGNITION_FAILED);
}

void recovered_duct_rise_can_confirm_without_fourth_on() {
    Rig rig(8, MISSING);
    CHECK(rig.start() == Command::ON);
    rig.advance(16 * MINUTE, 8, MISSING);
    CHECK(rig.step(MINUTE, 8, 30) == Command::NONE);
    CHECK(rig.step(MINUTE, 8, 30) == Command::NONE);
    CHECK(rig.step(MINUTE, 8, 35) == Command::NONE);
    CHECK(rig.controller.state() == HeaterState::ON);
    CHECK(rig.controller.active());
    CHECK(rig.count(Command::ON) == 3);
}

void both_sensor_failures_latch_auto_off_until_manual_restart() {
    Rig rig;
    CHECK(rig.start() == Command::ON);
    CHECK(rig.step(1, MISSING, MISSING) == Command::OFF);
    CHECK(!rig.controller.active());
    CHECK(rig.controller.stopReason() == StopReason::BOTH_SENSORS);
    rig.advance(20 * MINUTE, 8, 8);
    CHECK(rig.controller.roomValid(rig.now));
    CHECK(rig.controller.ductValid(rig.now));
    CHECK(rig.count(Command::ON) == 1);
    CHECK(!rig.controller.active());
    CHECK(rig.start() == Command::ON);
    CHECK(rig.controller.active());
}

void both_sensor_failures_disable_auto_even_while_already_off() {
    Rig rig(20, 20);
    CHECK(rig.start() == Command::NONE);
    CHECK(rig.step(1, MISSING, MISSING) == Command::OFF);
    CHECK(!rig.controller.active());
    CHECK(rig.controller.stopReason() == StopReason::BOTH_SENSORS);
}

void communication_timeout_occurs_at_configured_boundary() {
    Rig rig;
    CHECK(rig.start() == Command::ON);
    rig.now += ffheater::COMM_TIMEOUT_MS - 1;
    CHECK(rig.tick() == Command::NONE);
    ++rig.now;
    CHECK(rig.tick() == Command::OFF);
    CHECK(!rig.controller.active());
    CHECK(rig.controller.stopReason() == StopReason::COMM_TIMEOUT);
    rig.advance(20 * MINUTE, 8, 8);
    CHECK(rig.count(Command::ON) == 1);
}

void queued_recovery_packets_do_not_hide_an_expired_communication_gap() {
    Rig rig;
    CHECK(rig.start() == Command::ON);
    rig.now += 3 * MINUTE;
    rig.controller.receive(8, 8, rig.now);
    rig.now += MINUTE;
    rig.controller.receive(8, 8, rig.now);
    CHECK(rig.controller.roomValid(rig.now));
    CHECK(rig.tick() == Command::OFF);
    CHECK(!rig.controller.active());
    CHECK(rig.controller.stopReason() == StopReason::COMM_TIMEOUT);
}

void queued_recovery_cannot_hide_both_sensor_failure() {
    Rig rig;
    CHECK(rig.start() == Command::ON);
    rig.controller.receive(MISSING, MISSING, rig.now + MINUTE);
    rig.controller.receive(8, 8, rig.now + 2 * MINUTE);
    rig.controller.receive(8, 8, rig.now + 3 * MINUTE);
    rig.now += 3 * MINUTE;
    CHECK(rig.controller.roomValid(rig.now));
    CHECK(rig.tick() == Command::OFF);
    CHECK(!rig.controller.active());
    CHECK(rig.controller.stopReason() == StopReason::BOTH_SENSORS);
    rig.advance(20 * MINUTE, 8, 8);
    CHECK(!rig.controller.active());
    CHECK(rig.count(Command::ON) == 1);
}

void explicit_manual_stop_acknowledges_a_pending_fault() {
    Rig rig;
    CHECK(rig.start() == Command::ON);
    rig.now += 3 * MINUTE;
    rig.controller.receive(8, 8, rig.now);
    CHECK(rig.controller.stopByUser(rig.now) == Command::OFF);
    rig.advance(20 * MINUTE, 8, 8);
    CHECK(rig.start() == Command::ON);
    CHECK(rig.controller.active());
}

void timer_stop_precedes_an_ignition_retry() {
    Rig rig(8, MISSING);
    rig.controller.settings.durationMinutes = 5;
    CHECK(rig.start() == Command::ON);
    rig.advance(5 * MINUTE - 1, 8, MISSING);
    CHECK(rig.step(1, 8, MISSING) == Command::OFF);
    CHECK(!rig.controller.active());
    CHECK(rig.controller.stopReason() == StopReason::TIMER);
    CHECK(rig.count(Command::ON) == 1);
}

void manual_stop_cancels_ignition_and_requires_restart() {
    Rig rig;
    CHECK(rig.start() == Command::ON);
    CHECK(rig.controller.stopByUser(rig.now) == Command::OFF);
    CHECK(!rig.controller.active());
    CHECK(rig.controller.state() == HeaterState::OFF);
    CHECK(rig.controller.stopReason() == StopReason::USER);
    rig.advance(20 * MINUTE, 8, 8);
    CHECK(rig.count(Command::ON) == 1);
}

void restored_cooldown_blocks_ignition_until_exactly_ten_minutes() {
    Rig rig;
    rig.controller.restoreCooldown(rig.now);
    CHECK(rig.start() == Command::NONE);
    rig.advance(10 * MINUTE - 1, 8, 8);
    CHECK(rig.count(Command::ON) == 0);
    CHECK(rig.step(1, 8, 8) == Command::ON);
}

void room_fallback_and_cooldown_survive_millis_wrap() {
    Rig rig(MISSING, 8, std::numeric_limits<uint32_t>::max() - 2 * MINUTE);
    CHECK(rig.start() == Command::ON);
    rig.advance(30 * MINUTE - 1, MISSING, 20);
    CHECK(rig.controller.state() == HeaterState::ON);
    CHECK(rig.step(1, MISSING, 8) == Command::OFF);
    rig.advance(10 * MINUTE - 1, MISSING, 8);
    CHECK(rig.count(Command::ON) == 1);
    CHECK(rig.step(1, MISSING, 8) == Command::ON);
}

void ignition_and_communication_deadlines_survive_millis_wrap() {
    Rig ignition(8, MISSING, std::numeric_limits<uint32_t>::max() - 2 * MINUTE);
    CHECK(ignition.start() == Command::ON);
    ignition.advance(10 * MINUTE, 8, MISSING);
    CHECK(ignition.count(Command::ON) == 3);
    CHECK(ignition.controller.state() == HeaterState::UNCONFIRMED);

    Rig communication(8, 8, std::numeric_limits<uint32_t>::max() - 2 * MINUTE);
    CHECK(communication.start() == Command::ON);
    communication.now += 3 * MINUTE;
    CHECK(communication.tick() == Command::OFF);
    CHECK(communication.controller.stopReason() == StopReason::COMM_TIMEOUT);
}

} // namespace

int main() {
    const struct { const char* name; void (*run)(); } tests[] = {
        {"sensor recovery requires two fresh packets", recovery_requires_two_packets},
        {"loop iterations cannot count as recovery", repeated_ticks_do_not_count_as_sensor_recovery},
        {"missing initial packet blocks auto", no_packet_cannot_start_auto},
        {"cooled duct can substitute for room", cooled_duct_substitutes_for_missing_room},
        {"30-minute fallback and 10-minute repeat", room_fallback_stops_at_thirty_minutes_and_repeats_after_ten},
        {"room failure during a confirmed run", room_failure_while_running_starts_one_deadline},
        {"stable room recovery cancels fallback", two_normal_room_packets_cancel_deadline_and_new_failure_restarts_it},
        {"fallback deadline starts at ignition when off", room_failure_while_off_has_no_deadline_until_ignition},
        {"missing duct permits only three ON signals", missing_duct_sends_exactly_three_times_then_room_can_stop},
        {"room OFF has priority over retry", room_off_precedes_a_due_ignition_retry},
        {"healthy cold duct stops after third plus five minutes", healthy_duct_without_heat_stops_five_minutes_after_third_on},
        {"duct rise confirms ignition", duct_rise_confirms_ignition_and_prevents_retries},
        {"duct recovery establishes a fresh baseline", recovered_duct_gets_fresh_baseline_without_resetting_attempts},
        {"recovered duct can confirm without extra ON", recovered_duct_rise_can_confirm_without_fourth_on},
        {"both failures require manual restart", both_sensor_failures_latch_auto_off_until_manual_restart},
        {"both failures disable auto even if already off", both_sensor_failures_disable_auto_even_while_already_off},
        {"configured communication timeout boundary", communication_timeout_occurs_at_configured_boundary},
        {"queued recovery cannot conceal communication gap", queued_recovery_packets_do_not_hide_an_expired_communication_gap},
        {"queued recovery cannot conceal both sensor failure", queued_recovery_cannot_hide_both_sensor_failure},
        {"manual stop acknowledges a pending fault", explicit_manual_stop_acknowledges_a_pending_fault},
        {"timer takes priority over ignition retry", timer_stop_precedes_an_ignition_retry},
        {"manual stop cancels ignition", manual_stop_cancels_ignition_and_requires_restart},
        {"restored cooldown lasts ten minutes", restored_cooldown_blocks_ignition_until_exactly_ten_minutes},
        {"fallback deadlines survive millis wrap", room_fallback_and_cooldown_survive_millis_wrap},
        {"ignition and communication survive millis wrap", ignition_and_communication_deadlines_survive_millis_wrap},
    };
    size_t failed = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "PASS: " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "FAIL: " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (sizeof(tests) / sizeof(tests[0])) - failed << "/"
              << sizeof(tests) / sizeof(tests[0]) << " tests passed\n";
    return failed == 0 ? 0 : 1;
}
