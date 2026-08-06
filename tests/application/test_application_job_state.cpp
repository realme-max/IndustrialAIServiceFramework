#include <gtest/gtest.h>

#include <array>
#include <string_view>

#include "iaisf/application/application_job_state.hpp"

namespace iaisf::application {
namespace {

constexpr std::array<ApplicationJobState, 11U> kStates{
    ApplicationJobState::Accepted,
    ApplicationJobState::Queued,
    ApplicationJobState::Dispatching,
    ApplicationJobState::Running,
    ApplicationJobState::WaitingHuman,
    ApplicationJobState::Cancelling,
    ApplicationJobState::Cancelled,
    ApplicationJobState::Succeeded,
    ApplicationJobState::Failed,
    ApplicationJobState::TimedOut,
    ApplicationJobState::WorkerLost,
};

[[nodiscard]] bool expected_base_transition(
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

[[nodiscard]] bool expected_transition(
    const ApplicationJobState from,
    const ApplicationJobState to,
    const IndustrialApplication application) noexcept {
    if (from == to) {
        return false;
    }
    if ((from == ApplicationJobState::WaitingHuman ||
         to == ApplicationJobState::WaitingHuman) &&
        application != IndustrialApplication::WeldingGuidance) {
        return false;
    }
    return expected_base_transition(from, to);
}

TEST(ApplicationJobStateTest, UsesStableStrings) {
    constexpr std::array<std::string_view, kStates.size()> expected{
        "accepted", "queued", "dispatching", "running", "waiting_human",
        "cancelling", "cancelled", "succeeded", "failed", "timed_out",
        "worker_lost",
    };
    for (std::size_t index = 0U; index < kStates.size(); ++index) {
        EXPECT_EQ(to_string(kStates[index]), expected[index]);
    }
    EXPECT_EQ(to_string(static_cast<ApplicationJobState>(-1)), "unknown");
}

TEST(ApplicationJobStateTest, IdentifiesOnlyTerminalStates) {
    for (const auto state : kStates) {
        const bool expected = state == ApplicationJobState::Cancelled ||
                              state == ApplicationJobState::Succeeded ||
                              state == ApplicationJobState::Failed ||
                              state == ApplicationJobState::TimedOut ||
                              state == ApplicationJobState::WorkerLost;
        EXPECT_EQ(is_terminal(state), expected) << to_string(state);
    }
    EXPECT_FALSE(is_terminal(static_cast<ApplicationJobState>(-1)));
}

TEST(ApplicationJobStateTest, ImplementsTheCompleteTransitionMatrix) {
    constexpr std::array<IndustrialApplication, 2U> applications{
        IndustrialApplication::WeldInspection,
        IndustrialApplication::WeldingGuidance,
    };
    for (const auto application : applications) {
        for (const auto from : kStates) {
            for (const auto to : kStates) {
                const auto result = validate_transition(from, to, application);
                EXPECT_EQ(static_cast<bool>(result),
                          expected_transition(from, to, application))
                    << to_string(application) << ": " << to_string(from)
                    << " -> " << to_string(to);
            }
        }
    }
}

TEST(ApplicationJobStateTest, GuidanceCanPauseAndResumeForHumanReview) {
    EXPECT_TRUE(validate_transition(
        ApplicationJobState::Running,
        ApplicationJobState::WaitingHuman,
        IndustrialApplication::WeldingGuidance));
    EXPECT_TRUE(validate_transition(
        ApplicationJobState::WaitingHuman,
        ApplicationJobState::Running,
        IndustrialApplication::WeldingGuidance));
}

TEST(ApplicationJobStateTest, InspectionCannotUseWaitingHuman) {
    for (const auto state : kStates) {
        EXPECT_FALSE(validate_transition(
            state, ApplicationJobState::WaitingHuman,
            IndustrialApplication::WeldInspection));
        EXPECT_FALSE(validate_transition(
            ApplicationJobState::WaitingHuman, state,
            IndustrialApplication::WeldInspection));
    }
}

TEST(ApplicationJobStateTest, TerminalStatesCannotBeRevived) {
    constexpr std::array<ApplicationJobState, 5U> terminal_states{
        ApplicationJobState::Cancelled,
        ApplicationJobState::Succeeded,
        ApplicationJobState::Failed,
        ApplicationJobState::TimedOut,
        ApplicationJobState::WorkerLost,
    };
    for (const auto terminal : terminal_states) {
        for (const auto destination : kStates) {
            EXPECT_FALSE(validate_transition(
                terminal, destination,
                IndustrialApplication::WeldingGuidance));
        }
    }
}

TEST(ApplicationJobStateTest, InvalidEnumValuesFailClosed) {
    const auto invalid_state = static_cast<ApplicationJobState>(100);
    const auto invalid_application = static_cast<IndustrialApplication>(100);
    EXPECT_FALSE(validate_transition(
        invalid_state, ApplicationJobState::Queued,
        IndustrialApplication::WeldInspection));
    EXPECT_FALSE(validate_transition(
        ApplicationJobState::Accepted, invalid_state,
        IndustrialApplication::WeldInspection));
    EXPECT_FALSE(validate_transition(
        ApplicationJobState::Accepted, ApplicationJobState::Queued,
        invalid_application));
}

TEST(ApplicationJobStateTest, TransitionValidationReturnsStructuredResults) {
    const auto accepted = validate_transition(
        ApplicationJobState::Accepted,
        ApplicationJobState::Queued,
        IndustrialApplication::WeldInspection);
    EXPECT_TRUE(accepted);

    const auto rejected = validate_transition(
        ApplicationJobState::Accepted,
        ApplicationJobState::Running,
        IndustrialApplication::WeldInspection);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::InvalidState);
}

}  // namespace
}  // namespace iaisf::application
