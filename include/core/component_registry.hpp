#pragma once

#include <memory>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
#include <unordered_map>

#include <ryml/ryml.hpp>
#include <ryml/ryml_std.hpp>

// Forward declaration — avoids circular include with component.hpp.
// The full definition of Component is only needed in the factory lambda
// body, which lives in register_component_type<T>() template instantiation
// inside component headers that already include component.hpp.
namespace nuedcs::core {

class Component;

class ComponentRegistry {
public:
    using Factory = std::unique_ptr<Component> (*)(const char*, ryml::NodeRef);

    static ComponentRegistry& instance() {
        static ComponentRegistry reg;
        return reg;
    }

    void add(std::string_view type_name, Factory f) {
        factories_.emplace(std::string(type_name), f);
    }

    std::unique_ptr<Component> create(const std::string& type_name,
                                      const char* instance_name,
                                      ryml::NodeRef config) const;

    bool has(const std::string& type_name) const {
        return factories_.count(type_name) > 0;
    }

private:
    ComponentRegistry() = default;
    std::unordered_map<std::string, Factory> factories_;
};

} // namespace nuedcs::core
