//! USB bulk transport — pure Rust via nusb.
//! RX: background thread with EndpointRead (implements std::io::Read).
//! TX: owning EndpointWrite on main thread.


use nusb::transfer;
use std::io::Read;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

pub struct UsbTransport {
    connected: Arc<AtomicBool>,
    running: Arc<AtomicBool>,
    tx: Option<nusb::Endpoint<transfer::Bulk, transfer::Out>>,
    thread: Option<thread::JoinHandle<()>>,
}

impl UsbTransport {
    pub fn new<F: Fn(&[u8]) + Send + 'static>(vid: u16, pid: u16, on_rx: F) -> Self {
        let connected = Arc::new(AtomicBool::new(false));
        let running = Arc::new(AtomicBool::new(true));
        let conn = connected.clone();
        let run = running.clone();
        let mut tx_endpoint = None;

        let thread = thread::spawn(move || {
            use nusb::transfer::{Bulk, In, Out};
            let iface = pollster::block_on(async {
                let devs = nusb::list_devices().await.ok()?;
                let d = devs.into_iter().find(|d| d.vendor_id() == vid && d.product_id() == pid)?;
                d.open().await.ok()?.claim_interface(0).await.ok()
            });
            let Some(iface) = iface else { return; };

            // Get IN endpoint for reading
            let ep_in = match iface.endpoint::<Bulk, In>(0x81) {
                Ok(ep) => ep,
                Err(_) => return,
            };
            let mut reader = ep_in.reader(512);
            reader.set_read_timeout(Duration::from_millis(1));

            conn.store(true, Ordering::Relaxed);
            let mut buf = [0u8; 512];

            while run.load(Ordering::Relaxed) {
                match reader.read(&mut buf) {
                    Ok(n) if n > 0 => on_rx(&buf[..n]),
                    Err(e) if e.kind() == std::io::ErrorKind::TimedOut
                           || e.kind() == std::io::ErrorKind::WouldBlock => {},
                    Err(_) => break,
                    _ => {}
                }
            }
            conn.store(false, Ordering::Relaxed);
        });

        Self { connected, running, tx: tx_endpoint, thread: Some(thread) }
    }

    pub fn connected(&self) -> bool { self.connected.load(Ordering::Relaxed) }
    pub fn stop(&mut self) {
        self.running.store(false, Ordering::Relaxed);
        if let Some(t) = self.thread.take() { let _ = t.join(); }
    }

    /// Open TX endpoint (must be called after device is connected).
    pub fn init_tx(&mut self, vid: u16, pid: u16) {
        use nusb::transfer::{Bulk, Out};
        let tx = pollster::block_on(async {
            let devs = nusb::list_devices().await.ok()?;
            let d = devs.into_iter().find(|d| d.vendor_id() == vid && d.product_id() == pid)?;
            let iface = d.open().await.ok()?.claim_interface(0).await.ok()?;
            iface.endpoint::<Bulk, Out>(0x01).ok()
        });
        self.tx = tx;
    }
}
