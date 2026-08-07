#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>

#include "iaisf/application/application_executor.hpp"

namespace iaisf::application {
namespace {

TEST(ApplicationTaskManagerTest, BoundedQueueDrainsAndStops) {
    auto manager = ApplicationTaskManager::create(2U);
    ASSERT_TRUE(manager);
    std::promise<void> done;
    auto future = done.get_future();
    std::atomic<int> count{0};
    ASSERT_TRUE(manager.value()->submit([&] {
        count.fetch_add(1, std::memory_order_relaxed);
        done.set_value();
    }));
    EXPECT_EQ(future.wait_for(std::chrono::seconds{2}),
              std::future_status::ready);
    ASSERT_TRUE(manager.value()->shutdown());
    EXPECT_TRUE(manager.value()->stopped());
    EXPECT_EQ(count.load(std::memory_order_relaxed), 1);
}

TEST(ApplicationTaskManagerTest, RejectsAfterAdmissionStops) {
    auto manager = ApplicationTaskManager::create(1U);
    ASSERT_TRUE(manager);
    manager.value()->stop_admission();
    auto result = manager.value()->submit([] {});
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidState);
    ASSERT_TRUE(manager.value()->shutdown());
}

}  // namespace
}  // namespace iaisf::application
