#pragma once

#include "core/component.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>

namespace nuedcs::devices {

/// CAN-connected motor (DJI M3508/M2006/GM6020 or LK motor).
///
/// RMCS-style two-phase update:
///   store_status(can_data)    — CAN RX callback, atomic store raw 8 bytes
///   update_status()           — main loop, parse → convert → publish outputs
///   generate_command()        — read control inputs → return CAN packet
class CanMotor {
public:
    enum class Type : uint8_t { kM3508, kM2006, kGM6020, kLK5010, kLK4010, kLK6012 };

    struct Config {
        Type   motor_type;
        double reduction_ratio = 1.0;
        bool   reversed        = false;

        Config& set_reduction_ratio(double v) { reduction_ratio = v; return *this; }
        Config& set_reversed()               { reversed = true;      return *this; }
    };

    CanMotor(core::Component& status, core::Component& command,
             const std::string& prefix, uint32_t can_id)
        : can_id_(can_id) {
        status.register_output(prefix + "/angle",    angle_out_,    0.0);
        status.register_output(prefix + "/velocity", velocity_out_, 0.0);
        status.register_output(prefix + "/torque",   torque_out_,   0.0);

        command.register_input(prefix + "/control_torque",   ctrl_torque_in_,   false);
        command.register_input(prefix + "/control_velocity", ctrl_velocity_in_, false);
        command.register_input(prefix + "/control_angle",    ctrl_angle_in_,    false);
    }

    CanMotor(core::Component& status, core::Component& command,
             const std::string& prefix, uint32_t can_id, const Config& cfg)
        : CanMotor(status, command, prefix, can_id) { configure(cfg); }

    CanMotor(const CanMotor&) = delete;
    CanMotor& operator=(const CanMotor&) = delete;

    // ── Configuration ────────────────────────────────────────────────

    void configure(const Config& cfg) {
        double sign = cfg.reversed ? -1.0 : 1.0;

        switch (cfg.motor_type) {
        case Type::kM3508: raw_angle_max_ = 8192; torque_c_  = 0.3  * 187.0 / 3591.0; current_max_ = 20.0;  break;
        case Type::kM2006: raw_angle_max_ = 8192; torque_c_  = 0.18 * 1.0   / 36.0;   current_max_ = 10.0;  break;
        case Type::kGM6020:raw_angle_max_ = 8192; torque_c_  = 0.741;                 current_max_ = 3.0;   break;
        case Type::kLK5010:raw_angle_max_ = 65536;torque_c_ = 0.90909;                current_max_ = 33.0; raw_current_max_ = 2048; break;
        case Type::kLK4010:raw_angle_max_ = 65536;torque_c_ = 0.07;                   current_max_ = 33.0; raw_current_max_ = 2048; break;
        case Type::kLK6012:raw_angle_max_ = 65536;torque_c_ = 1.09;                   current_max_ = 33.0; raw_current_max_ = 2048; break;
        }

        angle_from_raw_    = sign / cfg.reduction_ratio / raw_angle_max_ * 2.0 * 3.141592653589793;
        velocity_from_raw_ = sign / cfg.reduction_ratio / 60.0 * 2.0 * 3.141592653589793;
        torque_from_raw_   = sign * cfg.reduction_ratio * torque_c_ / raw_current_max_ * current_max_;
        torque_to_raw_     = 1.0 / torque_from_raw_;

        max_torque_ = cfg.reduction_ratio * torque_c_ * current_max_;
    }

    // ── Raw input (CAN RX callback) ──────────────────────────────────

    void store_status(std::span<const uint8_t> can_data) {
        if (can_data.size() != 8) return;
        uint64_t raw;
        std::memcpy(&raw, can_data.data(), 8);
        raw_feedback_.store(raw, std::memory_order_relaxed);
    }

    // ── Status update (main loop) ────────────────────────────────────

    void update_status() {
        uint64_t raw = raw_feedback_.load(std::memory_order_relaxed);
        // DJI-standard feedback layout: angle(16) velocity(16) current(16) temp(8) _unused(8)
        int16_t raw_angle    = static_cast<int16_t>(raw >> 0);
        int16_t raw_velocity = static_cast<int16_t>(raw >> 16);
        int16_t raw_current  = static_cast<int16_t>(raw >> 32);
        int8_t  raw_temp     = static_cast<int8_t>(raw >> 48);

        temperature_ = static_cast<double>(raw_temp);

        // Angle (rad)
        int32_t calibrated = raw_angle - angle_zero_;
        if (calibrated < 0) calibrated += raw_angle_max_;
        angle_ = angle_from_raw_ * static_cast<double>(calibrated);
        if (angle_ < 0) angle_ += 2.0 * 3.141592653589793;

        // Velocity (rad/s), Torque (N·m)
        velocity_ = velocity_from_raw_ * static_cast<double>(raw_velocity);
        torque_   = torque_from_raw_   * static_cast<double>(raw_current);

        last_raw_angle_ = raw_angle;

        *angle_out_    = angle_;
        *velocity_out_ = velocity_;
        *torque_out_   = torque_;
    }

