#pragma once

#include <atomic>

namespace iaisf::health {

/** Lifecycle phase exposed by the process health contract. */
enum class HealthPhase {
    Created,
    Running,
    Stopping,
    Stopped,
};

struct HealthStatus {
    HealthPhase phase{HealthPhase::Created};
    bool live{true};
    bool ready{false};
};

enum class HealthTransitionOutcome {
    Applied,
    AlreadyInState,
    InvalidTransition,
};

/**
 * Thread-safe, dependency-free lifecycle health state.
 *
 * The service lifecycle owner performs transitions. Readers may call
 * snapshot() concurrently without taking a lock or creating a thread.
 */
class HealthChecker final {
public:
    HealthChecker() noexcept = default;
    HealthChecker(const HealthChecker&) = delete;
    HealthChecker& operator=(const HealthChecker&) = delete;
    HealthChecker(HealthChecker&&) = delete;
    HealthChecker& operator=(HealthChecker&&) = delete;
    ~HealthChecker() = default;

    [[nodiscard]] HealthStatus snapshot() const noexcept;

    /** Apply one of the monotonic lifecycle transitions. */
    [[nodiscard]] HealthTransitionOutcome transition_to(
        HealthPhase target) noexcept;

private:
    std::atomic<HealthPhase> phase_{HealthPhase::Created};
};

[[nodiscard]] const char* to_string(HealthPhase phase) noexcept;

}  // namespace iaisf::health
