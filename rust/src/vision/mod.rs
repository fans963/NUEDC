//! VisionTest — camera capture on background thread.
//! Uses nokhwa (pure Rust) + image (pure Rust JPEG).

use crate::core::component::{Component, InputDecl, Output, OutputDecl};
use crate::core::config::Config;
use crate::register;
use crate::register_component;
use image::ImageEncoder;
use nokhwa::utils::{CameraIndex, RequestedFormat, RequestedFormatType};
use nokhwa::Camera;
use serde_yaml::Value;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;

register_component!(VisionTest::from_yaml, "nuedcs::vision::VisionTest");

pub struct VisionTest {
    name: String,
    device: u32, width: u32, height: u32,
    frame_width: Output<i32>, frame_height: Output<i32>,
    edge_count: Output<i32>, image_out: Output<Vec<u8>>,
    running: Arc<AtomicBool>,
    thread: Option<thread::JoinHandle<()>>,
    latest: Arc<Mutex<Latest>>,
}
struct Latest { width: i32, height: i32, image: Vec<u8> }

impl VisionTest {
    pub fn from_yaml(name: &str, config: &Value) -> Self {
        let c = Config::new(config);
        Self {
            name: name.into(),
            device: c.child("device").get(0u32),
            width: c.child("width").get(640u32),
            height: c.child("height").get(480u32),
            frame_width: Output::new("/vision/frame_width", 0),
            frame_height: Output::new("/vision/frame_height", 0),
            edge_count: Output::new("/vision/edge_count", 0),
            image_out: Output::new("/vision/image", Vec::new()),
            running: Arc::new(AtomicBool::new(false)),
            thread: None,
            latest: Arc::new(Mutex::new(Latest { width: 0, height: 0, image: Vec::new() })),
        }
    }

    fn capture_loop(latest: Arc<Mutex<Latest>>, running: Arc<AtomicBool>,
                    device: u32, width: u32, height: u32) {
        let fmt = RequestedFormat::new::<nokhwa::pixel_format::RgbFormat>(RequestedFormatType::AbsoluteHighestFrameRate);
        let idx = CameraIndex::Index(device);
        let mut camera = match Camera::new(idx, fmt) {
            Ok(c) => c,
            Err(e) => { log::error!("[vision] camera open: {}", e); return; }
        };
        if let Err(e) = camera.open_stream() {
            log::error!("[vision] stream open: {}", e);
            return;
        }

        while running.load(Ordering::Relaxed) {
            let frame = match camera.frame() {
                Ok(f) => f,
                Err(_) => continue,
            };
            let rgb = frame.decode_image::<nokhwa::pixel_format::RgbFormat>().ok();
            let w = frame.resolution().width_x as i32;
            let h = frame.resolution().height_y as i32;

            // JPEG encode via `image` crate
            let mut jpeg = Vec::new();
            if let Some(ref rgb_img) = rgb {
                let img = image::RgbImage::from_raw(w as u32, h as u32, rgb_img.to_vec());
                if let Some(img) = img {
                    let mut enc = image::codecs::jpeg::JpegEncoder::new_with_quality(&mut jpeg, 80);
                    let _ = enc.write_image(img.as_raw(), w as u32, h as u32, image::ExtendedColorType::Rgb8);
                }
            }

            let mut l = latest.lock().unwrap();
            l.width = w; l.height = h; l.image = jpeg;
        }
    }
}

impl Component for VisionTest {
    fn name(&self) -> &str { &self.name }
    fn set_name(&mut self, n: String) { self.name = n; }
    fn init(&mut self) -> bool {
        self.running.store(true, Ordering::Relaxed);
        let latest = self.latest.clone();
        let running = self.running.clone();
        let (d, w, h) = (self.device, self.width, self.height);
        self.thread = Some(thread::spawn(move || Self::capture_loop(latest, running, d, w, h)));
        log::info!("[vision] Camera started: {}x{} on device {}", self.width, self.height, self.device);
        true
    }
    fn outputs(&mut self, out: &mut Vec<OutputDecl>) {
        register!(out, out, self.frame_width, self.frame_height, self.edge_count, self.image_out);
    }
    fn inputs(&mut self, _i: &mut Vec<InputDecl>) {}
    fn update(&mut self) {
        let l = self.latest.lock().unwrap();
        self.frame_width.set(l.width);
        self.frame_height.set(l.height);
        self.image_out.set(l.image.clone());
    }
}

impl Drop for VisionTest {
    fn drop(&mut self) {
        self.running.store(false, Ordering::Relaxed);
        if let Some(t) = self.thread.take() { let _ = t.join(); }
    }
}
