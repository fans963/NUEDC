//! Zero-overhead component I/O system.
//!
//! Mirrors the C++ `component.hpp`:
//!   - `Output<T>` / `Input<T>` — type-erased via `Any` + raw pointers
//!   - No Rc / RefCell (unnecessary — Executor guarantees topological order)
//!   - `unsafe` is confined to the library internals
//!
//! Architecture:
//!   pairing:  `*input.ptr = &output.value`   (unsafe, once)
//!   update:   `*input.ptr`                   (wrapped in safe read())

use std::any::{Any, TypeId};
use std::cell::UnsafeCell;

// ── IoDecl: type-erased I/O descriptor (like C++ InputDecl / OutputDecl) ─

pub struct OutputDecl {
    pub type_id: TypeId,
    pub name: String,
    pub data: *const u8, // points into Output<T>.value
}

pub struct InputDecl {
    pub type_id: TypeId,
    pub name: String,
    pub ptr: *mut *const u8, // pointer-to-pointer to be set during pairing
    pub required: bool,
}

// ── Output<T> ───────────────────────────────────────────────────────

/// A published value. Stores data inline (like C++ `alignas(T) std::byte d_[sizeof(T)]`).
pub struct Output<T: 'static> {
    pub name: String,
    value: UnsafeCell<T>,
}

impl<T: 'static> Output<T> {
    pub fn new(name: impl Into<String>, initial: T) -> Self {
        Self { name: name.into(), value: UnsafeCell::new(initial) }
    }

    /// Write the output. Safe — UnsafeCell ensures no &T aliases exist.
    pub fn set(&self, val: T) {
        // SAFETY: Executor's topological order guarantees the output is
        // written before any input reads it. No concurrent access.
        unsafe { *self.value.get() = val; }
    }

    /// Read the output (used by tests / introspection).
    pub fn get(&self) -> &T {
        // SAFETY: single-threaded access guaranteed by Executor.
        unsafe { &*self.value.get() }
    }

    pub fn data_ptr(&self) -> *const u8 {
        self.value.get().cast::<u8>()
    }

    pub fn describe(&self) -> OutputDecl {
        OutputDecl {
            type_id: TypeId::of::<T>(),
            name: self.name.clone(),
            data: self.data_ptr(),
        }
    }
}

// ── Input<T> ───────────────────────────────────────────────────────

/// Reads a value published by another component's Output<T>.
/// The pointer is set once during Executor::pair().
pub struct Input<T: 'static> {
    pub name: String,
    pub required: bool,
    ptr: UnsafeCell<*const T>,
}

impl<T: 'static> Input<T> {
    pub fn new(name: impl Into<String>) -> Self {
        Self::new_opt(name, true)
    }

    pub fn optional(name: impl Into<String>) -> Self {
        Self::new_opt(name, false)
    }

    fn new_opt(name: impl Into<String>, required: bool) -> Self {
        Self { name: name.into(), required, ptr: UnsafeCell::new(std::ptr::null()) }
    }

    /// Read the paired value. Safe — raw pointer access wrapped here.
    pub fn read(&self) -> Option<&T> {
        // SAFETY: Pointer was set during pairing. Topological order
        // guarantees the owner Output wrote before this read.
        let p = unsafe { *self.ptr.get() };
        if p.is_null() { None } else { Some(unsafe { &*p }) }
    }

    pub fn ptr_addr(&self) -> *mut *const u8 {
        self.ptr.get().cast::<*const u8>()
    }

    pub fn describe(&self) -> InputDecl {
        InputDecl {
            type_id: TypeId::of::<T>(),
            name: self.name.clone(),
            ptr: self.ptr_addr(),
            required: self.required,
        }
    }
}

// ── Component trait ────────────────────────────────────────────────

pub trait Component: Any {
    fn name(&self) -> &str;
    fn init(&mut self) -> bool { true }

    /// Called before pairing — inspect available outputs, adjust config.
    fn before_pairing(&mut self, _all_outputs: &std::collections::HashMap<(std::any::TypeId, &str), ()>) {}

    /// Collect output descriptors for pairing.
    fn outputs(&mut self, out: &mut Vec<OutputDecl>);

    /// Collect input descriptors for pairing.
    fn inputs(&mut self, inp: &mut Vec<InputDecl>);

    /// Called once per spin-loop iteration. Paired inputs are valid.
    fn update(&mut self);

    /// Return partner components (C++ `create_partner_component`).
    /// Partners are added to the executor automatically via `add()`.
    fn take_partners(&mut self) -> Vec<Box<dyn Component>> { Vec::new() }

    /// Internal: for registry factories to set the component name.
    #[doc(hidden)]
    fn set_name(&mut self, _name: String) {}
}

// ── Helper macro for registering I/O ───────────────────────────────

/// Batch-register outputs and inputs in `outputs()` / `inputs()`.
/// Usage: `register!(out, out_vec, self.a, self.b, self.c)`
#[macro_export]
macro_rules! register {
    // Single item
    ($kind:ident, $vec:expr, $f:expr $(,)?) => {
        $vec.push($f.describe());
    };
    // Multiple items — peel off first and recurse
    ($kind:ident, $vec:expr, $f:expr, $($rest:expr),+ $(,)?) => {
        $vec.push($f.describe());
        register!($kind, $vec, $($rest),+);
    };
    // Empty
    ($kind:ident, $vec:expr $(,)?) => {};
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn output_set_get() {
        let o = Output::<f64>::new("v", 0.0);
        o.set(3.14);
        assert!((*o.get() - 3.14).abs() < 1e-10);
    }

    #[test]
    fn input_null_by_default() {
        let i = Input::<f64>::new("v");
        assert!(i.read().is_none());
    }

    #[test]
    fn input_after_pairing() {
        let o = Output::<f64>::new("v", 0.0);
        let i = Input::<f64>::new("v");

        // Simulate pairing
        unsafe { *i.ptr.get() = o.get() as *const f64; }

        o.set(2.71);
        assert!((*i.read().unwrap() - 2.71).abs() < 1e-10);
    }
}
