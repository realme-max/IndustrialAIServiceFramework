#pragma once

#include <string_view>

#include "iaisf/application/application_identity.hpp"
#include "iaisf/core/result.hpp"

namespace iaisf::application {

enum class ApplicationJobState {
    Accepted,
    Queued,
    Dispatching,
    Running,
    WaitingHuman,
    Cancelling,
    Cancelled,
    Succeeded,
    Failed,
    TimedOut,
    WorkerLost,
};

[[nodiscard]] std::string_view to_string(ApplicationJobState state) noexcept;
[[nodiscard]] bool is_terminal(ApplicationJobState state) noexcept;

/**
 * Validates one deterministic state transition.
 *
 * Self-transitions and every transition out of a terminal state are rejected.
 * WaitingHuman is exclusive to WeldingGuidance.
 */
[[nodiscard]] Result<void> validate_transition(
    ApplicationJobState from,
    ApplicationJobState to,
    IndustrialApplication application);

}  // namespace iaisf::application
