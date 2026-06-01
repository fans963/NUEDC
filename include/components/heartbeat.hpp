#pragma once

#include "core/component_registry.hpp"

#include <chrono>
#include <spdlog/spdlog.h>

namespace nuedcs::components {

using core::node_val;

class Heartbeat : public core::Component {
public:
    Heartbeat(ryml::NodeRef config) {
        register_input("/camera1/test", wdad_);
        register_output("/flag", flag_, false);

        interval_ms_ = node_val<int64_t>(config["interval_ms"], 1000);
    }

    bool init() override {
        info("interval={}ms", interval_ms_);
        last_ = std::chrono::steady_clock::now();
        return true;
    }

    void update() override {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_).count() >= interval_ms_) {
            info("tick #{}", ++ticks_);
            last_ = now;
        }
    }

private:
    InputInterface<double> wdad_;
    OutputInterface<bool> flag_;
    int64_t interval_ms_ = 1000;
    int64_t ticks_ = 0;
    std::chrono::steady_clock::time_point last_;
};

} // namespace nuedcs::components

REGISTER_COMPONENT(nuedcs::components, Heartbeat);
