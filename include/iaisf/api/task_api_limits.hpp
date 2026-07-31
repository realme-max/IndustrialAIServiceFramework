#pragma once

#include <cstddef>
#include <cstdint>

#include "iaisf/core/result.hpp"
#include "iaisf/http/http_limits.hpp"
#include "iaisf/plugin/plugin_limits.hpp"
#include "iaisf/task/task_limits.hpp"

namespace iaisf::api {

/** Small immutable limits owned by the HTTP task facade. */
class TaskApiLimits final {
public:
    [[nodiscard]] static Result<TaskApiLimits> create(
        std::int64_t max_error_message_bytes = 256,
        std::int64_t max_status_url_bytes = 128);
    [[nodiscard]] static TaskApiLimits defaults() noexcept;

    [[nodiscard]] std::size_t max_error_message_bytes() const noexcept;
    [[nodiscard]] std::size_t max_status_url_bytes() const noexcept;

private:
    TaskApiLimits(
        std::size_t max_error_message_bytes,
        std::size_t max_status_url_bytes) noexcept;

    std::size_t max_error_message_bytes_;
    std::size_t max_status_url_bytes_;
};

/**
 * Validates every compact JSON and HTTP framing relationship used by the task
 * API. This function allocates no worker, socket, Channel, or route.
 */
[[nodiscard]] Result<void> validate_task_api_capacity(
    const task::TaskLimits& task_limits,
    const plugin::PluginLimits& plugin_limits,
    const http::HttpLimits& http_limits,
    const TaskApiLimits& api_limits);

}  // namespace iaisf::api