    // ── Command generation ───────────────────────────────────────────

    /// Build CAN command packet (8 bytes, little-endian).
    /// Priority: angle > velocity > torque. Returns 0 if no control input.
    uint64_t generate_command() {
        double torque   = ctrl_torque();
        double velocity = ctrl_velocity();
        double angle    = ctrl_angle();

        if (!std::isnan(angle))
            return build_angle_command(angle);
        if (!std::isnan(velocity))
            return build_velocity_command(velocity, torque);
        if (!std::isnan(torque))
            return build_torque_command(torque);
        return 0;  // disable: zero current
    }

    uint32_t can_id() const { return can_id_; }

    // ── Control accessors (NaN = not connected) ──────────────────────

    double ctrl_torque() const {
        return ctrl_torque_in_.ready() ? *ctrl_torque_in_
                                       : std::numeric_limits<double>::quiet_NaN();
    }
    double ctrl_velocity() const {
        return ctrl_velocity_in_.ready() ? *ctrl_velocity_in_
                                         : std::numeric_limits<double>::quiet_NaN();
    }
    double ctrl_angle() const {
        return ctrl_angle_in_.ready() ? *ctrl_angle_in_
                                      : std::numeric_limits<double>::quiet_NaN();
    }

    // ── Status accessors ─────────────────────────────────────────────

    double angle()       const { return angle_; }
    double velocity()    const { return velocity_; }
    double torque()      const { return torque_; }
    double max_torque()  const { return max_torque_; }
    double temperature() const { return temperature_; }

    /// Calibrate: set current position as zero.
    void calibrate() { angle_zero_ = last_raw_angle_; }

private:
    static constexpr double kPi = 3.141592653589793;

    uint64_t build_torque_command(double torque) {
        torque = std::clamp(torque, -max_torque_, max_torque_);
        int16_t iq = static_cast<int16_t>(std::round(torque_to_raw_ * torque));
        // DJI current command: hi16=iq, lo16=0
        return (static_cast<uint64_t>(static_cast<uint16_t>(iq)) << 16);
    }

    uint64_t build_velocity_command(double velocity, double torque_limit) {
        // Standard DJI velocity command: hi32=speed_rpm, lo16=current_limit
        double rpm = velocity / (2.0 * kPi) * 60.0;  // rad/s → rpm
        int32_t speed_rpm = static_cast<int32_t>(std::round(rpm));
        int16_t iq_limit  = 0;
        if (!std::isnan(torque_limit)) {
            torque_limit = std::clamp(torque_limit, -max_torque_, max_torque_);
            iq_limit = static_cast<int16_t>(std::round(torque_to_raw_ * torque_limit));
        }
        return static_cast<uint64_t>(static_cast<uint32_t>(speed_rpm))
             | (static_cast<uint64_t>(static_cast<uint16_t>(iq_limit)) << 32);
    }

    uint64_t build_angle_command(double angle) {
        // DJI angle command: hi32=angle_raw, lo16=speed_limit
        double raw_angle = angle / angle_from_raw_;
        int32_t  angle_cmd = static_cast<int32_t>(std::round(raw_angle));
        return static_cast<uint64_t>(static_cast<uint32_t>(angle_cmd));
    }

    // CAN identity
    uint32_t can_id_;

    // Config
    int32_t raw_angle_max_      = 8192;
    int32_t raw_current_max_    = 16384;
    double  current_max_        = 20.0;
    double  torque_c_           = 1.0;
    double  angle_from_raw_     = 0.0;
    double  velocity_from_raw_  = 0.0;
    double  torque_from_raw_    = 0.0;
    double  torque_to_raw_      = 0.0;

    // Raw feedback (lock-free atomic)
    std::atomic<uint64_t> raw_feedback_{0};
    static_assert(std::atomic<uint64_t>::is_always_lock_free);

    // Decoded state
    int32_t angle_zero_   = 0;
    int32_t last_raw_angle_ = 0;
    double  angle_        = 0.0;
    double  velocity_     = 0.0;
    double  torque_       = 0.0;
    double  max_torque_   = 0.0;
    double  temperature_  = 0.0;

    // I/O interfaces
    core::Component::OutputInterface<double> angle_out_;
    core::Component::OutputInterface<double> velocity_out_;
    core::Component::OutputInterface<double> torque_out_;
    core::Component::InputInterface<double>  ctrl_torque_in_;
    core::Component::InputInterface<double>  ctrl_velocity_in_;
    core::Component::InputInterface<double>  ctrl_angle_in_;
};

}  // namespace nuedcs::devices
