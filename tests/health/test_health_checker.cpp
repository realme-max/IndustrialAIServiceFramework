#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <gtest/gtest.h>

#include "iaisf/health/health_checker.hpp"

namespace {

using iaisf::health::HealthChecker;
using iaisf::health::HealthPhase;
using iaisf::health::HealthTransitionOutcome;

TEST(HealthCheckerTest, StartsCreatedAndReportsNotReady) {
    HealthChecker checker;
    const auto status = checker.snapshot();
    EXPECT_EQ(status.phase, HealthPhase::Created);
    EXPECT_TRUE(status.live);
    EXPECT_FALSE(status.ready);
}

TEST(HealthCheckerTest, AllowsOnlyMonotonicLifecycleTransitions) {
    HealthChecker checker;
    EXPECT_EQ(
        checker.transition_to(HealthPhase::Running),
        HealthTransitionOutcome::Applied);
    EXPECT_EQ(
        checker.transition_to(HealthPhase::Running),
        HealthTransitionOutcome::AlreadyInState);
    EXPECT_EQ(
        checker.transition_to(HealthPhase::Created),
        HealthTransitionOutcome::InvalidTransition);
    EXPECT_EQ(
        checker.transition_to(HealthPhase::Stopping),
        HealthTransitionOutcome::Applied);
    EXPECT_EQ(
        checker.transition_to(HealthPhase::Stopped),
        HealthTransitionOutcome::Applied);
    EXPECT_EQ(
        checker.transition_to(HealthPhase::Running),
        HealthTransitionOutcome::InvalidTransition);
    EXPECT_EQ(
        checker.transition_to(HealthPhase::Stopped),
        HealthTransitionOutcome::AlreadyInState);
}

TEST(HealthCheckerTest, ConcurrentSnapshotsObserveCompleteStates) {
    HealthChecker checker;
    std::mutex mutex;
    std::condition_variable condition;
    bool start = false;
    std::atomic<unsigned int> snapshots{0U};
    std::thread reader([&] {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return start; });
        lock.unlock();
        for (unsigned int i = 0U; i < 1000U; ++i) {
            const auto status = checker.snapshot();
            EXPECT_EQ(status.ready, status.phase == HealthPhase::Running);
            EXPECT_EQ(status.live, status.phase != HealthPhase::Stopped);
            snapshots.fetch_add(1U, std::memory_order_relaxed);
        }
    });
    {
        std::lock_guard<std::mutex> lock(mutex);
        start = true;
    }
    condition.notify_one();
    EXPECT_EQ(
        checker.transition_to(HealthPhase::Running),
        HealthTransitionOutcome::Applied);
    reader.join();
    EXPECT_EQ(snapshots.load(std::memory_order_relaxed), 1000U);
}

}  // namespace
