//! Car + CarCommand with NuedcSlave USB integration.
//! Equivalent to C++ hardware/car.hpp.

use crate::controller::ChassisIk;
use crate::core::component::{Component, Input, InputDecl, Output, OutputDecl};
use crate::devices::{Bmi088, CanMotor, CanMotorConfig, EncoderMotor, MotorType};
use crate::register;
use crate::register_component;
use crate::usb::nuedc_slave::{Handler, NuedcSlave};
// Car implements Handler — see impl block below
use crate::util::throttle::Throttle;
use serde_yaml::Value;

register_component!(Car::from_yaml, "nuedcs::hardware::Car");

// ── CarCommand (partner, throttled command writer) ─────────────────

pub struct CarCommand {
    pub name: String,
    pub car_sync: Input<bool>,
    pub throttle: Throttle,
    pub enc_left: EncoderMotor,
    pub enc_right: EncoderMotor,
    pub can_left: CanMotor,
    pub can_right: CanMotor,
}

impl CarCommand {
    pub fn new(parent: &str) -> Self {
        Self {
            name: format!("{}_command", parent),
            car_sync: Input::optional(format!("{}/_car_sync", parent)),
            throttle: Throttle::new(1000.0),
            enc_left: EncoderMotor::new(0),
            enc_right: EncoderMotor::new(1),
            can_left: CanMotor::new(
                0x201,
                CanMotorConfig {
                    motor_type: MotorType::M3508,
                    reduction_ratio: 19.0,
                    reversed: true,
                },
            ),
            can_right: CanMotor::new(
                0x202,
                CanMotorConfig {
                    motor_type: MotorType::M3508,
                    reduction_ratio: 19.0,
                    reversed: false,
                },
            ),
        }
    }
}

impl Component for CarCommand {
    fn name(&self) -> &str {
        &self.name
    }
    fn outputs(&mut self, _o: &mut Vec<OutputDecl>) {}
    fn inputs(&mut self, i: &mut Vec<InputDecl>) {
        register!(in, i, self.car_sync);
    }
    fn update(&mut self) {
        if !self.throttle.ready() {
            return;
        }
        let _ = self.enc_left.generate_command();
        let _ = self.enc_right.generate_command();
        let _ = self.can_left.generate_command();
        let _ = self.can_right.generate_command();
    }
}

// ── Car ─────────────────────────────────────────────────────────────

pub struct Car {
    pub name: String,
    pub car_sync: Output<bool>,
    pub velocity: Output<f64>,
    pub yaw_rate: Output<f64>,
    pub accel_x: Output<f64>,
    pub accel_y: Output<f64>,
    pub accel_z: Output<f64>,
    pub gyro_x: Output<f64>,
    pub gyro_y: Output<f64>,
    pub gyro_z: Output<f64>,
    pub q0: Output<f64>,
    pub q1: Output<f64>,
    pub q2: Output<f64>,
    pub q3: Output<f64>,
    pub enc_left: EncoderMotor,
    pub enc_right: EncoderMotor,
    pub can_left: CanMotor,
    pub can_right: CanMotor,
    pub imu: Bmi088,
    pub chassis: ChassisIk,
    pub slave: Option<NuedcSlave>,
    command: Option<Box<CarCommand>>,
    pub was_connected: bool,
}

impl Car {
    pub fn from_yaml(name: &str, config: &Value) -> Self {
        let c = crate::core::config::Config::new(config);
        let vid: u16 = c.child("vid").get(0x1209);
        let pid: u16 = c.child("pid").get(0x0963);

        let mut car = Self {
            name: name.into(),
            car_sync: Output::new(format!("{}/_car_sync", name), true),
            velocity: Output::new("/chassis/velocity", 0.0),
            yaw_rate: Output::new("/chassis/yaw_rate", 0.0),
            accel_x: Output::new("/imu/accel_x", 0.0),
            accel_y: Output::new("/imu/accel_y", 0.0),
            accel_z: Output::new("/imu/accel_z", 0.0),
            gyro_x: Output::new("/imu/gyro_x", 0.0),
            gyro_y: Output::new("/imu/gyro_y", 0.0),
            gyro_z: Output::new("/imu/gyro_z", 0.0),
            q0: Output::new("/imu/q0", 1.0),
            q1: Output::new("/imu/q1", 0.0),
            q2: Output::new("/imu/q2", 0.0),
            q3: Output::new("/imu/q3", 0.0),
            enc_left: EncoderMotor::new(0),
            enc_right: EncoderMotor::new(1),
            can_left: CanMotor::new(
                0x201,
                CanMotorConfig {
                    motor_type: MotorType::M3508,
                    reduction_ratio: 19.0,
                    reversed: true,
                },
            ),
            can_right: CanMotor::new(
                0x202,
                CanMotorConfig {
                    motor_type: MotorType::M3508,
                    reduction_ratio: 19.0,
                    reversed: false,
                },
            ),
            imu: Bmi088::new(1000.0, 0.2, 0.0),
            chassis: ChassisIk::new(0.20, 0.05),
            slave: None,
            command: None,
            was_connected: false,
        };
        car.enc_left.configure(11);
        car.enc_right.configure(11);
        // NuedcSlave — Handler is Car, passed to pump() as &mut self
        car.slave = Some(NuedcSlave::new(vid, pid));
        car.command = Some(Box::new(CarCommand::new(name)));
        car
    }
}

