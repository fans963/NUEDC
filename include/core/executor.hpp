#pragma once

#include "component.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <csignal>
#include <memory>
#include <string>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nuedcs::core {

class Executor {
public:
    Executor() = default;

    void add_component(std::unique_ptr<Component> c)
    {
        if (!c) return;
        auto partners = std::move(c->partner_component_list_);
        components_.push_back(std::move(c));
        for (auto& p : partners)
            add_component(std::move(p));
    }

    bool pair()
    {
        struct Entry { void* data; Component* owner; };
        std::unordered_map<std::string, Entry> outs;

        for (auto& c : components_) {
            for (auto& o : c->outputs_) {
                auto key = std::string(o.type.name()) + ":" + o.name;
                if (auto [it, ok] = outs.emplace(key, Entry{o.data, c.get()}); !ok) {
                    spdlog::error("[Executor] Duplicate output '{}' on [{}] and [{}]",
                        o.name, c->name(), it->second.owner->name());
                    return false;
                }
            }
        }

        for (auto& c : components_) {
            c->before_pairing({});
            c->dep_count_ = 0;
            c->wanted_by_.clear();
        }

        for (auto& c : components_) {
            for (auto& inp : c->inputs_) {
                auto key = std::string(inp.type.name()) + ":" + inp.name;
                auto it  = outs.find(key);
                if (it != outs.end()) {
                    *inp.ptr = it->second.data;
                    if (it->second.owner != c.get()
                    && it->second.owner->wanted_by_.insert(c.get()).second) {
                        c->dep_count_++;
                    }
                } else if (inp.req) {
                    spdlog::error("[Executor] Input '{}' on '{}' has no matching output",
                        inp.name, c->name());
                    return false;
                }
            }
        }

        // DFS 拓扑排序 + 打印树形依赖链
        updating_order_.clear();
        depth_ = 0;

        spdlog::info("[Executor] ── Dependency chain ──────────────────────");
        for (auto& c : components_) {
            if (c->dep_count_ == 0) append_order(c.get());
        }
        spdlog::info("[Executor] ────────────────────────────────────────");

        if (updating_order_.size() < components_.size()) {
            // DFS 追踪环路径
            std::vector<Component*> path;
            std::unordered_set<Component*> visited;
            std::unordered_set<Component*> in_stack;
            bool found = false;

            auto dfs = [&](auto& self, Component* cur) -> void {
                if (found) return;
                visited.insert(cur);
                in_stack.insert(cur);
                path.push_back(cur);

                for (auto& inp : cur->inputs_) {
                    auto key = std::string(inp.type.name()) + ":" + inp.name;
                    auto it  = outs.find(key);
                    if (it == outs.end()) continue;
                    auto* next = it->second.owner;
                    if (next == cur) continue;
                    if (in_stack.contains(next)) {
                        path.push_back(next);
                        std::string cycle;
                        bool in = false;
                        for (size_t i = 0; i < path.size(); i++) {
                            if (path[i] == next) in = true;
                            if (in) {
                                if (!cycle.empty()) cycle += " → ";
                                cycle += path[i]->name();
                            }
                        }
                        spdlog::error("[Executor] Circular dependency: {}", cycle);
                        found = true;
                        return;
                    }
                    if (!visited.contains(next)) self(self, next);
                }
                path.pop_back();
                in_stack.erase(cur);
            };

            for (auto& c : components_) {
                if (!visited.contains(c.get()) && c->dep_count_ > 0) dfs(dfs, c.get());
                if (found) break;
            }
            if (!found)
                spdlog::error("[Executor] Circular dependency ({} unresolvable)",
                    components_.size() - updating_order_.size());
            return false;
        }

        // 按拓扑序重排
        std::vector<std::unique_ptr<Component>> sorted;
        sorted.reserve(components_.size());
        for (auto* raw : updating_order_) {
            for (auto& c : components_) {
                if (c.get() == raw) { sorted.push_back(std::move(c)); break; }
            }
        }
        components_ = std::move(sorted);

        // 释放配对阶段的临时数据
        for (auto& c : components_) {
            c->inputs_.clear();  c->inputs_.shrink_to_fit();
            c->outputs_.clear(); c->outputs_.shrink_to_fit();
            c->wanted_by_.clear();
        }

        spdlog::info("[Executor] Pairing OK: {} components, {} outputs",
            components_.size(), outs.size());
        return true;
    }

    bool init_all()
    {
        if (!pair()) return false;

        for (auto& c : components_) {
            spdlog::info("[Executor] Init: {}", c->name());
            if (!c->init()) {
                spdlog::error("[Executor] '{}' init failed!", c->name());
                return false;
            }
        }
        return true;
    }

    void start()
    {
        std::signal(SIGINT, handler);
        std::signal(SIGTERM, handler);
    }

    void run()
    {
        while (!quit_.load(std::memory_order::relaxed))
            for (auto& c : components_) c->update();
    }

    [[nodiscard]] const std::vector<std::unique_ptr<Component>>& components() const { return components_; }

private:
    void append_order(Component* comp)
    {
        std::string indent(depth_ * 4, ' ');
        spdlog::info("[Executor]   - {}{}", indent, comp->name());
        updating_order_.push_back(comp);

        for (auto& c : components_) {
            if (comp->wanted_by_.contains(c.get())) {
                if (--c->dep_count_ == 0) {
                    depth_++;
                    append_order(c.get());
                    depth_--;
                }
            }
        }
    }

    static inline std::atomic<bool> quit_ { false };
    static void handler(int) { quit_.store(true, std::memory_order::relaxed); }

    std::vector<std::unique_ptr<Component>> components_;
    std::vector<Component*> updating_order_;
    size_t depth_ = 0;
};

} // namespace nuedcs::core
