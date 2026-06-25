//! Wire protocol: 0x5A | u32_le size | FlatBuffer body | 0xA5
//! Equivalent to C++ usb/protocol.hpp.

pub const FRAME_HEADER: u8 = 0x5A;
pub const FRAME_TAIL: u8 = 0xA5;
pub const MAX_FRAME_LEN: usize = 512;

/// Encode data into wire format. Returns wire length.
pub fn encode_into(data: &[u8], wire: &mut [u8]) -> usize {
    wire[0] = FRAME_HEADER;
    let len = data.len() as u32;
    wire[1..5].copy_from_slice(&len.to_le_bytes());
    let body_end = 5 + data.len();
    wire[5..body_end].copy_from_slice(data);
    wire[body_end] = FRAME_TAIL;
    body_end + 1
}

// ── Handler trait — default empty methods (like C++ if constexpr) ─

pub trait Handler {
    fn handle_imu(&mut self, _ax: f32, _ay: f32, _az: f32, _gx: f32, _gy: f32, _gz: f32) {}
    fn handle_encoder(&mut self, _id: u8, _velocity: f32) {}
    fn handle_adc(&mut self, _idx: u8, _channels: &[u8]) {}
    fn handle_can_rx(&mut self, _idx: u8, _id: u32, _dlc: u8, _data: &[u8]) {}
    fn handle_uart_rx(&mut self, _idx: u8, _data: &[u8]) {}
}

// ── Protocol decoder state machine ──────────────────────────────────

#[derive(Debug, Clone, Copy, PartialEq)]
enum DecodeState { WaitHead, ReadSize { size: u32, count: u8 }, ReadBody { size: u32, count: usize }, CheckTail }

pub struct ProtocolDecoder<F: FnMut(&[u8])> {
    state: DecodeState,
    buf: Vec<u8>,
    on_frame: F,
}

impl<F: FnMut(&[u8])> ProtocolDecoder<F> {
    pub fn new(on_frame: F) -> Self {
        Self { state: DecodeState::WaitHead, buf: Vec::new(), on_frame }
    }

    pub fn feed(&mut self, data: &[u8]) {
        for &byte in data {
            match self.state {
                DecodeState::WaitHead => {
                    if byte == FRAME_HEADER {
                        self.state = DecodeState::ReadSize { size: 0, count: 0 };
                    }
                }
                DecodeState::ReadSize { size, count } => {
                    let s = size | ((byte as u32) << (count * 8));
                    if count == 3 {
                        if s as usize > MAX_FRAME_LEN { self.state = DecodeState::WaitHead; }
                        else { self.state = DecodeState::ReadBody { size: s, count: 0 }; self.buf.clear(); }
                    } else { self.state = DecodeState::ReadSize { size: s, count: count + 1 }; }
                }
                DecodeState::ReadBody { size, count } => {
                    self.buf.push(byte);
                    if count + 1 == size as usize { self.state = DecodeState::CheckTail; }
                    else { self.state = DecodeState::ReadBody { size, count: count + 1 }; }
                }
                DecodeState::CheckTail => {
                    if byte == FRAME_TAIL { (self.on_frame)(&self.buf); }
                    self.state = DecodeState::WaitHead;
                }
            }
        }
    }
}
