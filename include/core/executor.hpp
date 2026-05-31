#pragma once

#include "component_registry.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <memory>
#include <string>
#include <thread>
#include <typeinfo>
#include <unordered_map>
#include <vector>

namespace nuedcs::core {

class Executor {
public:
    Executor() = default;

    void add_component(std::unique_ptr<Component> c) { components_.push_back(std::move(c)); }

    void set_loop_hz(double hz) { loop_hz_ = hz; }

    bool pair() {
        // 收集所有 output
        struct Entry {
            void* data;
            Component* owner;
        };
        std::unordered_map<std::string, Entry> outs;

        for (auto& c : components_) {
            for (auto& o : c->outputs_) {
                outs[std::string(o.type.name()) + ":" + o.name] = { o.data, c.get() };
            }
        }

        // 重置依赖状态
        for (auto& c : components_) {
            c->before_pairing({ });
            c->dep_count_ = 0;
            c->wanted_by_.clear();
        }

        // 匹配 input → output（不提前失败，先构建完整依赖图）
        for (auto& c : components_) {
            for (auto& inp : c->inputs_) {
                auto key = std::string(inp.type.name()) + ":" + inp.name;
                auto it  = outs.find(key);
                if (it != outs.end()) {
                    *inp.ptr = it->second.data;
                    if (it->second.owner != c.get()) {
                        it->second.owner->wanted_by_.insert(c.get());
                        c->dep_count_++;
                    }
                } else if (inp.req) {
                    spdlog::warn("[Executor] Input '{}' on '{}' has no matching output (may be circular)",
                        inp.name, c->name());
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

            std::function<void(Component*)> dfs = [&](Component* cur) {
                if (found) return;
                visited.insert(cur);
                in_stack.insert(cur);
                path.push_back(cur);

                for (auto& inp : cur->inputs_) {
                    auto key = std::string(inp.type.name()) + ":" + inp.name;
                    auto it  = outs.find(key);
                    if (it == outs.end()) continue;
                    auto* next = it->second.owner;
                    if (next == cur) continue; // 自环跳过
                    if (in_stack.contains(next)) {
                        // 找到环，输出路径
                        path.push_back(next);
                        std::string cycle;
                        bool in_cycle = false;
                        for (size_t i = 0; i < path.size(); i++) {
                            if (path[i] == next) in_cycle = true;
                            if (in_cycle) {
                                if (!cycle.empty()) cycle += " → ";
                                cycle += path[i]->name();
                            }
                        }
                        spdlog::error("[Executor] Circular dependency: {}", cycle);
                        found = true;
                        return;
                    }
                    if (!visited.contains(next))
                        dfs(next);
                }
                path.pop_back();
                in_stack.erase(cur);
            };

            for (auto& c : components_) {
                if (!visited.contains(c.get()) && c->dep_count_ > 0)
                    dfs(c.get());
                if (found) break;
            }
            if (!found)
                spdlog::error("[Executor] Circular dependency detected ({} unresolvable)",
                    components_.size() - updating_order_.size());
            return false;
        }

        // 按拓扑序重排 components_
        std::vector<std::unique_ptr<Component>> sorted;
        sorted.reserve(components_.size());
        for (auto* raw : updating_order_) {
            for (auto& c : components_) {
                if (c.get() == raw) {
                    sorted.push_back(std::move(c));
                    break;
                }
            }
        }
        components_ = std::move(sorted);

        // 释放配对阶段的临时数据
        for (auto& c : components_) {
            c->inputs_.clear();  c->inputs_.shrink_to_fit();
            c->outputs_.clear(); c->outputs_.shrink_to_fit();
            c->wanted_by_.clear();
        }

        spdlog::info(
            "[Executor] Pairing OK: {} components, {} outputs", components_.size(), outs.size());
        return true;
    }

    bool init_all() {
        if (!pair()) return false;

        for (auto& c : components_) {
            if (!c->init()) {
                spdlog::error("[Executor] '{}' init failed!", c->name());
                return false;
            }
        }
        return true;
    }

    void start() {
        std::signal(SIGINT, handler);
        std::signal(SIGTERM, handler);
    }

    void run() {
        using clock = std::chrono::steady_clock;
        using ns    = std::chrono::nanoseconds;

        const auto period = ns(static_cast<int64_t>(1e9 / loop_hz_));

        auto next_tick  = clock::now();
        auto stats_time = clock::now();

        while (!quit_.load(std::memory_order::relaxed)) {
            for (auto& c : components_)
                c->update();

            next_tick += period;
            auto now = clock::now();
            if (now < next_tick) std::this_thread::sleep_until(next_tick);
            else next_tick = now;

            if (clock::now() - stats_time >= std::chrono::seconds(1)) {
                stats_time = clock::now();
            }
        }
        spdlog::info("[Executor] Shutting down");
    }

    [[nodiscard]] const std::vector<std::unique_ptr<Component>>& components() const {
        return components_;
    }

private:
    // 递归 DFS：打印依赖树 + 确定更新顺序
    void append_order(Component* comp) {
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

    double loop_hz_ = 1000.0;
    std::vector<std::unique_ptr<Component>> components_;
    std::vector<Component*> updating_order_;
    size_t depth_ = 0;
};

} // namespace nuedcs::core
