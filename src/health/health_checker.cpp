#include "iaisf/health/health_checker.hpp"

namespace iaisf::health {

HealthStatus HealthChecker::snapshot() const noexcept {
    const auto phase = phase_.load(std::memory_order_acquire);
    return HealthStatus{
        phase,
        phase != HealthPhase::Stopped,
        phase == HealthPhase::Running};
}

HealthTransitionOutcome HealthChecker::transition_to(
    const HealthPhase target) noexcept {
    auto current = phase_.load(std::memory_order_acquire);
    for (;;) {
        if (current == target) {
            return HealthTransitionOutcome::AlreadyInState;
        }

        const bool allowed =
            (current == HealthPhase::Created &&
             (target == HealthPhase::Running ||
              target == HealthPhase::Stopping)) ||
            (current == HealthPhase::Running &&
             target == HealthPhase::Stopping) ||
            (current == HealthPhase::Stopping &&
             target == HealthPhase::Stopped);
        if (!allowed) {
            return HealthTransitionOutcome::InvalidTransition;
        }

        if (phase_.compare_exchange_weak(
                current,
                target,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return HealthTransitionOutcome::Applied;
        }
    }
}

const char* to_string(const HealthPhase phase) noexcept {
    switch (phase) {
        case HealthPhase::Created:
            return "created";
        case HealthPhase::Running:
            return "running";
        case HealthPhase::Stopping:
            return "stopping";
        case HealthPhase::Stopped:
            return "stopped";
    }
    return "unknown";
}

}  // namespace iaisf::health
