#pragma once

#include "core/component.hpp"
#include "core/component_registry.hpp"

#include "pid_calculator.hpp"

namespace nuedcs::controller::pid {

class PidController : public core::Component {
public:
    PidController(ryml::NodeRef config)
        : pid_calculator_(core::node_val<double>(config["kp"], nan),
              core::node_val<double>(config["ki"], nan), core::node_val<double>(config["kd"], nan),
              core::node_val<double>(config["output_min"], -inf),
              core::node_val<double>(config["output_max"], inf),
              core::node_val<double>(config["integral_min"], -inf),
              core::node_val<double>(config["integral_max"], inf)) {
        register_input(core::node_str(config["measurement"]), measurement_);
        register_input(core::node_str(config["setpoint"]), setpoint_);
        register_output(core::node_str(config["control"]), control_);
    }

    void update() override {
        auto err  = *setpoint_ - *measurement_;
        *control_ = pid_calculator_.update(err);
    }

private:
    PidCalculator pid_calculator_;
    static constexpr double inf = std::numeric_limits<double>::infinity();
    static constexpr double nan = std::numeric_limits<double>::quiet_NaN();

    InputInterface<double> measurement_;
    InputInterface<double> setpoint_;

    OutputInterface<double> control_;
};

} // namespace nuedcs::controller::pid

REGISTER_COMPONENT(nuedcs::controller::pid, PidController)