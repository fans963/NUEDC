pub mod core {
    pub mod component;
    pub mod config;
    pub mod executor;
}
pub mod devices;
pub mod hardware {
    pub mod car;
}
pub mod controller;
pub mod registry;
pub mod test {
    pub mod motor_test;
}
pub mod usb {
    pub mod framing;
    pub mod generated;
    pub mod nuedc_slave;
    pub mod transport;
}
pub mod util {
    pub mod throttle;
    pub mod ring_buffer;
    pub mod foxglove_bridge;
}
pub mod vision;

pub use core::component::{Component, Input, Output};
pub use core::config::Config;
pub use core::executor::Executor;
pub use util::foxglove_bridge::FoxgloveBridge;
pub use util::throttle::Throttle;
pub use vision::VisionTest;
