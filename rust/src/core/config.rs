//! YAML config wrapper — equivalent to C++ `core::Config`.
//!
//! Usage:
//!   let c = Config::new(&yaml);
//!   let val: f64 = c.child("key").get(3.14);
//!   let s = c.child("path").child("to").str("default");

use serde_yaml::Value;
use log::warn;

pub struct Config<'a> {
    root: &'a Value,
    path: String,
    dead: bool,
}

impl<'a> Config<'a> {
    pub fn new(root: &'a Value) -> Self {
        Self { root, path: String::new(), dead: false }
    }

    pub fn child(&self, key: &str) -> Self {
        if self.dead {
            return Self { root: self.root, path: format!("{}/{}", self.path, key), dead: true };
        }
        match self.root.get(key) {
            Some(v) => Self { root: v, path: format!("{}/{}", self.path, key), dead: false },
            None => {
                warn!("config key '{}' not set, using default", format!("{}/{}", self.path, key));
                Self { root: self.root, path: format!("{}/{}", self.path, key), dead: true }
            }
        }
    }

    pub fn get<T>(&self, default: T) -> T
    where T: serde::de::DeserializeOwned + Clone + std::fmt::Display
    {
        if !self.dead {
            if let Ok(v) = T::deserialize(self.root.clone()) {
                return v;
            }
        }
        if !self.path.is_empty() { warn!("config key '{}' → default", self.path); }
        default
    }

    pub fn str(&self, default: &str) -> String {
        if !self.dead {
            if let Some(s) = self.root.as_str() {
                return s.to_string();
            }
        }
        if !self.path.is_empty() { warn!("config key '{}' → default", self.path); }
        default.to_string()
    }

    pub fn is_map(&self) -> bool { self.root.is_mapping() }
    pub fn is_seq(&self) -> bool { self.root.is_sequence() }
    pub fn has_child(&self, key: &str) -> bool {
        !self.dead && self.root.get(key).is_some()
    }

    pub fn children(&self) -> Option<Vec<(String, Config<'_>)>> {
        self.root.as_mapping().map(|m| {
            m.iter().map(|(k, v)| {
                let key = k.as_str().unwrap_or("").to_string();
                (key.clone(), Config { root: v, path: key, dead: false })
            }).collect()
        })
    }
}
