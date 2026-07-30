#include "iaisf/config/app_config.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

namespace iaisf {
namespace {

using Json = nlohmann::json;

Result<void> config_failure(std::string message) {
    return Result<void>::failure(
        make_error(ErrorCode::ConfigError, std::move(message)));
}

template <typename T>
Result<T> typed_config_failure(std::string message) {
    return Result<T>::failure(
        make_error(ErrorCode::ConfigError, std::move(message)));
}

bool contains_only_whitespace(const std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](const char raw_character) {
        const auto character = static_cast<unsigned char>(raw_character);
        return character == ' ' || character == '\t' || character == '\n' ||
               character == '\r' || character == '\f' || character == '\v';
    });
}

bool contains_control_character(const std::string_view value) {
    return std::any_of(value.begin(), value.end(), [](const char raw_character) {
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

Result<void> reject_unknown_fields(
    const Json& object,
    const std::initializer_list<std::string_view> allowed_fields,
    const std::string_view group_name) {
    for (const auto& item : object.items()) {
        const std::string_view key{item.key()};
        const bool allowed =
            std::find(allowed_fields.begin(), allowed_fields.end(), key) !=
            allowed_fields.end();
        if (!allowed) {
            return config_failure(
                "unknown field in " + std::string{group_name} + ": " + item.key());
        }
    }
    return Result<void>::success();
}

Result<std::size_t> read_bounded_positive_size(
    const Json& value,
    const std::string_view field_name,
    const std::size_t maximum) {
    std::uint64_t parsed_value = 0;

    if (value.is_number_unsigned()) {
        parsed_value = value.get<std::uint64_t>();
    } else if (value.is_number_integer()) {
        const std::int64_t signed_value = value.get<std::int64_t>();
        if (signed_value <= 0) {
            return typed_config_failure<std::size_t>(
                std::string{field_name} + " must be greater than zero");
        }
        parsed_value = static_cast<std::uint64_t>(signed_value);
    } else {
        return typed_config_failure<std::size_t>(
            std::string{field_name} + " must be an integer");
    }

    if (parsed_value == 0U) {
        return typed_config_failure<std::size_t>(
            std::string{field_name} + " must be greater than zero");
    }
    if (parsed_value > static_cast<std::uint64_t>(maximum)) {
        return typed_config_failure<std::size_t>(
            std::string{field_name} + " exceeds the allowed maximum");
    }

    return Result<std::size_t>::success(static_cast<std::size_t>(parsed_value));
}

Result<void> apply_service_config(const Json& root, AppConfig& config) {
    if (!root.contains("service")) {
        return Result<void>::success();
    }

    const Json& service = root.at("service");
    if (!service.is_object()) {
        return config_failure("service must be an object");
    }

    auto fields_result = reject_unknown_fields(service, {"name"}, "service");
    if (!fields_result) {
        return fields_result;
    }

    if (service.contains("name")) {
        const Json& name = service.at("name");
        if (!name.is_string()) {
            return config_failure("service.name must be a string");
        }
        config.service_name = name.get<std::string>();
    }
    return Result<void>::success();
}

Result<void> apply_runtime_config(const Json& root, AppConfig& config) {
    if (!root.contains("runtime")) {
        return Result<void>::success();
    }

    const Json& runtime = root.at("runtime");
    if (!runtime.is_object()) {
        return config_failure("runtime must be an object");
    }

    auto fields_result = reject_unknown_fields(
        runtime, {"worker_threads", "task_queue_capacity"}, "runtime");
    if (!fields_result) {
        return fields_result;
    }

    if (runtime.contains("worker_threads")) {
        auto worker_result = read_bounded_positive_size(
            runtime.at("worker_threads"), "runtime.worker_threads", kMaxWorkerThreads);
        if (!worker_result) {
            return Result<void>::failure(worker_result.error());
        }
        config.worker_threads = worker_result.value();
    }

    if (runtime.contains("task_queue_capacity")) {
        auto capacity_result = read_bounded_positive_size(
            runtime.at("task_queue_capacity"),
            "runtime.task_queue_capacity",
            kMaxTaskQueueCapacity);
        if (!capacity_result) {
            return Result<void>::failure(capacity_result.error());
        }
        config.task_queue_capacity = capacity_result.value();
    }
    return Result<void>::success();
}

Result<void> apply_logging_config(const Json& root, AppConfig& config) {
    if (!root.contains("logging")) {
        return Result<void>::success();
    }

    const Json& logging = root.at("logging");
    if (!logging.is_object()) {
        return config_failure("logging must be an object");
    }

    auto fields_result = reject_unknown_fields(logging, {"level"}, "logging");
    if (!fields_result) {
        return fields_result;
    }

    if (logging.contains("level")) {
        const Json& level = logging.at("level");
        if (!level.is_string()) {
            return config_failure("logging.level must be a string");
        }
        auto level_result = parse_log_level(level.get<std::string>());
        if (!level_result) {
            return config_failure(
                "logging.level must be one of trace, debug, info, warn, error");
        }
        config.log_level = level_result.value();
    }
    return Result<void>::success();
}

Result<AppConfig> parse_app_config(const std::string& contents) {
    Json root = Json::parse(contents, nullptr, false);
    if (root.is_discarded()) {
        return typed_config_failure<AppConfig>("configuration file is not valid JSON");
    }
    if (!root.is_object()) {
        return typed_config_failure<AppConfig>("configuration root must be an object");
    }

    auto top_level_result =
        reject_unknown_fields(root, {"service", "runtime", "logging"}, "root");
    if (!top_level_result) {
        return Result<AppConfig>::failure(top_level_result.error());
    }

    AppConfig config = default_app_config();
    auto service_result = apply_service_config(root, config);
    if (!service_result) {
        return Result<AppConfig>::failure(service_result.error());
    }
    auto runtime_result = apply_runtime_config(root, config);
    if (!runtime_result) {
        return Result<AppConfig>::failure(runtime_result.error());
    }
    auto logging_result = apply_logging_config(root, config);
    if (!logging_result) {
        return Result<AppConfig>::failure(logging_result.error());
    }

    auto validation_result = validate_app_config(config);
    if (!validation_result) {
        return Result<AppConfig>::failure(validation_result.error());
    }
    return Result<AppConfig>::success(std::move(config));
}

}  // namespace

AppConfig default_app_config() {
    const unsigned int detected_threads = std::thread::hardware_concurrency();
    const std::size_t worker_threads = std::clamp<std::size_t>(
        detected_threads == 0U ? 1U : static_cast<std::size_t>(detected_threads),
        1U,
        kMaxWorkerThreads);

    return AppConfig{
        "IndustrialAIServiceFramework",
        worker_threads,
        1024U,
        LogLevel::Info,
    };
}

Result<void> validate_app_config(const AppConfig& config) {
    if (config.service_name.empty() || contains_only_whitespace(config.service_name)) {
        return config_failure("service.name must not be empty or whitespace-only");
    }
    if (config.service_name.size() > kMaxServiceNameLength) {
        return config_failure("service.name exceeds 128 bytes");
    }
    if (contains_control_character(config.service_name)) {
        return config_failure("service.name must not contain control characters");
    }
    if (config.worker_threads == 0U || config.worker_threads > kMaxWorkerThreads) {
        return config_failure("runtime.worker_threads is outside the allowed range");
    }
    if (config.task_queue_capacity == 0U ||
        config.task_queue_capacity > kMaxTaskQueueCapacity) {
        return config_failure("runtime.task_queue_capacity is outside the allowed range");
    }
    if (!is_known_log_level(config.log_level)) {
        return config_failure("logging.level is invalid");
    }
    return Result<void>::success();
}

Result<AppConfig> load_app_config(const std::filesystem::path& path) {
    std::error_code filesystem_error;
    const bool exists = std::filesystem::exists(path, filesystem_error);
    if (filesystem_error) {
        return Result<AppConfig>::failure(
            make_error(ErrorCode::IoError, "unable to inspect configuration file"));
    }
    if (!exists) {
        return Result<AppConfig>::failure(
            make_error(ErrorCode::IoError, "configuration file does not exist"));
    }
    if (!std::filesystem::is_regular_file(path, filesystem_error) || filesystem_error) {
        return Result<AppConfig>::failure(
            make_error(ErrorCode::IoError, "configuration path is not a readable file"));
    }

    std::ifstream input{path};
    if (!input.is_open()) {
        return Result<AppConfig>::failure(
            make_error(ErrorCode::IoError, "unable to open configuration file"));
    }

    std::ostringstream contents;
    contents << input.rdbuf();
    if (input.bad()) {
        return Result<AppConfig>::failure(
            make_error(ErrorCode::IoError, "unable to read configuration file"));
    }

    try {
        return parse_app_config(contents.str());
    } catch (const Json::exception&) {
        return typed_config_failure<AppConfig>(
            "configuration contains invalid JSON data");
    }
}

}  // namespace iaisf
