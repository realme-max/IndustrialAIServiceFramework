#include "iaisf/config/app_config.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <new>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace iaisf {
namespace {

using Json = nlohmann::json;
constexpr std::uint32_t kSupportedSchemaVersion = 1U;
constexpr std::int64_t kMaximumTimeoutMilliseconds =
    24LL * 60LL * 60LL * 1000LL;

class DuplicateKeySax final : public nlohmann::json_sax<Json> {
  public:
    bool null() override { return true; }
    bool boolean(bool) override { return true; }
    bool number_integer(number_integer_t) override { return true; }
    bool number_unsigned(number_unsigned_t) override { return true; }
    bool number_float(number_float_t, const string_t &) override {
        return true;
    }
    bool string(string_t &) override { return true; }
    bool binary(binary_t &) override { return true; }

    bool start_object(std::size_t) override {
        object_keys_.emplace_back();
        return true;
    }

    bool key(string_t &value) override {
        if (object_keys_.empty()) {
            return false;
        }
        if (!object_keys_.back().insert(value).second) {
            duplicate_key_ = true;
            return false;
        }
        return true;
    }

    bool end_object() override {
        if (object_keys_.empty()) {
            return false;
        }
        object_keys_.pop_back();
        return true;
    }

    bool start_array(std::size_t) override { return true; }
    bool end_array() override { return true; }

    bool parse_error(std::size_t, const std::string &,
                     const nlohmann::detail::exception &) override {
        return false;
    }

    [[nodiscard]] bool duplicate_key() const noexcept { return duplicate_key_; }

  private:
    std::vector<std::set<std::string>> object_keys_;
    bool duplicate_key_{false};
};

Result<void> config_failure(std::string message) {
    return Result<void>::failure(
        make_error(ErrorCode::ConfigError, std::move(message)));
}

template <typename T> Result<T> typed_config_failure(std::string message) {
    return Result<T>::failure(
        make_error(ErrorCode::ConfigError, std::move(message)));
}

bool contains_only_whitespace(const std::string_view value) {
    return std::all_of(
        value.begin(), value.end(), [](const char raw_character) {
            const auto character = static_cast<unsigned char>(raw_character);
            return character == ' ' || character == '\t' || character == '\n' ||
                   character == '\r' || character == '\f' || character == '\v';
        });
}

bool contains_control_character(const std::string_view value) {
    return std::any_of(
        value.begin(), value.end(), [](const char raw_character) {
            const auto character = static_cast<unsigned char>(raw_character);
            return character < 0x20U || character == 0x7FU;
        });
}

bool is_known_log_level(const LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace:
    case LogLevel::Debug:
    case LogLevel::Info:
    case LogLevel::Warn:
    case LogLevel::Error:
        return true;
    }
    return false;
}

Result<void> require_object(const Json &value, const std::string_view name) {
    if (!value.is_object()) {
        return config_failure(std::string{name} + " must be an object");
    }
    return Result<void>::success();
}

Result<void> reject_unknown_fields(
    const Json &object,
    const std::initializer_list<std::string_view> allowed_fields,
    const std::string_view group_name) {
    for (const auto &item : object.items()) {
        const std::string_view key{item.key()};
        if (std::find(allowed_fields.begin(), allowed_fields.end(), key) ==
            allowed_fields.end()) {
            return config_failure("unknown field in " +
                                  std::string{group_name} + ": " + item.key());
        }
    }
    return Result<void>::success();
}

Result<std::int64_t> read_integer(const Json &value,
                                  const std::string_view field_name,
                                  const bool allow_zero = false) {
    std::int64_t parsed = 0;
    if (value.is_number_unsigned()) {
        const std::uint64_t raw = value.get<std::uint64_t>();
        if (raw > static_cast<std::uint64_t>(
                      std::numeric_limits<std::int64_t>::max())) {
            return typed_config_failure<std::int64_t>(
                std::string{field_name} +
                " exceeds the supported integer range");
        }
        parsed = static_cast<std::int64_t>(raw);
    } else if (value.is_number_integer()) {
        parsed = value.get<std::int64_t>();
    } else {
        return typed_config_failure<std::int64_t>(std::string{field_name} +
                                                  " must be an integer");
    }
    if (parsed < 0 || (!allow_zero && parsed == 0)) {
        return typed_config_failure<std::int64_t>(
            std::string{field_name} + (allow_zero
                                           ? " must not be negative"
                                           : " must be greater than zero"));
    }
    return Result<std::int64_t>::success(parsed);
}

Result<std::size_t> read_size(const Json &value,
                              const std::string_view field_name,
                              const std::size_t maximum) {
    auto integer = read_integer(value, field_name);
    if (!integer) {
        return Result<std::size_t>::failure(std::move(integer).error());
    }
    if (static_cast<std::uint64_t>(integer.value()) >
        static_cast<std::uint64_t>(maximum)) {
        return typed_config_failure<std::size_t>(
            std::string{field_name} + " exceeds the allowed maximum");
    }
    return Result<std::size_t>::success(
        static_cast<std::size_t>(integer.value()));
}

Result<void> assign_integer(const Json &object, const char *const key,
                            const std::string_view path,
                            std::int64_t &destination) {
    if (!object.contains(key)) {
        return Result<void>::success();
    }
    auto value = read_integer(object.at(key), path);
    if (!value) {
        return Result<void>::failure(std::move(value).error());
    }
    destination = value.value();
    return Result<void>::success();
}

