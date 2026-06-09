#pragma once

#include "core/component.hpp"
// component_registry.hpp is included transitively via component.hpp

#include <algorithm>

namespace nuedcs::controller::chassis {

/// Differential-drive inverse kinematics.
///
/// Inputs:
///   /chassis/control_linear   — desired linear velocity (m/s)
///   /chassis/control_angular  — desired angular velocity (rad/s)
///
/// Outputs:
///   /chassis/left/target_speed   — left wheel target (rad/s)
///   /chassis/right/target_speed  — right wheel target (rad/s)
///
/// Config:
///   wheel_base    — distance between wheels (m), default 0.20
///   wheel_radius  — wheel radius (m), default 0.05
///   linear_max    — maximum linear speed (m/s), default 3.0
///   angular_max   — maximum angular speed (rad/s), default 8.0
class ChassisController final : public core::Component {
    static constexpr float kInf = std::numeric_limits<float>::infinity();
    static constexpr float kNan = std::numeric_limits<float>::quiet_NaN();

public:
    explicit ChassisController(ryml::NodeRef config) {
        auto c = core::Config{config};

        wheel_base_   = c["wheel_base"].get(0.20);
        wheel_radius_ = c["wheel_radius"].get(0.05);
        linear_max_   = c["linear_max"].get(3.0);
        angular_max_  = c["angular_max"].get(8.0);

        // Inputs: desired chassis motion
        register_input("/chassis/control_linear",  ctrl_linear_);
        register_input("/chassis/control_angular", ctrl_angular_);

        // Outputs: wheel targets
        register_output("/chassis/left/target_speed",  left_out_,  0.0);
        register_output("/chassis/right/target_speed", right_out_, 0.0);
    }

    void update() override {
        // Read control inputs (NaN if not connected → no motion)
        float linear  = ctrl_linear_.ready()  ? *ctrl_linear_  : 0.0;
        float angular = ctrl_angular_.ready() ? *ctrl_angular_ : 0.0;

        // Clamp
        linear  = std::clamp(linear,  -linear_max_,  linear_max_);
        angular = std::clamp(angular, -angular_max_, angular_max_);

        // Inverse kinematics: body velocity → wheel velocity (rad/s)
        float half_track = wheel_base_ / 2.0;
        float v_left  = (linear - angular * half_track) / wheel_radius_;
        float v_right = (linear + angular * half_track) / wheel_radius_;

        *left_out_  = v_left;
        *right_out_ = v_right;
    }

private:
    float wheel_base_   = 0.20;
    float wheel_radius_ = 0.05;
    float linear_max_   = 3.0;
    float angular_max_  = 8.0;

    InputInterface<float> ctrl_linear_;
    InputInterface<float> ctrl_angular_;

    OutputInterface<float> left_out_;
    OutputInterface<float> right_out_;
};

}  // namespace nuedcs::controller::chassis
