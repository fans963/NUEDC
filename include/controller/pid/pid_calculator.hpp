#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace nuedcs::controller::pid {

/// PID calculator with proper dt support.
/// update(err) auto-measures time since last call.
/// First call: P-only (no dt yet). Subsequent calls: full PID with dt scaling.
class PidCalculator {
    using clock = std::chrono::steady_clock;

public:
    PidCalculator(double kp, double ki, double kd,
                  double output_min = -inf, double output_max = inf,
                  double integral_min = -inf, double integral_max = inf)
        : kp(kp), ki(ki), kd(kd)
        , integral_min(integral_min), integral_max(integral_max)
        , output_min(output_min), output_max(output_max) {
        reset();
    }

    void reset() {
        last_err_     = nan;
        err_integral_ = 0;
        last_time_    = clock::time_point{};
    }

    /// @return control output. First call returns P-only, subsequent calls use dt.
    double update(double err) {
        if (!std::isfinite(err)) return nan;

        auto now = clock::now();
        double dt = 0;
        bool have_dt = false;

        if (last_time_ != clock::time_point{}) {
            dt = std::chrono::duration<double>(now - last_time_).count();
            have_dt = (dt > 0 && dt < 1.0);  // guard against absurd dt
        }
        last_time_ = now;

        double control = kp * err;

        if (have_dt) {
            err_integral_ += err * dt;
            double deriv = (err - last_err_) / dt;
            control += ki * err_integral_ + kd * deriv;
        }

        err_integral_ = std::clamp(err_integral_, integral_min, integral_max);
        last_err_     = err;
        return std::clamp(control, output_min, output_max);
    }

    double kp, ki, kd;
    double integral_min, integral_max;
    double output_min, output_max;

private:
    static constexpr double inf = std::numeric_limits<double>::infinity();
    static constexpr double nan = std::numeric_limits<double>::quiet_NaN();

    double   last_err_, err_integral_;
    clock::time_point last_time_;
};

}  // namespace nuedcs::controller::pid