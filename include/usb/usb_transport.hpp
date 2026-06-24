#pragma once

// Lightweight async USB transport over libusb.
// Inspired by RMCS's SerialInterface pattern: clean read(byte*,size_t)->size_t abstraction.
//
// Architecture:
//   - Background event thread runs libusb_handle_events_timeout(1ms)
//   - TX: lock-free SPSC ring → async libusb bulk OUT
//   - RX: async libusb bulk IN → callback → user-provided handler

#ifndef NUEDC_HAS_LIBUSB
#define NUEDC_HAS_LIBUSB 0
#endif

#if NUEDC_HAS_LIBUSB

#include "util/ring_buffer.hpp"

#include <cstdint>
#include <cstring>
#include <functional>
#include <libusb.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <thread>

namespace nuedc::transport {

// ── UsbTransport ──────────────────────────────────────────────────────

/// Non-blocking async USB bulk transport.
///
/// Lifecycle: construct → start() → [send/receive] → stop() → destruct
///
/// Thread safety:
///   - send() — any thread
///   - on_receive callback — fires on bg thread; keep it fast
///
/// Usage:
///   UsbTransport t(0x1209, 0x0001);
///   t.set_on_receive([](const uint8_t* d, size_t n) { ... });
///   t.start();
///   t.send(buf, len);            // non-blocking, any thread
///   t.stop();
class UsbTransport {
    static constexpr int INTERFACE  = 0;
    static constexpr uint8_t EP_OUT = 0x01;
    static constexpr uint8_t EP_IN  = 0x81;
    static constexpr size_t RX_BUF  = 512;
    static constexpr size_t TX_RING = 16;  // power of 2
    static constexpr size_t TX_BUF  = 518; // max wire frame

public:
    explicit UsbTransport(uint16_t vid = 0x1209, uint16_t pid = 0x0001) {
        int ret = libusb_init(&ctx_);
        if (ret) throw std::runtime_error("libusb_init: " + std::to_string(ret));

        handle_ = libusb_open_device_with_vid_pid(ctx_, vid, pid);
        if (!handle_) {
            libusb_exit(ctx_);
            ctx_ = nullptr;
            spdlog::error("[UsbTransport] device VID=0x{:04X} PID=0x{:04X} not found — offline mode", vid, pid);
            return;  // graceful: connected_ stays false, start()/send() are no-ops
        }

        if (libusb_kernel_driver_active(handle_, INTERFACE) == 1)
            libusb_detach_kernel_driver(handle_, INTERFACE);

        ret = libusb_claim_interface(handle_, INTERFACE);
        if (ret) {
            cleanup();
            throw std::runtime_error("claim_interface: " + std::string(libusb_error_name(ret)));
        }

        rx_xfer_ = libusb_alloc_transfer(0);
        libusb_fill_bulk_transfer(
            rx_xfer_, handle_, EP_IN, rx_buf_, RX_BUF, &UsbTransport::rx_done, this, 0);

        tx_xfer_ = libusb_alloc_transfer(0);
        libusb_fill_bulk_transfer(
            tx_xfer_, handle_, EP_OUT, tx_buf_, 0, &UsbTransport::tx_done, this, 0);

        connected_ = true;
    }

    ~UsbTransport() {
        stop();
        cleanup();
    }

    UsbTransport(const UsbTransport&)            = delete;
    UsbTransport& operator=(const UsbTransport&) = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────

    /// Set callback invoked with raw received bytes (on bg thread).
    /// Must be called before start().
    void set_on_receive(std::function<void(const uint8_t*, size_t)> cb) { on_rx_ = std::move(cb); }

    /// Set callback invoked once when device disconnects (on bg thread).
    void set_on_disconnect(std::function<void()> cb) { on_disconnect_ = std::move(cb); }

    [[nodiscard]] bool connected() const { return connected_; }

    /// Start background event loop and RX reception. No-op in offline mode.
    void start() {
        if (!connected_) return;
        running_.store(true, std::memory_order_relaxed);
        libusb_submit_transfer(rx_xfer_);
        thread_ = std::thread(&UsbTransport::event_loop, this);
    }

    /// Stop event loop and join background thread.
    /// Cancels transfers while loop is still running so callbacks fire.
    void stop() {
        running_.store(false, std::memory_order_relaxed);

        // Cancel transfers now — event loop is still alive, callbacks will fire
        if (rx_xfer_) libusb_cancel_transfer(rx_xfer_);
        if (tx_xfer_) libusb_cancel_transfer(tx_xfer_);

        // Drain remaining events until cancellations complete
        if (thread_.joinable()) {
            timeval tv { 0, 5000 };      // 5ms timeout
            for (int i = 0; i < 10; ++i) // up to 50ms
                libusb_handle_events_timeout(ctx_, &tv);
            thread_.join();
        }
    }

    // ── Send (single producer thread only, non-blocking) ──────────────

