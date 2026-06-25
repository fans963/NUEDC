//! Controllers: PID, Chassis inverse kinematics, Kalman filter.
//! Equivalent to C++ controller/{pid, chassis, kalman}/.

use std::time::Instant;

// ── PID Calculator ──────────────────────────────────────────────────

pub struct Pid {
    pub kp: f64, pub ki: f64, pub kd: f64,
    pub out_min: f64, pub out_max: f64,
    pub i_min: f64, pub i_max: f64,
    integral: f64,
    prev_error: f64,
    prev_output: f64,
    last_time: Option<Instant>,
    first_call: bool,
}

impl Pid {
    pub fn new(kp: f64, ki: f64, kd: f64) -> Self {
        Self {
            kp, ki, kd,
            out_min: -1000.0, out_max: 1000.0,
            i_min: -500.0, i_max: 500.0,
            integral: 0.0, prev_error: 0.0, prev_output: 0.0,
            last_time: None, first_call: true,
        }
    }

    pub fn set_limits(&mut self, out_min: f64, out_max: f64, i_min: f64, i_max: f64) {
        self.out_min = out_min; self.out_max = out_max;
        self.i_min = i_min; self.i_max = i_max;
    }

    pub fn update(&mut self, error: f64) -> f64 {
        let now = Instant::now();
        if self.first_call {
            self.first_call = false;
            self.last_time = Some(now);
            self.prev_error = error;
            self.prev_output = (self.kp * error).clamp(self.out_min, self.out_max);
            return self.prev_output;
        }

        let dt = self.last_time.unwrap().elapsed().as_secs_f64();
        self.last_time = Some(now);

        // Integral
        self.integral += self.ki * error * dt;
        self.integral = self.integral.clamp(self.i_min, self.i_max);

        // Derivative
        let derivative = if dt > 1e-9 { self.kd * (error - self.prev_error) / dt } else { 0.0 };
        self.prev_error = error;

        let out = (self.kp * error + self.integral + derivative).clamp(self.out_min, self.out_max);
        self.prev_output = out;
        out
    }

    pub fn reset(&mut self) {
        self.integral = 0.0;
        self.prev_error = 0.0;
        self.first_call = true;
    }
}

// ── Chassis inverse kinematics ─────────────────────────────────────

pub struct ChassisIk {
    pub wheel_base: f64,
    pub wheel_radius: f64,
    pub linear_max: f64,
    pub angular_max: f64,
}

impl ChassisIk {
    pub fn new(wheel_base: f64, wheel_radius: f64) -> Self {
        Self { wheel_base, wheel_radius, linear_max: 3.0, angular_max: 8.0 }
    }

    /// Body velocity → wheel velocities (rad/s).
    /// Returns (left, right).
    pub fn compute(&self, linear: f64, angular: f64) -> (f64, f64) {
        let linear = linear.clamp(-self.linear_max, self.linear_max);
        let angular = angular.clamp(-self.angular_max, self.angular_max);
        let half = self.wheel_base / 2.0;
        ((linear - angular * half) / self.wheel_radius,
         (linear + angular * half) / self.wheel_radius)
    }
}

// ── Kalman Filter template ─────────────────────────────────────────

pub struct KalmanFilter<const N: usize, const M: usize, const L: usize> {
    pub x: nalgebra::SVector<f64, N>,   // state
    pub p: nalgebra::SMatrix<f64, N, N>, // covariance
    last_time: Option<Instant>,
}

impl<const N: usize, const M: usize, const L: usize> KalmanFilter<N, M, L> {
    pub fn new(x0: nalgebra::SVector<f64, N>, p0: nalgebra::SMatrix<f64, N, N>) -> Self {
        Self { x: x0, p: p0, last_time: None }
    }

    pub fn predict(&mut self, a: &nalgebra::SMatrix<f64, N, N>, q: &nalgebra::SMatrix<f64, N, N>, b: Option<(&nalgebra::SMatrix<f64, N, L>, &nalgebra::SVector<f64, L>)>) {
        if let Some((b_mat, u)) = b {
            self.x = a * self.x + b_mat * u;
        } else {
            self.x = a * self.x;
        }
        self.p = a * self.p * a.transpose() + q;
    }

    pub fn update(&mut self, z: &nalgebra::SVector<f64, M>, h: &nalgebra::SMatrix<f64, M, N>, r: &nalgebra::SMatrix<f64, M, M>) {
        let y = z - h * self.x;
        let s = h * self.p * h.transpose() + r;
        let inv = s.try_inverse().unwrap_or(nalgebra::SMatrix::identity());
        let k = self.p * h.transpose() * inv;
        self.x += k * y;
        let i = nalgebra::SMatrix::<f64, N, N>::identity();
        self.p = (i - k * h) * self.p;
    }
}

pub type KalmanFilter2D = KalmanFilter<4, 2, 0>;
