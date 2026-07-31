#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "iaisf/core/error.hpp"
#include "iaisf/core/result.hpp"

namespace iaisf::task {

class TaskId {
public:
    constexpr TaskId() noexcept = default;
    explicit constexpr TaskId(const std::uint64_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr bool valid() const noexcept {
        return value_ != 0;
    }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept {
        return value_;
    }

    [[nodiscard]] std::string to_string() const;

    friend constexpr bool operator==(const TaskId lhs, const TaskId rhs) noexcept {
        return lhs.value_ == rhs.value_;
    }

    friend constexpr bool operator!=(const TaskId lhs, const TaskId rhs) noexcept {
        return !(lhs == rhs);
    }

private:
    std::uint64_t value_{0};
};

enum class TaskState {
    Queued,
    Running,
    Succeeded,
    Failed,
    TimedOut,
};

[[nodiscard]] std::string_view to_string(TaskState state) noexcept;
[[nodiscard]] bool is_terminal(TaskState state) noexcept;

struct TaskRequest {
    // TaskManager copies both fields before returning a successful submit().
    std::string operation;
    nlohmann::json input{nlohmann::json::object()};
};

using TaskHandler = std::function<Result<nlohmann::json>(const TaskRequest&)>;
using TaskValidator = std::function<Result<void>(const TaskRequest&)>;

struct TaskSnapshot {
    TaskId id;
    std::string operation;
    TaskState state{TaskState::Queued};
    std::chrono::system_clock::time_point created_at;
    std::optional<std::chrono::system_clock::time_point> started_at;
    std::optional<std::chrono::system_clock::time_point> finished_at;
    std::optional<nlohmann::json> result;
    std::optional<Error> error;
};

}  // namespace iaisf::task

namespace std {

template <>
struct hash<iaisf::task::TaskId> {
    [[nodiscard]] size_t operator()(const iaisf::task::TaskId id) const noexcept {
        return hash<std::uint64_t>{}(id.value());
    }
};

}  // namespace std
