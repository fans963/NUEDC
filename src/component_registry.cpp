#include "core/component_registry.hpp"
#include "core/component.hpp"

#include <spdlog/spdlog.h>

namespace nuedcs::core {

std::unique_ptr<Component> ComponentRegistry::create(const std::string& type_name,
                                                      const char* instance_name,
                                                      ryml::NodeRef config) const {
    auto it = factories_.find(type_name);
    if (it == factories_.end()) {
        spdlog::error("[Registry] Unknown type: '{}'", type_name);
        return nullptr;
    }
    return it->second(instance_name, config);
}

} // namespace nuedcs::core
