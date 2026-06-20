#include "mimalloc-new-delete.h"
#include "mimalloc-override.h"

// 组件头文件必须在 executor.hpp 之前 include，
// 这样 register_namespace_components() 才能通过反射发现它们。
#include "hardware/car.hpp"
#include "controller/pid/pid_controller.hpp"
#include "controller/pid/error_pid_controller.hpp"
#include "controller/chassis/chassis_controller.hpp"
#include "vision/vision_test.hpp"

#include "core/executor.hpp"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

// ── C++26 reflection: auto-discover and register all components ───────
// Enumerates every class in the listed namespaces via std::meta::members_of(),
// checks if it inherits from register_component, and registers it automatically.
// No macro, no per-component boilerplate — just list the namespaces.

void register_all_components() {
    nuedcs::core::register_namespace_components<^^nuedcs::hardware>();
    nuedcs::core::register_namespace_components<^^nuedcs::controller::pid>();
    nuedcs::core::register_namespace_components<^^nuedcs::controller::chassis>();
    nuedcs::core::register_namespace_components<^^nuedcs::vision>();
}

int main()
{
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] [%n] %v");
    auto sys_logger = std::make_shared<spdlog::logger>("system", spdlog::default_logger()->sinks().begin(),
        spdlog::default_logger()->sinks().end());
    spdlog::set_default_logger(sys_logger);

    // 注册所有组件（C++26 反射自动发现）
    register_all_components();

    // 加载配置
    auto exe_dir = std::filesystem::canonical("/proc/self/exe").parent_path();
    auto cfg_path = exe_dir / ".." / "config" / "config.yaml";

    std::ifstream f(cfg_path);
    if (!f) {
        spdlog::error("[main] Config not found: {}", cfg_path.string());
        return 1;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string contents = ss.str();

    ryml::Tree tree = ryml::parse_in_arena(c4::to_csubstr(cfg_path.string()), c4::to_csubstr(contents));
    ryml::NodeRef root = tree.rootref();

    spdlog::info("[main] Loaded config: {}", cfg_path.string());

    // 解析组件声明列表，格式: "Type -> instance_name" 或 "Type"
    auto& registry = nuedcs::core::ComponentRegistry::instance();
    nuedcs::core::Executor executor;

    if (!root.has_child("components") || !root["components"].is_seq()) {
        spdlog::error("[main] 'components' must be a YAML sequence");
        return 1;
    }

    std::regex pattern(R"(\s*(\S+)\s*->\s*(\S+)\s*)");

    for (ryml::NodeRef entry : root["components"].children()) {
        c4::csubstr s = entry.val();
        auto desc = std::string(s.begin(), s.size());
        std::string type_name, instance_name;

        std::smatch m;
        if (std::regex_match(desc, m, pattern) && m.size() == 3) {
            type_name     = m[1].str();
            instance_name = m[2].str();
        } else {
            type_name = instance_name = desc;
        }

        // 构造时传入配置
        auto config = root[c4::to_csubstr(instance_name)];
        auto comp = registry.create(type_name, instance_name.c_str(), config);
        if (!comp) {
            spdlog::error("[main] Failed to create: {}", desc);
            return 1;
        }

        executor.add_component(std::move(comp));
    }

    if (!executor.init_all()) return 1;

    executor.start();
    executor.run();

    return 0;
}
