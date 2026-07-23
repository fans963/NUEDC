#pragma once

#include "core/component.hpp"
#include "devices/gimbal_serial.hpp"
#include "util/throttle.hpp"

#include <thread>

namespace nuedcs::controller::gimbal {

class GimbalController final : public core::Component {
public:
    explicit GimbalController(ryml::NodeRef config) {
        auto c = core::Config { config };

        port_            = c["port"].str("/dev/ttyUSB0");
        baudrate_        = c["baudrate"].get(115200);
        int cmd_hz       = c["cmd_hz"].get(1000);
        int stat_hz      = c["status_hz"].get(1000);
        cmd_throttle_    = nuedc::Throttle { static_cast<double>(cmd_hz) };
        status_throttle_ = nuedc::Throttle { static_cast<double>(stat_hz) };

        register_input("/gimbal/control_mode", in_control_mode_, false);
        register_input("/gimbal/yaw_cmd", in_yaw_cmd_, false);
        register_input("/gimbal/pitch_cmd", in_pitch_cmd_, false);
        register_input("/gimbal/enable", in_enable_, false);
        register_input("/gimbal/stability", in_stability_, false);
        register_input("/gimbal/laser", in_laser_, false);
        register_input("/gimbal/reset_imu", in_reset_imu_, false);

        register_output("/gimbal/enabled", out_enabled_, 0.0);
        register_output("/gimbal/stability_enabled", out_stability_enabled_, 0.0);
        register_output("/gimbal/laser_enabled", out_laser_enabled_, 0.0);
        register_output("/gimbal/imu_speed_yaw", out_imu_speed_yaw_, 0.0);
        register_output("/gimbal/imu_speed_pitch", out_imu_speed_pitch_, 0.0);
        register_output("/gimbal/imu_angle_yaw", out_imu_angle_yaw_, 0.0);
        register_output("/gimbal/imu_angle_pitch", out_imu_angle_pitch_, 0.0);
        register_output("/gimbal/angle_yaw", out_angle_yaw_, 0.0);
        register_output("/gimbal/angle_pitch", out_angle_pitch_, 0.0);
        register_output("/gimbal/speed_yaw", out_speed_yaw_, 0.0);
        register_output("/gimbal/speed_pitch", out_speed_pitch_, 0.0);
        register_output("/gimbal/current_yaw", out_current_yaw_, 0.0);
        register_output("/gimbal/current_pitch", out_current_pitch_, 0.0);
    }

    ~GimbalController() override {
        if (device_.is_open()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            device_.send_command(devices::GimbalCmd::Disable);
            device_.drain_tx();
        }
        device_.close();
    }

    bool init() override {
        if (!device_.open(port_, baudrate_)) {
            error("Failed to open serial port: {} @ {} baud", port_, baudrate_);
            return false;
        }
        info("Serial port opened: {} @ {} baud", port_, baudrate_);
        return true;
    }