Result<void> assign_optional_timeout(const Json &object, const char *const key,
                                     const std::string_view path,
                                     std::optional<std::int64_t> &destination) {
    if (!object.contains(key)) {
        return Result<void>::success();
    }
    const Json &raw = object.at(key);
    if (raw.is_null()) {
        destination.reset();
        return Result<void>::success();
    }
    auto value = read_integer(raw, path);
    if (!value) {
        return Result<void>::failure(std::move(value).error());
    }
    if (value.value() > kMaximumTimeoutMilliseconds) {
        return config_failure(std::string{path} +
                              " exceeds the 24 hour maximum");
    }
    destination = value.value();
    return Result<void>::success();
}

Result<void>
assign_optional_positive_integer(const Json &object, const char *const key,
                                 const std::string_view path,
                                 std::optional<std::int64_t> &destination) {
    if (!object.contains(key)) {
        return Result<void>::success();
    }
    const Json &raw = object.at(key);
    if (raw.is_null()) {
        destination.reset();
        return Result<void>::success();
    }
    auto value = read_integer(raw, path);
    if (!value) {
        return Result<void>::failure(std::move(value).error());
    }
    destination = value.value();
    return Result<void>::success();
}

Result<void> apply_service_config(const Json &root, AppConfig &config) {
    if (!root.contains("service")) {
        return Result<void>::success();
    }
    const Json &service = root.at("service");
    auto object = require_object(service, "service");
    if (!object) {
        return object;
    }
    auto fields = reject_unknown_fields(service, {"name"}, "service");
    if (!fields) {
        return fields;
    }
    if (service.contains("name")) {
        if (!service.at("name").is_string()) {
            return config_failure("service.name must be a string");
        }
        config.service.name = service.at("name").get<std::string>();
    }
    return Result<void>::success();
}

