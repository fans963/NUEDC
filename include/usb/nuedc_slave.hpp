#pragma once

// Host-side USB wrapper for nuedc_slave board.
// Uses libusb for bulk transfers, FlatBuffers for serialization.
// Wire protocol: 0x5A | u32_le_size | FlatBuffer | 0xA5
// Only available when NUEDC_HAS_LIBUSB=1 (set by CMake when libusb is found).

#ifndef NUEDC_HAS_LIBUSB
#define NUEDC_HAS_LIBUSB 0
#endif

#if NUEDC_HAS_LIBUSB

#include "usb/protocol.hpp"

#include <flatbuffers/flatbuffers.h>
#include <host_to_slave_generated.h>
#include <slave_to_host_generated.h>

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <libusb.h>
#include <stdexcept>
#include <vector>

namespace nuedc {

class NuedcSlave {
public:
    // ── Lifecycle ───────────────────────────────────────────────────────

    explicit NuedcSlave(uint16_t vendor_id = 0x1209, uint16_t product_id = 0x0001)
        : decoder_(*this)
    {
        int ret = libusb_init(&ctx_);
        if (ret != 0)
            throw std::runtime_error("libusb_init failed: " + std::to_string(ret));

        handle_ = libusb_open_device_with_vid_pid(ctx_, vendor_id, product_id);
        if (!handle_) {
            libusb_exit(ctx_);
            throw std::runtime_error("Device not found (VID=0x1209 PID=0x0001)");
        }

        // Detach kernel driver on Linux
        libusb_set_auto_detach_kernel_driver(handle_, 1);

        ret = libusb_claim_interface(handle_, INTERFACE_NUM);
        if (ret != 0) {
            libusb_close(handle_);
            libusb_exit(ctx_);
            throw std::runtime_error("claim_interface failed: " + std::to_string(ret));
        }

        // Allocate async receive transfer
        recv_xfer_ = libusb_alloc_transfer(0);
        if (!recv_xfer_)
            throw std::runtime_error("libusb_alloc_transfer failed");

        libusb_fill_bulk_transfer(
            recv_xfer_, handle_, EP_IN, recv_buf_, sizeof(recv_buf_),
            [](libusb_transfer* xfer) {
                static_cast<NuedcSlave*>(xfer->user_data)->on_usb_rx(xfer);
            },
            this, 0);

        spdlog::info("[NuedcSlave] Connected (VID=0x{:04X} PID=0x{:04X})", vendor_id, product_id);
    }

    ~NuedcSlave()
    {
        running_.store(false, std::memory_order::relaxed);

        if (recv_xfer_) {
            libusb_free_transfer(recv_xfer_);
        }
        if (handle_) {
            libusb_release_interface(handle_, INTERFACE_NUM);
            libusb_close(handle_);
        }
        if (ctx_) {
            libusb_exit(ctx_);
        }
    }

    NuedcSlave(const NuedcSlave&)            = delete;
    NuedcSlave& operator=(const NuedcSlave&) = delete;

    // ── Event loop ──────────────────────────────────────────────────────

    void handle_events()
    {
        int ret = libusb_submit_transfer(recv_xfer_);
        if (ret != 0) {
            spdlog::error("[NuedcSlave] submit_transfer failed: {}", ret);
            return;
        }

        running_.store(true, std::memory_order::relaxed);
        while (running_.load(std::memory_order::relaxed)) {
            libusb_handle_events(ctx_);
        }
    }

    void stop() { running_.store(false, std::memory_order::relaxed); }

    // ── Send methods (host → slave) ─────────────────────────────────────

    void set_motor_speed(uint8_t motor_id, float target_speed_rad_s)
    {
        fbb_.Clear();
        auto cmd = Protocol::HostToSlave::CreateMotorCommandPack(fbb_, motor_id, target_speed_rad_s);
        auto frame = Protocol::HostToSlave::CreateHostToSlaveFrame(
            fbb_, Protocol::HostToSlave::MsgPayload::MotorCommandPack, cmd.Union());
        fbb_.FinishSizePrefixed(frame);
        send_frame(fbb_.GetBufferPointer(), fbb_.GetSize());
    }

