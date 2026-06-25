//! NuedcSlave — FlatBuffers protocol over USB. RX only for now.

use super::generated;
use super::transport::UsbTransport;
use crate::util::ring_buffer::RingBuffer;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use generated::slave::protocol::slave_to_host as rx;

const RX_RING_SIZE: usize = 256;
const MAX_FRAME_LEN: usize = 512;
struct RxFrame { data: Vec<u8> }

pub trait Handler {
    fn handle_imu(&mut self, _ax: f32, _ay: f32, _az: f32, _gx: f32, _gy: f32, _gz: f32) {}
    fn handle_encoder(&mut self, _id: u8, _velocity: f32) {}
}

pub struct NuedcSlave {
    connected: Arc<AtomicBool>,
    _transport: UsbTransport,
    rx_ring: Arc<RingBuffer<RxFrame, RX_RING_SIZE>>,
}

impl NuedcSlave {
    pub fn new(vid: u16, pid: u16) -> Self {
        let connected = Arc::new(AtomicBool::new(false));
        let rx_ring = Arc::new(RingBuffer::new());
        let conn = connected.clone();
        let rx = rx_ring.clone();

        let transport = UsbTransport::new(vid, pid, move |data| {
            feed_decoder(data, &rx);
        });
        if transport.connected() { conn.store(true, Ordering::Relaxed); }

        Self { connected, _transport: transport, rx_ring }
    }

    pub fn connected(&self) -> bool { self.connected.load(Ordering::Relaxed) }

    pub fn pump<H: Handler>(&mut self, handler: &mut H) {
        while let Some(frame) = self.rx_ring.pop() {
            Self::dispatch(handler, &frame.data);
        }
    }

    pub fn stop(&mut self) { self._transport.stop(); }

    fn dispatch<H: Handler>(handler: &mut H, data: &[u8]) {
        let frame = match flatbuffers::root::<rx::SlaveToHostFrame>(data) { Ok(f) => f, Err(_) => return };
        match frame.payload_type() {
            rx::MsgPayload::ImuPack => {
                if let Some(p) = frame.payload_as_imu_pack() {
                    handler.handle_imu(p.accel_x(), p.accel_y(), p.accel_z(), p.gyro_x(), p.gyro_y(), p.gyro_z());
                }
            }
            rx::MsgPayload::EncoderPack => {
                if let Some(p) = frame.payload_as_encoder_pack() {
                    handler.handle_encoder(p.encoder_id(), p.velocity_rad_s());
                }
            }
            _ => {}
        }
    }
}

fn feed_decoder(data: &[u8], rx_ring: &RingBuffer<RxFrame, RX_RING_SIZE>) {
    let mut state: u8 = 0; let mut size: u32 = 0; let mut count: u8 = 0;
    let mut buf = Vec::with_capacity(MAX_FRAME_LEN);
    for &byte in data {
        match state {
            0 => { if byte == 0x5A { state = 1; size = 0; count = 0; buf.clear(); } }
            1 => { size |= (byte as u32) << (count * 8); count += 1; if count == 4 {
                   if size <= MAX_FRAME_LEN as u32 { state = 2; count = 0; } else { state = 0; } } }
            2 => { buf.push(byte); count += 1; if count as u32 == size { state = 3; } }
            3 => { if byte == 0xA5 && !buf.is_empty() { rx_ring.push(RxFrame { data: buf.clone() }); } state = 0; }
            _ => state = 0,
        }
    }
}
