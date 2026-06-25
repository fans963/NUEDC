//! Kahn topological sort + spin-loop executor.
//! Equivalent to C++ core/executor.hpp.

use super::component;
use std::collections::{HashMap, HashSet};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

pub struct Executor {
    components: Vec<Box<dyn component::Component>>,
    update_order: Vec<(usize, usize)>,  // (component_index, depth)
    quit: Arc<AtomicBool>,
}

impl Executor {
    pub fn new() -> Self {
        Self { components: Vec::new(), update_order: Vec::new(), quit: Arc::new(AtomicBool::new(false)) }
    }

    pub fn add(&mut self, mut comp: Box<dyn component::Component>) {
        let partners = comp.take_partners();
        self.components.push(comp);
        for p in partners { self.add(p); }
    }

    pub fn request_quit(&self) { self.quit.store(true, Ordering::Relaxed); }

    pub fn pair(&mut self) -> Result<(), String> {
        let n = self.components.len();
        let mut outs: HashMap<(std::any::TypeId, String), (*const u8, usize)> = HashMap::new();
        for (i, c) in self.components.iter_mut().enumerate() {
            let mut decls = Vec::new();
            c.outputs(&mut decls);
            for o in decls {
                let key = (o.type_id, o.name.clone());
                if outs.contains_key(&key) {
                    return Err(format!("dup output '{}' on [{}]", o.name, c.name()));
                }
                outs.insert((o.type_id, o.name), (o.data, i));
            }
        }
        for c in &mut self.components { c.before_pairing(&HashMap::new()); }

        let mut dep_count = vec![0usize; n];
        let mut wanted_by = vec![HashSet::<usize>::new(); n];

        for (ci, c) in self.components.iter_mut().enumerate() {
            let mut decls = Vec::new();
            c.inputs(&mut decls);
            for inp in decls {
                let key = (inp.type_id, inp.name.clone());
                if let Some(&(data, owner_idx)) = outs.get(&key) {
                    unsafe { *inp.ptr = data; }
                    if owner_idx != ci && wanted_by[owner_idx].insert(ci) { dep_count[ci] += 1; }
                } else if inp.required {
                    return Err(format!("Input '{}' on '{}' unmatched", inp.name, c.name()));
                }
            }
        }

        self.update_order.clear();
        let roots: Vec<usize> = (0..n).filter(|&i| dep_count[i] == 0).collect();
        fn dfs(comp: usize, depth: usize, wb: &[HashSet<usize>], dc: &mut [usize],
               order: &mut Vec<(usize, usize)>, visited: &mut HashSet<usize>) {
            if !visited.insert(comp) { return; }
            order.push((comp, depth));
            for &child in &wb[comp] { dc[child] -= 1; if dc[child] == 0 { dfs(child, depth + 1, wb, dc, order, visited); } }
        }
        let mut visited = HashSet::new();
        for r in roots { dfs(r, 0, &wanted_by, &mut dep_count, &mut self.update_order, &mut visited); }
        if self.update_order.len() != n { return Err("cycle detected".into()); }
        Ok(())
    }

    pub fn run(&mut self) {
        let ptr = self.components.as_mut_ptr();
        for &(idx, _) in &self.update_order {
            if !unsafe { &mut *ptr.add(idx) }.init() {
                log::error!("'{}' init failed!", unsafe { &*ptr.add(idx) }.name());
                return;
            }
        }

        let quit = self.quit.clone();
        let _ = ctrlc::set_handler(move || {
            log::info!("SIGINT — stopping");
            quit.store(true, Ordering::Relaxed);
        });

        log::info!("Executor: {} components, running (Ctrl+C to stop)", self.update_order.len());
        while !self.quit.load(Ordering::Relaxed) {
            let ptr = self.components.as_mut_ptr();
            for &(idx, _) in &self.update_order { unsafe { &mut *ptr.add(idx) }.update(); }
        }
        log::info!("Executor: stopped");
    }

    pub fn print_deps(&self) {
        log::info!("── Dependency chain ──────────────────────");
        for &(idx, depth) in &self.update_order {
            let indent = "    ".repeat(depth);
            log::info!("{}- {}", indent, self.components[idx].name());
        }
        log::info!("────────────────────────────────────────");
        log::info!("Pairing OK: {} components", self.components.len());
    }
}
