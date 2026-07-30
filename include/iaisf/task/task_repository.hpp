#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "iaisf/core/result.hpp"
#include "iaisf/task/task_limits.hpp"
#include "iaisf/task/task_types.hpp"

namespace iaisf::task {

class TaskRepositoryTestAccess;

enum class TransitionOutcome {
    Applied,
    AlreadyTerminal,
};

/**
 * Bounded in-memory repository and sole arbiter of task state transitions.
 *
 * All public methods are thread-safe. Returned snapshots are independent
 * copies. No user callback is invoked while the repository mutex is held.
 */
class TaskRepository {
public:
    explicit TaskRepository(TaskLimits limits);

    TaskRepository(const TaskRepository&) = delete;
    TaskRepository& operator=(const TaskRepository&) = delete;

    [[nodiscard]] Result<TaskId> create_queued(const TaskRequest& request);
    [[nodiscard]] Result<void> rollback_queued(TaskId id);
    [[nodiscard]] Result<TransitionOutcome> mark_running(TaskId id);
    [[nodiscard]] Result<TransitionOutcome> mark_succeeded(
        TaskId id,
        const nlohmann::json& result);
    [[nodiscard]] Result<TransitionOutcome> mark_failed(
        TaskId id,
        const Error& error);
    [[nodiscard]] Result<TransitionOutcome> mark_timed_out(
        TaskId id,
        const Error& error = Error{ErrorCode::InvalidState, "task timed out"});

    [[nodiscard]] Result<TaskSnapshot> get_snapshot(TaskId id) const;
    [[nodiscard]] Result<void> erase_terminal(TaskId id);

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] const TaskLimits& limits() const noexcept;

private:
    friend class TaskRepositoryTestAccess;

    struct Record {
        TaskId id;
        TaskRequest request;
        TaskState state{TaskState::Queued};
        std::chrono::system_clock::time_point created_at;
        std::optional<std::chrono::system_clock::time_point> started_at;
        std::optional<std::chrono::system_clock::time_point> finished_at;
        std::optional<nlohmann::json> result;
        std::optional<Error> error;
    };

    [[nodiscard]] Result<TransitionOutcome> mark_error_terminal(
        TaskId id,
        TaskState terminal_state,
        const Error& error);
    [[nodiscard]] static std::chrono::system_clock::time_point ordered_now(
        std::chrono::system_clock::time_point lower_bound) noexcept;

    TaskLimits limits_;
    mutable std::mutex mutex_;
    std::unordered_map<TaskId, Record> records_;
    std::uint64_t next_id_{1};
    bool id_exhausted_{false};
};

}  // namespace iaisf::task
