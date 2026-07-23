#pragma once

#include "core/component.hpp"
#include "devices/gimbal_serial.hpp"

#include <chrono>

namespace nuedcs::test {

/// Gimbal 电机测试 — yaw 始终 0，pitch 角度随时间递增。
///
/// Outputs:
///   /gimbal/enable       (double) – 1.0
///   /gimbal/control_mode (int)    – AngleCtrl (5)
///   /gimbal/yaw_cmd      (double) – 0.0
///   /gimbal/pitch_cmd    (double) – 递增角度 [rad]
///
/// Config:
///   enable_delay_ms     – 使能后等待时间 [ms], 默认 200
///   step_rad_per_sec    – pitch 角速度 [rad/s], 默认 0.5
class GimbalMotorTest final : public core::Component {
public:
    explicit GimbalMotorTest(ryml::NodeRef config) {
        auto c = core::Config { config };

        enable_delay_ms_  = c["enable_delay_ms"].get(200);
        step_rad_per_sec_ = c["step_rad_per_sec"].get(0.5);

        register_output("/gimbal/enable", out_enable_, 0.0);
        register_output("/gimbal/control_mode", out_control_mode_, 0);
        register_output("/gimbal/yaw_cmd", out_yaw_cmd_, 0.0);
        register_output("/gimbal/pitch_cmd", out_pitch_cmd_, 0.0);
    }

    bool init() override {
        t0_ = std::chrono::steady_clock::now();
        info("GimbalMotorTest: enable → delay {} ms → pitch ramp {:.2f} rad/s  yaw=0",
            enable_delay_ms_, step_rad_per_sec_);
        return true;
    }

    void update() override {
        using namespace std::chrono;

        *out_enable_ = 1.0;

        *out_control_mode_ = static_cast<int>(devices::GimbalCmd::AngleCtrl);
        *out_yaw_cmd_      = 0.0;
        *out_pitch_cmd_    = 0.0;
    }

private:
    int enable_delay_ms_    = 200;
    float step_rad_per_sec_ = 0.5f;

    std::chrono::steady_clock::time_point t0_;

    OutputInterface<double> out_enable_;
    OutputInterface<int> out_control_mode_;
    OutputInterface<double> out_yaw_cmd_;
    OutputInterface<double> out_pitch_cmd_;
};

} // namespace nuedcs::test
