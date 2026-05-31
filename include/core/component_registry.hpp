#pragma once

#include "component.hpp"

#include <functional>
#include <memory>
#include <spdlog/spdlog.h>
#include <string>
#include <unordered_map>

namespace nuedcs::core {

class ComponentRegistry {
public:
    using Factory = std::function<std::unique_ptr<Component>(const char* instance_name)>;

    static ComponentRegistry& instance()
    {
        static ComponentRegistry reg;
        return reg;
    }

    void add(const std::string& type_name, Factory f)
    {
        factories_[type_name] = std::move(f);
    }

    std::unique_ptr<Component> create(const std::string& type_name, const char* instance_name) const
    {
        auto it = factories_.find(type_name);
        if (it == factories_.end()) {
            spdlog::error("[Registry] Unknown component type: '{}'", type_name);
            return nullptr;
        }
        return it->second(instance_name);
    }

    bool has(const std::string& type_name) const
    {
        return factories_.count(type_name) > 0;
    }

private:
    ComponentRegistry() = default;
    std::unordered_map<std::string, Factory> factories_;
};

} // namespace nuedcs::core
