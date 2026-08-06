#include "iaisf/application/application_job_state.hpp"

#include "iaisf/core/error.hpp"

namespace iaisf::application {
namespace {

[[nodiscard]] bool valid_application(
    const IndustrialApplication application) noexcept {
    switch (application) {
        case IndustrialApplication::WeldInspection:
        case IndustrialApplication::WeldingGuidance:
            return true;
    }
    return false;
}

[[nodiscard]] bool valid_state(const ApplicationJobState state) noexcept {
    switch (state) {
        case ApplicationJobState::Accepted:
        case ApplicationJobState::Queued:
        case ApplicationJobState::Dispatching:
        case ApplicationJobState::Running:
        case ApplicationJobState::WaitingHuman:
        case ApplicationJobState::Cancelling:
        case ApplicationJobState::Cancelled:
        case ApplicationJobState::Succeeded:
        case ApplicationJobState::Failed:
        case ApplicationJobState::TimedOut:
        case ApplicationJobState::WorkerLost:
            return true;
    }
    return false;
}

[[nodiscard]] bool base_transition_allowed(
    const ApplicationJobState from,
    const ApplicationJobState to) noexcept {
    switch (from) {
        case ApplicationJobState::Accepted:
            return to == ApplicationJobState::Queued ||
                   to == ApplicationJobState::Cancelled ||
                   to == ApplicationJobState::Failed;
        case ApplicationJobState::Queued:
            return to == ApplicationJobState::Dispatching ||
                   to == ApplicationJobState::Cancelled ||
                   to == ApplicationJobState::TimedOut ||
                   to == ApplicationJobState::Failed;
        case ApplicationJobState::Dispatching:
            return to == ApplicationJobState::Running ||
                   to == ApplicationJobState::Cancelling ||
                   to == ApplicationJobState::Failed ||
                   to == ApplicationJobState::TimedOut ||
                   to == ApplicationJobState::WorkerLost;
        case ApplicationJobState::Running:
            return to == ApplicationJobState::Succeeded ||
                   to == ApplicationJobState::Failed ||
                   to == ApplicationJobState::TimedOut ||
                   to == ApplicationJobState::WorkerLost ||
                   to == ApplicationJobState::Cancelling ||
                   to == ApplicationJobState::WaitingHuman;
        case ApplicationJobState::WaitingHuman:
            return to == ApplicationJobState::Running ||
                   to == ApplicationJobState::Cancelled ||
                   to == ApplicationJobState::TimedOut ||
                   to == ApplicationJobState::Failed;
        case ApplicationJobState::Cancelling:
            return to == ApplicationJobState::Cancelled ||
                   to == ApplicationJobState::Failed ||
                   to == ApplicationJobState::TimedOut ||
                   to == ApplicationJobState::WorkerLost;
        case ApplicationJobState::Cancelled:
        case ApplicationJobState::Succeeded:
        case ApplicationJobState::Failed:
        case ApplicationJobState::TimedOut:
        case ApplicationJobState::WorkerLost:
            return false;
    }
    return false;
}

}  // namespace

std::string_view to_string(const ApplicationJobState state) noexcept {
    switch (state) {
        case ApplicationJobState::Accepted:
            return "accepted";
        case ApplicationJobState::Queued:
            return "queued";
        case ApplicationJobState::Dispatching:
            return "dispatching";
        case ApplicationJobState::Running:
            return "running";
        case ApplicationJobState::WaitingHuman:
            return "waiting_human";
        case ApplicationJobState::Cancelling:
            return "cancelling";
        case ApplicationJobState::Cancelled:
            return "cancelled";
        case ApplicationJobState::Succeeded:
            return "succeeded";
        case ApplicationJobState::Failed:
            return "failed";
        case ApplicationJobState::TimedOut:
            return "timed_out";
        case ApplicationJobState::WorkerLost:
            return "worker_lost";
    }
    return "unknown";
}

bool is_terminal(const ApplicationJobState state) noexcept {
    switch (state) {
        case ApplicationJobState::Cancelled:
        case ApplicationJobState::Succeeded:
        case ApplicationJobState::Failed:
        case ApplicationJobState::TimedOut:
        case ApplicationJobState::WorkerLost:
            return true;
        case ApplicationJobState::Accepted:
        case ApplicationJobState::Queued:
        case ApplicationJobState::Dispatching:
        case ApplicationJobState::Running:
        case ApplicationJobState::WaitingHuman:
        case ApplicationJobState::Cancelling:
            return false;
    }
    return false;
}

Result<void> validate_transition(
    const ApplicationJobState from,
    const ApplicationJobState to,
    const IndustrialApplication application) {
    if (!valid_application(application) || !valid_state(from) ||
        !valid_state(to)) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "application job transition contains an invalid value"));
    }
    if (from == to) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "application job self-transition is not allowed"));
    }
    if ((from == ApplicationJobState::WaitingHuman ||
         to == ApplicationJobState::WaitingHuman) &&
        application != IndustrialApplication::WeldingGuidance) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "waiting_human is only valid for welding guidance"));
    }
    if (!base_transition_allowed(from, to)) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "application job state transition is not allowed"));
    }
    return Result<void>::success();
}

}  // namespace iaisf::application
