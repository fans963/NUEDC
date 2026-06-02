#pragma once

#include <chrono>

namespace nuedc {

/// Simple self-throttle — returns true only if enough time has passed.
/// Components use this to rate-limit their own update() calls.
///
/// Usage:
///   Throttle cmd_timer{2000};  // max 2000 Hz
///   void update() { if (cmd_timer.ready()) send_command(); }
struct Throttle {
    using clock = std::chrono::steady_clock;

    explicit Throttle(double hz) : period_(static_cast<long long>(1e9 / hz)) {}

    bool ready() {
        auto now = clock::now();
        if (now < next_) return false;
        next_ = now + std::chrono::nanoseconds{period_};
        return true;
    }

    void reset() { next_ = clock::now(); }

private:
    long long period_;
    clock::time_point next_{};
};

}  // namespace nuedc