Result<void> apply_reactor_config(const Json &server, AppConfig &config) {
    if (!server.contains("reactor")) {
        return Result<void>::success();
    }
    const Json &reactor = server.at("reactor");
    auto object = require_object(reactor, "server.reactor");
    if (!object) {
        return object;
    }
    auto fields = reject_unknown_fields(
        reactor, {"max_events", "pending_callback_capacity", "max_timers"},
        "server.reactor");
    if (!fields) {
        return fields;
    }
    if (reactor.contains("max_events")) {
        auto value = read_size(reactor.at("max_events"),
                               "server.reactor.max_events", 65'536U);
        if (!value) {
            return Result<void>::failure(std::move(value).error());
        }
        config.server.reactor.max_events = value.value();
    }
    if (reactor.contains("pending_callback_capacity")) {
        auto value =
            read_size(reactor.at("pending_callback_capacity"),
                      "server.reactor.pending_callback_capacity", 1'000'000U);
        if (!value) {
            return Result<void>::failure(std::move(value).error());
        }
        config.server.reactor.pending_callback_capacity = value.value();
    }
    if (reactor.contains("max_timers")) {
        auto value = read_size(reactor.at("max_timers"),
                               "server.reactor.max_timers", 1'000'000U);
        if (!value) {
            return Result<void>::failure(std::move(value).error());
        }
        config.server.reactor.max_timers = value.value();
    }
    return Result<void>::success();
}

Result<void> apply_tcp_config(const Json &server, AppConfig &config) {
    if (!server.contains("tcp")) {
        return Result<void>::success();
    }
    const Json &tcp = server.at("tcp");
    auto object = require_object(tcp, "server.tcp");
    if (!object) {
        return object;
    }
    auto fields = reject_unknown_fields(
        tcp,
        {"listen_backlog", "max_connections", "input_initial_capacity_bytes",
         "input_maximum_capacity_bytes", "output_initial_capacity_bytes",
         "output_high_water_mark_bytes", "output_maximum_capacity_bytes",
         "socket_send_buffer_bytes", "idle_timeout_ms"},
        "server.tcp");
    if (!fields) {
        return fields;
    }
    const std::array<std::tuple<const char *, const char *, std::int64_t *>, 7U>
        integers{{
            {"listen_backlog", "server.tcp.listen_backlog",
             &config.server.tcp.listen_backlog},
            {"max_connections", "server.tcp.max_connections",
             &config.server.tcp.max_connections},
            {"input_initial_capacity_bytes",
             "server.tcp.input_initial_capacity_bytes",
             &config.server.tcp.input_initial_capacity_bytes},
            {"input_maximum_capacity_bytes",
             "server.tcp.input_maximum_capacity_bytes",
             &config.server.tcp.input_maximum_capacity_bytes},
            {"output_initial_capacity_bytes",
             "server.tcp.output_initial_capacity_bytes",
             &config.server.tcp.output_initial_capacity_bytes},
            {"output_high_water_mark_bytes",
             "server.tcp.output_high_water_mark_bytes",
             &config.server.tcp.output_high_water_mark_bytes},
            {"output_maximum_capacity_bytes",
             "server.tcp.output_maximum_capacity_bytes",
             &config.server.tcp.output_maximum_capacity_bytes},
        }};
    for (const auto &[key, path, destination] : integers) {
        auto assigned = assign_integer(tcp, key, path, *destination);
        if (!assigned) {
            return assigned;
        }
    }
    auto send_buffer = assign_optional_positive_integer(
        tcp, "socket_send_buffer_bytes", "server.tcp.socket_send_buffer_bytes",
        config.server.tcp.socket_send_buffer_bytes);
    if (!send_buffer) {
        return send_buffer;
    }
    return assign_optional_timeout(tcp, "idle_timeout_ms",
                                   "server.tcp.idle_timeout_ms",
                                   config.server.tcp.idle_timeout_ms);
}

Result<void> apply_server_config(const Json &root, AppConfig &config) {
    if (!root.contains("server")) {
        return Result<void>::success();
    }
    const Json &server = root.at("server");
    auto object = require_object(server, "server");
    if (!object) {
        return object;
    }
    auto fields = reject_unknown_fields(
        server, {"host", "port", "reactor", "tcp"}, "server");
    if (!fields) {
        return fields;
    }
    if (server.contains("host")) {
        if (!server.at("host").is_string()) {
            return config_failure("server.host must be a string");
        }
        config.server.host = server.at("host").get<std::string>();
    }
    if (server.contains("port")) {
        auto port = read_integer(server.at("port"), "server.port", true);
        if (!port) {
            return Result<void>::failure(std::move(port).error());
        }
        if (port.value() > 65'535) {
            return config_failure("server.port exceeds 65535");
        }
        config.server.port = static_cast<std::uint16_t>(port.value());
    }
    auto reactor = apply_reactor_config(server, config);
    if (!reactor) {
        return reactor;
    }
    return apply_tcp_config(server, config);
}

Result<void> apply_http_limits(const Json &http, AppConfig &config) {
    if (!http.contains("limits")) {
        return Result<void>::success();
    }
    const Json &limits = http.at("limits");
    auto object = require_object(limits, "http.limits");
    if (!object) {
        return object;
    }
    auto fields = reject_unknown_fields(
        limits,
        {"max_request_line_bytes", "max_method_bytes", "max_target_bytes",
         "max_header_line_bytes", "max_header_bytes", "max_header_count",
         "max_body_bytes", "max_response_body_bytes", "max_routes",
         "max_requests_per_dispatch"},
        "http.limits");
    if (!fields) {
        return fields;
    }
    const std::array<std::tuple<const char *, const char *, std::int64_t *>,
                     10U>
        values{{
            {"max_request_line_bytes", "http.limits.max_request_line_bytes",
             &config.http.limits.max_request_line_bytes},
            {"max_method_bytes", "http.limits.max_method_bytes",
             &config.http.limits.max_method_bytes},
            {"max_target_bytes", "http.limits.max_target_bytes",
             &config.http.limits.max_target_bytes},
            {"max_header_line_bytes", "http.limits.max_header_line_bytes",
             &config.http.limits.max_header_line_bytes},
            {"max_header_bytes", "http.limits.max_header_bytes",
             &config.http.limits.max_header_bytes},
            {"max_header_count", "http.limits.max_header_count",
             &config.http.limits.max_header_count},
            {"max_body_bytes", "http.limits.max_body_bytes",
             &config.http.limits.max_body_bytes},
            {"max_response_body_bytes", "http.limits.max_response_body_bytes",
             &config.http.limits.max_response_body_bytes},
            {"max_routes", "http.limits.max_routes",
             &config.http.limits.max_routes},
            {"max_requests_per_dispatch",
             "http.limits.max_requests_per_dispatch",
             &config.http.limits.max_requests_per_dispatch},
        }};
    for (const auto &[key, path, destination] : values) {
        auto assigned = assign_integer(limits, key, path, *destination);
        if (!assigned) {
            return assigned;
        }
    }
    return Result<void>::success();
}

Result<void> apply_http_config(const Json &root, AppConfig &config) {
    if (!root.contains("http")) {
        return Result<void>::success();
    }
    const Json &http = root.at("http");
    auto object = require_object(http, "http");
    if (!object) {
        return object;
    }
    auto fields = reject_unknown_fields(
        http, {"header_timeout_ms", "body_timeout_ms", "limits"}, "http");
    if (!fields) {
        return fields;
    }
    auto header = assign_optional_timeout(http, "header_timeout_ms",
                                          "http.header_timeout_ms",
                                          config.http.header_timeout_ms);
    if (!header) {
        return header;
    }
    auto body =
        assign_optional_timeout(http, "body_timeout_ms", "http.body_timeout_ms",
                                config.http.body_timeout_ms);
    if (!body) {
        return body;
    }
    return apply_http_limits(http, config);
}

Result<void> apply_runtime_config(const Json &root, AppConfig &config) {
    if (!root.contains("runtime")) {
        return Result<void>::success();
    }
    const Json &runtime = root.at("runtime");
    auto object = require_object(runtime, "runtime");
    if (!object) {
        return object;
    }
    auto fields = reject_unknown_fields(
        runtime, {"worker_threads", "task_queue_capacity"}, "runtime");
    if (!fields) {
        return fields;
    }
    if (runtime.contains("worker_threads")) {
        auto workers = read_size(runtime.at("worker_threads"),
                                 "runtime.worker_threads", kMaxWorkerThreads);
        if (!workers) {
            return Result<void>::failure(std::move(workers).error());
        }
        config.runtime.worker_threads = workers.value();
    }
    if (runtime.contains("task_queue_capacity")) {
        auto capacity =
            read_size(runtime.at("task_queue_capacity"),
                      "runtime.task_queue_capacity", kMaxTaskQueueCapacity);
        if (!capacity) {
            return Result<void>::failure(std::move(capacity).error());
        }
        config.runtime.task_queue_capacity = capacity.value();
    }
    return Result<void>::success();
}

Result<void> apply_task_config(const Json &root, AppConfig &config) {
    if (!root.contains("tasks")) {
        return Result<void>::success();
    }
    const Json &tasks = root.at("tasks");
    auto object = require_object(tasks, "tasks");
    if (!object) {
        return object;
    }
    auto fields = reject_unknown_fields(
        tasks,
        {"max_repository_tasks", "max_operation_bytes", "max_input_bytes",
         "max_result_bytes", "max_error_message_bytes", "max_json_depth",
         "max_json_elements", "max_json_string_bytes"},
        "tasks");
    if (!fields) {
        return fields;
    }
    const std::array<std::tuple<const char *, const char *, std::int64_t *>, 8U>
        values{{
            {"max_repository_tasks", "tasks.max_repository_tasks",
             &config.tasks.max_repository_tasks},
            {"max_operation_bytes", "tasks.max_operation_bytes",
             &config.tasks.max_operation_bytes},
            {"max_input_bytes", "tasks.max_input_bytes",
             &config.tasks.max_input_bytes},
            {"max_result_bytes", "tasks.max_result_bytes",
             &config.tasks.max_result_bytes},
            {"max_error_message_bytes", "tasks.max_error_message_bytes",
             &config.tasks.max_error_message_bytes},
            {"max_json_depth", "tasks.max_json_depth",
             &config.tasks.max_json_depth},
            {"max_json_elements", "tasks.max_json_elements",
             &config.tasks.max_json_elements},
            {"max_json_string_bytes", "tasks.max_json_string_bytes",
             &config.tasks.max_json_string_bytes},
        }};
    for (const auto &[key, path, destination] : values) {
        auto assigned = assign_integer(tasks, key, path, *destination);
        if (!assigned) {
            return assigned;
        }
    }
    return Result<void>::success();
}

Result<void> apply_plugin_limits(const Json &plugins, AppConfig &config) {
    if (!plugins.contains("limits")) {
        return Result<void>::success();
    }
    const Json &limits = plugins.at("limits");
    auto object = require_object(limits, "plugins.limits");
    if (!object) {
        return object;
    }
    auto fields = reject_unknown_fields(
        limits,
        {"max_plugins", "max_operation_bytes", "max_name_bytes",
         "max_version_bytes", "max_description_bytes",
         "max_error_message_bytes", "max_input_bytes", "max_output_bytes",
         "max_json_depth", "max_json_elements", "max_string_bytes",
         "max_capabilities", "max_capability_bytes"},
        "plugins.limits");
    if (!fields) {
        return fields;
    }
    const std::array<std::tuple<const char *, const char *, std::int64_t *>,
                     13U>
        values{{
            {"max_plugins", "plugins.limits.max_plugins",
             &config.plugins.limits.max_plugins},
            {"max_operation_bytes", "plugins.limits.max_operation_bytes",
             &config.plugins.limits.max_operation_bytes},
            {"max_name_bytes", "plugins.limits.max_name_bytes",
             &config.plugins.limits.max_name_bytes},
            {"max_version_bytes", "plugins.limits.max_version_bytes",
             &config.plugins.limits.max_version_bytes},
            {"max_description_bytes", "plugins.limits.max_description_bytes",
             &config.plugins.limits.max_description_bytes},
            {"max_error_message_bytes",
             "plugins.limits.max_error_message_bytes",
             &config.plugins.limits.max_error_message_bytes},
            {"max_input_bytes", "plugins.limits.max_input_bytes",
             &config.plugins.limits.max_input_bytes},
            {"max_output_bytes", "plugins.limits.max_output_bytes",
             &config.plugins.limits.max_output_bytes},
            {"max_json_depth", "plugins.limits.max_json_depth",
             &config.plugins.limits.max_json_depth},
            {"max_json_elements", "plugins.limits.max_json_elements",
             &config.plugins.limits.max_json_elements},
            {"max_string_bytes", "plugins.limits.max_string_bytes",
             &config.plugins.limits.max_string_bytes},
            {"max_capabilities", "plugins.limits.max_capabilities",
             &config.plugins.limits.max_capabilities},
            {"max_capability_bytes", "plugins.limits.max_capability_bytes",
             &config.plugins.limits.max_capability_bytes},
        }};
    for (const auto &[key, path, destination] : values) {
        auto assigned = assign_integer(limits, key, path, *destination);
        if (!assigned) {
            return assigned;
        }
    }
    return Result<void>::success();
}

Result<void> apply_plugin_switch(const Json &plugins, const char *const key,
                                 bool &destination) {
    if (!plugins.contains(key)) {
        return Result<void>::success();
    }
    const Json &plugin = plugins.at(key);
    const std::string path = std::string{"plugins."} + key;
    auto object = require_object(plugin, path);
    if (!object) {
        return object;
    }
    auto fields = reject_unknown_fields(plugin, {"enabled"}, path);
    if (!fields) {
        return fields;
    }
    if (plugin.contains("enabled")) {
        if (!plugin.at("enabled").is_boolean()) {
            return config_failure(path + ".enabled must be a boolean");
        }
        destination = plugin.at("enabled").get<bool>();
    }
    return Result<void>::success();
}

Result<void> apply_plugin_config(const Json &root, AppConfig &config) {
    if (!root.contains("plugins")) {
        return Result<void>::success();
    }
    const Json &plugins = root.at("plugins");
    auto object = require_object(plugins, "plugins");
    if (!object) {
        return object;
    }
    auto fields = reject_unknown_fields(
        plugins, {"echo", "mock_vision", "limits"}, "plugins");
    if (!fields) {
        return fields;
    }
    auto echo =
        apply_plugin_switch(plugins, "echo", config.plugins.enable_echo);
    if (!echo) {
        return echo;
    }
    auto mock = apply_plugin_switch(plugins, "mock_vision",
                                    config.plugins.enable_mock_vision);
    if (!mock) {
        return mock;
    }
    return apply_plugin_limits(plugins, config);
}

Result<void> apply_task_api_config(const Json &root, AppConfig &config) {
    if (!root.contains("task_api")) {
        return Result<void>::success();
    }
    const Json &api = root.at("task_api");
    auto object = require_object(api, "task_api");
    if (!object) {
        return object;
    }
    auto fields = reject_unknown_fields(
        api, {"max_error_message_bytes", "max_status_url_bytes"}, "task_api");
    if (!fields) {
        return fields;
    }
    auto error = assign_integer(api, "max_error_message_bytes",
                                "task_api.max_error_message_bytes",
                                config.task_api.max_error_message_bytes);
    if (!error) {
        return error;
    }
    return assign_integer(api, "max_status_url_bytes",
                          "task_api.max_status_url_bytes",
                          config.task_api.max_status_url_bytes);
}

Result<void> apply_logging_config(const Json &root, AppConfig &config) {
    if (!root.contains("logging")) {
        return Result<void>::success();
    }
    const Json &logging = root.at("logging");
    auto object = require_object(logging, "logging");
    if (!object) {
        return object;
    }
    auto fields = reject_unknown_fields(
        logging,
        {"level", "queue_capacity", "reserved_critical_capacity", "batch_size",
         "flush_interval_ms", "console", "file"},
        "logging");
    if (!fields) {
        return fields;
    }
    if (logging.contains("level")) {
        if (!logging.at("level").is_string()) {
            return config_failure("logging.level must be a string");
        }
        auto level = parse_log_level(logging.at("level").get<std::string>());
        if (!level) {
            return config_failure(
                "logging.level must be one of trace, debug, info, warn, error");
        }
        config.logging.level = level.value();
    }

    if (logging.contains("queue_capacity")) {
        auto value = read_size(logging.at("queue_capacity"),
                               "logging.queue_capacity", kMaxLogQueueCapacity);
        if (!value) {
            return Result<void>::failure(std::move(value).error());
        }
        config.logging.queue_capacity = value.value();
    }
    if (logging.contains("reserved_critical_capacity")) {
        auto value = read_integer(logging.at("reserved_critical_capacity"),
                                  "logging.reserved_critical_capacity", true);
        if (!value) {
            return Result<void>::failure(std::move(value).error());
        }
        if (static_cast<std::uint64_t>(value.value()) > kMaxLogQueueCapacity) {
            return config_failure(
                "logging.reserved_critical_capacity exceeds the allowed maximum");
        }
        config.logging.reserved_critical_capacity =
            static_cast<std::size_t>(value.value());
    }
    if (logging.contains("batch_size")) {
        auto value = read_size(logging.at("batch_size"), "logging.batch_size",
                               kMaxLogBatchSize);
        if (!value) {
            return Result<void>::failure(std::move(value).error());
        }
        config.logging.batch_size = value.value();
    }
    if (logging.contains("flush_interval_ms")) {
        auto value = read_integer(logging.at("flush_interval_ms"),
                                  "logging.flush_interval_ms", true);
        if (!value) {
            return Result<void>::failure(std::move(value).error());
        }
        if (static_cast<std::uint64_t>(value.value()) > kMaxLogFlushIntervalMs) {
            return config_failure("logging.flush_interval_ms exceeds the allowed maximum");
        }
        config.logging.flush_interval_ms =
            static_cast<std::uint64_t>(value.value());
    }

    if (logging.contains("console")) {
        const Json &console = logging.at("console");
        auto console_object = require_object(console, "logging.console");
        if (!console_object) {
            return console_object;
        }
        auto console_fields =
            reject_unknown_fields(console, {"enabled"}, "logging.console");
        if (!console_fields) {
            return console_fields;
        }
        if (console.contains("enabled")) {
            if (!console.at("enabled").is_boolean()) {
                return config_failure("logging.console.enabled must be a boolean");
            }
            config.logging.console.enabled = console.at("enabled").get<bool>();
        }
    }

    if (logging.contains("file")) {
        const Json &file = logging.at("file");
        auto file_object = require_object(file, "logging.file");
        if (!file_object) {
            return file_object;
        }
        auto file_fields = reject_unknown_fields(
            file, {"enabled", "path", "max_file_bytes", "max_archives"},
            "logging.file");
        if (!file_fields) {
            return file_fields;
        }
        if (file.contains("enabled")) {
            if (!file.at("enabled").is_boolean()) {
                return config_failure("logging.file.enabled must be a boolean");
            }
            config.logging.file.enabled = file.at("enabled").get<bool>();
        }
        if (file.contains("path")) {
            if (!file.at("path").is_string()) {
                return config_failure("logging.file.path must be a string");
            }
            config.logging.file.path = file.at("path").get<std::string>();
        }
        if (file.contains("max_file_bytes")) {
            auto value = read_size(file.at("max_file_bytes"),
                                   "logging.file.max_file_bytes",
                                   static_cast<std::size_t>(kMaxLogFileBytes));
            if (!value) {
                return Result<void>::failure(std::move(value).error());
            }
            config.logging.file.max_file_bytes =
                static_cast<std::uintmax_t>(value.value());
        }
        if (file.contains("max_archives")) {
            auto value = read_integer(file.at("max_archives"),
                                      "logging.file.max_archives", true);
            if (!value) {
                return Result<void>::failure(std::move(value).error());
            }
            if (static_cast<std::uint64_t>(value.value()) > kMaxLogArchives) {
                return config_failure(
                    "logging.file.max_archives exceeds the allowed maximum");
            }
            config.logging.file.max_archives =
                static_cast<std::size_t>(value.value());
        }
    }
    return Result<void>::success();
}

Result<void> apply_metrics_config(const Json &root, AppConfig &config) {
    if (!root.contains("metrics")) {
        return Result<void>::success();
    }
    const Json& metrics = root.at("metrics");
    auto object = require_object(metrics, "metrics");
    if (!object) {
        return object;
    }
    auto fields = reject_unknown_fields(
        metrics, {"enabled", "endpoint"}, "metrics");
    if (!fields) {
        return fields;
    }
    if (metrics.contains("enabled")) {
        if (!metrics.at("enabled").is_boolean()) {
            return config_failure("metrics.enabled must be a boolean");
        }
        config.metrics.enabled = metrics.at("enabled").get<bool>();
    }
    if (metrics.contains("endpoint")) {
        if (!metrics.at("endpoint").is_string()) {
            return config_failure("metrics.endpoint must be a string");
        }
        config.metrics.endpoint = metrics.at("endpoint").get<std::string>();
    }
    return Result<void>::success();
}

Result<void> apply_diagnostics_config(const Json &root, AppConfig &config) {
    if (!root.contains("diagnostics")) {
        return Result<void>::success();
    }
    const Json& diagnostics = root.at("diagnostics");
    auto object = require_object(diagnostics, "diagnostics");
    if (!object) {
        return object;
    }
    auto fields = reject_unknown_fields(
        diagnostics, {"enabled", "endpoint"}, "diagnostics");
    if (!fields) {
        return fields;
    }
    if (diagnostics.contains("enabled")) {
        if (!diagnostics.at("enabled").is_boolean()) {
            return config_failure("diagnostics.enabled must be a boolean");
        }
        config.diagnostics.enabled = diagnostics.at("enabled").get<bool>();
    }
    if (diagnostics.contains("endpoint")) {
        if (!diagnostics.at("endpoint").is_string()) {
            return config_failure("diagnostics.endpoint must be a string");
        }
        config.diagnostics.endpoint = diagnostics.at("endpoint").get<std::string>();
    }
    return Result<void>::success();
}

Result<void> apply_legacy_flat_config(const Json &root, AppConfig &config) {
    if (root.contains("service_name")) {
        if (root.contains("service")) {
            return config_failure(
                "service_name and service cannot both be specified");
        }
        const Json &name = root.at("service_name");
        if (!name.is_string()) {
            return config_failure("service_name must be a string");
        }
        config.service.name = name.get<std::string>();
    }

    const bool has_legacy_runtime = root.contains("worker_threads") ||
                                    root.contains("task_queue_capacity");
    if (has_legacy_runtime && root.contains("runtime")) {
        return config_failure(
            "legacy runtime fields and runtime cannot both be specified");
    }
    if (root.contains("worker_threads")) {
        auto workers = read_size(root.at("worker_threads"), "worker_threads",
                                 kMaxWorkerThreads);
        if (!workers) {
            return Result<void>::failure(std::move(workers).error());
        }
        config.runtime.worker_threads = workers.value();
    }
    if (root.contains("task_queue_capacity")) {
        auto capacity = read_size(root.at("task_queue_capacity"),
                                  "task_queue_capacity",
                                  kMaxTaskQueueCapacity);
        if (!capacity) {
            return Result<void>::failure(std::move(capacity).error());
        }
        config.runtime.task_queue_capacity = capacity.value();
    }

    if (root.contains("log_level")) {
        if (root.contains("logging")) {
            return config_failure(
                "log_level and logging cannot both be specified");
        }
        const Json &level_value = root.at("log_level");
        if (!level_value.is_string()) {
            return config_failure("log_level must be a string");
        }
        auto level = parse_log_level(level_value.get<std::string>());
        if (!level) {
            return config_failure(
                "log_level must be one of trace, debug, info, warn, error");
        }
        config.logging.level = level.value();
    }
    return Result<void>::success();
}

Result<AppConfig> parse_app_config(const std::string &contents) {
    DuplicateKeySax duplicate_detector;
    const bool structurally_valid =
        Json::sax_parse(contents, &duplicate_detector);
    if (duplicate_detector.duplicate_key()) {
        return typed_config_failure<AppConfig>(
            "configuration contains a duplicate object key");
    }
    if (!structurally_valid) {
        return typed_config_failure<AppConfig>(
            "configuration file is not valid JSON");
    }
    Json root = Json::parse(contents, nullptr, false);
    if (!root.is_object()) {
        return typed_config_failure<AppConfig>(
            "configuration root must be an object");
    }
    auto top = reject_unknown_fields(root,
                                     {"schema_version", "service", "server",
                                      "http", "runtime", "tasks", "plugins",
                                      "task_api", "logging", "metrics", "diagnostics", "service_name",
                                      "worker_threads", "task_queue_capacity",
                                      "log_level"},
                                     "root");
    if (!top) {
        return Result<AppConfig>::failure(std::move(top).error());
    }

    AppConfig config = default_app_config();
    if (root.contains("schema_version")) {
        auto version =
            read_integer(root.at("schema_version"), "schema_version");
        if (!version || version.value() != kSupportedSchemaVersion) {
            return typed_config_failure<AppConfig>(
                "schema_version must be the integer 1");
        }
        config.schema_version = static_cast<std::uint32_t>(version.value());
    }

    const std::array<Result<void> (*)(const Json &, AppConfig &), 10U> appliers{{
        &apply_service_config,
        &apply_server_config,
        &apply_http_config,
        &apply_runtime_config,
        &apply_task_config,
        &apply_plugin_config,
        &apply_task_api_config,
        &apply_logging_config,
        &apply_metrics_config,
        &apply_diagnostics_config,
    }};
    for (const auto apply : appliers) {
        auto result = apply(root, config);
        if (!result) {
            return Result<AppConfig>::failure(std::move(result).error());
        }
    }
    auto legacy = apply_legacy_flat_config(root, config);
    if (!legacy) {
        return Result<AppConfig>::failure(std::move(legacy).error());
    }
    auto valid = validate_app_config(config);
    if (!valid) {
        return Result<AppConfig>::failure(std::move(valid).error());
    }
    return Result<AppConfig>::success(std::move(config));
}

} // namespace

