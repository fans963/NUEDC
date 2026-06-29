//! Device abstractions: EncoderMotor, CanMotor (QD4310), Bmi088 Mahony AHRS.
//! Equivalent to C++ devices/{encoder_motor, can_motor, bmi088}.hpp.

use std::sync::atomic::{AtomicU64, Ordering};
use std::f64::consts::PI;

// ── EncoderMotor ────────────────────────────────────────────────────

pub struct EncoderMotor {
    pub motor_id: u8,
    pub lines_per_rev: u16,
    raw_velocity: AtomicU64, // f32 bits stored as u64
    pub velocity: f32,
    pub target_speed: f32,
    pub config_pending: bool,
}

impl EncoderMotor {
    pub fn new(motor_id: u8) -> Self {
        Self { motor_id, lines_per_rev: 0, raw_velocity: AtomicU64::new(0),
               velocity: 0.0, target_speed: 0.0, config_pending: false }
    }
    pub fn configure(&mut self, lines_per_rev: u16) { self.lines_per_rev = lines_per_rev; self.config_pending = true; }
    pub fn store_velocity(&self, v: f32) { self.raw_velocity.store(v.to_bits() as u64, Ordering::Relaxed); }
    pub fn update_status(&mut self) { self.velocity = f32::from_bits(self.raw_velocity.load(Ordering::Relaxed) as u32); }
    pub fn generate_command(&self) -> f32 { self.target_speed }
}

// ── CanMotor (QD4310 FOC servo) ────────────────────────────────────
// QD4310 CAN protocol:
//   Host→Motor: ID=0x400+id, DLC=3: [cmd, val_lo, val_hi] (int16 LE)
//   Motor→Host: ID=0x500+id, DLC=8: [status,_, curr_lo,curr_hi, speed_lo,speed_hi, angle_lo,angle_hi]
// Encoder: 15-bit (32768 cpr). Torque constant: 0.27 Nm/A.

const ENC_MAX: f64 = 32768.0;
const RPM_MAX: f64 = 1000.0;
const CUR_MAX: f64 = 10.0;
const CUR_SCALE: f64 = 32767.0 / CUR_MAX;
const SPEED_SCALE: f64 = 32767.0 / RPM_MAX;
const ANGLE_SCALE: f64 = 65535.0 / (2.0 * PI);
const TORQUE_K: f64 = 0.27;

pub struct CanMotor {
    pub can_id: u32,
    raw_feedback: AtomicU64,
    pub angle: f64, pub velocity: f64, pub torque: f64, pub temperature: f64,
    pub ctrl_torque: f64, pub ctrl_velocity: f64, pub ctrl_angle: f64,
    pub enabled: bool,
}

impl CanMotor {
    pub fn new(can_id: u32) -> Self {
        Self { can_id, raw_feedback: AtomicU64::new(0),
               angle: 0.0, velocity: 0.0, torque: 0.0, temperature: 0.0,
               ctrl_torque: f64::NAN, ctrl_velocity: f64::NAN, ctrl_angle: f64::NAN,
               enabled: false }
    }

    pub fn store_status(&self, data: &[u8; 8]) {
        self.raw_feedback.store(u64::from_le_bytes(*data), Ordering::Relaxed);
    }

    pub fn update_status(&mut self) {
        let raw = self.raw_feedback.load(Ordering::Relaxed);
        let status    = (raw & 0xFF) as u8;
        let raw_cur   = ((raw >> 16) & 0xFFFF) as i16;
        let raw_speed = ((raw >> 32) & 0xFFFF) as i16;
        let raw_ang   = ((raw >> 48) & 0xFFFF) as u16;

        self.enabled = (status & 0x01) != 0;
        let amps = raw_cur as f64 / CUR_SCALE;
        let rpm  = raw_speed as f64 / SPEED_SCALE;
        self.torque   = amps * TORQUE_K;
        self.velocity = rpm / 60.0 * 2.0 * PI;  // rad/s
        self.angle    = raw_ang as f64 / ANGLE_SCALE;
        self.temperature = 0.0;
    }

    pub fn generate_command(&self) -> Option<u64> {
        if !self.ctrl_angle.is_nan()    { return Some(build_cmd(0x05, angle_to_raw(self.ctrl_angle) as i16)); }
        if !self.ctrl_velocity.is_nan() { return Some(build_cmd(0x04, speed_to_raw(self.ctrl_velocity))); }
        if !self.ctrl_torque.is_nan()   { return Some(build_cmd(0x03, current_to_raw(self.ctrl_torque))); }
        Some(build_cmd(0x00, 0))  // NOP
    }

