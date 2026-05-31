// 组件头文件必须在 executor.hpp 之前 include，
// 这样注册宏的静态初始化器才能在 main 之前执行。
#include "components/camera.hpp"
#include "components/motor.hpp"
#include "components/heartbeat.hpp"
#include "core/predefined_msg_provider.hpp"

#include "core/executor.hpp"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

int main()
{
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] [%n] %v");
    auto sys_logger = std::make_shared<spdlog::logger>("system", spdlog::default_logger()->sinks().begin(),
        spdlog::default_logger()->sinks().end());
    spdlog::set_default_logger(sys_logger);

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

    if (root.has_child("loop_hz"))
        executor.set_loop_hz(nuedcs::core::node_val<double>(root["loop_hz"], 1000.0));

    if (!root.has_child("components") || !root["components"].is_seq()) {
        spdlog::error("[main] 'components' must be a YAML sequence");
        return 1;
    }

    std::regex pattern(R"(\s*(\S+)\s*->\s*(\S+)\s*)");

    for (ryml::NodeRef entry : root["components"].children()) {
        auto desc = nuedcs::core::node_str(entry);
        std::string type_name, instance_name;

        std::smatch m;
        if (std::regex_match(desc, m, pattern) && m.size() == 3) {
            type_name     = m[1].str();
            instance_name = m[2].str();
        } else {
            type_name = instance_name = desc;
        }

        auto comp = registry.create(type_name, instance_name.c_str());
        if (!comp) {
            spdlog::error("[main] Failed to create: {}", desc);
            return 1;
        }

        // 用实例名查找对应的 YAML 配置段
        if (root.has_child(c4::to_csubstr(instance_name))) {
            comp->configure(root[c4::to_csubstr(instance_name)]);
        }

        executor.add_component(std::move(comp));
    }

    if (!executor.init_all()) return 1;

    executor.start();
    executor.run();

    return 0;
}
