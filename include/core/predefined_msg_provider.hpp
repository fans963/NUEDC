#pragma once

#include "core/component_registry.hpp"

#include <chrono>

namespace nuedcs::components {

using core::node_val;

class PredefinedMsgProvider : public core::Component {
public:
    PredefinedMsgProvider(ryml::NodeRef config) {
        register_output("/status/loop_hz", loop_hz_, node_val<double>(config["loop_hz"], 1000));
        register_output("/status/update_count", update_count_, int64_t{0});
    }

    bool init() override {
        last_log_ = std::chrono::steady_clock::now();
        start_    = std::chrono::steady_clock::now();
        info("ready, log_interval={}ms", log_interval_ms_);
        return true;
    }

    void update() override { ++*update_count_; }

private:
    OutputInterface<double> loop_hz_;
    OutputInterface<int64_t> update_count_;

    int64_t log_interval_ms_ = 1000;
    std::chrono::steady_clock::time_point start_;
    std::chrono::steady_clock::time_point last_log_;
};

} // namespace nuedcs::components

REGISTER_COMPONENT(nuedcs::components, PredefinedMsgProvider);