// ── Handler impl — USB RX callbacks dispatched by NuedcSlave ───────

impl Handler for Car {
    fn handle_imu(&mut self, ax: f32, ay: f32, az: f32, gx: f32, gy: f32, gz: f32) {
        self.imu.update(
            ax as i16, ay as i16, az as i16, gx as i16, gy as i16, gz as i16,
        );
    }
    fn handle_encoder(&mut self, id: u8, velocity: f32) {
        log::info!("velocity:{}", velocity);
        if id == 0 {
            self.enc_left.store_velocity(velocity);
        }
        if id == 1 {
            self.enc_right.store_velocity(velocity);
        }
    }
}

impl Component for Car {
    fn name(&self) -> &str {
        &self.name
    }
    fn set_name(&mut self, n: String) {
        self.name = n;
    }
    fn outputs(&mut self, o: &mut Vec<OutputDecl>) {
        register!(
            out,
            o,
            self.car_sync,
            self.velocity,
            self.yaw_rate,
            self.accel_x,
            self.accel_y,
            self.accel_z,
            self.gyro_x,
            self.gyro_y,
            self.gyro_z,
            self.q0,
            self.q1,
            self.q2,
            self.q3
        );
    }
    fn inputs(&mut self, _i: &mut Vec<InputDecl>) {}

    fn init(&mut self) -> bool { true }

    fn take_partners(&mut self) -> Vec<Box<dyn Component>> {
        self.command
            .take()
            .into_iter()
            .map(|c| c as Box<dyn Component>)
            .collect()
    }

    fn update(&mut self) {
        // Pump USB RX → dispatch handle_imu / handle_encoder callbacks
        // SAFETY: pump() only accesses fields through &mut self (the handler),
        // which is the Car itself. The raw pointer cast avoids Rust's
        // borrow checker conflict between `slave` and `self` fields.
        let car_ptr = self as *mut Car;
        if let Some(ref mut slave) = self.slave {
            unsafe {
                slave.pump(&mut *car_ptr);
            }
            let now = slave.connected();
            if !self.was_connected && now {
                log::info!("[car] USB connected");
                self.was_connected = true;
            } else if self.was_connected && !now {
                log::error!("[car] USB lost, stopping");
                return;
            }
        }

        self.enc_left.update_status();
        self.enc_right.update_status();
        self.can_left.update_status();
        self.can_right.update_status();
        self.imu.update_status();

        let vl = self.enc_left.velocity as f64;
        let vr = self.enc_right.velocity as f64;
        self.velocity.set((vl + vr) / 2.0);
        self.yaw_rate.set((vr - vl) / 0.20);

        // IMU outputs
        self.accel_x.set(self.imu.ax);
        self.accel_y.set(self.imu.ay);
        self.accel_z.set(self.imu.az);
        self.gyro_x.set(self.imu.gx);
        self.gyro_y.set(self.imu.gy);
        self.gyro_z.set(self.imu.gz);
        self.q0.set(self.imu.q0);
        self.q1.set(self.imu.q1);
        self.q2.set(self.imu.q2);
        self.q3.set(self.imu.q3);
        self.car_sync.set(true);
    }
}

impl Drop for Car {
    fn drop(&mut self) {
        if let Some(ref mut slave) = self.slave {
            slave.stop();
        }
    }
}
