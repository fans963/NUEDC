#pragma once

#include "CSerialPort/SerialPort.h"

#include <re2/re2.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

namespace nuedcs::devices {

// ── Types ────────────────────────────────────────────────────────────────

enum class GimbalCmd : uint8_t {
    Enable           = 0x01,
    Disable          = 0x02,
    CurrentCtrl      = 0x03,
    SpeedCtrl        = 0x04,
    AngleCtrl        = 0x05,
    LowSpeedCtrl     = 0x06,
    StepAngleCtrl    = 0x07,
    EnableStability  = 0xFF,
    DisableStability = 0xFE,
    EnableLaser      = 0xFD,
    DisableLaser     = 0xFC,
    ResetIMU         = 0xFB,
};

struct GimbalStatus {
    bool  enabled           = false;
    bool  stability_enabled = false;
    bool  laser_enabled     = false;
    float imu_speed_yaw     = 0.0f;
    float imu_speed_pitch   = 0.0f;
    float imu_angle_yaw     = 0.0f;
    float imu_angle_pitch   = 0.0f;
    float angle_yaw         = 0.0f;
    float angle_pitch       = 0.0f;
    float speed_yaw         = 0.0f;
    float speed_pitch       = 0.0f;
    float current_yaw       = 0.0f;
    float current_pitch     = 0.0f;
};

// ── Device ───────────────────────────────────────────────────────────────

class GimbalSerialDevice {
public:
    GimbalSerialDevice()  = default;
    ~GimbalSerialDevice() { close(); }

    GimbalSerialDevice(const GimbalSerialDevice&)            = delete;
    GimbalSerialDevice& operator=(const GimbalSerialDevice&) = delete;

    bool open(const std::string& port, int baudrate) {
        serial_.init(port.c_str(), baudrate);
        serial_.setOperateMode(itas109::SynchronousOperate);
        serial_.setReadIntervalTimeout(5);
        if (!serial_.open()) return false;
        opened_ = true;
        rx_running_.store(true, std::memory_order_relaxed);
        rx_thread_ = std::thread(&GimbalSerialDevice::rx_thread_func, this);
        return true;
    }

    void close() {
        if (!opened_) return;
        rx_running_.store(false, std::memory_order_relaxed);
        if (rx_thread_.joinable()) rx_thread_.join();
        serial_.close();
        opened_ = false;
    }

    [[nodiscard]] bool is_open() const { return opened_; }

    bool send_command(GimbalCmd cmd, float data1 = 0.0f, float data2 = 0.0f) {
        if (!opened_) return false;
        auto text = cmd_to_text(cmd, data1, data2);
        if (text.empty()) return false;
        text += "\r\n";
        int n = serial_.writeData(text.data(), static_cast<int>(text.size()));
        return n == static_cast<int>(text.size());
    }

    void request_status() {
        if (!opened_) return;
        const char* c = "status\r\n";
        serial_.writeData(c, static_cast<int>(std::strlen(c)));
    }

    void drain_tx() {
        if (!opened_) return;
        serial_.flushWriteBuffers();
    }

    std::optional<GimbalStatus> get_latest_status() {
        std::lock_guard<std::mutex> lock(status_mutex_);
        if (!status_valid_) return std::nullopt;
        return latest_status_;
    }

private:
    // ── RE2 patterns (compiled once) ────────────────────────────────────
    static const re2::RE2& re_enabled()    { static const re2::RE2 r(R"(Enabled\s*:\s*(Yes|No))"); return r; }
    static const re2::RE2& re_stability()  { static const re2::RE2 r(R"(Stability\s+Enabled\s*:\s*(Yes|No))"); return r; }
    static const re2::RE2& re_laser()      { static const re2::RE2 r(R"(Laser\s+Enabled\s*:\s*(Yes|No))"); return r; }
    static const re2::RE2& re_imu_speed()  { static const re2::RE2 r(R"(IMU\s+Speed\s*:\s*yaw:\s*([-\d.]+)\s*rpm,\s*pitch:\s*([-\d.]+))"); return r; }
    static const re2::RE2& re_imu_angle()  { static const re2::RE2 r(R"(IMU\s+Angle\s*:\s*yaw:\s*([-\d.]+)\s*rad,\s*pitch:\s*([-\d.]+))"); return r; }
    static const re2::RE2& re_angle()      { static const re2::RE2 r(R"(^\s*Angle\s*:\s*yaw:\s*([-\d.]+)\s*rad,\s*pitch:\s*([-\d.]+))"); return r; }
    static const re2::RE2& re_speed()      { static const re2::RE2 r(R"(^\s*Speed\s*:\s*yaw:\s*([-\d.]+)\s*rpm,\s*pitch:\s*([-\d.]+))"); return r; }
    static const re2::RE2& re_current()    { static const re2::RE2 r(R"(Current\s*:\s*yaw:\s*([-\d.]+)\s*A\s*,\s*pitch:\s*([-\d.]+))"); return r; }

