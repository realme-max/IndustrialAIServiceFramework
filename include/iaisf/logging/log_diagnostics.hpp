#pragma once

#include <cstdint>

namespace iaisf {

enum class LogDiagnosticsState {
    Running,
    Draining,
    Stopped,
};

/** Immutable, allocation-free logger observation used by diagnostics. */
struct LogDiagnosticsSnapshot {
    LogDiagnosticsState state{LogDiagnosticsState::Stopped};
    std::uint64_t accepted{0U};
    std::uint64_t filtered{0U};
    std::uint64_t dropped{0U};
    std::uint64_t rejected_after_shutdown{0U};
    std::uint64_t sink_failures{0U};
};

/** Optional read-only logger diagnostics capability. */
class ILogDiagnostics {
public:
    virtual ~ILogDiagnostics() = default;
    [[nodiscard]] virtual LogDiagnosticsSnapshot
    diagnostics_snapshot() const noexcept = 0;
};

} // namespace iaisf
