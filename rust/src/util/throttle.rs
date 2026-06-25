//! Simple rate-limiter (equivalent to C++ util/throttle.hpp).

use std::time::{Duration, Instant};

/// Returns true at most once per configured interval.
pub struct Throttle {
    period: Duration,
    next: Instant,
}

impl Throttle {
    pub fn new(hz: f64) -> Self {
        let period = Duration::from_secs_f64(1.0 / hz);
        Self { period, next: Instant::now() }
    }

    pub fn ready(&mut self) -> bool {
        let now = Instant::now();
        if now < self.next {
            return false;
        }
        self.next = now + self.period;
        true
    }
}