    // ── Helpers ────────────────────────────────────────────────────────
    static std::string cmd_to_text(GimbalCmd cmd, float yaw, float pitch) {
        char yb[32], pb[32];
        auto tc = [](char* b, float v) {
            auto r = std::to_chars(b, b + 31, v, std::chars_format::fixed, 3);
            *r.ptr = '\0';
        };
        tc(yb, yaw); tc(pb, pitch);
        switch (cmd) {
            case GimbalCmd::Enable:           return "enable";
            case GimbalCmd::Disable:          return "disable";
            case GimbalCmd::CurrentCtrl:      return std::string("ctrl current ")  + yb + " " + pb;
            case GimbalCmd::SpeedCtrl:        return std::string("ctrl speed ")    + yb + " " + pb;
            case GimbalCmd::AngleCtrl:        return std::string("ctrl angle ")    + yb + " " + pb;
            case GimbalCmd::LowSpeedCtrl:     return std::string("ctrl low_speed ") + yb + " " + pb;
            case GimbalCmd::StepAngleCtrl:    return std::string("ctrl step_angle ") + yb + " " + pb;
            case GimbalCmd::EnableStability:  return "enable_stability";
            case GimbalCmd::DisableStability: return "disable_stability";
            case GimbalCmd::EnableLaser:      return "enable_laser";
            case GimbalCmd::DisableLaser:     return "disable_laser";
            case GimbalCmd::ResetIMU:         return "config zero_pos --imu";
        }
        return "";
    }

    bool parse_status_block(const std::string& text, GimbalStatus& out) {
        if (text.find("Gimbal Status:") == std::string::npos) return false;
        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::string s1, s2;
            float f1 = 0, f2 = 0;
            if (re2::RE2::PartialMatch(line, re_enabled(), &s1))
                out.enabled = (s1 == "Yes");
            else if (re2::RE2::PartialMatch(line, re_stability(), &s1))
                out.stability_enabled = (s1 == "Yes");
            else if (re2::RE2::PartialMatch(line, re_laser(), &s1))
                out.laser_enabled = (s1 == "Yes");
            else if (re2::RE2::PartialMatch(line, re_imu_speed(), &f1, &f2))
                { out.imu_speed_yaw = f1; out.imu_speed_pitch = f2; }
            else if (re2::RE2::PartialMatch(line, re_imu_angle(), &f1, &f2))
                { out.imu_angle_yaw = f1; out.imu_angle_pitch = f2; }
            else if (re2::RE2::PartialMatch(line, re_angle(), &f1, &f2))
                { out.angle_yaw = f1; out.angle_pitch = f2; }
            else if (re2::RE2::PartialMatch(line, re_speed(), &f1, &f2))
                { out.speed_yaw = f1; out.speed_pitch = f2; }
            else if (re2::RE2::PartialMatch(line, re_current(), &f1, &f2))
                { out.current_yaw = f1; out.current_pitch = f2; }
        }
        return true;
    }

    void rx_thread_func() {
        char tmp[256];
        while (rx_running_.load(std::memory_order_relaxed)) {
            int n = serial_.readData(tmp, sizeof(tmp) - 1);
            if (n > 0) {
                tmp[n] = '\0';
                rx_buf_.append(tmp, n);
                auto pos = rx_buf_.find("Gimbal Status:");
                if (pos != std::string::npos) {
                    auto end = rx_buf_.find("\n->", pos);
                    if (end == std::string::npos) end = rx_buf_.find("\r\n->", pos);
                    std::string block;
                    if (end != std::string::npos) {
                        block = rx_buf_.substr(pos, end - pos);
                        rx_buf_.erase(0, end);
                    } else if (rx_buf_.size() > 2000) {
                        block = rx_buf_.substr(pos);
                        rx_buf_.clear();
                    }
                    if (!block.empty()) {
                        GimbalStatus s;
                        if (parse_status_block(block, s)) {
                            std::lock_guard<std::mutex> lock(status_mutex_);
                            latest_status_ = s;
                            status_valid_  = true;
                        }
                    }
                }
                if (rx_buf_.size() > 4096) rx_buf_.clear();
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        }
    }

    itas109::CSerialPort serial_;
    bool                 opened_ = false;
    std::thread          rx_thread_;
    std::atomic<bool>    rx_running_{false};
    std::string          rx_buf_;
    std::mutex           status_mutex_;
    GimbalStatus         latest_status_;
    bool                 status_valid_ = false;
};

} // namespace nuedcs::devices
