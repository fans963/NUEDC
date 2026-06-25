//! MotorTest — sine-wave test signal. Moved from main.rs.

use crate::core::component::{Component, InputDecl, Output, OutputDecl};
use crate::core::config::Config;
use crate::register;
use crate::register_component;
use serde_yaml::Value;
use std::f64::consts::TAU;
use std::time::Instant;

pub struct MotorTest {
    name: String,
    left_speed: Output<f32>, right_speed: Output<f32>,
    speed_max: f32, period_sec: f32, start: Instant,
}

impl MotorTest {
    pub fn from_yaml(name: &str, config: &Value) -> Self {
        let c = Config::new(config);
        Self { name: name.into(), left_speed: Output::new("/chassis/left_encoder/target_speed", 0.0),
               right_speed: Output::new("/chassis/right_encoder/target_speed", 0.0),
               speed_max: c.child("speed_max").get(5.0), period_sec: c.child("period_sec").get(2.0),
               start: Instant::now() }
    }
}

impl Component for MotorTest {
    fn name(&self) -> &str { &self.name }
    fn set_name(&mut self, n: String) { self.name = n; }
    fn outputs(&mut self, out: &mut Vec<OutputDecl>) { register!(out, out, self.left_speed, self.right_speed); }
    fn inputs(&mut self, _i: &mut Vec<InputDecl>) {}
    fn update(&mut self) {
        let t = self.start.elapsed().as_secs_f64();
        let v = self.speed_max * (TAU as f32 / self.period_sec * t as f32).sin();
        self.left_speed.set(v); self.right_speed.set(v);
    }
}

register_component!(MotorTest::from_yaml, "nuedcs::test::MotorTest");
