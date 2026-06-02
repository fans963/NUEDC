#pragma once

// FlatBuffers protocol over async USB transport.
//
// Template on Handler type — compile-time dispatch, guaranteed inlining.
// Handler must provide any subset of:
//   handle_imu(float ax, ay, az, gx, gy, gz)
//   handle_encoder(uint8_t id, float velocity)
//   handle_adc(uint8_t idx, const uint16_t* channels, size_t count)
//   handle_can_rx(uint8_t idx, uint32_t id, uint8_t dlc, bool ext, bool rtr, const uint8_t* data)
//   handle_uart_rx(uint8_t idx, const uint8_t* data, size_t len)
//
// Missing handlers are silently skipped (if constexpr).

#ifndef NUEDC_HAS_LIBUSB
#define NUEDC_HAS_LIBUSB 0
#endif

#if NUEDC_HAS_LIBUSB

#include "usb/protocol.hpp"
#include "usb/usb_transport.hpp"

#include <flatbuffers/flatbuffers.h>
#include <host_to_slave_generated.h>
#include <slave_to_host_generated.h>

#include <spdlog/spdlog.h>

#include <cstdint>
#include <cstring>

namespace nuedc {

/// Concept checked at dispatch site (if constexpr), not here —
/// Handler may be incomplete at member declaration point (e.g. Car inside Car).
template <typename Handler>
class NuedcSlave {
    static constexpr size_t RX_RING_SIZE = 32;

    struct RxFrame {
        uint8_t data[4 + protocol::MAX_FRAME_LEN];
        size_t  len = 0;
    };

public:
    explicit NuedcSlave(Handler& handler, uint16_t vid = 0x1209, uint16_t pid = 0x0001)
        : handler_(handler)
        , transport_(vid, pid)
        , decoder_(*this)
    {
        transport_.set_on_receive([this](const uint8_t* data, size_t len) {
            decoder_.feed(data, len);
        });
    }

    [[nodiscard]] bool connected() const { return transport_.connected(); }
    void start() { transport_.start(); }
    void stop()  { transport_.stop(); }

    /// Drain received frames, dispatch to handler. Handler passed by ref → inlined.
    void pump() {
        RxFrame f;
        while (rx_ring_.pop(f))
            dispatch(f.data, f.len);
    }

    // ── Async send (thread-safe, non-blocking) ─────────────────────────

    bool set_motor_speed(uint8_t motor_id, float target_speed) {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto cmd = Protocol::HostToSlave::CreateMotorCommandPack(fbb, motor_id, target_speed);
        auto frame = Protocol::HostToSlave::CreateHostToSlaveFrame(
            fbb, Protocol::HostToSlave::MsgPayload::MotorCommandPack, cmd.Union());
        return finish_and_send(fbb, frame);
    }

    bool set_pid(uint8_t id, float kp, float ki, float kd,
                 float out_min = -1000.f, float out_max = 1000.f,
                 float i_min = -500.f, float i_max = 500.f, bool reset = false) {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto pid = Protocol::HostToSlave::CreatePidConfigPack(
            fbb, id, kp, ki, kd, out_min, out_max, i_min, i_max, reset);
        auto frame = Protocol::HostToSlave::CreateHostToSlaveFrame(
            fbb, Protocol::HostToSlave::MsgPayload::PidConfigPack, pid.Union());
        return finish_and_send(fbb, frame);
    }

    bool send_can(uint8_t idx, uint32_t id, uint8_t dlc, const uint8_t* data,
                  bool extended = false, bool rtr = false) {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto d = fbb.CreateVector(data, dlc);
        auto can = Protocol::HostToSlave::CreateCanPack(fbb, idx, id, dlc, extended, rtr, d);
        auto frame = Protocol::HostToSlave::CreateHostToSlaveFrame(
            fbb, Protocol::HostToSlave::MsgPayload::CanPack, can.Union());
        return finish_and_send(fbb, frame);
    }

    bool send_uart(uint8_t idx, const uint8_t* data, size_t len) {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto d = fbb.CreateVector(data, len);
        auto uart = Protocol::HostToSlave::CreateUartPack(fbb, idx, d);
        auto frame = Protocol::HostToSlave::CreateHostToSlaveFrame(
            fbb, Protocol::HostToSlave::MsgPayload::UartPack, uart.Union());
        return finish_and_send(fbb, frame);
    }

    bool set_encoder_config(uint8_t id, uint16_t lines_per_rev) {
        flatbuffers::FlatBufferBuilder fbb(64);
        auto enc = Protocol::HostToSlave::CreateEncoderConfigPack(fbb, id, lines_per_rev);
        auto frame = Protocol::HostToSlave::CreateHostToSlaveFrame(
            fbb, Protocol::HostToSlave::MsgPayload::EncoderConfigPack, enc.Union());
        return finish_and_send(fbb, frame);
    }

private:
    // ── Framing + send ─────────────────────────────────────────────────