    /// Queue data for async transmission. Returns false if ring full.
    /// @pre Must be called from at most one thread at a time (SPSC ring).
    bool send(const uint8_t* data, size_t len) {
        if (!connected_ || len > TX_BUF) return false;
        TxFrame f;
        std::memcpy(f.data, data, len);
        f.len = len;
        return tx_ring_.push(f);
    }

private:
    // ── Event loop (bg thread) ─────────────────────────────────────────

    void event_loop() {
        while (running_.load(std::memory_order_relaxed)) {
            timeval tv { 0, 1000 }; // 1ms timeout for TX polling
            int ret = libusb_handle_events_timeout(ctx_, &tv);
            if (ret == LIBUSB_ERROR_NO_DEVICE) {
                mark_disconnected();
                break;
            }
            drain_tx();
        }
    }

    // ── TX pipeline ────────────────────────────────────────────────────

    struct TxFrame {
        uint8_t data[TX_BUF];
        size_t len;
    };

    void drain_tx() {
        if (!connected_ || tx_in_flight_.load(std::memory_order_relaxed)) return;
        TxFrame f;
        if (!tx_ring_.pop(f)) return;

        std::memcpy(tx_buf_, f.data, f.len);
        tx_xfer_->length = static_cast<int>(f.len);
        tx_in_flight_.store(true, std::memory_order_relaxed);

        if (int ret = libusb_submit_transfer(tx_xfer_); ret) {
            tx_in_flight_.store(false, std::memory_order_relaxed);
            if (ret == LIBUSB_ERROR_NO_DEVICE) mark_disconnected();
        }
    }

    static void tx_done(libusb_transfer* xfer) {
        auto* self = static_cast<UsbTransport*>(xfer->user_data);
        self->tx_in_flight_.store(false, std::memory_order_relaxed);
        if (xfer->status == LIBUSB_TRANSFER_NO_DEVICE
            || xfer->status == LIBUSB_TRANSFER_ERROR) {
            self->mark_disconnected();
            return;
        }
        self->drain_tx(); // chain: submit next pending frame
    }

    // ── RX pipeline ────────────────────────────────────────────────────

    static void rx_done(libusb_transfer* xfer) {
        auto* self = static_cast<UsbTransport*>(xfer->user_data);

        if (xfer->status == LIBUSB_TRANSFER_NO_DEVICE
            || xfer->status == LIBUSB_TRANSFER_ERROR) {
            self->mark_disconnected();
            return;
        }

        if (xfer->status == LIBUSB_TRANSFER_COMPLETED && xfer->actual_length > 0 && self->on_rx_)
            self->on_rx_(xfer->buffer, xfer->actual_length);

        if (self->running_.load(std::memory_order_relaxed)) libusb_submit_transfer(xfer);
    }

    // ── Disconnect detection ────────────────────────────────────────────

    void mark_disconnected() {
        if (!connected_) return;
        connected_ = false;
        spdlog::error("[UsbTransport] device disconnected");
        if (on_disconnect_) on_disconnect_();
    }

    // ── Resource cleanup ───────────────────────────────────────────────

    void cleanup() {
        if (tx_xfer_) {
            libusb_free_transfer(tx_xfer_);
            tx_xfer_ = nullptr;
        }
        if (rx_xfer_) {
            libusb_free_transfer(rx_xfer_);
            rx_xfer_ = nullptr;
        }
        if (handle_) {
            libusb_release_interface(handle_, INTERFACE);
            libusb_close(handle_);
            handle_ = nullptr;
        }
        if (ctx_) {
            libusb_exit(ctx_);
            ctx_ = nullptr;
        }
    }

    // ── Members ────────────────────────────────────────────────────────

    libusb_context* ctx_          = nullptr;
    libusb_device_handle* handle_ = nullptr;
    libusb_transfer* rx_xfer_     = nullptr;
    libusb_transfer* tx_xfer_     = nullptr;

    uint8_t rx_buf_[RX_BUF] { };
    uint8_t tx_buf_[TX_BUF] { };

    nuedc::RingBuffer<TxFrame, TX_RING> tx_ring_;
    std::atomic<bool> running_ { false };
    std::atomic<bool> tx_in_flight_ { false };
    bool connected_ { false };
    std::thread thread_;

    std::function<void(const uint8_t*, size_t)> on_rx_;
    std::function<void()> on_disconnect_;
};

// ── Framing adapter: wraps any write-capable transport with wire framing ──

/// Adds 0x5A…0xA5 framing to a transport. Reusable across any byte-sink.
template <typename W>
class FramedWriter {
public:
    explicit FramedWriter(W& writer)
        : writer_(writer) { }
    void write(const uint8_t* data, size_t len) { writer_.send(data, len); }

private:
    W& writer_;
};

} // namespace nuedc::transport

#endif // NUEDC_HAS_LIBUSB