AppConfig default_app_config() {
    const unsigned int detected_threads = std::thread::hardware_concurrency();
    const std::size_t workers = std::clamp<std::size_t>(
        detected_threads == 0U ? 1U
                               : static_cast<std::size_t>(detected_threads),
        1U, kMaxWorkerThreads);
    return AppConfig{
        1U,
        ServiceConfig{"IndustrialAIServiceFramework"},
        ServerConfig{"0.0.0.0", 8080U, ReactorConfig{256U, 1024U, 1024U},
                     TcpConfig{128, 1024, 4096, 2 * 1024 * 1024, 4096,
                               512 * 1024, 2 * 1024 * 1024, std::nullopt,
                               std::nullopt}},
        HttpConfig{std::nullopt, std::nullopt,
                   HttpLimitConfig{16 * 1024, 32, 8 * 1024, 8 * 1024, 32 * 1024,
                                   100, 1024 * 1024, 1024 * 1024, 256, 16}},
        RuntimeConfig{workers, 1024U},
        TaskConfig{100000, 256, 512 * 1024, 512 * 1024, 1024, 64, 100000,
                   512 * 1024},
        PluginConfig{true, true,
                     PluginLimitConfig{128, 128, 128, 64, 1024, 1024,
                                       512 * 1024, 512 * 1024, 64, 100000,
                                       512 * 1024, 64, 128}},
        TaskApiConfig{256, 128},
        LoggingConfig{LogLevel::Info, 1024U, 32U, 64U, 1000U,
                      LoggingConsoleConfig{true},
                      LoggingFileConfig{false, {}, 10U * 1024U * 1024U, 3U}},
        MetricsConfig{true, "/metrics"},
        DiagnosticsConfig{false, "/debug/status"},
    };
}

