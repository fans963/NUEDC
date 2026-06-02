#pragma once

#include "util/ring_buffer.hpp"

#include <cmath>
#include <cstdint>
#include <numbers>

namespace nuedcs::devices {

/// Default no-op coordinate mapping.
struct NoMapping {
    void operator()(double&, double&, double&) const {}
};

/// Raw IMU sample from one read.
struct ImuSample {
    int16_t ax, ay, az, gx, gy, gz;
};

/// BMI088 6-axis IMU with Mahony AHRS sensor fusion.
///
/// Ring-buffered: USB callback pushes samples, update_status() drains all.
/// No samples are lost as long as the ring is large enough for the rate mismatch.
///
/// @tparam Mapping   Coordinate remapping functor (default: NoMapping).
/// @tparam RingSize  SPSC ring capacity, power of 2 (default: 32 — ~16ms @ 2000Hz).
template <typename Mapping = NoMapping, size_t RingSize = 32>
class Bmi088 {
public:
    struct Config {
        double sample_freq = 1000.0;
        double kp = 2.0, ki = 0.0;
        double q0 = 1, q1 = 0, q2 = 0, q3 = 0;

        Config& set_sample_freq(double v) { sample_freq = v; return *this; }
        Config& set_kp(double v)          { kp = v;          return *this; }
        Config& set_ki(double v)          { ki = v;          return *this; }
    };

    explicit Bmi088(const Config& cfg, Mapping mapping = {})
        : inv_sample_freq_(1.0 / cfg.sample_freq)
        , double_kp_(2.0 * cfg.kp)
        , double_ki_(2.0 * cfg.ki)
        , q0_(cfg.q0), q1_(cfg.q1), q2_(cfg.q2), q3_(cfg.q3)
        , mapping_(std::move(mapping)) {}

    // ── Raw input (USB callback — bg thread) ─────────────────────────

    /// Push one paired accel+gyro sample. Returns false if ring full (drop).
    bool store_sample(int16_t ax, int16_t ay, int16_t az,
                      int16_t gx, int16_t gy, int16_t gz) {
        return ring_.push({ax, ay, az, gx, gy, gz});
    }

    // ── Status update (main loop) ────────────────────────────────────

    /// Drain all buffered samples through Mahony AHRS.
    void update_status() {
        ImuSample s;
        while (ring_.pop(s)) {
            double ax = s.ax / 32767.0 * 6.0;
            double ay = s.ay / 32767.0 * 6.0;
            double az = s.az / 32767.0 * 6.0;
            double gx = s.gx / 32767.0 * 2000.0 / 180.0 * std::numbers::pi;
            double gy = s.gy / 32767.0 * 2000.0 / 180.0 * std::numbers::pi;
            double gz = s.gz / 32767.0 * 2000.0 / 180.0 * std::numbers::pi;

            mapping_(gx, gy, gz);
            mapping_(ax, ay, az);

            mahony_update(ax, ay, az, gx, gy, gz);
        }
        ax_ = last_ax_; ay_ = last_ay_; az_ = last_az_;
        gx_ = last_gx_; gy_ = last_gy_; gz_ = last_gz_;
    }

    // ── Accessors ────────────────────────────────────────────────────

    double ax() const { return ax_; }  double ay() const { return ay_; }  double az() const { return az_; }
    double gx() const { return gx_; }  double gy() const { return gy_; }  double gz() const { return gz_; }
    double q0() const { return q0_; }  double q1() const { return q1_; }
    double q2() const { return q2_; }  double q3() const { return q3_; }

private:
    void mahony_update(double ax, double ay, double az, double gx, double gy, double gz) {
        double recip_norm, halfvx, halfvy, halfvz, halfex, halfey, halfez, qa, qb, qc;

        if ((ax != 0.0) || (ay != 0.0) || (az != 0.0)) {
            recip_norm = 1.0 / std::sqrt(ax * ax + ay * ay + az * az);
            ax *= recip_norm; ay *= recip_norm; az *= recip_norm;

            halfvx = q1_ * q3_ - q0_ * q2_;
            halfvy = q0_ * q1_ + q2_ * q3_;
            halfvz = q0_ * q0_ - 0.5 + q3_ * q3_;

            halfex = ay * halfvz - az * halfvy;
            halfey = az * halfvx - ax * halfvz;
            halfez = ax * halfvy - ay * halfvx;

            if (double_ki_ > 0.0) {
                integral_fbx_ += double_ki_ * halfex * inv_sample_freq_;
                integral_fby_ += double_ki_ * halfey * inv_sample_freq_;
                integral_fbz_ += double_ki_ * halfez * inv_sample_freq_;
                gx += integral_fbx_; gy += integral_fby_; gz += integral_fbz_;
            } else {
                integral_fbx_ = integral_fby_ = integral_fbz_ = 0.0;
            }
            gx += double_kp_ * halfex;
            gy += double_kp_ * halfey;
            gz += double_kp_ * halfez;
        }

        gx *= 0.5 * inv_sample_freq_;
        gy *= 0.5 * inv_sample_freq_;
        gz *= 0.5 * inv_sample_freq_;
        qa = q0_; qb = q1_; qc = q2_;
        q0_ += -qb * gx - qc * gy - q3_ * gz;
        q1_ += qa * gx + qc * gz - q3_ * gy;
        q2_ += qa * gy - qb * gz + q3_ * gx;
        q3_ += qa * gz + qb * gy - qc * gx;

        recip_norm = 1.0 / std::sqrt(q0_ * q0_ + q1_ * q1_ + q2_ * q2_ + q3_ * q3_);
        q0_ *= recip_norm; q1_ *= recip_norm; q2_ *= recip_norm; q3_ *= recip_norm;

        // Cache last values for accessors
        last_ax_ = ax; last_ay_ = ay; last_az_ = az;
        last_gx_ = gx; last_gy_ = gy; last_gz_ = gz;
    }

    double inv_sample_freq_, double_kp_, double_ki_;

    nuedc::RingBuffer<ImuSample, RingSize> ring_;

    double ax_ = 0, ay_ = 0, az_ = 0, gx_ = 0, gy_ = 0, gz_ = 0;
    double last_ax_ = 0, last_ay_ = 0, last_az_ = 0;
    double last_gx_ = 0, last_gy_ = 0, last_gz_ = 0;
    double q0_ = 1, q1_ = 0, q2_ = 0, q3_ = 0;
    double integral_fbx_ = 0, integral_fby_ = 0, integral_fbz_ = 0;

    [[no_unique_address]] Mapping mapping_;
};

}  // namespace nuedcs::devices
