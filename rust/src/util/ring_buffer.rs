//! Lock-free SPSC ring buffer (equivalent to C++ util/ring_buffer.hpp).
//! Capacity must be a power of 2. Cache-line padded to prevent false sharing.

use std::sync::atomic::{AtomicUsize, Ordering};

#[repr(align(64))]
struct PaddedAtomic(AtomicUsize);

pub struct RingBuffer<T, const N: usize> {
    buf: Box<[Option<T>; N]>,
    read_idx: PaddedAtomic,
    write_idx: PaddedAtomic,
}

impl<T, const N: usize> RingBuffer<T, N> {
    pub fn new() -> Self {
        const { assert!(N.is_power_of_two(), "RingBuffer capacity must be power of 2"); }
        Self {
            buf: Box::new(std::array::from_fn(|_| None)),
            read_idx: PaddedAtomic(AtomicUsize::new(0)),
            write_idx: PaddedAtomic(AtomicUsize::new(0)),
        }
    }

    pub fn push(&self, val: T) -> bool {
        let w = self.write_idx.0.load(Ordering::Relaxed);
        let r = self.read_idx.0.load(Ordering::Acquire);
        if w.wrapping_sub(r) >= N { return false; }
        // SAFETY: single producer — only one thread writes to this index
        unsafe {
            let ptr = self.buf.as_ptr() as *mut Option<T>;
            (*ptr.add(w & (N - 1))) = Some(val);
        }
        self.write_idx.0.store(w.wrapping_add(1), Ordering::Release);
        true
    }

    pub fn pop(&self) -> Option<T> {
        let r = self.read_idx.0.load(Ordering::Relaxed);
        let w = self.write_idx.0.load(Ordering::Acquire);
        if r == w { return None; }
        // SAFETY: single consumer
        let val = unsafe {
            let ptr = self.buf.as_ptr() as *mut Option<T>;
            (*ptr.add(r & (N - 1))).take()
        };
        self.read_idx.0.store(r.wrapping_add(1), Ordering::Release);
        val
    }
}

unsafe impl<T: Send, const N: usize> Send for RingBuffer<T, N> {}
unsafe impl<T: Send, const N: usize> Sync for RingBuffer<T, N> {}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn push_pop() {
        let rb = RingBuffer::<i32, 4>::new();
        assert!(rb.push(1));
        assert!(rb.push(2));
        assert_eq!(rb.pop(), Some(1));
        assert_eq!(rb.pop(), Some(2));
        assert_eq!(rb.pop(), None);
    }
}
