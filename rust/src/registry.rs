//! Component registry — compile-time registration via `inventory`.
//! Equivalent to C++ `register_namespace_components<^^ns>()`.
//!
//! config.yaml must use the full type name: `nuedcs::hardware::Car -> car`.

use crate::core::component::Component;
use serde_yaml::Value;

pub struct Factory {
    pub type_name: &'static str,
    pub create: fn(name: &str, config: &Value) -> Option<Box<dyn Component>>,
}

inventory::collect!(Factory);

pub fn create_component(type_name: &str, instance_name: &str, config: &Value) -> Option<Box<dyn Component>> {
    for f in inventory::iter::<Factory> {
        if f.type_name == type_name {
            return (f.create)(instance_name, config);
        }
    }
    None
}

#[macro_export]
macro_rules! register_component {
    ($ctor:path, $type_name:literal) => {
        inventory::submit! {
            $crate::registry::Factory {
                type_name: $type_name,
                create: |name, config| Some(Box::new($ctor(name, config))),
            }
        }
    };
}
