#include "iaisf/task/task_limits.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace iaisf::task {
namespace {

constexpr std::int64_t kMaximumRepositoryCapacity = 1000000;
constexpr std::int64_t kMaximumOperationBytes = 4096;
constexpr std::int64_t kMaximumJsonBytes = 64 * 1024 * 1024;
constexpr std::int64_t kMaximumErrorBytes = 64 * 1024;

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

Result<std::size_t> serialized_size(const nlohmann::json& value) {
    try {
        return Result<std::size_t>::success(value.dump().size());
    } catch (const nlohmann::json::exception&) {
        return Result<std::size_t>::failure(make_error(
            ErrorCode::InvalidArgument,
            "JSON value cannot be serialized as valid UTF-8"));
    } catch (const std::bad_alloc&) {
        return Result<std::size_t>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate JSON serialization storage"));
    } catch (const std::length_error&) {
        return Result<std::size_t>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "JSON serialization exceeds the platform size limit"));
    }
}

}  // namespace

Result<TaskLimits> TaskLimits::create(
    const std::int64_t repository_capacity,
    const std::int64_t max_operation_bytes,
    const std::int64_t max_input_bytes,
    const std::int64_t max_result_bytes,
    const std::int64_t max_error_bytes) {
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

    return Result<TaskLimits>::success(TaskLimits{
        capacity.value(),
        operation.value(),
        input.value(),
        result.value(),
        error.value()});
}

TaskLimits::TaskLimits(
    const std::size_t repository_capacity,
    const std::size_t max_operation_bytes,
    const std::size_t max_input_bytes,
    const std::size_t max_result_bytes,
    const std::size_t max_error_bytes) noexcept
    : repository_capacity_(repository_capacity),
      max_operation_bytes_(max_operation_bytes),
      max_input_bytes_(max_input_bytes),
      max_result_bytes_(max_result_bytes),
      max_error_bytes_(max_error_bytes) {}

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

    const auto input_size = serialized_size(request.input);
    if (!input_size) {
        return Result<void>::failure(input_size.error());
    }
    if (input_size.value() > max_input_bytes_) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "task input exceeds the configured serialized byte limit"));
    }
    return Result<void>::success();
}

Result<void> TaskLimits::validate_result(const nlohmann::json& result) const {
    const auto result_size = serialized_size(result);
    if (!result_size) {
        return Result<void>::failure(result_size.error());
    }
    if (result_size.value() > max_result_bytes_) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "task result exceeds the configured serialized byte limit"));
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
