//! NUEDC Rust — auto-registering component system.
//!
//! Components self-register via `register_component!` macro.
//! main.rs only parses YAML and delegates to `registry::create_component()`.

use nuedc::core::executor::Executor;
use nuedc::registry;
use serde_yaml::Value;

fn main() {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();

    // Read config relative to executable, same as C++
    let exe_dir = std::env::current_exe()
        .and_then(|p| p.canonicalize()).unwrap_or_default()
        .parent().map(|p| p.to_path_buf()).unwrap_or_default();
    let cfg_path = exe_dir.join("..").join("config").join("config.yaml");
    let cfg_content = std::fs::read_to_string(&cfg_path).unwrap_or_else(|e| {
        log::error!("config not found at {}: {}", cfg_path.display(), e);
        std::process::exit(1);
    });
    let yaml: Value = serde_yaml::from_str(&cfg_content).unwrap_or_else(|e| {
        log::error!("config parse: {}", e); std::process::exit(1);
    });

    let mut exec = Executor::new();
    for entry in yaml["components"].as_sequence().unwrap_or(&vec![]) {
        let desc = entry.as_str().unwrap_or("");
        let (type_name, instance_name) = desc.split_once("->")
            .map(|(t, n)| (t.trim(), n.trim()))
            .unwrap_or((desc, desc));

        match registry::create_component(type_name, instance_name, &yaml[instance_name]) {
            Some(comp) => exec.add(comp),
            None => log::warn!("unknown type: {}", type_name),
        }
    }

    if let Err(e) = exec.pair() { log::error!("{}", e); return; }
    exec.print_deps();
    exec.run();
}
