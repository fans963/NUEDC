#pragma once

#include "core/component_registry.hpp"

namespace nuedcs::hardware {

class Car : public core::Component {
public:
    // Registry 构造：config 来自 YAML
    Car(ryml::NodeRef /*config*/)
        : command_(create_partner_component<CarCommand>(name() + "_command", *this)) {
        register_output("/motor/speed", speed_out_, 0.0f);
    }

    void update() override { }

private:
    class CarCommand : public core::Component {
    public:
        // Partner 构造：自定义参数，不走 Registry
        CarCommand(Car& car)
            : car_(car) {
            register_input("/imu/gyro", gyro_in_);
            register_input("/encoder/velocity", encoder_in_);
        }
        void update() override {
            car_.command_update();
            info("dawda{}", *gyro_in_);
        }

    private:
        Car& car_;

        InputInterface<float> gyro_in_;
        InputInterface<float> encoder_in_;
    };

    OutputInterface<float> speed_out_;
    CarCommand* command_;

    void command_update() { /* 从 sensor 读取，计算，写入 CarCommand 的 output */ }
};

} // namespace nuedcs::hardware

// 只注册 Car，CarCommand 不对外暴露
REGISTER_COMPONENT(nuedcs::hardware, Car)
