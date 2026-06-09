#pragma once

#include "core/component.hpp"
// component_registry.hpp is included transitively via component.hpp
#include "devices/bmi088.hpp"
#include "devices/can_motor.hpp"
#include "devices/encoder_motor.hpp"
#include "fast_tf/fast_tf.hpp"
#include "usb/nuedc_slave.hpp"
#include "util/throttle.hpp"
#include <cstdint>
#include <sys/types.h>

// ── TF tree: link definitions (compile-time type tags) ────────────────

struct OdomLink : fast_tf::Link<OdomLink> { static constexpr auto name = "odom"; };
struct BaseLink : fast_tf::Link<BaseLink> { static constexpr auto name = "base_link"; };
struct ImuLink  : fast_tf::Link<ImuLink>  { static constexpr auto name = "imu"; };

// Joint specializations must be in fast_tf namespace
namespace fast_tf {
template<> struct Joint<OdomLink>        { using Parent = Null; };
// Dynamic odom→base_link transform updated each iteration from IMU
template<> struct Joint<BaseLink> {
    using Parent = OdomLink;
    Eigen::Isometry3d transform = Eigen::Isometry3d::Identity();
};
// Static imu offset on chassis
template<> struct Joint<ImuLink> {
    using Parent = BaseLink;
    Eigen::Translation3d transform = Eigen::Translation3d{0.0, 0.0, 0.1};
};
}  // namespace fast_tf

namespace nuedcs::hardware {
using namespace devices;

/// Differential-drive car with firmware-controlled motors + IMU.
///
/// Protocol (see schemas/):
///   Firmware → host:  EncoderPack { encoder_id, velocity_rad_s }   — motor feedback
///   Firmware → host:  ImuPack     { accel, gyro }                  — IMU float raw
///   Firmware → host:  CanPack     { can_idx, can_id, rx_data }     — CAN passthrough RX
///   Host → firmware:  MotorCommandPack { motor_id, target_speed }  — motor control
///   Host → firmware:  EncoderConfigPack { encoder_id, lines_per_rev }
///   Host → firmware:  CanPack { can_idx, can_id, tx_data }         — CAN passthrough TX
///
/// RMCS-style two-phase update:
///   update()         — pump USB RX → store to devices → update device statuses
///   command_update() — read control inputs → send commands via USB
class Car final : public core::Component {
    static constexpr uint8_t  MOTOR_LEFT  = 0;
    static constexpr uint8_t  MOTOR_RIGHT = 1;
    static constexpr uint32_t CAN_ID_LEFT  = 0x201;
    static constexpr uint32_t CAN_ID_RIGHT = 0x202;
    static constexpr double   WHEEL_BASE   = 0.20;

public:
    explicit Car(ryml::NodeRef config)
        : command_(create_partner_component<CarCommand>(name() + "_command", *this))
        , left_encoder_(*this, *command_, "/chassis/left_encoder",  MOTOR_LEFT)
        , right_encoder_(*this, *command_, "/chassis/right_encoder", MOTOR_RIGHT)
        , left_motor_(*this, *command_, "/chassis/left_motor",   CAN_ID_LEFT)
        , right_motor_(*this, *command_, "/chassis/right_motor", CAN_ID_RIGHT)
        , imu_(Bmi088<>::Config{}.set_sample_freq(1000).set_kp(0.2))
        , slave_(*this,core::Config{config}["vid"].get<uint16_t>(0x1209),
            core::Config{config}["pid"].get<uint16_t>(0x0001))
    {
        left_motor_.configure(
            CanMotor::Config{CanMotor::Type::kM3508}
                .set_reduction_ratio(19.0).set_reversed());
        right_motor_.configure(
            CanMotor::Config{CanMotor::Type::kM3508}
                .set_reduction_ratio(19.0));

        left_encoder_.configure(EncoderMotor::Config{}.set_lines_per_rev(11));
        right_encoder_.configure(EncoderMotor::Config{}.set_lines_per_rev(11));

        register_output(name() + "/_car_sync", car_sync_out_, true);

        register_output("/chassis/velocity", chassis_velocity_, 0.0);
        register_output("/chassis/yaw_rate", chassis_yaw_rate_, 0.0);
        register_output("/imu/accel_x", accel_x_, 0.0);
        register_output("/imu/accel_y", accel_y_, 0.0);
        register_output("/imu/accel_z", accel_z_, 0.0);
        register_output("/imu/gyro_x",  gyro_x_,  0.0);
        register_output("/imu/gyro_y",  gyro_y_,  0.0);
        register_output("/imu/gyro_z",  gyro_z_,  0.0);
        register_output("/imu/q0", q0_, 1.0);
        register_output("/imu/q1", q1_, 0.0);
        register_output("/imu/q2", q2_, 0.0);
        register_output("/imu/q3", q3_, 0.0);
    }

