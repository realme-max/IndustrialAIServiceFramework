#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "iaisf/application/application_job_clock.hpp"
#include "detail/application_job_clock_detail.hpp"

namespace iaisf::application {
namespace {

class FakeClock final : public IApplicationJobClock {
public:
    explicit FakeClock(std::vector<ApplicationJobTimePoint> values)
        : values_(std::move(values)) {}

    [[nodiscard]] Result<ApplicationJobTimePoint> now() const override {
        const auto index = next_++;
        return Result<ApplicationJobTimePoint>::success(values_[index]);
    }

private:
    std::vector<ApplicationJobTimePoint> values_;
    mutable std::size_t next_{0U};
};

TEST(ApplicationJobClockTest, SystemClockReturnsRepresentableNonNegativeTime) {
    const SystemApplicationJobClock clock;
    const auto result = clock.now();

    ASSERT_TRUE(result);
    EXPECT_GE(result.value(), ApplicationJobTimePoint{});
}

TEST(ApplicationJobClockTest, FakeClockCanProvideDeterministicSequence) {
    const auto epoch = ApplicationJobTimePoint{};
    const auto later = epoch + std::chrono::milliseconds{17};
    FakeClock clock{{epoch, later}};

    const auto first = clock.now();
    const auto second = clock.now();

    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first.value(), epoch);
    EXPECT_EQ(second.value(), later);
}

TEST(ApplicationJobClockTest, EpochIsAcceptedAndPreEpochIsRejected) {
    const auto epoch = detail::validate_application_job_time_point(
        ApplicationJobTimePoint{});
    const auto before = detail::validate_application_job_time_point(
        ApplicationJobTimePoint{} - std::chrono::milliseconds{1});

    EXPECT_TRUE(epoch);
    ASSERT_FALSE(before);
    EXPECT_EQ(before.error().code, ErrorCode::InvalidArgument);
}

TEST(ApplicationJobClockTest, TimePointMaximumUsesTheRealIntegerBoundary) {
    const auto result = detail::validate_application_job_time_point(
        ApplicationJobTimePoint::max());

    ASSERT_TRUE(result);
}

TEST(ApplicationJobClockTest, ClockApiIsNotNoexcept) {
    static_assert(!noexcept(std::declval<SystemApplicationJobClock&>().now()));
    static_assert(!noexcept(std::declval<IApplicationJobClock&>().now()));
    SUCCEED();
}

TEST(ApplicationJobClockTest, ConcurrentReadsAreIndependent) {
    SystemApplicationJobClock clock;
    constexpr std::size_t kThreads = 4U;
    constexpr std::size_t kCallsPerThread = 8U;
    std::vector<std::thread> workers;
    std::vector<unsigned char> success(kThreads * kCallsPerThread, 0U);
    for (std::size_t thread_index = 0U; thread_index < kThreads; ++thread_index) {
        workers.emplace_back([&, thread_index] {
            for (std::size_t call = 0U; call < kCallsPerThread; ++call) {
                const auto result = clock.now();
                success[thread_index * kCallsPerThread + call] =
                    static_cast<unsigned char>(
                        result && result.value() >= ApplicationJobTimePoint{});
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    for (const unsigned char value : success) {
        EXPECT_NE(value, 0U);
    }
}

}  // namespace
}  // namespace iaisf::application
