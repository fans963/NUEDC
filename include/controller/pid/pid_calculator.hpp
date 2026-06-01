#pragma once

#include <cmath>

#include <algorithm>
#include <limits>

namespace nuedcs::controller::pid {

class PidCalculator {
public:
    PidCalculator(double kp, double ki, double kd,double output_min = -inf, double output_max = inf,
                  double integral_min = -inf, double integral_max = inf)
        : kp(kp)
        , ki(ki)
        , kd(kd)
        , integral_min(integral_min)
        , integral_max(integral_max)
        , output_min(output_min)
        , output_max(output_max) {
        reset();
    }

    virtual ~PidCalculator() = default;

    void reset() {
        last_err_     = nan;
        err_integral_ = 0;
    }

    double update(double err) {
        if (!std::isfinite(err)) {
            return nan;
        } else {
            double control = kp * err + ki * err_integral_;
            err_integral_  = std::clamp(err_integral_ + err, integral_min, integral_max);

            if (!std::isnan(last_err_)) control += kd * (err - last_err_);
            last_err_ = err;

            return std::clamp(control, output_min, output_max);
        }
    }

    double kp, ki, kd;
    double integral_min, integral_max;
    double output_min, output_max;

private:
    static constexpr double inf = std::numeric_limits<double>::infinity();
    static constexpr double nan = std::numeric_limits<double>::quiet_NaN();

    double last_err_, err_integral_;
};

} // namespace nuedcs::controller::pid