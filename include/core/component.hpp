#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <unordered_set>
#include <vector>

#include <ryml/ryml.hpp>
#include <ryml/ryml_std.hpp>
#include <spdlog/spdlog.h>

#include <meta>  // C++26 reflection (P2996) — requires -freflection
#include "component_registry.hpp"

namespace nuedcs::core {

class Executor;
class Component;

inline thread_local const char* tl_component_name = nullptr;

// 共享 logger，所有组件共用，组件名通过格式化参数传入
inline std::shared_ptr<spdlog::logger> shared_logger() {
    static auto logger = [] {
        auto l = std::make_shared<spdlog::logger>("comp", spdlog::default_logger()->sinks().begin(),
            spdlog::default_logger()->sinks().end());
        l->set_pattern("[%H:%M:%S.%e] [%^%l%$] [%n] %v");
        return l;
    }();
    return logger;
}

// ── Config access helper ──────────────────────────────────────────────
// Thin wrapper over ryml::NodeRef. Missing keys at any depth emit a warning.
//
//   Config config{root};
//   auto vid = config["vid"].get<uint16_t>(0x1209);
//   auto wb  = config["chassis"]["wheel_base"].get<double>(0.2);
//   auto dev = config["camera"]["device"].str("/dev/video0");

struct Config {
    ryml::NodeRef root;

    struct Key {
        ryml::NodeRef node;
        std::string   path;
        bool          dead = false;

        Key operator[](const char* sub) {
            if (dead) return {node, path + "/" + sub, true};
            auto child = node[sub];
            if (child.invalid() || !child.readable()) {
                warn(path + "/" + sub);
                return {node, path + "/" + sub, true};
            }
            return {node.find_child(c4::to_csubstr(sub)), path + "/" + sub, false};
        }

        template <typename T>
        T get(T def) const {
            if (!dead && !node.invalid() && node.readable() && node.has_val()) {
                T val; node >> val; return val;
            }
            if (dead || !node.has_val()) warn(path);
            return def;
        }

        std::string str(const char* def = "") const {
            if (!dead && !node.invalid() && node.readable() && node.has_val()) {
                c4::csubstr s = node.val();
                return {s.begin(), s.size()};
            }
            if (dead || !node.has_val()) warn(path);
            return def;
        }

        static void warn(const std::string& p) {
            if (tl_component_name)
                spdlog::warn("[{}] config key '{}' not set, using default", tl_component_name, p);
            else
                spdlog::warn("config key '{}' not set, using default", p);
        }
    };

    Key operator[](const char* key) {
        auto child = root[key];
        if (!child.invalid() && child.readable())
            return {root.find_child(c4::to_csubstr(key)), key, false};
        Key::warn(key);
        return {root, key, true};
    }
};


class Component {
public:
    friend class Executor;

    Component(const Component&)            = delete;
    Component& operator=(const Component&) = delete;
    Component(Component&&)                 = delete;
    Component& operator=(Component&&)      = delete;

    virtual ~Component() = default;

    [[nodiscard]] const std::string& name() const { return name_; }