    pub fn can_rx_id(&self) -> u32 { 0x500 + (self.can_id & 0x0F) }
}

fn current_to_raw(torque: f64) -> i16 {
    let amps = (torque / TORQUE_K).clamp(-CUR_MAX, CUR_MAX);
    (amps * CUR_SCALE).round() as i16
}
fn speed_to_raw(rad_s: f64) -> i16 {
    let rpm = (rad_s / (2.0 * PI) * 60.0).clamp(-RPM_MAX, RPM_MAX);
    (rpm * SPEED_SCALE).round() as i16
}
fn angle_to_raw(rad: f64) -> u16 {
    let norm = rad % (2.0 * PI);
    let norm = if norm < 0.0 { norm + 2.0 * PI } else { norm };
    (norm * ANGLE_SCALE).round() as u16
}
fn build_cmd(cmd: u8, val: i16) -> u64 {
    0x400 | ((cmd as u64) << 16) | ((val as u16 as u64) << 24)
}

// ── Bmi088 IMU with Mahony AHRS ────────────────────────────────────

pub struct Bmi088 {
    pub q0: f64, pub q1: f64, pub q2: f64, pub q3: f64,
    pub ax: f64, pub ay: f64, pub az: f64, pub gx: f64, pub gy: f64, pub gz: f64,
    kp: f64, ki: f64, integral_fb: (f64, f64, f64), sample_freq: f64,
    accel_scale: f64, gyro_scale: f64,
}

impl Bmi088 {
    pub fn new(sample_freq: f64, kp: f64, ki: f64) -> Self {
        Self { q0: 1.0, q1: 0.0, q2: 0.0, q3: 0.0, ax: 0.0, ay: 0.0, az: 0.0,
               gx: 0.0, gy: 0.0, gz: 0.0, kp, ki, integral_fb: (0.0, 0.0, 0.0),
               sample_freq, accel_scale: 6.0 / 32768.0, gyro_scale: (2000.0 * PI / 180.0) / 32768.0 }
    }
    pub fn store_sample(&self, _ax: i16, _ay: i16, _az: i16, _gx: i16, _gy: i16, _gz: i16) {}
    pub fn update_status(&mut self) {}

    pub fn update(&mut self, raw_ax: i16, raw_ay: i16, raw_az: i16,
                  raw_gx: i16, raw_gy: i16, raw_gz: i16) {
        let ax = raw_ax as f64 * self.accel_scale; let ay = raw_ay as f64 * self.accel_scale;
        let az = raw_az as f64 * self.accel_scale;
        let gx = raw_gx as f64 * self.gyro_scale; let gy = raw_gy as f64 * self.gyro_scale;
        let gz = raw_gz as f64 * self.gyro_scale;
        let norm = (ax*ax + ay*ay + az*az).sqrt();
        if norm < 1e-6 { return; }
        let (ax_n, ay_n, az_n) = (ax/norm, ay/norm, az/norm);
        let (q0,q1,q2,q3) = (self.q0, self.q1, self.q2, self.q3);
        let vx = 2.0*(q1*q3 - q0*q2); let vy = 2.0*(q0*q1 + q2*q3);
        let vz = q0*q0 - q1*q1 - q2*q2 + q3*q3;
        let (ex, ey, ez) = (ay_n*vz - az_n*vy, az_n*vx - ax_n*vz, ax_n*vy - ay_n*vx);
        self.integral_fb.0 += self.ki * ex; self.integral_fb.1 += self.ki * ey; self.integral_fb.2 += self.ki * ez;
        let gx_c = gx + self.kp*ex + self.integral_fb.0;
        let gy_c = gy + self.kp*ey + self.integral_fb.1;
        let gz_c = gz + self.kp*ez + self.integral_fb.2;
        let half = 0.5 / self.sample_freq;
        self.q0 += (-q1*gx_c - q2*gy_c - q3*gz_c) * half;
        self.q1 += ( q0*gx_c + q2*gz_c - q3*gy_c) * half;
        self.q2 += ( q0*gy_c - q1*gz_c + q3*gx_c) * half;
        self.q3 += ( q0*gz_c + q1*gy_c - q2*gx_c) * half;
        let nq = (self.q0*self.q0+self.q1*self.q1+self.q2*self.q2+self.q3*self.q3).sqrt();
        self.q0/=nq; self.q1/=nq; self.q2/=nq; self.q3/=nq;
        self.ax=ax; self.ay=ay; self.az=az; self.gx=gx; self.gy=gy; self.gz=gz;
    }
}
