//! Device abstractions: EncoderMotor, CanMotor, Bmi088 with Mahony AHRS.
//! Equivalent to C++ devices/{encoder_motor, can_motor, bmi088}.hpp.

use std::sync::atomic::{AtomicU64, Ordering};
use std::f64::consts::PI;

// ── EncoderMotor ────────────────────────────────────────────────────

/// Firmware-side encoder + motor. Velocity pre-computed by firmware.
pub struct EncoderMotor {
    pub motor_id: u8,
    pub lines_per_rev: u16,
    raw_velocity: AtomicU64, // actually f32 bits
    pub velocity: f32,
    pub target_speed: f32,
    pub config_pending: bool,
}

impl EncoderMotor {
    pub fn new(motor_id: u8) -> Self {
        Self {
            motor_id, lines_per_rev: 0,
            raw_velocity: AtomicU64::new(0),
            velocity: 0.0, target_speed: 0.0, config_pending: false,
        }
    }

    pub fn configure(&mut self, lines_per_rev: u16) {
        self.lines_per_rev = lines_per_rev;
        self.config_pending = true;
    }

    /// Called from USB callback context — atomic store.
    pub fn store_velocity(&self, v: f32) {
        self.raw_velocity.store(v.to_bits() as u64, Ordering::Relaxed);
    }

    /// Main loop: load atomic → publish.
    pub fn update_status(&mut self) {
        self.velocity = f32::from_bits(self.raw_velocity.load(Ordering::Relaxed) as u32);
    }

    /// Read target speed from controller. 0 if not connected.
    pub fn generate_command(&self) -> f32 {
        self.target_speed
    }
}

// ── CanMotor ────────────────────────────────────────────────────────

#[derive(Clone, Copy)]
pub enum MotorType { M3508, M2006, GM6020, LK5010, LK4010, LK6012 }

pub struct CanMotorConfig {
    pub motor_type: MotorType,
    pub reduction_ratio: f64,
    pub reversed: bool,
}

pub struct CanMotor {
    pub can_id: u32,
    pub config: CanMotorConfig,
    raw_feedback: AtomicU64,
    pub angle: f64, pub velocity: f64, pub torque: f64, pub temperature: f64,
    pub ctrl_torque: f64, pub ctrl_velocity: f64, pub ctrl_angle: f64,
    angle_zero: i32, last_raw_angle: i32,
    scale: MotorScale,
}

struct MotorScale { angle: f64, velocity: f64, torque: f64, torque_to_raw: f64, max_torque: f64, raw_max: i32 }

impl MotorScale {
    fn new(cfg: &CanMotorConfig) -> Self {
        let sign = if cfg.reversed { -1.0 } else { 1.0 };
        let (raw_max, torque_c, current_max, raw_current_max) = match cfg.motor_type {
            MotorType::M3508 => (8192_i32, 0.3 * 187.0 / 3591.0, 20.0, 16384_i32),
            MotorType::M2006 => (8192, 0.18 * 1.0 / 36.0, 10.0, 16384),
            MotorType::GM6020 => (8192, 0.741, 3.0, 16384),
            MotorType::LK5010 => (65536, 0.90909, 33.0, 2048),
            MotorType::LK4010 => (65536, 0.07, 33.0, 2048),
            MotorType::LK6012 => (65536, 1.09, 33.0, 2048),
        };
        let angle_scale = sign / cfg.reduction_ratio / raw_max as f64 * 2.0 * PI as f64;
        let vel_scale = sign / cfg.reduction_ratio / 60.0 * 2.0 * PI as f64;
        let torque_scale = sign * cfg.reduction_ratio * torque_c / raw_current_max as f64 * current_max;
        Self {
            angle: angle_scale, velocity: vel_scale, torque: torque_scale,
            torque_to_raw: 1.0 / torque_scale,
            max_torque: cfg.reduction_ratio * torque_c * current_max,
            raw_max,
        }
    }
}

impl CanMotor {
    pub fn new(can_id: u32, config: CanMotorConfig) -> Self {
        let scale = MotorScale::new(&config);
        Self {
            can_id, config, scale,
            raw_feedback: AtomicU64::new(0),
            angle: 0.0, velocity: 0.0, torque: 0.0, temperature: 0.0,
            ctrl_torque: f64::NAN, ctrl_velocity: f64::NAN, ctrl_angle: f64::NAN,
            angle_zero: 0, last_raw_angle: 0,
        }
    }

    pub fn store_status(&self, data: &[u8; 8]) {
        self.raw_feedback.store(u64::from_le_bytes(*data), Ordering::Relaxed);
    }

    pub fn update_status(&mut self) {
        let raw = self.raw_feedback.load(Ordering::Relaxed);
        let raw_angle = (raw as i16) as i32;
        let raw_velocity = ((raw >> 16) as i16) as i32;
        let raw_current = ((raw >> 32) as i16) as i32;
        self.temperature = ((raw >> 48) as i8) as f64;

        let calibrated = raw_angle - self.angle_zero;
        let calibrated = if calibrated < 0 { calibrated + self.scale.raw_max } else { calibrated };
        self.angle = self.scale.angle * calibrated as f64;
        if self.angle < 0.0 { self.angle += 2.0 * PI as f64; }
        self.velocity = self.scale.velocity * raw_velocity as f64;
        self.torque = self.scale.torque * raw_current as f64;
        self.last_raw_angle = raw_angle;
    }