    // ── NuedcSlave handler methods (dispatched by pump(), if constexpr) ──

    void handle_imu(float ax, float ay, float az, float gx, float gy, float gz) {
        imu_.store_sample(static_cast<int16_t>(ax), static_cast<int16_t>(ay),
                          static_cast<int16_t>(az),
                          static_cast<int16_t>(gx), static_cast<int16_t>(gy),
                          static_cast<int16_t>(gz));
    }

    void handle_encoder(uint8_t id, float velocity) {
        if (id == MOTOR_LEFT)  left_encoder_.store_velocity(velocity);
        if (id == MOTOR_RIGHT) right_encoder_.store_velocity(velocity);
    }

    Car(const Car&) = delete;
    Car& operator=(const Car&) = delete;

    bool init() override {
        slave_.start();
        if (slave_.connected())
            info("car initialized, USB started");
        else
            warn("car initialized — USB offline, no device communication");
        return true;
    }

    ~Car() override { slave_.stop(); }

    // ── Sensor update ────────────────────────────────────────────────

    void update() override {
        slave_.pump();  // dispatch USB frames → device store_xxx

        left_encoder_.update_status();
        right_encoder_.update_status();
        left_motor_.update_status();
        right_motor_.update_status();
        imu_.update_status();

        // Chassis kinematics from firmware-computed encoder velocities
        float vl = left_encoder_.velocity();
        float vr = right_encoder_.velocity();
        *chassis_velocity_ = (vl + vr) / 2.0;
        *chassis_yaw_rate_ = (vr - vl) / WHEEL_BASE;

        // ── TF tree: orientation from IMU AHRS quaternion ────────────
        Eigen::Isometry3d odom_to_base = Eigen::Isometry3d::Identity();
        odom_to_base.linear() = Eigen::Quaterniond{
            imu_.q0(), imu_.q1(), imu_.q2(), imu_.q3()}.toRotationMatrix();
        tf_tree_.template get_joint<OdomLink, BaseLink>().transform = odom_to_base;

        // IMU outputs
        *accel_x_ = imu_.ax(); *accel_y_ = imu_.ay(); *accel_z_ = imu_.az();
        *gyro_x_  = imu_.gx(); *gyro_y_  = imu_.gy(); *gyro_z_  = imu_.gz();
        *q0_ = imu_.q0(); *q1_ = imu_.q1(); *q2_ = imu_.q2(); *q3_ = imu_.q3();
    }

private:
    // ── Partner: command phase ───────────────────────────────────────

    class CarCommand final : public core::Component {
    public:
        explicit CarCommand(Car& car)
            : car_(car) {
            register_input(car.name() + "/_car_sync", car_sync_in_);
        }
        void update() override { car_.command_update(); }
    private:
        Car& car_;
        InputInterface<bool> car_sync_in_;
    };

    /// Generate commands from controllers → send via USB. Self-throttled.
    void command_update() {
        if (!cmd_throttle_.ready()) return;

        send_encoder_motor(left_encoder_);
        send_encoder_motor(right_encoder_);
        send_can_motor(left_motor_);
        send_can_motor(right_motor_);
        send_encoder_config(left_encoder_);
        send_encoder_config(right_encoder_);
    }

    void send_encoder_motor(EncoderMotor& m) {
        float target = m.generate_command();
        if (target != 0.0f)
            slave_.set_motor_speed(m.motor_id(), target);
    }

    void send_can_motor(CanMotor& m) {
        uint64_t cmd = m.generate_command();
        if (cmd)
            slave_.send_can(0, m.can_id(), 8,
                            reinterpret_cast<const uint8_t*>(&cmd));
    }

    void send_encoder_config(EncoderMotor& m) {
        if (m.config_pending()) {
            slave_.set_encoder_config(m.motor_id(), m.lines_per_rev());
            m.clear_config_pending();
        }
    }

    // ── Devices ──────────────────────────────────────────────────────

    CarCommand*  command_;
    fast_tf::JointCollection<BaseLink, ImuLink> tf_tree_;
    EncoderMotor left_encoder_;
    EncoderMotor right_encoder_;
    CanMotor     left_motor_;
    CanMotor     right_motor_;
    Bmi088<>           imu_;
    nuedc::NuedcSlave<Car> slave_;
    nuedc::Throttle cmd_throttle_{1000};  // max 2000Hz command rate

    // ── Outputs ──────────────────────────────────────────────────────

    OutputInterface<bool>   car_sync_out_;
    OutputInterface<double> chassis_velocity_;
    OutputInterface<double> chassis_yaw_rate_;
    OutputInterface<double> accel_x_, accel_y_, accel_z_;
    OutputInterface<double> gyro_x_, gyro_y_, gyro_z_;
    OutputInterface<double> q0_, q1_, q2_, q3_;
};

}  // namespace nuedcs::hardware
