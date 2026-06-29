#pragma once

// QDrive QD4310 CAN motor — FOC servo with built-in position/speed/torque loops.
// Protocol:
//   Host → Motor: CAN ID=0x400+id, DLC=3: [cmd, val_lo, val_hi] (int16 LE)
//   Motor → Host: CAN ID=0x500+id, DLC=8: [status,_, curr_lo,curr_hi, speed_lo,speed_hi, angle_lo,angle_hi]
//
// Encoder: 15-bit (32768 cpr), angle resolution 0.0109°. Torque constant: 0.27 Nm/A.

#include "core/component.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>

namespace nuedcs::devices {

class CanMotor {
    static constexpr double kPi     = 3.141592653589793;
    static constexpr double kEncMax = 32768.0;   // 15-bit encoder
    static constexpr double kRpmMax = 1000.0;    // ±1000 rpm full range
    static constexpr double kCurMax = 10.0;      // ±10A full range
    static constexpr double kCurScale = 32767.0 / kCurMax;  // raw/amp
    static constexpr double kSpeedScale = 32767.0 / kRpmMax;
    static constexpr double kAngleScale = 65535.0 / (2.0 * kPi);

public:
    CanMotor(core::Component& status, core::Component& command,
             const std::string& prefix, uint32_t can_id)
        : can_id_(can_id) {
        status.register_output(prefix + "/angle",    angle_out_,    0.0);
        status.register_output(prefix + "/velocity", velocity_out_, 0.0);
        status.register_output(prefix + "/torque",   torque_out_,   0.0);
        status.register_output(prefix + "/temperature", temp_out_, 0.0);

        command.register_input(prefix + "/control_torque",   ctrl_torque_in_,   false);
        command.register_input(prefix + "/control_velocity", ctrl_velocity_in_, false);
        command.register_input(prefix + "/control_angle",    ctrl_angle_in_,    false);
    }

    CanMotor(const CanMotor&)            = delete;
    CanMotor& operator=(const CanMotor&) = delete;

    // ── Raw input (CAN RX callback) ──────────────────────────────────

    void store_status(std::span<const uint8_t> data) {
        if (data.size() != 8) return;
        uint64_t raw;
        std::memcpy(&raw, data.data(), 8);
        raw_feedback_.store(raw, std::memory_order_relaxed);
    }

    // ── Status update (main loop) ────────────────────────────────────

    void update_status() {
        uint64_t raw = raw_feedback_.load(std::memory_order_relaxed);
        uint8_t status    = static_cast<uint8_t>(raw & 0xFF);
        int16_t raw_cur   = static_cast<int16_t>((raw >> 16) & 0xFFFF);
        int16_t raw_speed = static_cast<int16_t>((raw >> 32) & 0xFFFF);
        uint16_t raw_ang  = static_cast<uint16_t>((raw >> 48) & 0xFFFF);

        enabled_    = (status & 0x01) != 0;
        current_    = static_cast<double>(raw_cur)   / kCurScale;        // A
        velocity_   = static_cast<double>(raw_speed) / kSpeedScale / 60.0 * 2.0 * kPi; // rad/s
        angle_      = static_cast<double>(raw_ang)   / kAngleScale;     // rad
        temperature_ = 0.0; // QD4310 doesn't report temperature in CAN feedback

        *angle_out_    = angle_;
        *velocity_out_ = velocity_;
        *torque_out_   = current_ * 0.27;  // 0.27 Nm/A
        *temp_out_     = temperature_;
    }

    // ── Command generation ───────────────────────────────────────────

    uint64_t generate_command() {
        double torque   = ctrl_torque();
        double velocity = ctrl_velocity();
        double angle    = ctrl_angle();

        if (!std::isnan(angle))
            return build_cmd(0x05, angle_to_raw(angle));       // 角度控制
        if (!std::isnan(velocity))
            return build_cmd(0x04, speed_to_raw(velocity));    // 速度控制
        if (!std::isnan(torque))
            return build_cmd(0x03, current_to_raw(torque));    // 电流控制
        return build_cmd(0x00, 0);                              // NOP
    }

    uint32_t can_id() const { return can_id_; }
    uint32_t can_rx_id() const { return 0x500 + (can_id_ & 0x0F); }

    // ── Control accessors ────────────────────────────────────────────

    double ctrl_torque() const {
        return ctrl_torque_in_.ready() ? *ctrl_torque_in_ : std::numeric_limits<double>::quiet_NaN();
    }
    double ctrl_velocity() const {
        return ctrl_velocity_in_.ready() ? *ctrl_velocity_in_ : std::numeric_limits<double>::quiet_NaN();
    }
    double ctrl_angle() const {
        return ctrl_angle_in_.ready() ? *ctrl_angle_in_ : std::numeric_limits<double>::quiet_NaN();
    }

    // ── Status accessors ─────────────────────────────────────────────

    double angle()       const { return angle_; }
    double velocity()    const { return velocity_; }
    double torque()      const { return current_ * 0.27; }
    double temperature() const { return temperature_; }
    bool   enabled()     const { return enabled_; }

private:
    static constexpr uint32_t kCmdBaseId  = 0x400;
    static constexpr uint32_t kFeedbackId = 0x500;

    static int16_t current_to_raw(double torque) {
        double amps = torque / 0.27;  // Nm → A
        return static_cast<int16_t>(std::clamp(amps, -kCurMax, kCurMax) * kCurScale);
    }
    static int16_t speed_to_raw(double rad_s) {
        double rpm = rad_s / (2.0 * kPi) * 60.0;
        return static_cast<int16_t>(std::clamp(rpm, -kRpmMax, kRpmMax) * kSpeedScale);
    }
    static uint16_t angle_to_raw(double rad) {
        double norm = std::fmod(rad, 2.0 * kPi);
        if (norm < 0) norm += 2.0 * kPi;
        return static_cast<uint16_t>(norm * kAngleScale);
    }

    // QD4310 CAN command: [cmd_byte][val_lo][val_hi], sent as 3 bytes in a u64
    static uint64_t build_cmd(uint8_t cmd, int16_t val) {
        // Pack as CAN frame: ID | (cmd) | (val_lo) | (val_hi)
        return (static_cast<uint64_t>(kCmdBaseId + 0) << 32)
             | (static_cast<uint64_t>(cmd) << 16)
             | static_cast<uint64_t>(static_cast<uint16_t>(val));
    }

    uint32_t can_id_;

    std::atomic<uint64_t> raw_feedback_{0};
    static_assert(std::atomic<uint64_t>::is_always_lock_free);

    double angle_       = 0.0;
    double velocity_    = 0.0;
    double current_     = 0.0;
    double temperature_ = 0.0;
    bool   enabled_     = false;

    core::Component::OutputInterface<double> angle_out_;
    core::Component::OutputInterface<double> velocity_out_;
    core::Component::OutputInterface<double> torque_out_;
    core::Component::OutputInterface<double> temp_out_;
    core::Component::InputInterface<double>  ctrl_torque_in_;
    core::Component::InputInterface<double>  ctrl_velocity_in_;
    core::Component::InputInterface<double>  ctrl_angle_in_;
};

}  // namespace nuedcs::devices
