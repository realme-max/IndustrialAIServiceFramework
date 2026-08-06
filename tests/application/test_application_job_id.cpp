#include <gtest/gtest.h>

#include <array>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>

#include "iaisf/application/application_job_id.hpp"

namespace iaisf::application {
namespace {

TEST(ApplicationJobIdTest, AcceptsExactBoundariesAndAllowedCharacters) {
    EXPECT_TRUE(ApplicationJobId::parse("a"));
    EXPECT_TRUE(ApplicationJobId::parse("9"));
    EXPECT_TRUE(ApplicationJobId::parse("Job_ABC-123"));
    EXPECT_TRUE(ApplicationJobId::parse(
        std::string(ApplicationJobId::kMaxBytes, 'z')));
}

TEST(ApplicationJobIdTest, RejectsEmptyTooLongAndInvalidSyntax) {
    EXPECT_FALSE(ApplicationJobId::parse(""));
    EXPECT_FALSE(ApplicationJobId::parse(
        std::string(ApplicationJobId::kMaxBytes + 1U, 'a')));
    EXPECT_FALSE(ApplicationJobId::parse("_job"));
    EXPECT_FALSE(ApplicationJobId::parse("-job"));
    EXPECT_FALSE(ApplicationJobId::parse("job.name"));
    EXPECT_FALSE(ApplicationJobId::parse("job name"));
}

TEST(ApplicationJobIdTest, RejectsPathsUrlsControlsAndNul) {
    for (const auto& text : std::array<std::string, 9U>{
             "../job",
             "folder/job",
             R"(C:\job)",
             R"(\\server\share)",
             "https://example.test/job",
             "job:one",
             "job\n",
             "job\t",
             std::string{"job\0one", 7U}}) {
        EXPECT_FALSE(ApplicationJobId::parse(text));
    }
}

TEST(ApplicationJobIdTest, RejectsNonAsciiBytesFailClosed) {
    EXPECT_FALSE(ApplicationJobId::parse(std::string{"job-\xC3\xA9", 6U}));
    EXPECT_FALSE(ApplicationJobId::parse(std::string{"\xFF", 1U}));
}

TEST(ApplicationJobIdTest, IsCaseSensitiveAndHashStableForEqualValues) {
    auto upper = ApplicationJobId::parse("Job-1");
    auto lower = ApplicationJobId::parse("job-1");
    ASSERT_TRUE(upper);
    ASSERT_TRUE(lower);
    EXPECT_NE(upper.value(), lower.value());

    std::unordered_set<ApplicationJobId> ids;
    ids.insert(upper.value());
    ids.insert(lower.value());
    ids.insert(ApplicationJobId::parse("Job-1").value());
    EXPECT_EQ(ids.size(), 2U);
}

TEST(ApplicationJobIdTest, MoveConstructionPreservesBothValues) {
    auto parsed = ApplicationJobId::parse("job-move-source");
    ASSERT_TRUE(parsed);
    auto source = parsed.value();
    ApplicationJobId destination{std::move(source)};

    EXPECT_TRUE(source.valid());
    EXPECT_TRUE(destination.valid());
    EXPECT_EQ(source.value(), "job-move-source");
    EXPECT_EQ(destination.value(), source.value());
}

TEST(ApplicationJobIdTest, MoveAssignmentPreservesBothValues) {
    auto source_result = ApplicationJobId::parse("job-assignment-source");
    auto destination_result = ApplicationJobId::parse("job-destination");
    ASSERT_TRUE(source_result);
    ASSERT_TRUE(destination_result);
    auto source = source_result.value();
    auto destination = destination_result.value();

    destination = std::move(source);

    EXPECT_TRUE(source.valid());
    EXPECT_TRUE(destination.valid());
    EXPECT_EQ(source.value(), "job-assignment-source");
    EXPECT_EQ(destination.value(), source.value());
}

TEST(ApplicationJobIdTest, AllocatingMoveOperationsAreNotNoexcept) {
    static_assert(!std::is_nothrow_move_constructible_v<ApplicationJobId>);
    static_assert(!std::is_nothrow_move_assignable_v<ApplicationJobId>);
    static_assert(!noexcept(ApplicationJobId::parse("job")));
    SUCCEED();
}

TEST(ApplicationJobIdTest, ErrorIsBoundedAndDoesNotEchoInput) {
    const std::string sensitive = R"(C:\private\controller-token\job)";
    const auto result = ApplicationJobId::parse(sensitive);
    ASSERT_FALSE(result);
    EXPECT_LE(result.error().message.size(), 128U);
    EXPECT_EQ(result.error().message.find(sensitive), std::string::npos);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

}  // namespace
}  // namespace iaisf::application
