#include "iaisf/task/task_types.hpp"

#include <iomanip>
#include <sstream>

namespace iaisf::task {

std::string TaskId::to_string() const {
    std::ostringstream output;
    output << "task-" << std::setfill('0') << std::setw(16) << value_;
    return output.str();
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
