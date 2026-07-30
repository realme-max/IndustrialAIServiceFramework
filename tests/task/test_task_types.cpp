#include <string>

#include <gtest/gtest.h>

#include "iaisf/task/task_types.hpp"

namespace {

using iaisf::task::TaskId;
using iaisf::task::TaskState;

TEST(TaskIdTest, FormatsStableServerGeneratedIdentifier) {
    EXPECT_FALSE(TaskId{}.valid());
    EXPECT_TRUE(TaskId{42}.valid());
    EXPECT_EQ(TaskId{42}.value(), 42U);
    EXPECT_EQ(TaskId{42}.to_string(), "task-0000000000000042");
}

TEST(TaskIdTest, SupportsEqualityAndHashing) {
    const std::hash<TaskId> hash;
    EXPECT_EQ(TaskId{7}, TaskId{7});
    EXPECT_NE(TaskId{7}, TaskId{8});
    EXPECT_EQ(hash(TaskId{7}), hash(TaskId{7}));
}

TEST(TaskStateTest, ExposesStableNamesAndTerminalClassification) {
    EXPECT_EQ(iaisf::task::to_string(TaskState::Queued), "queued");
    EXPECT_EQ(iaisf::task::to_string(TaskState::Running), "running");
    EXPECT_EQ(iaisf::task::to_string(TaskState::Succeeded), "succeeded");
    EXPECT_EQ(iaisf::task::to_string(TaskState::Failed), "failed");
    EXPECT_EQ(iaisf::task::to_string(TaskState::TimedOut), "timed_out");
    EXPECT_FALSE(iaisf::task::is_terminal(TaskState::Queued));
    EXPECT_FALSE(iaisf::task::is_terminal(TaskState::Running));
    EXPECT_TRUE(iaisf::task::is_terminal(TaskState::Succeeded));
    EXPECT_TRUE(iaisf::task::is_terminal(TaskState::Failed));
    EXPECT_TRUE(iaisf::task::is_terminal(TaskState::TimedOut));
}

}  // namespace
