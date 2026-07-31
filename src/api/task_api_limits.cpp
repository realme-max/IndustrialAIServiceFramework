#include "iaisf/api/task_api_limits.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>

#include "iaisf/core/error.hpp"
#include "iaisf/task/task_types.hpp"

namespace iaisf::api {
namespace {

constexpr std::int64_t kMaximumApiStringBytes = 64 * 1024;

bool can_add(const std::size_t left, const std::size_t right) noexcept {
    return right <= std::numeric_limits<std::size_t>::max() - left;
}

Result<std::size_t> checked_add(
    const std::size_t left,
    const std::size_t right) {
    if (!can_add(left, right)) {
        return Result<std::size_t>::failure(make_error(
            ErrorCode::InvalidArgument,
            "task API capacity calculation overflowed"));
    }
    return Result<std::size_t>::success(left + right);
}

Result<std::size_t> checked_sum(
    const std::initializer_list<std::size_t> values) {
    std::size_t total = 0U;
    for (const auto value : values) {
        auto next = checked_add(total, value);
        if (!next) {
            return next;
        }
        total = next.value();
    }
    return Result<std::size_t>::success(total);
}

std::size_t decimal_digits(std::size_t value) noexcept {
    std::size_t digits = 1U;
    while (value >= 10U) {
        value /= 10U;
        ++digits;
    }
    return digits;
}

}  // namespace

Result<TaskApiLimits> TaskApiLimits::create(
    const std::int64_t max_error_message_bytes,
    const std::int64_t max_status_url_bytes) {
    if (max_error_message_bytes <= 0 ||
        max_error_message_bytes > kMaximumApiStringBytes ||
        max_status_url_bytes <= 0 ||
        max_status_url_bytes > kMaximumApiStringBytes) {
        return Result<TaskApiLimits>::failure(make_error(
            ErrorCode::InvalidArgument,
            "task API limits are outside the supported range"));
    }
    return Result<TaskApiLimits>::success(TaskApiLimits{
        static_cast<std::size_t>(max_error_message_bytes),
        static_cast<std::size_t>(max_status_url_bytes)});
}

TaskApiLimits TaskApiLimits::defaults() noexcept {
    return TaskApiLimits{256U, 128U};
}

TaskApiLimits::TaskApiLimits(
    const std::size_t max_error_message_bytes,
    const std::size_t max_status_url_bytes) noexcept
    : max_error_message_bytes_(max_error_message_bytes),
      max_status_url_bytes_(max_status_url_bytes) {}

std::size_t TaskApiLimits::max_error_message_bytes() const noexcept {
    return max_error_message_bytes_;
}

std::size_t TaskApiLimits::max_status_url_bytes() const noexcept {
    return max_status_url_bytes_;
}

Result<void> validate_task_api_capacity(
    const task::TaskLimits& task_limits,
    const plugin::PluginLimits& plugin_limits,
    const http::HttpLimits& http_limits,
    const TaskApiLimits& api_limits) {
    constexpr std::string_view kStatusPathPrefix{"/v1/tasks/"};
    constexpr std::string_view kRequestSkeleton{
        R"({"input":null,"operation":""})"};
    constexpr std::string_view kSuccessSkeleton{
        R"({"operation":"","result":null,"state":"succeeded","task_id":""})"};
    constexpr std::string_view kFailureBody{
        R"({"error":{"code":"internal_error","message":"task execution failed"},"operation":"","state":"failed","task_id":""})"};
    constexpr std::string_view kApiErrorSkeleton{
        R"({"error":{"code":"internal_error","message":""}})"};
    constexpr std::string_view kJsonContentTypeLine{
        "Content-Type: application/json; charset=utf-8\r\n"};
    constexpr std::string_view kConnectionLine{
        "Connection: keep-alive\r\n"};
    constexpr std::string_view kLongestStatusLine{
        "HTTP/1.1 415 Unsupported Media Type\r\n"};
    constexpr std::size_t kNullBytes = 4U;

    if (plugin_limits.max_operation_bytes() >
            task_limits.max_operation_bytes() ||
        plugin_limits.max_input_bytes() > task_limits.max_input_bytes() ||
        plugin_limits.max_output_bytes() > task_limits.max_result_bytes()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "plugin limits must fit inside task runtime limits"));
    }

    auto status_url_bytes = checked_sum({
        kStatusPathPrefix.size(),
        task::TaskId::kMaximumTextBytes});
    if (!status_url_bytes ||
        status_url_bytes.value() > api_limits.max_status_url_bytes() ||
        status_url_bytes.value() > http_limits.max_target_bytes()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "task status URL does not fit configured API and HTTP limits"));
    }

    auto request_bytes = checked_sum({
        kRequestSkeleton.size() - kNullBytes,
        plugin_limits.max_input_bytes(),
        plugin_limits.max_operation_bytes()});
    if (!request_bytes ||
        request_bytes.value() > http_limits.max_body_bytes()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "maximum compact task request does not fit the HTTP body limit"));
    }

    auto success_bytes = checked_sum({
        kSuccessSkeleton.size() - kNullBytes,
        plugin_limits.max_output_bytes(),
        plugin_limits.max_operation_bytes(),
        task::TaskId::kMaximumTextBytes});
    auto failure_bytes = checked_sum({
        kFailureBody.size(),
        plugin_limits.max_operation_bytes(),
        task::TaskId::kMaximumTextBytes});
    auto api_error_bytes = checked_sum({
        kApiErrorSkeleton.size(),
        api_limits.max_error_message_bytes()});
    if (!success_bytes || !failure_bytes || !api_error_bytes) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "task response capacity calculation overflowed"));
    }
    const auto required_response_bytes = std::max({
        success_bytes.value(),
        failure_bytes.value(),
        api_error_bytes.value()});
    if (required_response_bytes > http_limits.max_response_body_bytes()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "maximum task response does not fit the HTTP body limit"));
    }

    const auto content_length_line_bytes =
        std::string_view{"Content-Length: "}.size() +
        decimal_digits(http_limits.max_response_body_bytes()) + 2U;
    const auto required_header_line = std::max({
        kJsonContentTypeLine.size(),
        kConnectionLine.size(),
        content_length_line_bytes});
    auto required_header_bytes = checked_sum({
        kLongestStatusLine.size(),
        kJsonContentTypeLine.size(),
        content_length_line_bytes,
        kConnectionLine.size(),
        2U});
    if (!required_header_bytes ||
        http_limits.max_header_count() < 3U ||
        http_limits.max_header_line_bytes() < required_header_line ||
        http_limits.max_header_bytes() < required_header_bytes.value()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "HTTP header limits cannot frame task API JSON responses"));
    }
    return Result<void>::success();
}

}  // namespace iaisf::api
