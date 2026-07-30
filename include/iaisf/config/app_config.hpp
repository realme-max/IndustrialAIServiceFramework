#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include "iaisf/core/result.hpp"
#include "iaisf/logging/log_level.hpp"

namespace iaisf {

inline constexpr std::size_t kMaxServiceNameLength = 128;
inline constexpr std::size_t kMaxWorkerThreads = 256;
inline constexpr std::size_t kMaxTaskQueueCapacity = 1'000'000;

struct AppConfig {
    std::string service_name;
    std::size_t worker_threads;
    std::size_t task_queue_capacity;
    LogLevel log_level;
};

[[nodiscard]] AppConfig default_app_config();
[[nodiscard]] Result<AppConfig> load_app_config(const std::filesystem::path& path);
[[nodiscard]] Result<void> validate_app_config(const AppConfig& config);

}  // namespace iaisf

