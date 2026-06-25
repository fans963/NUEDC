//! Wrap FlatBuffer-generated code into separate sub-modules.

#[path = ""]
pub mod slave {
    #![allow(unused_imports, dead_code, non_snake_case)]
    include!(concat!(env!("OUT_DIR"), "/slave_to_host_generated.rs"));
}

#[path = ""]
pub mod host {
    #![allow(unused_imports, dead_code, non_snake_case)]
    include!(concat!(env!("OUT_DIR"), "/host_to_slave_generated.rs"));
}
