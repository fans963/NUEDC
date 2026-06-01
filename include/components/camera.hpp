#pragma once

#include "core/component_registry.hpp"

#include <spdlog/spdlog.h>

namespace nuedcs::components {

using core::node_str;
using core::node_val;

class Camera : public core::Component {
public:
    Camera(ryml::NodeRef config) {
        register_input("/status/loop_hz", test_);
        register_output("/" + name() + "/test", dad_, 0.0);

        register_output("/imu/gyro", wdaffsa,1.0);
        register_output("/encoder/velocity", wdwda,2.0);

        device_ = node_str(config["device"], "/dev/video0");
        width_  = node_val<int>(config["width"], 640);
        height_ = node_val<int>(config["height"], 480);
        fps_    = node_val<int>(config["fps"], 30);
    }

    bool init() override {
        info("{} ({}x{} @ {}fps)", device_, width_, height_, fps_);
        return true;
    }

    void update() override { frame_count_++; }

private:
    InputInterface<double> test_;
    OutputInterface<double> dad_;
    OutputInterface<float> wdwda;
    OutputInterface<float> wdaffsa;
    std::string device_;
    int width_       = 640;
    int height_      = 480;
    int fps_         = 30;
    int frame_count_ = 0;
};

} // namespace nuedcs::components

REGISTER_COMPONENT(nuedcs::components, Camera);