    template <typename T>
    bool finish_and_send(flatbuffers::FlatBufferBuilder& fbb,
                         flatbuffers::Offset<T> frame) {
        fbb.FinishSizePrefixed(frame);

        uint8_t wire[protocol::MAX_FRAME_LEN + 6];
        size_t  wpos = 0;
        struct {
            uint8_t* buf; size_t& pos;
            void write(const uint8_t* data, size_t len) {
                std::memcpy(buf + pos, data, len); pos += len;
            }
        } writer{wire, wpos};

        protocol::protocol_encode(writer, fbb.GetBufferPointer(), fbb.GetSize());
        return transport_.send(wire, wpos);
    }

    // ── Protocol decoder callback (on USB bg thread) ───────────────────

    friend class protocol::ProtocolDecoder<NuedcSlave>;
    void on_frame(const uint8_t* data, size_t len) {
        RxFrame f;
        if (len > sizeof(f.data)) return;
        std::memcpy(f.data, data, len);
        f.len = len;
        if (!rx_ring_.push(f))
            spdlog::warn("[NuedcSlave] RX ring full, frame dropped");
    }

    // ── Frame dispatch (main thread, Handler by ref) ───────────────────

    void dispatch(const uint8_t* data, size_t len) {
        flatbuffers::Verifier v(data, len);
        if (!v.VerifySizePrefixedBuffer<Protocol::SlaveToHost::SlaveToHostFrame>(nullptr))
            return;

        auto frame = flatbuffers::GetSizePrefixedRoot<
            Protocol::SlaveToHost::SlaveToHostFrame>(data);
        if (!frame || !frame->payload()) return;

        switch (frame->payload_type()) {
        case Protocol::SlaveToHost::MsgPayload::ImuPack:
            if (auto* p = frame->payload_as_ImuPack(); p)
                handle_imu(p);
            break;
        case Protocol::SlaveToHost::MsgPayload::EncoderPack:
            if (auto* p = frame->payload_as_EncoderPack(); p)
                handle_encoder(p);
            break;
        case Protocol::SlaveToHost::MsgPayload::AdcPack:
            if (auto* p = frame->payload_as_AdcPack(); p)
                handle_adc(p);
            break;
        case Protocol::SlaveToHost::MsgPayload::CanPack:
            if (auto* p = frame->payload_as_CanPack(); p)
                handle_can_rx(p);
            break;
        case Protocol::SlaveToHost::MsgPayload::UartPack:
            if (auto* p = frame->payload_as_UartPack(); p)
                handle_uart_rx(p);
            break;
        default: break;
        }
    }

    // ── Handler dispatch: h.method(args) — direct call, fully inlined ──

    void handle_imu(auto* p) {
        if constexpr (requires(Handler& h, float ax, float ay, float az,
                               float gx, float gy, float gz) {
                h.handle_imu(ax, ay, az, gx, gy, gz); })
            handler_.handle_imu(
                p->accel_x(), p->accel_y(), p->accel_z(),
                p->gyro_x(),  p->gyro_y(),  p->gyro_z());
    }

    void handle_encoder(auto* p) {
        if constexpr (requires(Handler& h, uint8_t id, float v) {
                h.handle_encoder(id, v); })
            handler_.handle_encoder(p->encoder_id(), p->velocity_rad_s());
    }

    void handle_adc(auto* p) {
        if constexpr (requires(Handler& h, uint8_t idx, const uint16_t* ch, size_t n) {
                h.handle_adc(idx, ch, n); })
            if (p->channels())
                handler_.handle_adc(p->adc_idx(), p->channels()->data(), p->channels()->size());
    }

    void handle_can_rx(auto* p) {
        if constexpr (requires(Handler& h, uint8_t idx, uint32_t id, uint8_t dlc,
                               bool ext, bool rtr, const uint8_t* d) {
                h.handle_can_rx(idx, id, dlc, ext, rtr, d); })
            if (p->rx_data())
                handler_.handle_can_rx(
                    p->can_idx(), p->can_id(), p->can_dlc(),
                    p->is_extended(), p->is_rtr(), p->rx_data()->data());
    }

    void handle_uart_rx(auto* p) {
        if constexpr (requires(Handler& h, uint8_t idx, const uint8_t* d, size_t n) {
                h.handle_uart_rx(idx, d, n); })
            if (p->rx_data())
                handler_.handle_uart_rx(p->uart_idx(), p->rx_data()->data(), p->rx_data()->size());
    }

    // ── Members ────────────────────────────────────────────────────────

    Handler&                                   handler_;
    transport::UsbTransport                    transport_;
    protocol::ProtocolDecoder<NuedcSlave>      decoder_;
    nuedc::RingBuffer<RxFrame, RX_RING_SIZE>  rx_ring_;
};

}  // namespace nuedc

#endif  // NUEDC_HAS_LIBUSB
