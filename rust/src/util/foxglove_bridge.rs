//! Foxglove WebSocket bridge — official `foxglove` crate.
//! Matches C++ foxglove_bridge.hpp but using native Rust SDK.

use crate::core::component::{Component, Input, OutputDecl, InputDecl};
use crate::core::config::Config;
use crate::util::throttle::Throttle;
use crate::register_component;
use foxglove::{ChannelBuilder, Schema, websocket::Capability};
use serde_yaml::Value;
use std::sync::Arc;
use std::io::Write;

// Safety: Executor is single-threaded, Input pointers never cross threads.
unsafe impl<T> Send for Input<T> {}

// ── Scalar channel ───────────────────────────────────────────────────

struct ScalarChan<T: Copy + 'static> {
    channel: Option<Arc<foxglove::RawChannel>>,
    input: Input<T>,
    throttle: Throttle,
    serialize: fn(T, &mut Vec<u8>),
}

fn ser_f64(v: f64, buf: &mut Vec<u8>) { write!(buf, r#"{{"value":{}}}"#, v).unwrap(); }
fn ser_f32(v: f32, buf: &mut Vec<u8>) { write!(buf, r#"{{"value":{}}}"#, v).unwrap(); }
fn ser_i32(v: i32, buf: &mut Vec<u8>) { write!(buf, r#"{{"value":{}}}"#, v).unwrap(); }

impl ScalarChan<f64> {
    fn new_f64(topic: &str, hz: f64) -> Box<dyn DynChannel> {
        Box::new(ScalarChan { channel: None, input: Input::new(topic),
                              throttle: Throttle::new(hz), serialize: ser_f64 })
    }
}
impl ScalarChan<f32> {
    fn new_f32(topic: &str, hz: f64) -> Box<dyn DynChannel> {
        Box::new(ScalarChan { channel: None, input: Input::new(topic),
                              throttle: Throttle::new(hz), serialize: ser_f32 })
    }
}
impl ScalarChan<i32> {
    fn new_i32(topic: &str, hz: f64) -> Box<dyn DynChannel> {
        Box::new(ScalarChan { channel: None, input: Input::new(topic),
                              throttle: Throttle::new(hz), serialize: ser_i32 })
    }
}

trait DynChannel: Send {
    fn create(&mut self);
    fn inputs(&mut self, inp: &mut Vec<InputDecl>);
    fn log(&mut self, _now_ns: u64);
}

impl<T: Copy + Send + 'static> DynChannel for ScalarChan<T> {
    fn create(&mut self) {
        let schema = Schema::new(
            "foxglove.TimestampedValue",
            "jsonschema",
            br#"{"type":"object","properties":{"value":{"type":"number"}}}"#,
        );
        match ChannelBuilder::new(&self.input.name)
            .message_encoding("json")
            .schema(schema)
            .build_raw()
        {
            Ok(ch) => self.channel = Some(ch),
            Err(e) => log::error!("[foxglove] channel create failed: {:?}", e),
        }
    }

    fn inputs(&mut self, inp: &mut Vec<InputDecl>) {
        inp.push(self.input.describe());
    }

    fn log(&mut self, _now_ns: u64) {
        if !self.throttle.ready() { return; }
        let Some(ref ch) = self.channel else { return; };
        if let Some(val) = self.input.read() {
            let mut buf = Vec::new();
            (self.serialize)(*val, &mut buf);
            ch.log(&buf);
        }
    }
}

// ── Image channel (official foxglove::messages::CompressedImage) ────

struct ImageChan {
    channel: Option<foxglove::Channel<foxglove::messages::CompressedImage>>,
    input: Input<Vec<u8>>,
    throttle: Throttle,
}

impl ImageChan {
    fn new(topic: &str, hz: f64) -> Box<dyn DynChannel> {
        Box::new(Self { channel: None, input: Input::new(topic), throttle: Throttle::new(hz) })
    }
}

impl DynChannel for ImageChan {
    fn create(&mut self) {
        self.channel = Some(foxglove::Channel::new(&self.input.name));
    }

    fn inputs(&mut self, inp: &mut Vec<InputDecl>) { inp.push(self.input.describe()); }

    fn log(&mut self, _now_ns: u64) {
        if !self.throttle.ready() { return; }
        let Some(ref ch) = self.channel else { return; };
        let Some(jpeg) = self.input.read() else { return; };
        if jpeg.is_empty() { return; }

        use foxglove::messages::CompressedImage;
        let msg = CompressedImage {
            timestamp: Some(Default::default()),
            frame_id: String::new(),
            data: bytes::Bytes::copy_from_slice(jpeg),
            format: "jpeg".into(),
        };
        ch.log(&msg);
    }
}

// ── FoxgloveBridge component ─────────────────────────────────────────

pub struct FoxgloveBridge {
    name: String,
    host: String,
    port: u16,
    channels: Vec<Box<dyn DynChannel>>,
}

impl FoxgloveBridge {
    pub fn from_config(name: &str, config: &Value) -> Self {
        let c = Config::new(config);
        let host = c.child("host").str("0.0.0.0");
        let port: u16 = c.child("port").get(8765);
        let mut channels: Vec<Box<dyn DynChannel>> = Vec::new();

        if let Some(chs) = c.child("channels").children() {
            for (topic, ch_cfg) in chs {
                let hz: f64 = ch_cfg.child("hz").get(10.0);
                match ch_cfg.child("type").str("").as_str() {
                    "float" => channels.push(ScalarChan::<f32>::new_f32(&topic, hz)),
                    "double" => channels.push(ScalarChan::<f64>::new_f64(&topic, hz)),
                    "int" | "int32" => channels.push(ScalarChan::<i32>::new_i32(&topic, hz)),
                    "image" => channels.push(ImageChan::new(&topic, hz)),
                    _ => log::warn!("[foxglove] unknown type for {}", topic),
                }
            }
        }

        FoxgloveBridge { name: name.into(), host, port, channels }
    }
}

impl Component for FoxgloveBridge {
    fn name(&self) -> &str { &self.name }
    fn set_name(&mut self, n: String) { self.name = n; }

    fn init(&mut self) -> bool {
        if self.channels.is_empty() {
            log::warn!("[foxglove] no channels");
            return true;
        }

        for ch in &mut self.channels { ch.create(); }

        // start() returns a Future — block on current thread's tokio runtime
        let rt = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .expect("tokio runtime");

        let handle = rt.block_on(async {
            foxglove::WebSocketServer::new()
                .name("NUEDC (Rust)")
                .bind(&self.host, self.port)
                .capabilities([Capability::ClientPublish])
                .supported_encodings([String::from("json")])
                .start()
                .await
        });

        match handle {
            Ok(_h) => {
                // Server started — keep runtime alive on background thread
                std::thread::spawn(move || { rt.block_on(async { tokio::signal::ctrl_c().await }); });
            }
            Err(e) => {
                log::error!("[foxglove] failed to start: {:?}", e);
                return false;
            }
        }

        log::info!("[foxglove] ws://{}:{} started, {} channels",
                       self.host, self.port, self.channels.len());
        true
    }

    fn outputs(&mut self, _out: &mut Vec<OutputDecl>) {}
    fn inputs(&mut self, inp: &mut Vec<InputDecl>) {
        for ch in &mut self.channels { ch.inputs(inp); }
    }

    fn update(&mut self) {
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_nanos() as u64;

        for ch in &mut self.channels { ch.log(now); }
    }
}

register_component!(FoxgloveBridge::from_config, "nuedcs::util::FoxgloveBridge");
