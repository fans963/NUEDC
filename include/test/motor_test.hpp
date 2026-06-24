#pragma once

#include "core/component.hpp"
#include <cmath>

namespace nuedcs::test {

/// 测试组件：输出正弦波目标速度，用于验证 USB 电机通信。
///
/// 输出:
///   /chassis/left_encoder/target_speed  — 左电机目标速度 (rad/s)
///   /chassis/right_encoder/target_speed — 右电机目标速度 (rad/s)
///
/// Config:
///   speed_max   — 最大速度 (rad/s), 默认 5.0
///   period_sec  — 正弦周期 (秒), 默认 2.0
class MotorTest final : public core::Component {
public:
    explicit MotorTest(ryml::NodeRef config) {
        auto c = core::Config{config};
        speed_max_  = c["speed_max"].get(5.0f);
        period_sec_ = c["period_sec"].get(2.0f);

        register_output("/chassis/left_encoder/target_speed", left_speed_, 0.0f);
        register_output("/chassis/right_encoder/target_speed", right_speed_, 0.0f);
    }

    bool init() override {
        t0_ = std::chrono::steady_clock::now();
        info("MotorTest started: speed_max={:.1f} rad/s, period={:.1f} s", speed_max_, period_sec_);
        return true;
    }

    void update() override {
        using namespace std::chrono;
        float t = duration<float>(steady_clock::now() - t0_).count();
        float w = 2.0f * 3.1415926f / period_sec_;
        float v = speed_max_ * std::sin(w * t);

        *left_speed_  = v;
        *right_speed_ = v;
    }

private:
    float speed_max_  = 5.0f;
    float period_sec_ = 2.0f;
    std::chrono::steady_clock::time_point t0_;

    OutputInterface<float> left_speed_;
    OutputInterface<float> right_speed_;
};

} // namespace nuedcs::test