    void set_pid(uint8_t id, float kp, float ki, float kd,
                 float out_min = -1000.f, float out_max = 1000.f,
                 float i_min = -500.f, float i_max = 500.f, bool reset = false)
    {
        fbb_.Clear();
        auto pid = Protocol::HostToSlave::CreatePidConfigPack(
            fbb_, id, kp, ki, kd, out_min, out_max, i_min, i_max, reset);
        auto frame = Protocol::HostToSlave::CreateHostToSlaveFrame(
            fbb_, Protocol::HostToSlave::MsgPayload::PidConfigPack, pid.Union());
        fbb_.FinishSizePrefixed(frame);
        send_frame(fbb_.GetBufferPointer(), fbb_.GetSize());
    }

    void send_can(uint8_t can_idx, uint32_t can_id, uint8_t dlc,
                  const uint8_t* data, bool is_extended = false, bool is_rtr = false)
    {
        fbb_.Clear();
        auto tx_data = fbb_.CreateVector(data, dlc);
        auto can = Protocol::HostToSlave::CreateCanPack(
            fbb_, can_idx, can_id, dlc, is_extended, is_rtr, tx_data);
        auto frame = Protocol::HostToSlave::CreateHostToSlaveFrame(
            fbb_, Protocol::HostToSlave::MsgPayload::CanPack, can.Union());
        fbb_.FinishSizePrefixed(frame);
        send_frame(fbb_.GetBufferPointer(), fbb_.GetSize());
    }

    void send_uart(uint8_t uart_idx, const uint8_t* data, size_t len)
    {
        fbb_.Clear();
        auto tx_data = fbb_.CreateVector(data, len);
        auto uart = Protocol::HostToSlave::CreateUartPack(fbb_, uart_idx, tx_data);
        auto frame = Protocol::HostToSlave::CreateHostToSlaveFrame(
            fbb_, Protocol::HostToSlave::MsgPayload::UartPack, uart.Union());
        fbb_.FinishSizePrefixed(frame);
        send_frame(fbb_.GetBufferPointer(), fbb_.GetSize());
    }

    void set_encoder_config(uint8_t encoder_id, uint16_t lines_per_rev)
    {
        fbb_.Clear();
        auto enc = Protocol::HostToSlave::CreateEncoderConfigPack(fbb_, encoder_id, lines_per_rev);
        auto frame = Protocol::HostToSlave::CreateHostToSlaveFrame(
            fbb_, Protocol::HostToSlave::MsgPayload::EncoderConfigPack, enc.Union());
        fbb_.FinishSizePrefixed(frame);
        send_frame(fbb_.GetBufferPointer(), fbb_.GetSize());
    }

    // ── Receive callbacks (slave → host) ────────────────────────────────
    // Override these in your subclass to handle incoming data.

    virtual void on_imu(float ax, float ay, float az, float gx, float gy, float gz)
    {
        (void)ax; (void)ay; (void)az; (void)gx; (void)gy; (void)gz;
    }

    virtual void on_encoder(uint8_t encoder_id, float velocity_rad_s)
    {
        (void)encoder_id; (void)velocity_rad_s;
    }

    virtual void on_adc(uint8_t adc_idx, const uint16_t* channels, size_t count)
    {
        (void)adc_idx; (void)channels; (void)count;
    }

    virtual void on_can_rx(uint8_t can_idx, uint32_t can_id, uint8_t dlc,
                           bool is_extended, bool is_rtr, const uint8_t* data)
    {
        (void)can_idx; (void)can_id; (void)dlc; (void)is_extended; (void)is_rtr; (void)data;
    }

    virtual void on_uart_rx(uint8_t uart_idx, const uint8_t* data, size_t len)
    {
        (void)uart_idx; (void)data; (void)len;
    }

private:
    // ── USB constants ───────────────────────────────────────────────────

    static constexpr int INTERFACE_NUM = 1;
    static constexpr uint8_t EP_OUT    = 0x01;
    static constexpr uint8_t EP_IN     = 0x81;

    // ── USB receive callback ────────────────────────────────────────────

