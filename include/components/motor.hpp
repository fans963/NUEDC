#pragma once

#include "core/component_registry.hpp"

#include <spdlog/spdlog.h>

namespace nuedcs::components {

using core::node_str;
using core::node_val;

class Motor : public core::Component {
public:
    Motor() = default;

    void configure(ryml::NodeRef config) override
    {
        kp_ = node_val<double>(config["kp"], 1.0);
        ki_ = node_val<double>(config["ki"], 0.0);
        kd_ = node_val<double>(config["kd"], 0.0);
    }

    bool init() override
    {
        info("PID: kp={:.3f}, ki={:.3f}, kd={:.3f}", kp_, ki_, kd_);
        return true;
    }

    void update() override
    {
        double err = target_ - current_;
        integral_ += err;
        output_ = kp_ * err + ki_ * integral_ + kd_ * (err - prev_);
        prev_ = err;
    }

private:
    double kp_ = 1.0, ki_ = 0.0, kd_ = 0.0;
    double target_ = 0, current_ = 0, integral_ = 0, prev_ = 0, output_ = 0;
};

} // namespace nuedcs::components

REGISTER_COMPONENT(nuedcs::components, Motor);