    // 便捷日志方法，自动带组件名前缀
    template <typename... Args>
    void info(fmt::format_string<Args...> fmt, Args&&... args) {
        shared_logger()->log(spdlog::source_loc { }, spdlog::level::info, "[{}] {}", name_,
            fmt::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void warn(fmt::format_string<Args...> fmt, Args&&... args) {
        shared_logger()->log(spdlog::source_loc { }, spdlog::level::warn, "[{}] {}", name_,
            fmt::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void error(fmt::format_string<Args...> fmt, Args&&... args) {
        shared_logger()->log(spdlog::source_loc { }, spdlog::level::err, "[{}] {}", name_,
            fmt::format(fmt, std::forward<Args>(args)...));
    }

    virtual bool init() { return true; }
    virtual void update() = 0;
    virtual void before_pairing(const std::map<std::string, const std::type_info&>&) { }
    virtual void before_updating() { }

    // --- Input/Output ---
    template <typename T>
        requires(!std::is_reference_v<T> && !std::is_unbounded_array_v<T>)
    class InputInterface {
    public:
        friend class Component;
        InputInterface()                                 = default;
        InputInterface(const InputInterface&)            = delete;
        InputInterface& operator=(const InputInterface&) = delete;
        InputInterface(InputInterface&&)                 = delete;
        InputInterface& operator=(InputInterface&&)      = delete;
        ~InputInterface() {
            if (del_) {
                if constexpr (std::is_array_v<T>) delete[] p_;
                else delete p_;
            }
        }
        [[nodiscard]] bool active() const { return act_; }
        [[nodiscard]] bool ready() const { return p_ != nullptr; }
        [[nodiscard]] void* raw_ptr() const { return static_cast<void*>(p_); }
        template <typename... Args>
        void make_and_bind_directly(Args&&... a) {
            if (ready()) throw std::runtime_error("already bound");
            p_   = new T(std::forward<Args>(a)...);
            act_ = true;
            del_ = true;
        }
        void bind_directly(T& d) {
            if (ready()) throw std::runtime_error("already bound");
            p_   = &d;
            act_ = true;
        }
        const T* operator->() const { return p_; }
        const T& operator*() const { return *p_; }

    private:
        void** activate() {
            act_ = true;
            return reinterpret_cast<void**>(&p_);
        }
        T* p_     = nullptr;
        bool act_ = false;
        bool del_ = false;
    };

    template <typename T>
        requires(!std::is_reference_v<T> && !std::is_unbounded_array_v<T>)
    class OutputInterface {
    public:
        friend class Component;
        OutputInterface()                                  = default;
        OutputInterface(const OutputInterface&)            = delete;
        OutputInterface& operator=(const OutputInterface&) = delete;
        OutputInterface(OutputInterface&&)                 = delete;
        OutputInterface& operator=(OutputInterface&&)      = delete;
        ~OutputInterface() {
            if (active()) std::destroy_at(std::launder(reinterpret_cast<T*>(&d_)));
        }
        [[nodiscard]] bool active() const { return act_; }
        T* operator->() { return reinterpret_cast<T*>(&d_); }
        const T* operator->() const { return reinterpret_cast<const T*>(&d_); }
        T& operator*() { return *reinterpret_cast<T*>(&d_); }
        const T& operator*() const { return *reinterpret_cast<const T*>(&d_); }

    private:
        template <typename... Args>
        void* activate(Args&&... a) {
            ::new (&d_) T(std::forward<Args>(a)...);
            act_ = true;
            return reinterpret_cast<void*>(&d_);
        }
        alignas(T) std::byte d_[sizeof(T)];
        bool act_ = false;
    };

    template <typename T>
    void register_input(std::string n, InputInterface<T>& i, bool req = true) {
        if (i.active()) throw std::runtime_error("already activated");
        inputs_.emplace_back(typeid(T), std::move(n), req, i.activate());
    }

    template <typename T, typename... Args>
    void register_output(std::string n, OutputInterface<T>& i, Args&&... a) {
        if (i.active()) throw std::runtime_error("already activated");
        outputs_.emplace_back(typeid(T), std::move(n), i.activate(std::forward<Args>(a)...), this);
    }

    template <typename T, typename... Args>
    T* create_partner_component(const std::string& name, Args&&... args) {
        tl_component_name = name.c_str();
        auto component = std::make_unique<T>(std::forward<Args>(args)...);
        auto* ptr = component.get();
        partner_component_list_.push_back(std::move(component));
        return ptr;
    }

    Component() {
        if (tl_component_name) {
            name_             = tl_component_name;
            tl_component_name = nullptr;
        }
    }

protected:
    void set_name(std::string_view n) { name_ = n; }

private:
    std::string name_;

    struct InputDecl {
        const std::type_info& type;
        std::string name;
        bool req;
        void** ptr;
    };
    struct OutputDecl {
        const std::type_info& type;
        std::string name;
        void* data;
        Component* owner;
    };

    std::vector<InputDecl> inputs_;
    std::vector<OutputDecl> outputs_;
    std::vector<std::unique_ptr<Component>> partner_component_list_;
    size_t dep_count_ = 0;
    std::unordered_set<Component*> wanted_by_;
};

// ── C++26 reflection-based component registration ─────────────────────
//
// No macro needed. To register a component:
//   1. Inherit from both Component and register_component
//   2. In main.cpp, call register_namespace_components<^^ns>() for each
//      namespace — it auto-discovers all classes via std::meta::members_of().

/// Register a single component type. Class name extracted from AST.
template <typename T>
    requires(std::is_base_of_v<Component, T> && std::is_class_v<T> && !std::is_abstract_v<T>)
inline void register_component_type() {
    constexpr auto name = std::define_static_string(std::meta::display_string_of(^^T));
    ComponentRegistry::instance().add(
        name, [](const char* inst, ryml::NodeRef config) -> std::unique_ptr<Component> {
            tl_component_name = inst;
            return std::make_unique<T>(config);
        });
}

/// Enumerate all classes in a namespace via std::meta::members_of(),
/// and auto-register those that inherit from Component.
/// Requires all component headers for that namespace to be included beforehand.
template <auto NS>
    requires(std::meta::is_namespace(NS))
inline void register_namespace_components() {
    constexpr auto ctx = std::meta::access_context::unchecked();
    template for (constexpr auto m : std::define_static_array(std::meta::members_of(NS, ctx))) {
        if constexpr (std::meta::is_type(m)
                      && std::meta::is_class_type(m)
                      && !std::meta::is_abstract_type(m)) {
            using T = [:m:];
            if constexpr (std::is_base_of_v<Component, T>) {
                register_component_type<T>();
            }
        }
    }
}

} // namespace nuedcs::core
