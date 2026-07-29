#pragma once

#include "communication/slave_link.hpp"
#include "core/component.hpp"

#include <chrono>
#include <cstdio>

namespace nuedcs::test {

/// SlaveLink 通信闭环测试 — 自动测试 MCU 速度收发。
///
/// Test cycle (auto-repeats):
///   1. RAMP  (5s):  0 → +target m/s
///   2. HOLD  (3s):  steady at target
///   3. REV   (5s):  target → -target m/s
///   4. STOP  (2s):  0 m/s
///   5. target += step, repeat until max_speed, then loop.
///
/// Config:
///   port           – serial device, default /dev/ttyACM0
///   baudrate       – default 115200
///   tx_interval_ms – command TX interval, default 5
///   step           – speed increment per cycle [m/s], default 0.1
///   max_speed      – max test speed [m/s], default 0.5
class SlaveLinkTest final : public core::Component {
public:
    explicit SlaveLinkTest(ryml::NodeRef config)
        : core::Component()
    {
        // Read config early — partner components created after construction
        cfg_ = core::Config{config};
        step_      = cfg_["step"].get<float>(0.1f);
        max_speed_ = cfg_["max_speed"].get<float>(0.5f);
    }

    bool init() override {
        // Create SlaveLink as a partner component — Executor manages its lifecycle
        link_ = create_partner_component<nuedcs::communication::SlaveLink>(
            "slave_link", cfg_.root);

        if (!link_->init()) {
            error("SlaveLink init failed");
            return false;
        }

        info("SlaveLinkTest ready — step={:.2f} max={:.2f} m/s", step_, max_speed_);
        phase_       = Phase::RampUp;
        target_      = step_;
        phase_start_ = std::chrono::steady_clock::now();
        return true;
    }

    void update() override {
        using namespace std::chrono;

        if (!link_) return;

        auto  now     = steady_clock::now();
        float elapsed = duration<float>(now - phase_start_).count();

        // ── Phase state machine ──────────────────────────────────────────
        switch (phase_) {
        case Phase::RampUp: {
            float frac = std::clamp(elapsed / kRampDuration, 0.0f, 1.0f);
            cmd_left_  = target_ * frac;
            cmd_right_ = target_ * frac;
            if (frac >= 1.0f) {
                info("─── HOLD  {:.2f} m/s, {}s ───", target_, kHoldDuration);
                phase_       = Phase::Hold;
                phase_start_ = now;
            }
            break;
        }
        case Phase::Hold:
            if (elapsed >= kHoldDuration) {
                info("─── REV   {:.2f} → {:.2f} m/s, {}s ───",
                    target_, -target_, kRampDuration);
                phase_       = Phase::Reverse;
                phase_start_ = now;
            }
            break;
        case Phase::Reverse: {
            float frac = std::clamp(elapsed / kRampDuration, 0.0f, 1.0f);
            cmd_left_  = target_ * (1.0f - 2.0f * frac);
            cmd_right_ = target_ * (1.0f - 2.0f * frac);
            if (frac >= 1.0f) {
                info("─── STOP  2s ───");
                phase_       = Phase::Stop;
                phase_start_ = now;
                cmd_left_    = 0.0f;
                cmd_right_   = 0.0f;
            }
            break;
        }
        case Phase::Stop:
            if (elapsed >= kStopDuration) {
                target_ += step_;
                if (target_ > max_speed_ + 0.001f) {
                    target_ = step_;
                    info("═══ CYCLE DONE — restart at {:.2f} m/s ═══", target_);
                }
                info("─── RAMP  0 → {:.2f} m/s, {}s ───", target_, kRampDuration);
                phase_       = Phase::RampUp;
                phase_start_ = now;
            }
            break;
        }

        // ── Send command to MCU ──────────────────────────────────────────
        link_->set_speed(cmd_left_, cmd_right_);

        // ── Log telemetry every 200 ms ───────────────────────────────────
        if (now - last_log_ >= 200ms) {
            last_log_ = now;
            float tl, tr;
            link_->get_telemetry(tl, tr);

            char buf[128];
            std::snprintf(buf, sizeof(buf),
                "[%7.1f] CMD L=%+6.3f R=%+6.3f | TLM L=%+6.3f R=%+6.3f | %s",
                elapsed_total(),
                cmd_left_, cmd_right_, tl, tr, phase_name());
            info("{}", buf);
        }

        // SlaveLink needs its update() called for TX
        link_->update();
    }

    ~SlaveLinkTest() override = default;

private:
    enum class Phase : uint8_t { RampUp, Hold, Reverse, Stop };

    [[nodiscard]] const char* phase_name() const {
        switch (phase_) {
        case Phase::RampUp:  return "RAMP↑";
        case Phase::Hold:    return "HOLD-";
        case Phase::Reverse: return "REV ↓";
        case Phase::Stop:    return "STOP ";
        }
        return "???";
    }

    [[nodiscard]] float elapsed_total() const {
        return std::chrono::duration<float>(
            std::chrono::steady_clock::now() - t0_).count();
    }

    // ── Config ──────────────────────────────────────────────────────────
    core::Config cfg_{ryml::NodeRef{}};
    float step_{0.1f};
    float max_speed_{0.5f};

    // ── Timing ──────────────────────────────────────────────────────────
    static constexpr float kRampDuration = 5.0f;
    static constexpr float kHoldDuration = 3.0f;
    static constexpr float kStopDuration = 2.0f;

    // ── Partner ─────────────────────────────────────────────────────────
    nuedcs::communication::SlaveLink* link_{nullptr};

    // ── State ───────────────────────────────────────────────────────────
    std::chrono::steady_clock::time_point t0_{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point phase_start_{};
    std::chrono::steady_clock::time_point last_log_{};

    Phase phase_{Phase::RampUp};
    float target_{0.0f};
    float cmd_left_{0.0f};
    float cmd_right_{0.0f};
};

} // namespace nuedcs::test
