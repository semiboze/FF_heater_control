#pragma once

#include <cmath>
#include <cstdint>

namespace ffheater {

constexpr uint32_t COMM_TIMEOUT_MS = 150000UL;
constexpr uint32_t COOLDOWN_MS = 600000UL;
constexpr uint32_t ROOM_FALLBACK_MS = 1800000UL;
constexpr uint32_t IGNITION_INTERVAL_MS = 300000UL;
constexpr uint8_t MAX_IGNITION_ATTEMPTS = 3;

enum class HeaterState : uint8_t { OFF, IGNITING, ON, UNCONFIRMED };
enum class Command : uint8_t { NONE, ON, OFF };
enum class ControlMode : uint8_t { NORMAL, ROOM_FALLBACK, DUCT_UNCONFIRMED, SENSOR_STOP };
enum class StopReason : uint8_t {
    NONE, USER, TIMER, BOTH_SENSORS, COMM_TIMEOUT, IGNITION_FAILED, ROOM_LIMIT, TEMPERATURE
};

struct Settings {
    uint32_t durationMinutes = 60;
    float onTemperature = 10.0f;
    float offTemperature = 15.0f;
    float ductRise = 5.0f;
};

// A failed measurement takes effect immediately. Recovery needs two new packets,
// not two passes through loop(). NaN must never become a substitute zero reading.
struct SensorHealth {
    float temperature = NAN;
    uint32_t lastValidAt = 0;
    uint8_t normalCount = 0;
    bool healthy = false;

    void receive(float value, uint32_t now) {
        if (!std::isfinite(value)) {
            temperature = NAN;
            normalCount = 0;
            healthy = false;
            return;
        }
        temperature = value;
        lastValidAt = now;
        if (normalCount < 2) ++normalCount;
        healthy = normalCount >= 2;
    }

    void invalidate() {
        healthy = false;
        normalCount = 0;
        temperature = NAN;
    }
};

// All methods run on the Arduino loop task; radio callbacks only enqueue data.
// The clock is injected so the actual production state machine can be tested.
class Controller {
public:
    Settings settings;
    SensorHealth room;
    SensorHealth duct;

    void receive(float roomTemperature, float ductTemperature, uint32_t now) {
        // A long gap breaks the consecutive-normal recovery sequence, even when
        // the next packet arrives before tick() notices the gap.
        if (hasPacket_ && now - lastPacketAt_ >= COMM_TIMEOUT_MS) {
            if (active_) pendingCommStop_ = true;
            room.invalidate();
            duct.invalidate();
        }
        hasPacket_ = true;
        lastPacketAt_ = now;
        room.receive(roomTemperature, now);
        duct.receive(ductTemperature, now);
        if (active_ && !room.healthy && !duct.healthy) pendingSensorStop_ = true;
    }

    void startAuto(uint32_t now) {
        active_ = true;
        autoStartedAt_ = now;
        stopReason_ = StopReason::NONE;
        // Do not forget an existing ignition/run when a user starts the timer.
    }

    Command stopByUser(uint32_t now) { return stop(StopReason::USER, now, true); }

    void recordManualOn(uint32_t now) {
        beginIgnition(now);
        if (!active_) state_ = HeaterState::UNCONFIRMED;
        cooldown_ = false;
        stopReason_ = StopReason::NONE;
    }

    void restoreCooldown(uint32_t now) {
        cooldown_ = true;
        cooldownStartedAt_ = now;
    }

    Command tick(uint32_t now) {
        if (!active_) return Command::NONE;

        // Stop decisions always precede ignition and retries, including while
        // OFF or waiting for confirmation. An OFF command is not proof of stop.
        if (pendingCommStop_ || !communicationValid(now)) {
            pendingCommStop_ = false;
            room.invalidate();
            duct.invalidate();
            return stop(StopReason::COMM_TIMEOUT, now, true);
        }
        if (pendingSensorStop_ || (!room.healthy && !duct.healthy))
            return stop(StopReason::BOTH_SENSORS, now, true);
        if (now - autoStartedAt_ >= settings.durationMinutes * 60000UL)
            return stop(StopReason::TIMER, now, true);

        if (state_ != HeaterState::OFF) {
            if (room.healthy && room.temperature >= settings.offTemperature)
                return stop(StopReason::TEMPERATURE, now, false);

            if (room.healthy) {
                fallback_ = false;
            } else if (!fallback_) {
                fallback_ = true;
                fallbackStartedAt_ = now;
            }
            if (fallback_ && now - fallbackStartedAt_ >= ROOM_FALLBACK_MS)
                return stop(StopReason::ROOM_LIMIT, now, false);

            if (state_ == HeaterState::ON) return Command::NONE;

            if (duct.healthy) {
                if (!baselineValid_) {
                    // No valid duct temperature existed at ignition. Start a
                    // fresh observation after recovery, never compare with 0.
                    baselineValid_ = true;
                    baselineTemperature_ = duct.temperature;
                    baselineStartedAt_ = now;
                } else if (duct.temperature >= baselineTemperature_ + settings.ductRise) {
                    state_ = HeaterState::ON;
                    return Command::NONE;
                }
            }

            if (now - lastAttemptAt_ >= IGNITION_INTERVAL_MS) {
                if (attempts_ < MAX_IGNITION_ATTEMPTS) {
                    ++attempts_;
                    lastAttemptAt_ = now;
                    if (attempts_ == MAX_IGNITION_ATTEMPTS && !duct.healthy)
                        state_ = HeaterState::UNCONFIRMED;
                    return Command::ON;
                }
                if (duct.healthy) {
                    if (now - baselineStartedAt_ >= IGNITION_INTERVAL_MS)
                        return stop(StopReason::IGNITION_FAILED, now, true);
                } else {
                    state_ = HeaterState::UNCONFIRMED;
                }
            }
            return Command::NONE;
        }

        if (cooldownActive(now)) return Command::NONE;
        const float controlTemperature = room.healthy ? room.temperature : duct.temperature;
        if (controlTemperature <= settings.onTemperature) {
            beginIgnition(now);
            return Command::ON;
        }
        return Command::NONE;
    }

