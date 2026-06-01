#pragma once

#include "core/component.hpp"
#include "core/component_registry.hpp"
#include "hardware/car.hpp"
#include <cstdint>

namespace nuedcs::devices {
class EncoderMotor {
public:
    EncoderMotor(nuedcs::core::Component& status_component,
        nuedcs::core::Component& command_component, const std::string& name_prefix) {
        status_component.register_output(
            "/" + name_prefix + "/velocity_rad_s", velocity_rad_s_, 0.0);

        command_component.register_input(
            "/" + name_prefix + "/encoder_lines_per_rev", encoder_lines_per_rev_);
        command_component.register_input(
            "/" + name_prefix + "/target_speed_rad_s", target_speed_rad_s_);
    }

private:
    nuedcs::core::Component::InputInterface<uint16_t> encoder_lines_per_rev_;
    nuedcs::core::Component::InputInterface<float> target_speed_rad_s_;
    nuedcs::core::Component::OutputInterface<float> velocity_rad_s_;
};
}
