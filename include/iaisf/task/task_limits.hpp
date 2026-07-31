#pragma once

#include <cstddef>
#include <cstdint>

#include <nlohmann/json_fwd.hpp>

#include "iaisf/core/error.hpp"
#include "iaisf/core/result.hpp"
#include "iaisf/task/task_types.hpp"

namespace iaisf::task {

/**
 * Validated immutable limits for the in-memory task runtime.
 *
 * JSON limits cover compact serialized UTF-8 bytes, depth, total value nodes,
 * and bytes per string/object key. Operation and error limits use std::string
 * byte counts.
 */
class TaskLimits {
public:
    [[nodiscard]] static Result<TaskLimits> create(
        std::int64_t repository_capacity = 100000,
        std::int64_t max_operation_bytes = 256,
        std::int64_t max_input_bytes = 1024 * 1024,
        std::int64_t max_result_bytes = 4 * 1024 * 1024,
        std::int64_t max_error_bytes = 1024,
        std::int64_t max_json_depth = 64,
        std::int64_t max_json_elements = 100000,
        std::int64_t max_json_string_bytes = 1024 * 1024);

    [[nodiscard]] std::size_t max_repository_tasks() const noexcept;
    [[nodiscard]] std::size_t max_operation_bytes() const noexcept;
    [[nodiscard]] std::size_t max_input_bytes() const noexcept;
    [[nodiscard]] std::size_t max_result_bytes() const noexcept;
    [[nodiscard]] std::size_t max_error_message_bytes() const noexcept;
    [[nodiscard]] std::size_t max_json_depth() const noexcept;
    [[nodiscard]] std::size_t max_json_elements() const noexcept;
    [[nodiscard]] std::size_t max_json_string_bytes() const noexcept;

    [[nodiscard]] Result<void> validate_request(const TaskRequest& request) const;
    [[nodiscard]] Result<void> validate_result(const nlohmann::json& result) const;
    [[nodiscard]] Result<Error> sanitize_error(const Error& error) const;

private:
    TaskLimits(
        std::size_t repository_capacity,
        std::size_t max_operation_bytes,
        std::size_t max_input_bytes,
        std::size_t max_result_bytes,
        std::size_t max_error_bytes,
        std::size_t max_json_depth,
        std::size_t max_json_elements,
        std::size_t max_json_string_bytes) noexcept;

    std::size_t repository_capacity_;
    std::size_t max_operation_bytes_;
    std::size_t max_input_bytes_;
    std::size_t max_result_bytes_;
    std::size_t max_error_bytes_;
    std::size_t max_json_depth_;
    std::size_t max_json_elements_;
    std::size_t max_json_string_bytes_;
};

}  // namespace iaisf::task
