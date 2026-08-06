#pragma once

#include <filesystem>
#include <memory>
#include <set>
#include <string>

#include "iaisf/core/result.hpp"

namespace iaisf::plugin {

class DynamicPluginAdapter;
class PluginLimits;

namespace detail {

class DynamicModule;

struct DynamicPluginLoaderOptions {
    std::filesystem::path plugin_root;
};

struct DynamicPluginModuleSpec {
    std::string id;
    std::filesystem::path library;
    std::string config_json{"{}"};
};

/** Loads validated modules and creates unregistered dynamic adapters. */
class DynamicPluginLoader final {
public:
    [[nodiscard]] static Result<std::unique_ptr<DynamicPluginLoader>> create(
        DynamicPluginLoaderOptions options);

    [[nodiscard]] Result<std::shared_ptr<DynamicModule>> load_module(
        const DynamicPluginModuleSpec& spec);

    /** Negotiates the module entry point and creates an unregistered adapter. */
    [[nodiscard]] Result<std::shared_ptr<DynamicPluginAdapter>> load_plugin(
        const DynamicPluginModuleSpec& spec);

    [[nodiscard]] Result<std::shared_ptr<DynamicPluginAdapter>> load_plugin(
        const DynamicPluginModuleSpec& spec,
        const PluginLimits& limits);

    [[nodiscard]] const std::filesystem::path& plugin_root() const noexcept;

private:
    explicit DynamicPluginLoader(std::filesystem::path plugin_root);

    std::filesystem::path plugin_root_;
    std::set<std::filesystem::path> loaded_paths_;
};

}  // namespace detail
}  // namespace iaisf::plugin
