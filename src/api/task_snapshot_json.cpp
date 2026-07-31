#include "iaisf/api/task_snapshot_json.hpp"

#include <exception>
#include <new>

#include "iaisf/core/error.hpp"

namespace iaisf::api {

Result<nlohmann::json> task_snapshot_to_json(
    const task::TaskSnapshot& snapshot) {
    if (!snapshot.id.valid()) {
        return Result<nlohmann::json>::failure(make_error(
            ErrorCode::InternalError,
            "task snapshot contains an invalid id"));
    }
    const bool invalid_combination =
        ((snapshot.state == task::TaskState::Queued ||
          snapshot.state == task::TaskState::Running) &&
         (snapshot.result.has_value() || snapshot.error.has_value())) ||
        (snapshot.state == task::TaskState::Succeeded &&
         (!snapshot.result.has_value() || snapshot.error.has_value())) ||
        (snapshot.state == task::TaskState::Failed &&
         (snapshot.result.has_value() || !snapshot.error.has_value())) ||
        (snapshot.state == task::TaskState::TimedOut &&
         (snapshot.result.has_value() || !snapshot.error.has_value()));
    if (invalid_combination) {
        return Result<nlohmann::json>::failure(make_error(
            ErrorCode::InternalError,
            "task snapshot contains an inconsistent terminal payload"));
    }
    try {
        nlohmann::json view{
            {"task_id", snapshot.id.to_string()},
            {"operation", snapshot.operation},
            {"state", task::to_string(snapshot.state)}};
        if (snapshot.state == task::TaskState::Succeeded) {
            view["result"] = *snapshot.result;
        } else if (snapshot.state == task::TaskState::Failed) {
            view["error"] = {
                {"code", "internal_error"},
                {"message", "task execution failed"}};
        }
        return Result<nlohmann::json>::success(std::move(view));
    } catch (const std::bad_alloc&) {
        return Result<nlohmann::json>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate task snapshot response"));
    } catch (const std::exception&) {
        return Result<nlohmann::json>::failure(make_error(
            ErrorCode::InternalError,
            "unable to build task snapshot response"));
    }
}

}  // namespace iaisf::api
