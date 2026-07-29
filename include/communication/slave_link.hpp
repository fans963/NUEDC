#pragma once
/// Host-side USB CDC ↔ MCU link over serial port.
///
/// Protocol: 0x5A | len_lo | len_hi | FlatBuffer payload | 0xA5
/// Payload types:
///   TX (host→slave): Protocol::HostToSlave::SpeedCommand  { left, right }
///   RX (slave→host): Protocol::SlaveToHost::SpeedTelemetry { left, right }
///
/// Usage (YAML config):
///   SlaveLink -> slave_link:
///     port: "/dev/ttyACM0"
///     baudrate: 115200   # optional, USB CDC ignores this
///     tx_interval_ms: 5  # telemetry send interval

#include "CSerialPort/SerialPort.h"
#include "core/component.hpp"
#include "host_to_slave_generated.h"
#include "slave_to_host_generated.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace nuedcs::communication {

// ── Protocol layer (same 0x5A/0xA5 framing as MCU side) ─────────────────────

inline constexpr uint8_t  kFrameHeader = 0x5A;
inline constexpr uint8_t  kFrameFooter = 0xA5;
inline constexpr uint16_t kMaxPayload  = 512;
inline constexpr size_t   kOverhead    = 4;  // header(1) + len(2) + footer(1)

class FrameDecoder {
public:
    struct Packet {
        std::vector<uint8_t> payload;
    };

    [[nodiscard]] std::optional<Packet> feed(uint8_t byte) noexcept {
        switch (state_) {
        case State::Header:
            if (byte == kFrameHeader) { state_ = State::LenLo; }
            return std::nullopt;
        case State::LenLo:
            len_   = byte;
            state_ = State::LenHi;
            return std::nullopt;
        case State::LenHi:
            len_ |= static_cast<uint16_t>(byte) << 8;
            if (len_ == 0 || len_ > kMaxPayload) { reset(); return std::nullopt; }
            buf_.resize(len_);
            pos_   = 0;
            state_ = State::Payload;
            return std::nullopt;
        case State::Payload:
            buf_[pos_++] = byte;
            if (pos_ >= len_) { state_ = State::Footer; }
            return std::nullopt;
        case State::Footer:
            if (byte == kFrameFooter) {
                Packet pkt{std::move(buf_)};
                reset();
                return pkt;
            }
            reset();
            return std::nullopt;
        }
        return std::nullopt;
    }

    void reset() noexcept { state_ = State::Header; len_ = 0; pos_ = 0; }

private:
    enum class State : uint8_t { Header, LenLo, LenHi, Payload, Footer };
    State                state_{State::Header};
    uint16_t             len_{0};
    size_t               pos_{0};
    std::vector<uint8_t> buf_;
};

/// Encode a FlatBuffer into a framed packet ready for serial write.
template <size_t BufSize = 256>
struct FrameEncoder {
    std::array<uint8_t, BufSize> buf{};
    size_t                       len{0};

    [[nodiscard]] std::span<const uint8_t> encode(
        flatbuffers::FlatBufferBuilder& fbb) noexcept {
        auto* data = fbb.GetBufferPointer();
        auto  size = fbb.GetSize();
        if (size > kMaxPayload) [[unlikely]] return {};

        buf[0]        = kFrameHeader;
        buf[1]        = static_cast<uint8_t>(size & 0xFF);
        buf[2]        = static_cast<uint8_t>((size >> 8) & 0xFF);
        std::memcpy(&buf[3], data, size);
        buf[3 + size] = kFrameFooter;
        len           = 3 + size + 1;
        return {buf.data(), len};
    }
};

// ── SlaveLink Component ──────────────────────────────────────────────────────

class SlaveLink : public core::Component {
public:
    explicit SlaveLink(ryml::NodeRef config) {
        auto cfg = core::Config{config};
        port_       = cfg["port"].str("/dev/ttyACM0");
        baudrate_   = cfg["baudrate"].get<int>(115200);
        tx_interval_ = std::chrono::milliseconds(cfg["tx_interval_ms"].get<int>(5));
    }

    bool init() override {
        serial_.init(port_.c_str(), baudrate_);
        serial_.setOperateMode(itas109::SynchronousOperate);
        serial_.setReadIntervalTimeout(5);
        if (!serial_.open()) {
            error("Failed to open serial: {}", port_);
            return false;
        }
        info("Serial opened: {} @ {} baud", port_, baudrate_);

        running_ = true;
        rx_thread_ = std::thread(&SlaveLink::rx_loop, this);
        return true;
    }

