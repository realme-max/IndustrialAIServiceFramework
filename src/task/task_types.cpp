#include "iaisf/task/task_types.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <exception>
#include <new>
#include <system_error>

namespace iaisf::task {

std::string TaskId::to_string() const {
    std::array<char, kMaximumDecimalDigits> digits{};
    const auto converted = std::to_chars(
        digits.data(),
        digits.data() + digits.size(),
        value_);
    if (converted.ec != std::errc{}) {
        std::terminate();
    }
    const auto digit_count =
        static_cast<std::size_t>(converted.ptr - digits.data());
    const auto padded_count = std::max(kMinimumDecimalDigits, digit_count);

    std::string text;
    text.reserve(kTextPrefix.size() + padded_count);
    text.append(kTextPrefix);
    text.append(padded_count - digit_count, '0');
    text.append(digits.data(), digit_count);
    return text;
}

Result<TaskId> TaskId::parse(const std::string_view text) {
    if (text.size() < kTextPrefix.size() + kMinimumDecimalDigits ||
        text.size() > kMaximumTextBytes ||
        text.compare(0U, kTextPrefix.size(), kTextPrefix) != 0) {
        return Result<TaskId>::failure(make_error(
            ErrorCode::InvalidArgument,
            "task id is not in canonical form"));
    }
    const auto digits = text.substr(kTextPrefix.size());
    if (!std::all_of(
            digits.begin(),
            digits.end(),
            [](const char character) {
                return character >= '0' && character <= '9';
            })) {
        return Result<TaskId>::failure(make_error(
            ErrorCode::InvalidArgument,
            "task id is not in canonical form"));
    }

    std::uint64_t value = 0U;
    const auto converted = std::from_chars(
        digits.data(),
        digits.data() + digits.size(),
        value);
    if (converted.ec != std::errc{} ||
        converted.ptr != digits.data() + digits.size() ||
        value == 0U) {
        return Result<TaskId>::failure(make_error(
            ErrorCode::InvalidArgument,
            "task id is outside the canonical range"));
    }
    try {
        TaskId id{value};
        if (id.to_string() != text) {
            return Result<TaskId>::failure(make_error(
                ErrorCode::InvalidArgument,
                "task id is not in canonical form"));
        }
        return Result<TaskId>::success(id);
    } catch (const std::bad_alloc&) {
        return Result<TaskId>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to validate task id"));
    } catch (const std::exception&) {
        return Result<TaskId>::failure(make_error(
            ErrorCode::InternalError,
            "unable to validate task id"));
    }
}

std::string_view to_string(const TaskState state) noexcept {
    switch (state) {
        case TaskState::Queued:
            return "queued";
        case TaskState::Running:
            return "running";
        case TaskState::Succeeded:
            return "succeeded";
        case TaskState::Failed:
            return "failed";
        case TaskState::TimedOut:
            return "timed_out";
    }
    return "unknown";
}

bool is_terminal(const TaskState state) noexcept {
    return state == TaskState::Succeeded || state == TaskState::Failed ||
           state == TaskState::TimedOut;
}

}  // namespace iaisf::task
