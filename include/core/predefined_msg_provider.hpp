#pragma once

#include "core/component_registry.hpp"

#include <chrono>

namespace nuedcs::components {

// 预置组件：提供更新计数、频率、时间戳等公共信息。
// 其他组件通过 InputInterface 绑定即可读取。
//
// YAML:
//   components:
//     - PredefinedMsgProvider -> status
//
// 其他组件使用：
//   InputInterface<double> rate_in_;
//   register_input("/status/update_rate", rate_in_);
//   if (rate_in_.ready()) log().info("rate={}", *rate_in_);
class PredefinedMsgProvider : public core::Component {
public:
    PredefinedMsgProvider() {
        register_output("/status/loop_hz", loop_hz_, 0.0);
        register_output("/status/update_count", update_count_, int64_t { 0 });
    }

    void configure(ryml::NodeRef config) override {
        *loop_hz_ = core::node_val<double>(config["loop_hz"], 1000);
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
    InputInterface<bool> wda_;

    int64_t log_interval_ms_ = 1000;
    std::chrono::steady_clock::time_point start_;
    std::chrono::steady_clock::time_point last_log_;
};

} // namespace nuedcs::components

REGISTER_COMPONENT(nuedcs::components, PredefinedMsgProvider);
