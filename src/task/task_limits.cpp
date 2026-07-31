#include "iaisf/task/task_limits.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "iaisf/core/json_value_limits.hpp"

namespace iaisf::task {
namespace {

constexpr std::int64_t kMaximumRepositoryCapacity = 1000000;
constexpr std::int64_t kMaximumOperationBytes = 4096;
constexpr std::int64_t kMaximumJsonBytes = 64 * 1024 * 1024;
constexpr std::int64_t kMaximumErrorBytes = 64 * 1024;
constexpr std::int64_t kMaximumJsonDepth = 256;
constexpr std::int64_t kMaximumJsonElements = 1000000;
constexpr std::int64_t kMaximumJsonStringBytes = 16 * 1024 * 1024;

Result<std::size_t> checked_limit(
    const std::int64_t value,
    const std::int64_t maximum,
    const char* const name) {
    if (value <= 0 || value > maximum) {
        return Result<std::size_t>::failure(make_error(
            ErrorCode::InvalidArgument,
            std::string{name} + " is outside the supported range"));
    }
    return Result<std::size_t>::success(static_cast<std::size_t>(value));
}

}  // namespace

Result<TaskLimits> TaskLimits::create(
    const std::int64_t repository_capacity,
    const std::int64_t max_operation_bytes,
    const std::int64_t max_input_bytes,
    const std::int64_t max_result_bytes,
    const std::int64_t max_error_bytes,
    const std::int64_t max_json_depth,
    const std::int64_t max_json_elements,
    const std::int64_t max_json_string_bytes) {
    const auto capacity = checked_limit(
        repository_capacity, kMaximumRepositoryCapacity, "repository capacity");
    if (!capacity) {
        return Result<TaskLimits>::failure(capacity.error());
    }
    const auto operation =
        checked_limit(max_operation_bytes, kMaximumOperationBytes, "operation limit");
    if (!operation) {
        return Result<TaskLimits>::failure(operation.error());
    }
    const auto input =
        checked_limit(max_input_bytes, kMaximumJsonBytes, "input limit");
    if (!input) {
        return Result<TaskLimits>::failure(input.error());
    }
    const auto result =
        checked_limit(max_result_bytes, kMaximumJsonBytes, "result limit");
    if (!result) {
        return Result<TaskLimits>::failure(result.error());
    }
    const auto error =
        checked_limit(max_error_bytes, kMaximumErrorBytes, "error limit");
    if (!error) {
        return Result<TaskLimits>::failure(error.error());
    }
    const auto depth =
        checked_limit(max_json_depth, kMaximumJsonDepth, "JSON depth limit");
    if (!depth) {
        return Result<TaskLimits>::failure(depth.error());
    }
    const auto elements = checked_limit(
        max_json_elements,
        kMaximumJsonElements,
        "JSON element limit");
    if (!elements) {
        return Result<TaskLimits>::failure(elements.error());
    }
    const auto string_bytes = checked_limit(
        max_json_string_bytes,
        kMaximumJsonStringBytes,
        "JSON string limit");
    if (!string_bytes) {
        return Result<TaskLimits>::failure(string_bytes.error());
    }

    return Result<TaskLimits>::success(TaskLimits{
        capacity.value(),
        operation.value(),
        input.value(),
        result.value(),
        error.value(),
        depth.value(),
        elements.value(),
        string_bytes.value()});
}

TaskLimits::TaskLimits(
    const std::size_t repository_capacity,
    const std::size_t max_operation_bytes,
    const std::size_t max_input_bytes,
    const std::size_t max_result_bytes,
    const std::size_t max_error_bytes,
    const std::size_t max_json_depth,
    const std::size_t max_json_elements,
    const std::size_t max_json_string_bytes) noexcept
    : repository_capacity_(repository_capacity),
      max_operation_bytes_(max_operation_bytes),
      max_input_bytes_(max_input_bytes),
      max_result_bytes_(max_result_bytes),
      max_error_bytes_(max_error_bytes),
      max_json_depth_(max_json_depth),
      max_json_elements_(max_json_elements),
      max_json_string_bytes_(max_json_string_bytes) {}

std::size_t TaskLimits::max_repository_tasks() const noexcept {
    return repository_capacity_;
}

std::size_t TaskLimits::max_operation_bytes() const noexcept {
    return max_operation_bytes_;
}

std::size_t TaskLimits::max_input_bytes() const noexcept {
    return max_input_bytes_;
}

std::size_t TaskLimits::max_result_bytes() const noexcept {
    return max_result_bytes_;
}

std::size_t TaskLimits::max_error_message_bytes() const noexcept {
    return max_error_bytes_;
}

std::size_t TaskLimits::max_json_depth() const noexcept {
    return max_json_depth_;
}

std::size_t TaskLimits::max_json_elements() const noexcept {
    return max_json_elements_;
}

std::size_t TaskLimits::max_json_string_bytes() const noexcept {
    return max_json_string_bytes_;
}

Result<void> TaskLimits::validate_request(const TaskRequest& request) const {
    if (request.operation.empty()) {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidArgument, "task operation must not be empty"));
    }
    if (request.operation.size() > max_operation_bytes_) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "task operation exceeds the configured byte limit"));
    }

    const bool only_whitespace = std::all_of(
        request.operation.begin(),
        request.operation.end(),
        [](const char character) {
            return std::isspace(static_cast<unsigned char>(character)) != 0;
        });
    const bool contains_control = std::any_of(
        request.operation.begin(),
        request.operation.end(),
        [](const char character) {
            const auto byte = static_cast<unsigned char>(character);
            return byte < 0x20U || byte == 0x7FU;
        });
    if (only_whitespace || contains_control) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "task operation contains invalid characters"));
    }

    const auto input = validate_json_value(
        request.input,
        JsonValueLimits{
            max_input_bytes_,
            max_json_depth_,
            max_json_elements_,
            max_json_string_bytes_,
        },
        "task input");
    if (!input) {
        return Result<void>::failure(input.error());
    }
    return Result<void>::success();
}

Result<void> TaskLimits::validate_result(const nlohmann::json& result) const {
    const auto valid = validate_json_value(
        result,
        JsonValueLimits{
            max_result_bytes_,
            max_json_depth_,
            max_json_elements_,
            max_json_string_bytes_,
        },
        "task result");
    if (!valid) {
        return Result<void>::failure(valid.error());
    }
    return Result<void>::success();
}

Result<Error> TaskLimits::sanitize_error(const Error& error) const {
    try {
        Error sanitized = error;
        if (sanitized.message.empty()) {
            sanitized.message = "unspecified error";
        }
        if (sanitized.message.size() > max_error_bytes_) {
            sanitized.message.assign(max_error_bytes_, '#');
        }
        return Result<Error>::success(std::move(sanitized));
    } catch (const std::bad_alloc&) {
        return Result<Error>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate sanitized task error"));
    } catch (const std::length_error&) {
        return Result<Error>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "task error exceeds the platform size limit"));
    }
}

}  // namespace iaisf::task