    void on_usb_rx(libusb_transfer* xfer)
    {
        if (xfer->status != LIBUSB_TRANSFER_COMPLETED) {
            spdlog::warn("[NuedcSlave] RX transfer status={}", (int)xfer->status);
            if (running_.load(std::memory_order::relaxed))
                libusb_submit_transfer(xfer); // resubmit
            return;
        }

        // Feed raw bytes into protocol decoder
        decoder_.feed(xfer->buffer, xfer->actual_length);

        // Resubmit for next reception
        if (running_.load(std::memory_order::relaxed))
            libusb_submit_transfer(xfer);
    }

    // ── Protocol frame handler (called by ProtocolDecoder) ──────────────

    void on_frame(const uint8_t* data, size_t len)
    {
        // Verify FlatBuffer
        flatbuffers::Verifier verifier(data, len);
        if (!verifier.VerifySizePrefixedBuffer<Protocol::SlaveToHost::SlaveToHostFrame>(nullptr))
            return;

        auto frame = flatbuffers::GetSizePrefixedRoot<Protocol::SlaveToHost::SlaveToHostFrame>(data);
        if (!frame || !frame->payload())
            return;

        switch (frame->payload_type()) {
        case Protocol::SlaveToHost::MsgPayload::ImuPack: {
            auto imu = frame->payload_as_ImuPack();
            if (imu)
                on_imu(imu->accel_x(), imu->accel_y(), imu->accel_z(),
                       imu->gyro_x(), imu->gyro_y(), imu->gyro_z());
            break;
        }
        case Protocol::SlaveToHost::MsgPayload::EncoderPack: {
            auto enc = frame->payload_as_EncoderPack();
            if (enc)
                on_encoder(enc->encoder_id(), enc->velocity_rad_s());
            break;
        }
        case Protocol::SlaveToHost::MsgPayload::AdcPack: {
            auto adc = frame->payload_as_AdcPack();
            if (adc && adc->channels())
                on_adc(adc->adc_idx(), adc->channels()->data(), adc->channels()->size());
            break;
        }
        case Protocol::SlaveToHost::MsgPayload::CanPack: {
            auto can = frame->payload_as_CanPack();
            if (can && can->rx_data())
                on_can_rx(can->can_idx(), can->can_id(), can->can_dlc(),
                          can->is_extended(), can->is_rtr(), can->rx_data()->data());
            break;
        }
        case Protocol::SlaveToHost::MsgPayload::UartPack: {
            auto uart = frame->payload_as_UartPack();
            if (uart && uart->rx_data())
                on_uart_rx(uart->uart_idx(), uart->rx_data()->data(), uart->rx_data()->size());
            break;
        }
        default:
            break;
        }
    }

    // ── Send helper ─────────────────────────────────────────────────────

    void send_frame(const uint8_t* size_prefixed_buf, size_t len)
    {
        // Build wire frame: 0x5A | size_prefixed_flatbuf | 0xA5
        std::vector<uint8_t> wire;
        wire.reserve(1 + len + 1);
        wire.push_back(0x5A);
        wire.insert(wire.end(), size_prefixed_buf, size_prefixed_buf + len);
        wire.push_back(0xA5);

        // Synchronous bulk send
        int transferred = 0;
        int ret = libusb_bulk_transfer(handle_, EP_OUT,
                                       const_cast<uint8_t*>(wire.data()),
                                       static_cast<int>(wire.size()),
                                       &transferred, 100);
        if (ret != 0) {
            spdlog::error("[NuedcSlave] TX failed: {}", ret);
        }
    }

    // ── Members ─────────────────────────────────────────────────────────

    libusb_context* ctx_           = nullptr;
    libusb_device_handle* handle_  = nullptr;
    libusb_transfer* recv_xfer_    = nullptr;
    uint8_t recv_buf_[512]{};

    std::atomic<bool> running_{false};

    protocol::ProtocolDecoder<NuedcSlave> decoder_;
    flatbuffers::FlatBufferBuilder fbb_;
};

} // namespace nuedc

#endif // NUEDC_HAS_LIBUSB