    void update() override {
        // TX: send SpeedCommand periodically
        // auto now = std::chrono::steady_clock::now();
        // if (now - last_tx_ < tx_interval_) return;
        // last_tx_ = now;

        float left  = cmd_left_.load(std::memory_order_relaxed);
        float right = cmd_right_.load(std::memory_order_relaxed);

        flatbuffers::FlatBufferBuilder fbb(64);
        auto root = Protocol::HostToSlave::CreateSpeedCommand(fbb, left, right);
        fbb.Finish(root);

        auto frame = encoder_.encode(fbb);
        if (frame.empty()) return;
        serial_.writeData(frame.data(), static_cast<int>(frame.size()));

        // Hex dump @ 5 Hz
        if (++tx_count_ % 40 == 0) {
            std::string hex;
            auto pl = frame.subspan(3, frame.size() - 4);
            for (size_t i = 0; i < pl.size(); ++i) {
                char b[4];
                std::snprintf(b, sizeof(b), "%02X ", pl[i]);
                hex += b;
            }
            info("TX frame={}B  payload={}B  [{}]  → L={:+.3f} R={:+.3f}",
                frame.size(), pl.size(), hex, left, right);
        }
    }

    ~SlaveLink() override {
        running_ = false;
        if (rx_thread_.joinable()) rx_thread_.join();
        serial_.close();
    }

    // ── Public API for other components ──────────────────────────────────

    /// Set the target speed to send to the MCU (called by controller component).
    void set_speed(float left, float right) noexcept {
        cmd_left_.store(left, std::memory_order_relaxed);
        cmd_right_.store(right, std::memory_order_relaxed);
    }

    /// Get the latest received telemetry from the MCU.
    [[nodiscard]] float telemetry_left() const noexcept {
        return telem_left_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] float telemetry_right() const noexcept {
        return telem_right_.load(std::memory_order_relaxed);
    }

    /// Copy latest telemetry into output references.
    void get_telemetry(float& left, float& right) const noexcept {
        left  = telem_left_.load(std::memory_order_relaxed);
        right = telem_right_.load(std::memory_order_relaxed);
    }

private:
    void rx_loop() noexcept {
        FrameDecoder decoder;
        uint8_t tmp[256];

        while (running_.load(std::memory_order_relaxed)) {
            int n = serial_.readData(tmp, sizeof(tmp));
            if (n <= 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
                continue;
            }
            for (int i = 0; i < n; ++i) {
                auto pkt = decoder.feed(tmp[i]);
                if (!pkt.has_value()) continue;

                auto* telem = flatbuffers::GetRoot<Protocol::SlaveToHost::SpeedTelemetry>(
                    pkt->payload.data());
                if (!telem) {
                    warn("RX: invalid FlatBuffer");
                    continue;
                }
                telem_left_.store(telem->left(), std::memory_order_relaxed);
                telem_right_.store(telem->right(), std::memory_order_relaxed);

                // Hex dump @ 5 Hz
                if (++rx_count_ % 40 == 0) {
                    std::string hex;
                    for (size_t j = 0; j < pkt->payload.size(); ++j) {
                        char b[4];
                        std::snprintf(b, sizeof(b), "%02X ", pkt->payload[j]);
                        hex += b;
                    }
                    info("RX frame={}B  payload={}B  [{}]  → L={:+.3f} R={:+.3f}",
                        pkt->payload.size() + 4, pkt->payload.size(),
                        hex, telem->left(), telem->right());
                }
            }
        }
    }

    // ── Config ───────────────────────────────────────────────────────────
    std::string              port_;
    int                      baudrate_;
    std::chrono::milliseconds tx_interval_{5};

    // ── Hardware ─────────────────────────────────────────────────────────
    itas109::CSerialPort     serial_;
    FrameEncoder<256>        encoder_;

    // ── State ────────────────────────────────────────────────────────────
    std::atomic<bool>        running_{false};
    std::thread              rx_thread_;
    std::chrono::steady_clock::time_point last_tx_{};

    // ── Data (lock-free atomics) ─────────────────────────────────────────
    std::atomic<float>       cmd_left_{0.0f};
    std::atomic<float>       cmd_right_{0.0f};
    std::atomic<float>       telem_left_{0.0f};
    std::atomic<float>       telem_right_{0.0f};
    int                      rx_count_{0};
    int                      tx_count_{0};
};

} // namespace nuedcs::communication