    pub fn generate_command(&self) -> Option<u64> {
        if !self.ctrl_angle.is_nan() { return Some(self.build_angle_cmd()); }
        if !self.ctrl_velocity.is_nan() { return Some(self.build_velocity_cmd()); }
        if !self.ctrl_torque.is_nan() { return Some(self.build_torque_cmd()); }
        None
    }

    pub fn calibrate(&mut self) { self.angle_zero = self.last_raw_angle; }

    fn build_torque_cmd(&self) -> u64 {
        let t = self.ctrl_torque.clamp(-self.scale.max_torque, self.scale.max_torque);
        let iq = (self.scale.torque_to_raw * t).round() as i16;
        (iq as u16 as u64) << 16
    }
    fn build_velocity_cmd(&self) -> u64 {
        let rpm = (self.ctrl_velocity / (2.0 * PI as f64) * 60.0).round() as i32;
        rpm as u32 as u64
    }
    fn build_angle_cmd(&self) -> u64 {
        let raw = (self.ctrl_angle / self.scale.angle).round() as i32;
        raw as u32 as u64
    }
}

// ── Bmi088 IMU with Mahony AHRS ────────────────────────────────────

pub struct Bmi088 {
    pub q0: f64, pub q1: f64, pub q2: f64, pub q3: f64,
    pub ax: f64, pub ay: f64, pub az: f64,
    pub gx: f64, pub gy: f64, pub gz: f64,
    kp: f64, ki: f64,
    integral_fb: (f64, f64, f64),
    sample_freq: f64,
    accel_scale: f64,   // +/-6g
    gyro_scale: f64,    // +/-2000 deg/s
}

impl Bmi088 {
    pub fn new(sample_freq: f64, kp: f64, ki: f64) -> Self {
        Self {
            q0: 1.0, q1: 0.0, q2: 0.0, q3: 0.0,
            ax: 0.0, ay: 0.0, az: 0.0,
            gx: 0.0, gy: 0.0, gz: 0.0,
            kp, ki, integral_fb: (0.0, 0.0, 0.0),
            sample_freq,
            accel_scale: 6.0 / 32768.0,
            gyro_scale: (2000.0 * PI / 180.0) / 32768.0,
        }
    }

    /// Store raw sample (from USB callback).
    pub fn store_sample(&self, _ax: i16, _ay: i16, _az: i16, _gx: i16, _gy: i16, _gz: i16) {
        // In real code: push to ring buffer
    }

    /// Main loop: drain ring buffer, update AHRS (called by Car.update())
    pub fn update_status(&mut self) {
        // Drain ring buffer and process samples through Mahony
        // In real code: while let Some(sample) = ring.pop() { self.update(sample...) }
    }

    /// Process one sample through Mahony AHRS.
    pub fn update(&mut self, raw_ax: i16, raw_ay: i16, raw_az: i16,
                  raw_gx: i16, raw_gy: i16, raw_gz: i16) {
        let ax = raw_ax as f64 * self.accel_scale;
        let ay = raw_ay as f64 * self.accel_scale;
        let az = raw_az as f64 * self.accel_scale;
        let gx = raw_gx as f64 * self.gyro_scale;
        let gy = raw_gy as f64 * self.gyro_scale;
        let gz = raw_gz as f64 * self.gyro_scale;

        let norm = (ax*ax + ay*ay + az*az).sqrt();
        if norm < 1e-6 { return; }
        let (ax_n, ay_n, az_n) = (ax/norm, ay/norm, az/norm);

        let (q0,q1,q2,q3) = (self.q0, self.q1, self.q2, self.q3);

        // Estimated gravity direction
        let vx = 2.0*(q1*q3 - q0*q2);
        let vy = 2.0*(q0*q1 + q2*q3);
        let vz = q0*q0 - q1*q1 - q2*q2 + q3*q3;

        // Error = cross(accel_normalized, v)
        let (ex, ey, ez) = (ay_n*vz - az_n*vy, az_n*vx - ax_n*vz, ax_n*vy - ay_n*vx);

        // Integral feedback
        self.integral_fb.0 += self.ki * ex;
        self.integral_fb.1 += self.ki * ey;
        self.integral_fb.2 += self.ki * ez;

        // Gyro correction
        let gx_c = gx + self.kp*ex + self.integral_fb.0;
        let gy_c = gy + self.kp*ey + self.integral_fb.1;
        let gz_c = gz + self.kp*ez + self.integral_fb.2;

        // Quaternion integration
        let half_dt = 0.5 / self.sample_freq;
        self.q0 += (-q1*gx_c - q2*gy_c - q3*gz_c) * half_dt;
        self.q1 += ( q0*gx_c + q2*gz_c - q3*gy_c) * half_dt;
        self.q2 += ( q0*gy_c - q1*gz_c + q3*gx_c) * half_dt;
        self.q3 += ( q0*gz_c + q1*gy_c - q2*gx_c) * half_dt;

        let norm_q = (self.q0*self.q0 + self.q1*self.q1 + self.q2*self.q2 + self.q3*self.q3).sqrt();
        self.q0 /= norm_q; self.q1 /= norm_q; self.q2 /= norm_q; self.q3 /= norm_q;

        self.ax = ax; self.ay = ay; self.az = az;
        self.gx = gx; self.gy = gy; self.gz = gz;
    }
}