    bool active() const { return active_; }
    HeaterState state() const { return state_; }
    StopReason stopReason() const { return stopReason_; }
    uint8_t ignitionAttempts() const { return attempts_; }
    bool communicationValid(uint32_t now) const {
        return hasPacket_ && now - lastPacketAt_ < COMM_TIMEOUT_MS;
    }
    uint32_t dataAgeSeconds(uint32_t now) const {
        return hasPacket_ ? (now - lastPacketAt_) / 1000UL : COMM_TIMEOUT_MS / 1000UL;
    }
    bool roomValid(uint32_t now) const { return communicationValid(now) && room.healthy; }
    bool ductValid(uint32_t now) const { return communicationValid(now) && duct.healthy; }
    ControlMode mode(uint32_t now) const {
        if (!roomValid(now) && !ductValid(now)) return ControlMode::SENSOR_STOP;
        if (!roomValid(now)) return ControlMode::ROOM_FALLBACK;
        if (!ductValid(now)) return ControlMode::DUCT_UNCONFIRMED;
        return ControlMode::NORMAL;
    }
    bool cooldownActive(uint32_t now) const {
        return cooldown_ && now - cooldownStartedAt_ < COOLDOWN_MS;
    }
    uint32_t cooldownRemainingSeconds(uint32_t now) const {
        return cooldownActive(now) ? (COOLDOWN_MS - (now - cooldownStartedAt_) + 999UL) / 1000UL : 0;
    }
    int32_t fallbackRemainingSeconds(uint32_t now) const {
        if (!active_ || !fallback_ || state_ == HeaterState::OFF) return -1;
        const uint32_t elapsed = now - fallbackStartedAt_;
        return elapsed < ROOM_FALLBACK_MS ? (ROOM_FALLBACK_MS - elapsed + 999UL) / 1000UL : 0;
    }
    uint32_t autoRemainingSeconds(uint32_t now) const {
        if (!active_) return 0;
        const uint32_t elapsed = now - autoStartedAt_;
        const uint32_t duration = settings.durationMinutes * 60000UL;
        return elapsed < duration ? (duration - elapsed) / 1000UL : 0;
    }

private:
    bool active_ = false;
    bool hasPacket_ = false;
    bool pendingCommStop_ = false;
    bool pendingSensorStop_ = false;
    uint32_t lastPacketAt_ = 0;
    uint32_t autoStartedAt_ = 0;
    HeaterState state_ = HeaterState::OFF;
    StopReason stopReason_ = StopReason::NONE;
    bool cooldown_ = false;
    uint32_t cooldownStartedAt_ = 0;
    bool fallback_ = false;
    uint32_t fallbackStartedAt_ = 0;
    uint8_t attempts_ = 0;
    uint32_t lastAttemptAt_ = 0;
    bool baselineValid_ = false;
    float baselineTemperature_ = NAN;
    uint32_t baselineStartedAt_ = 0;

    void beginIgnition(uint32_t now) {
        stopReason_ = StopReason::NONE;
        state_ = HeaterState::IGNITING;
        attempts_ = 1;
        lastAttemptAt_ = now;
        baselineValid_ = ductValid(now);
        baselineTemperature_ = baselineValid_ ? duct.temperature : NAN;
        baselineStartedAt_ = now;
        fallback_ = !roomValid(now) && ductValid(now);
        fallbackStartedAt_ = now;
    }

    Command stop(StopReason reason, uint32_t now, bool disableAuto) {
        state_ = HeaterState::OFF;
        if (disableAuto) {
            active_ = false;
            pendingCommStop_ = false;
            pendingSensorStop_ = false;
        }
        stopReason_ = reason;
        fallback_ = false;
        baselineValid_ = false;
        cooldown_ = true;
        cooldownStartedAt_ = now;
        return Command::OFF;
    }
};

} // namespace ffheater