Result<void> validate_app_config(const AppConfig &config) {
    if (config.schema_version != kSupportedSchemaVersion) {
        return config_failure("schema_version must be 1");
    }
    if (config.service.name.empty() ||
        contains_only_whitespace(config.service.name)) {
        return config_failure(
            "service.name must not be empty or whitespace-only");
    }
    if (config.service.name.size() > kMaxServiceNameLength) {
        return config_failure("service.name exceeds 128 bytes");
    }
    if (contains_control_character(config.service.name)) {
        return config_failure(
            "service.name must not contain control characters");
    }
    if (config.server.host.empty() ||
        contains_control_character(config.server.host)) {
        return config_failure(
            "server.host must be a non-empty numeric IPv4 address");
    }
    if (config.server.reactor.max_events == 0U ||
        config.server.reactor.max_events > 65'536U ||
        config.server.reactor.pending_callback_capacity == 0U ||
        config.server.reactor.pending_callback_capacity > 1'000'000U ||
        config.server.reactor.max_timers == 0U ||
        config.server.reactor.max_timers > 1'000'000U) {
        return config_failure(
            "server.reactor values are outside supported ranges");
    }
    if (config.runtime.worker_threads == 0U ||
        config.runtime.worker_threads > kMaxWorkerThreads) {
        return config_failure(
            "runtime.worker_threads is outside the allowed range");
    }
    if (config.runtime.task_queue_capacity == 0U ||
        config.runtime.task_queue_capacity > kMaxTaskQueueCapacity) {
        return config_failure(
            "runtime.task_queue_capacity is outside the allowed range");
    }
    if (!is_known_log_level(config.logging.level)) {
        return config_failure("logging.level is invalid");
    }
    if (config.logging.queue_capacity == 0U ||
        config.logging.queue_capacity > kMaxLogQueueCapacity ||
        config.logging.reserved_critical_capacity >
            config.logging.queue_capacity ||
        config.logging.batch_size == 0U ||
        config.logging.batch_size > config.logging.queue_capacity ||
        config.logging.flush_interval_ms > kMaxLogFlushIntervalMs) {
        return config_failure("logging queue options are outside supported ranges");
    }
    if (!config.logging.console.enabled && !config.logging.file.enabled) {
        return config_failure("at least one logging sink must be enabled");
    }
    if (config.logging.file.max_file_bytes == 0U ||
        config.logging.file.max_file_bytes > kMaxLogFileBytes ||
        config.logging.file.max_archives > kMaxLogArchives) {
        return config_failure("logging file limits are outside supported ranges");
    }
    if (config.logging.file.enabled) {
        if (config.logging.file.path.empty() ||
            contains_only_whitespace(config.logging.file.path) ||
            contains_control_character(config.logging.file.path)) {
            return config_failure(
                "logging.file.path must be a non-empty path without control characters");
        }
    }
    if (config.metrics.endpoint.empty() ||
        config.metrics.endpoint.size() > kMaxMetricsEndpointBytes ||
        config.metrics.endpoint.front() != '/' ||
        config.metrics.endpoint.find_first_of("?#\\") != std::string::npos ||
        contains_control_character(config.metrics.endpoint)) {
        return config_failure(
            "metrics.endpoint must be a valid path of at most 128 bytes");
    }
    if (config.diagnostics.endpoint.empty() ||
        config.diagnostics.endpoint.size() > kMaxDiagnosticsEndpointBytes ||
        config.diagnostics.endpoint.front() != '/' ||
        config.diagnostics.endpoint.find_first_of("?#\\") != std::string::npos ||
        contains_control_character(config.diagnostics.endpoint)) {
        return config_failure(
            "diagnostics.endpoint must be a valid path of at most 128 bytes");
    }
    if (config.diagnostics.enabled &&
        config.server.host.rfind("127.", 0U) != 0U) {
        return config_failure(
            "diagnostics must only be enabled on loopback server.host");
    }
    const auto reserved = [](const std::string& endpoint) {
        return endpoint == "/health" || endpoint == "/ready" ||
               endpoint == "/version" || endpoint == "/v1/tasks" ||
               endpoint.rfind("/v1/tasks/", 0U) == 0U;
    };
    if (reserved(config.diagnostics.endpoint) ||
        (config.metrics.enabled &&
         config.diagnostics.endpoint == config.metrics.endpoint)) {
        return config_failure("diagnostics endpoint conflicts with a service route");
    }
    return Result<void>::success();
}

