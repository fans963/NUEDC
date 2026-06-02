#pragma once

#include "core/component.hpp"
#include <atomic>
#include <cstdint>
#include <string>

namespace nuedcs::devices {

/// Firmware-side encoder + motor — velocity already computed by firmware.
///
/// Protocol (see schemas/):
///   Firmware → host:  EncoderPack { encoder_id, velocity_rad_s }  (float)
///   Host → firmware:  MotorCommandPack  { motor_id, target_speed_rad_s }
///   Host → firmware:  EncoderConfigPack { encoder_id, lines_per_rev }
///
/// RMCS-style two-phase update:
///   store_velocity(v)  — USB callback, atomic store firmware-computed velocity
///   update_status()    — main loop, publish to OutputInterface
///   generate_command() — read target_speed from InputInterface
class EncoderMotor {
public:
    struct Config {
        uint16_t lines_per_rev = 0;
        Config& set_lines_per_rev(uint16_t v) { lines_per_rev = v; return *this; }
    };

    EncoderMotor(core::Component& status, core::Component& command,
                 const std::string& prefix, uint8_t motor_id)
        : motor_id_(motor_id) {
        status.register_output(prefix + "/velocity", velocity_out_, 0.0f);
        command.register_input(prefix + "/target_speed", target_speed_in_);
        command.register_input(prefix + "/encoder_lines_per_rev", lines_per_rev_in_);
    }

    EncoderMotor(core::Component& status, core::Component& command,
                 const std::string& prefix, uint8_t motor_id, const Config& cfg)
        : EncoderMotor(status, command, prefix, motor_id) { configure(cfg); }

    EncoderMotor(const EncoderMotor&) = delete;
    EncoderMotor& operator=(const EncoderMotor&) = delete;

    // ── Configuration ────────────────────────────────────────────────

    void configure(const Config& cfg) {
        lines_per_rev_ = cfg.lines_per_rev;
    }

    uint8_t  motor_id()       const { return motor_id_; }
    uint16_t lines_per_rev()  const { return lines_per_rev_; }
    bool     config_pending() const { return config_pending_; }

    /// Call after sending EncoderConfigPack to firmware to clear pending flag.
    void clear_config_pending() { config_pending_ = false; }

    // ── Raw input (USB callback context) ─────────────────────────────

    /// Firmware-computed velocity (rad/s), received via EncoderPack.
    void store_velocity(float velocity) {
        raw_velocity_.store(velocity, std::memory_order_relaxed);
    }

    // ── Status update (main loop) ────────────────────────────────────

    void update_status() {
        // Dynamic reconfig: detect lines_per_rev change from InputInterface
        if (lines_per_rev_in_.ready()) {
            uint16_t v = *lines_per_rev_in_;
            if (v != lines_per_rev_ && v != 0) {
                configure(Config{}.set_lines_per_rev(v));
                config_pending_ = true;  // signal caller to send EncoderConfigPack
            }
        }

        velocity_ = raw_velocity_.load(std::memory_order_relaxed);
        *velocity_out_ = velocity_;
    }

    // ── Command generation ───────────────────────────────────────────

    /// Read target_speed from controller (rad/s). Returns 0 if not connected.
    float generate_command() const {
        return target_speed_in_.ready() ? *target_speed_in_ : 0.0f;
    }

    // ── Accessors ────────────────────────────────────────────────────

    float velocity() const { return velocity_; }
    float target_speed() const {
        return target_speed_in_.ready() ? *target_speed_in_ : 0.0f;
    }

private:
    uint8_t  motor_id_;
    uint16_t lines_per_rev_    = 0;

    // Atomic cross-thread: firmware-computed velocity (float)
    std::atomic<float> raw_velocity_{0.0f};
    static_assert(std::atomic<float>::is_always_lock_free);

    float velocity_     = 0.0f;
    bool  config_pending_ = false;

    core::Component::OutputInterface<float>   velocity_out_;
    core::Component::InputInterface<float>    target_speed_in_;
    core::Component::InputInterface<uint16_t> lines_per_rev_in_;
};

}  // namespace nuedcs::devices
