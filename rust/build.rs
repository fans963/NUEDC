//! Generate Rust FlatBuffer code from shared .fbs schemas.
//! Equivalent to the CMake `flatc --cpp` step in C++.
//!
//! Output: $OUT_DIR/slave_to_host_generated.rs, host_to_slave_generated.rs

use std::process::Command;

fn main() {
    let schema_dir = std::path::Path::new("../schemas");
    let out_dir = std::env::var("OUT_DIR").unwrap();

    let schemas = [
        "slave_to_host.fbs",
        "host_to_slave.fbs",
    ];

    for fbs in &schemas {
        let status = Command::new("flatc")
            .args(["--rust", "-o", &out_dir])
            .arg(schema_dir.join(fbs))
            .status();

        match status {
            Ok(s) if s.success() => {
                println!("cargo:rerun-if-changed={}", schema_dir.join(fbs).display());
            }
            Ok(s) => {
                eprintln!("flatc failed for {}: exit {}", fbs, s);
                panic!("flatc failed");
            }
            Err(e) => {
                eprintln!("flatc not found: {}. Install: cargo install flatbuffers", e);
                // Don't fail — allow build without FlatBuffers for basic usage
            }
        }
    }

    // Also generate foxglove CompressedImage schema for completeness
    let foxglove_schema = schema_dir.join("CompressedImage.fbs");
    if foxglove_schema.exists() {
        let _ = Command::new("flatc")
            .args(["--rust", "-o", &out_dir])
            .arg(&foxglove_schema)
            .status();
        println!("cargo:rerun-if-changed={}", foxglove_schema.display());
    }
}