Result<AppConfig> load_app_config(const std::filesystem::path &path) {
    try {
        std::error_code filesystem_error;
        const bool exists = std::filesystem::exists(path, filesystem_error);
        if (filesystem_error) {
            return Result<AppConfig>::failure(make_error(
                ErrorCode::IoError, "unable to inspect configuration file"));
        }
        if (!exists) {
            return Result<AppConfig>::failure(make_error(
                ErrorCode::IoError, "configuration file does not exist"));
        }
        if (!std::filesystem::is_regular_file(path, filesystem_error) ||
            filesystem_error) {
            return Result<AppConfig>::failure(
                make_error(ErrorCode::IoError,
                           "configuration path is not a readable file"));
        }
        const auto file_bytes =
            std::filesystem::file_size(path, filesystem_error);
        if (filesystem_error) {
            return Result<AppConfig>::failure(
                make_error(ErrorCode::IoError,
                           "unable to inspect configuration file size"));
        }
        if (file_bytes > kMaxConfigurationFileBytes) {
            return typed_config_failure<AppConfig>(
                "configuration file exceeds the 1 MiB limit");
        }

        std::ifstream input{path, std::ios::binary};
        if (!input.is_open()) {
            return Result<AppConfig>::failure(make_error(
                ErrorCode::IoError, "unable to open configuration file"));
        }
        std::string contents;
        contents.reserve(static_cast<std::size_t>(file_bytes));
        std::array<char, 4096U> buffer{};
        while (input) {
            input.read(buffer.data(),
                       static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count > 0) {
                const auto bytes = static_cast<std::size_t>(count);
                if (bytes > kMaxConfigurationFileBytes - contents.size()) {
                    return typed_config_failure<AppConfig>(
                        "configuration file exceeds the 1 MiB limit");
                }
                contents.append(buffer.data(), bytes);
            }
        }
        if (input.bad()) {
            return Result<AppConfig>::failure(make_error(
                ErrorCode::IoError, "unable to read configuration file"));
        }
        return parse_app_config(contents);
    } catch (const std::bad_alloc &) {
        return Result<AppConfig>::failure(
            make_error(ErrorCode::ResourceExhausted,
                       "unable to allocate configuration storage"));
    } catch (const Json::exception &) {
        return typed_config_failure<AppConfig>(
            "configuration contains invalid JSON data");
    } catch (const std::exception &) {
        return Result<AppConfig>::failure(make_error(
            ErrorCode::IoError, "unable to load configuration file"));
    }
}

} // namespace iaisf
