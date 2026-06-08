#pragma once

#include "core/component.hpp"
// component_registry.hpp is included transitively via component.hpp
#include "pid_calculator.hpp"

namespace nuedcs::controller::pid {

class ErrorPidController : public core::Component {
public:
    ErrorPidController(ryml::NodeRef config)
        : pid_calculator_([&] {
              auto c = core::Config{config};
              return PidCalculator(
                  c["kp"].get(nan), c["ki"].get(nan), c["kd"].get(nan),
                  c["output_min"].get(-inf), c["output_max"].get(inf),
                  c["integral_min"].get(-inf), c["integral_max"].get(inf));
          }()) {
        auto c = core::Config{config};
        register_input(c["measurement"].str(), measurement_);
        register_output(c["control"].str(), control_);
    }
    void update() override {
        auto err  = *measurement_;
        *control_ = pid_calculator_.update(err);
    }

private:
    PidCalculator pid_calculator_;
    static constexpr double inf = std::numeric_limits<double>::infinity();
    static constexpr double nan = std::numeric_limits<double>::quiet_NaN();

    InputInterface<double> measurement_;

    OutputInterface<double> control_;
};

} // namespace nuedcs::controller::pid