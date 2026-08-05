#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "iaisf/core/result.hpp"
#include "iaisf/logging/log_level.hpp"

namespace iaisf {

inline constexpr std::size_t kMaxConfigurationFileBytes = 1024U * 1024U;
inline constexpr std::size_t kMaxServiceNameLength = 128U;
inline constexpr std::size_t kMaxWorkerThreads = 256U;
inline constexpr std::size_t kMaxTaskQueueCapacity = 1'000'000U;
inline constexpr std::size_t kMaxLogQueueCapacity = 1'000'000U;
inline constexpr std::size_t kMaxLogBatchSize = 1'000'000U;
inline constexpr std::uint64_t kMaxLogFlushIntervalMs = 60ULL * 60ULL * 1000ULL;
inline constexpr std::uintmax_t kMaxLogFileBytes = 1ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kMaxLogArchives = 1000U;

struct ServiceConfig {
    std::string name;
};

struct ReactorConfig {
    std::size_t max_events{};
    std::size_t pending_callback_capacity{};
    std::size_t max_timers{};
};

struct TcpConfig {
    std::int64_t listen_backlog{};
    std::int64_t max_connections{};
    std::int64_t input_initial_capacity_bytes{};
    std::int64_t input_maximum_capacity_bytes{};
    std::int64_t output_initial_capacity_bytes{};
    std::int64_t output_high_water_mark_bytes{};
    std::int64_t output_maximum_capacity_bytes{};
    std::optional<std::int64_t> socket_send_buffer_bytes;
    std::optional<std::int64_t> idle_timeout_ms;
};

struct ServerConfig {
    std::string host;
    std::uint16_t port{};
    ReactorConfig reactor;
    TcpConfig tcp;
};

struct HttpLimitConfig {
    std::int64_t max_request_line_bytes{};
    std::int64_t max_method_bytes{};
    std::int64_t max_target_bytes{};
    std::int64_t max_header_line_bytes{};
    std::int64_t max_header_bytes{};
    std::int64_t max_header_count{};
    std::int64_t max_body_bytes{};
    std::int64_t max_response_body_bytes{};
    std::int64_t max_routes{};
    std::int64_t max_requests_per_dispatch{};
};

struct HttpConfig {
    std::optional<std::int64_t> header_timeout_ms;
    std::optional<std::int64_t> body_timeout_ms;
    HttpLimitConfig limits;
};

struct RuntimeConfig {
    std::size_t worker_threads{};
    std::size_t task_queue_capacity{};
};

struct TaskConfig {
    std::int64_t max_repository_tasks{};
    std::int64_t max_operation_bytes{};
    std::int64_t max_input_bytes{};
    std::int64_t max_result_bytes{};
    std::int64_t max_error_message_bytes{};
    std::int64_t max_json_depth{};
    std::int64_t max_json_elements{};
    std::int64_t max_json_string_bytes{};
};

struct PluginLimitConfig {
    std::int64_t max_plugins{};
    std::int64_t max_operation_bytes{};
    std::int64_t max_name_bytes{};
    std::int64_t max_version_bytes{};
    std::int64_t max_description_bytes{};
    std::int64_t max_error_message_bytes{};
    std::int64_t max_input_bytes{};
    std::int64_t max_output_bytes{};
    std::int64_t max_json_depth{};
    std::int64_t max_json_elements{};
    std::int64_t max_string_bytes{};
    std::int64_t max_capabilities{};
    std::int64_t max_capability_bytes{};
};

struct PluginConfig {
    bool enable_echo{};
    bool enable_mock_vision{};
    PluginLimitConfig limits;
};

struct TaskApiConfig {
    std::int64_t max_error_message_bytes{};
    std::int64_t max_status_url_bytes{};
};

struct LoggingConsoleConfig {
    bool enabled{true};
};

struct LoggingFileConfig {
    bool enabled{false};
    std::string path;
    std::uintmax_t max_file_bytes{10U * 1024U * 1024U};
    std::size_t max_archives{3U};
};

struct LoggingConfig {
    LogLevel level{LogLevel::Info};
    std::size_t queue_capacity{1024U};
    std::size_t reserved_critical_capacity{32U};
    std::size_t batch_size{64U};
    std::uint64_t flush_interval_ms{1000U};
    LoggingConsoleConfig console;
    LoggingFileConfig file;
};

/** Portable, resource-free application configuration value. */
struct AppConfig {
    std::uint32_t schema_version{};
    ServiceConfig service;
    ServerConfig server;
    HttpConfig http;
    RuntimeConfig runtime;
    TaskConfig tasks;
    PluginConfig plugins;
    TaskApiConfig task_api;
    LoggingConfig logging;
};

[[nodiscard]] AppConfig default_app_config();
[[nodiscard]] Result<AppConfig>
load_app_config(const std::filesystem::path &path);
[[nodiscard]] Result<void> validate_app_config(const AppConfig &config);

} // namespace iaisf