    void update() override {
        cycle_++;

        // ── Edge‑triggered commands ──────────────────────────────────
        // Read input value WITHOUT overwriting prev_* before edge check

        // Enable
        double en = in_enable_.ready() ? *in_enable_ : prev_enable_;
        if (prev_enable_ < 0.5 && en >= 0.5) {
            if (device_.send_command(devices::GimbalCmd::Enable)) prev_enable_ = en;
        } else if (prev_enable_ >= 0.5 && en < 0.5) {
            if (device_.send_command(devices::GimbalCmd::Disable)) prev_enable_ = en;
        } else {
            prev_enable_ = en;
        }

        // Stability
        double st = in_stability_.ready() ? *in_stability_ : prev_stability_;
        if (prev_stability_ < 0.5 && st >= 0.5) {
            if (device_.send_command(devices::GimbalCmd::EnableStability)) prev_stability_ = st;
        } else if (prev_stability_ >= 0.5 && st < 0.5) {
            if (device_.send_command(devices::GimbalCmd::DisableStability)) prev_stability_ = st;
        } else {
            prev_stability_ = st;
        }

        // Laser
        double la = in_laser_.ready() ? *in_laser_ : prev_laser_;
        if (prev_laser_ < 0.5 && la >= 0.5) {
            if (device_.send_command(devices::GimbalCmd::EnableLaser)) prev_laser_ = la;
        } else if (prev_laser_ >= 0.5 && la < 0.5) {
            if (device_.send_command(devices::GimbalCmd::DisableLaser)) prev_laser_ = la;
        } else {
            prev_laser_ = la;
        }

        // Reset IMU
        double ri = in_reset_imu_.ready() ? *in_reset_imu_ : prev_reset_imu_;
        if (prev_reset_imu_ < 0.5 && ri >= 0.5) {
            if (device_.send_command(devices::GimbalCmd::ResetIMU)) prev_reset_imu_ = ri;
        } else {
            prev_reset_imu_ = ri;
        }

        // ── Throttled control command ─────────────────────────────────
        if (cmd_throttle_.ready()) {
            int mode = static_cast<int>(devices::GimbalCmd::SpeedCtrl);
            if (in_control_mode_.ready()) mode = static_cast<int>(*in_control_mode_);

            double yaw   = in_yaw_cmd_.ready() ? *in_yaw_cmd_ : yaw_cmd_;
            double pitch = in_pitch_cmd_.ready() ? *in_pitch_cmd_ : pitch_cmd_;
            yaw_cmd_     = yaw;
            pitch_cmd_   = pitch;

            device_.send_command(static_cast<devices::GimbalCmd>(mode), static_cast<float>(yaw),
                static_cast<float>(pitch));
        }

        // ── Throttled status request ──────────────────────────────────
        if (status_throttle_.ready()) {
            device_.request_status();
        }

        // ── Publish latest status ─────────────────────────────────────
        auto status = device_.get_latest_status();
        if (!status) return;

        *out_enabled_           = status->enabled ? 1.0 : 0.0;
        *out_stability_enabled_ = status->stability_enabled ? 1.0 : 0.0;
        *out_laser_enabled_     = status->laser_enabled ? 1.0 : 0.0;
        *out_imu_speed_yaw_     = static_cast<double>(status->imu_speed_yaw);
        *out_imu_speed_pitch_   = static_cast<double>(status->imu_speed_pitch);
        *out_imu_angle_yaw_     = static_cast<double>(status->imu_angle_yaw);
        *out_imu_angle_pitch_   = static_cast<double>(status->imu_angle_pitch);
        *out_angle_yaw_         = static_cast<double>(status->angle_yaw);
        *out_angle_pitch_       = static_cast<double>(status->angle_pitch);
        *out_speed_yaw_         = static_cast<double>(status->speed_yaw);
        *out_speed_pitch_       = static_cast<double>(status->speed_pitch);
        *out_current_yaw_       = static_cast<double>(status->current_yaw);
        *out_current_pitch_     = static_cast<double>(status->current_pitch);
    }

private:
    std::string port_ = "/dev/ttyUSB0";
    int baudrate_     = 115200;
    devices::GimbalSerialDevice device_;

    nuedc::Throttle cmd_throttle_ { 1000 };
    nuedc::Throttle status_throttle_ { 1000 };

    double yaw_cmd_   = 0.0;
    double pitch_cmd_ = 0.0;

    double prev_enable_    = 0.0;
    double prev_stability_ = 0.0;
    double prev_laser_     = 0.0;
    double prev_reset_imu_ = 0.0;
    int cycle_             = 0;

    InputInterface<int> in_control_mode_;
    InputInterface<double> in_yaw_cmd_;
    InputInterface<double> in_pitch_cmd_;
    InputInterface<double> in_enable_;
    InputInterface<double> in_stability_;
    InputInterface<double> in_laser_;
    InputInterface<double> in_reset_imu_;

    OutputInterface<double> out_enabled_;
    OutputInterface<double> out_stability_enabled_;
    OutputInterface<double> out_laser_enabled_;
    OutputInterface<double> out_imu_speed_yaw_;
    OutputInterface<double> out_imu_speed_pitch_;
    OutputInterface<double> out_imu_angle_yaw_;
    OutputInterface<double> out_imu_angle_pitch_;
    OutputInterface<double> out_angle_yaw_;
    OutputInterface<double> out_angle_pitch_;
    OutputInterface<double> out_speed_yaw_;
    OutputInterface<double> out_speed_pitch_;
    OutputInterface<double> out_current_yaw_;
    OutputInterface<double> out_current_pitch_;
};

} // namespace nuedcs::controller::gimbal
