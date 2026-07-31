#include <cstdint>
#include <limits>
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

TEST(TaskIdTest, CanonicalParserUsesTheSingleFormatterContract) {
    const std::string ordinary{"task-0000000000000001"};
    const auto parsed = TaskId::parse(ordinary);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed.value(), TaskId{1U});
    EXPECT_EQ(parsed.value().to_string(), ordinary);

    const auto maximum =
        TaskId{std::numeric_limits<std::uint64_t>::max()}.to_string();
    EXPECT_EQ(maximum, "task-18446744073709551615");
    const auto parsed_maximum = TaskId::parse(maximum);
    ASSERT_TRUE(parsed_maximum);
    EXPECT_EQ(
        parsed_maximum.value().value(),
        std::numeric_limits<std::uint64_t>::max());
}

TEST(TaskIdTest, CanonicalParserRejectsEveryTextualVariant) {
    EXPECT_FALSE(TaskId::parse(""));
    EXPECT_FALSE(TaskId::parse("TASK-0000000000000001"));
    EXPECT_FALSE(TaskId::parse("task-0000000000000000"));
    EXPECT_FALSE(TaskId::parse("task-000000000000001"));
    EXPECT_FALSE(TaskId::parse("task-00000000000000001"));
    EXPECT_FALSE(TaskId::parse("task-+000000000000001"));
    EXPECT_FALSE(TaskId::parse("task- 000000000000001"));
    EXPECT_FALSE(TaskId::parse("task-18446744073709551616"));
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
