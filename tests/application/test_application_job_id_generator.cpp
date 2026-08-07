#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "iaisf/application/application_job_id_generator.hpp"
#include "detail/application_job_id_entropy.hpp"

namespace iaisf::application {
namespace {

class ScriptedEntropyReader final
    : public detail::ApplicationJobIdEntropyReader {
public:
    struct Event {
        detail::EntropyReadStatus status;
        std::size_t bytes;
    };

    explicit ScriptedEntropyReader(
        std::array<std::uint8_t, 16U> bytes,
        std::vector<Event> events)
        : bytes_(bytes), events_(std::move(events)) {}

    [[nodiscard]] detail::EntropyReadResult read(
        std::uint8_t* const destination,
        const std::size_t maximum_bytes) override {
        if (event_index_ >= events_.size()) {
            return detail::EntropyReadResult{
                detail::EntropyReadStatus::Failed, 0U};
        }
        const auto event = events_[event_index_++];
        if (event.status != detail::EntropyReadStatus::Bytes) {
            return detail::EntropyReadResult{event.status, 0U};
        }
        if (event.bytes > maximum_bytes) {
            return detail::EntropyReadResult{
                detail::EntropyReadStatus::Bytes,
                event.bytes};
        }
        const auto count = event.bytes;
        if (count == 0U) {
            return detail::EntropyReadResult{
                detail::EntropyReadStatus::Bytes,
                0U};
        }
        if (cursor_ + count > bytes_.size()) {
            return detail::EntropyReadResult{
                detail::EntropyReadStatus::Failed, 0U};
        }
        std::memcpy(destination, bytes_.data() + cursor_, count);
        cursor_ += count;
        return detail::EntropyReadResult{
            detail::EntropyReadStatus::Bytes, count};
    }

private:
    std::array<std::uint8_t, 16U> bytes_{};
    std::vector<Event> events_;
    std::size_t cursor_{0U};
    std::size_t event_index_{0U};
};

[[nodiscard]] std::shared_ptr<ScriptedEntropyReader> reader_for(
    const std::array<std::uint8_t, 16U>& bytes,
    std::vector<ScriptedEntropyReader::Event> events) {
    return std::make_shared<ScriptedEntropyReader>(bytes, std::move(events));
}

[[nodiscard]] ApplicationJobIdGenerationResult generate_with_reader(
    const std::shared_ptr<ScriptedEntropyReader>& reader,
    const IndustrialApplication application) {
    return detail::generate_with_entropy_reader(application, *reader);
}

TEST(ApplicationJobIdGeneratorTest, ProducesCanonicalInspectionId) {
    std::array<std::uint8_t, 16U> entropy{};
    for (std::size_t index = 0U; index < entropy.size(); ++index) {
        entropy[index] = static_cast<std::uint8_t>(index);
    }
    const auto reader = reader_for(
        entropy,
        {{detail::EntropyReadStatus::Bytes, entropy.size()}});

    const auto result = generate_with_reader(
        reader,
        IndustrialApplication::WeldInspection);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().value(), "wi_000102030405060708090a0b0c0d0e0f");
    EXPECT_EQ(result.value().value().size(), 35U);
}

TEST(ApplicationJobIdGeneratorTest, ProducesCanonicalGuidanceIdWithAllZeroEntropy) {
    const std::array<std::uint8_t, 16U> entropy{};
    const auto reader = reader_for(
        entropy,
        {{detail::EntropyReadStatus::Bytes, entropy.size()}});

    const auto result = generate_with_reader(
        reader,
        IndustrialApplication::WeldingGuidance);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().value(), "wg_00000000000000000000000000000000");
}

TEST(ApplicationJobIdGeneratorTest, RejectsInvalidApplicationBeforeReadingEntropy) {
    const auto reader = reader_for(
        {},
        {{detail::EntropyReadStatus::Failed, 0U}});

    const auto result = generate_with_reader(
        reader,
        static_cast<IndustrialApplication>(99));

    ASSERT_FALSE(result);
    EXPECT_EQ(
        result.error().category,
        ApplicationJobIdGenerationFailure::InvalidApplication);
}

TEST(ApplicationJobIdGeneratorTest, MapsEntropyFailureWithoutSystemDetails) {
    const auto reader = reader_for(
        {},
        {{detail::EntropyReadStatus::Failed, 0U}});

    const auto result = generate_with_reader(
        reader,
        IndustrialApplication::WeldInspection);

    ASSERT_FALSE(result);
    EXPECT_EQ(
        result.error().category,
        ApplicationJobIdGenerationFailure::EntropyUnavailable);
    EXPECT_LE(result.error().message.size(), 128U);
    EXPECT_EQ(result.error().message.find("errno"), std::string::npos);
    EXPECT_EQ(result.error().message.find("NTSTATUS"), std::string::npos);
}

TEST(ApplicationJobIdGeneratorTest, RetriesInterruptedAndShortReads) {
    std::array<std::uint8_t, 16U> entropy{};
    for (std::size_t index = 0U; index < entropy.size(); ++index) {
        entropy[index] = static_cast<std::uint8_t>(0xF0U + index);
    }
    const auto reader = reader_for(
        entropy,
        {{detail::EntropyReadStatus::Interrupted, 0U},
         {detail::EntropyReadStatus::Bytes, 3U},
         {detail::EntropyReadStatus::Bytes, 5U},
         {detail::EntropyReadStatus::Bytes, 8U}});

    const auto result = generate_with_reader(
        reader,
        IndustrialApplication::WeldInspection);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().value(), "wi_f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
}

TEST(ApplicationJobIdGeneratorTest, ZeroReadFailsClosedWithoutPartialId) {
    const auto reader = reader_for(
        {},
        {{detail::EntropyReadStatus::EndOfStream, 0U}});

    const auto result = generate_with_reader(
        reader,
        IndustrialApplication::WeldInspection);

    ASSERT_FALSE(result);
    EXPECT_EQ(
        result.error().category,
        ApplicationJobIdGenerationFailure::EntropyUnavailable);
}

TEST(ApplicationJobIdGeneratorTest, ReaderContractViolationsAreInternalFailures) {
    const auto zero_reader = reader_for(
        {},
        {{detail::EntropyReadStatus::Bytes, 0U}});
    const auto zero_result = generate_with_reader(
        zero_reader,
        IndustrialApplication::WeldInspection);
    ASSERT_FALSE(zero_result);
    EXPECT_EQ(
        zero_result.error().category,
        ApplicationJobIdGenerationFailure::InternalFailure);

    const auto oversized_reader = reader_for(
        {},
        {{detail::EntropyReadStatus::Bytes, 17U}});
    const auto oversized_result = generate_with_reader(
        oversized_reader,
        IndustrialApplication::WeldInspection);
    ASSERT_FALSE(oversized_result);
    EXPECT_EQ(
        oversized_result.error().category,
        ApplicationJobIdGenerationFailure::InternalFailure);

    std::array<std::uint8_t, 16U> entropy{};
    const auto partial_reader = reader_for(
        entropy,
        {{detail::EntropyReadStatus::Bytes, 3U},
         {detail::EntropyReadStatus::Bytes, 14U}});
    const auto partial_result = generate_with_reader(
        partial_reader,
        IndustrialApplication::WeldInspection);
    ASSERT_FALSE(partial_result);
    EXPECT_EQ(
        partial_result.error().category,
        ApplicationJobIdGenerationFailure::InternalFailure);
}

TEST(ApplicationJobIdGeneratorTest, InvalidFailureEnumStringIsUnknown) {
    EXPECT_EQ(
        to_string(static_cast<ApplicationJobIdGenerationFailure>(99)),
        "unknown");
}

TEST(ApplicationJobIdGeneratorTest, AllocationBearingApiIsNotNoexcept) {
    static_assert(!noexcept(std::declval<OsApplicationJobIdGenerator&>().generate(
        IndustrialApplication::WeldInspection)));
    static_assert(!noexcept(ApplicationJobIdGenerationResult::success(
        std::declval<ApplicationJobId>())));
    static_assert(!std::is_move_constructible_v<OsApplicationJobIdGenerator>);
    static_assert(!std::is_move_assignable_v<OsApplicationJobIdGenerator>);
    SUCCEED();
}

TEST(ApplicationJobIdGeneratorTest, ResultCopyPreservesAlternativesAndSources) {
    static_assert(!std::is_move_constructible_v<ApplicationJobIdGenerationResult>);
    static_assert(!std::is_move_assignable_v<ApplicationJobIdGenerationResult>);
    static_assert(std::is_copy_constructible_v<ApplicationJobIdGenerationResult>);
    static_assert(std::is_copy_assignable_v<ApplicationJobIdGenerationResult>);

    const auto success_reader = reader_for(
        {},
        {{detail::EntropyReadStatus::Bytes, 16U}});
    auto success_source = generate_with_reader(
        success_reader,
        IndustrialApplication::WeldingGuidance);
    const auto success_copy(success_source);
    ASSERT_TRUE(success_source);
    ASSERT_TRUE(success_copy);
    EXPECT_EQ(success_source.value().value(), success_copy.value().value());

    const auto success_target_reader = reader_for(
        {},
        {{detail::EntropyReadStatus::Bytes, 16U}});
    auto success_target = generate_with_reader(
        success_target_reader,
        IndustrialApplication::WeldInspection);
    const auto success_source_value = success_source.value().value();
    success_target = success_source;
    ASSERT_TRUE(success_source);
    ASSERT_TRUE(success_target);
    EXPECT_EQ(success_source.value().value(), success_source_value);
    EXPECT_EQ(success_target.value().value(), success_source_value);

    auto failure_source = ApplicationJobIdGenerationResult::failure(
        ApplicationJobIdGenerationError{
            static_cast<ApplicationJobIdGenerationFailure>(99),
            std::string(256U, 'x')});
    const auto failure_copy(failure_source);
    ASSERT_FALSE(failure_source);
    ASSERT_FALSE(failure_copy);
    EXPECT_EQ(
        failure_source.error().category,
        ApplicationJobIdGenerationFailure::InternalFailure);
    EXPECT_EQ(failure_source.error().message, "application job id generation failed");
    EXPECT_EQ(failure_source.error().message, failure_copy.error().message);

    auto failure_target = ApplicationJobIdGenerationResult::failure(
        ApplicationJobIdGenerationError{
            ApplicationJobIdGenerationFailure::InternalFailure,
            "internal"});
    failure_target = failure_source;
    ASSERT_FALSE(failure_source);
    ASSERT_FALSE(failure_target);
    EXPECT_EQ(
        failure_source.error().category,
        ApplicationJobIdGenerationFailure::InternalFailure);
    EXPECT_EQ(failure_target.error().message, failure_source.error().message);

    auto failure_from_success = ApplicationJobIdGenerationResult::failure(
        ApplicationJobIdGenerationError{
            ApplicationJobIdGenerationFailure::ResourceFailure,
            "resource"});
    failure_from_success = success_source;
    ASSERT_TRUE(success_source);
    ASSERT_TRUE(failure_from_success);
    EXPECT_EQ(failure_from_success.value().value(), success_source_value);

    auto success_from_failure = ApplicationJobIdGenerationResult::success(
        ApplicationJobId::parse("wg_00000000000000000000000000000000").value());
    success_from_failure = failure_source;
    ASSERT_FALSE(failure_source);
    ASSERT_FALSE(success_from_failure);
    EXPECT_EQ(
        success_from_failure.error().category,
        ApplicationJobIdGenerationFailure::InternalFailure);

    const auto empty_error = ApplicationJobIdGenerationResult::failure(
        ApplicationJobIdGenerationError{
            ApplicationJobIdGenerationFailure::EntropyUnavailable,
            {}});
    EXPECT_EQ(
        empty_error.error().message,
        "application job id entropy is unavailable");

    EXPECT_THROW(
        {
            const auto& error = success_source.error();
            (void)error;
        },
        std::logic_error);
    EXPECT_THROW(
        {
            const auto& value = failure_source.value();
            (void)value;
        },
        std::logic_error);
}

TEST(ApplicationJobIdGeneratorTest, OsGeneratorIsUsableWithoutMove) {
    OsApplicationJobIdGenerator generator;
    const auto result = generator.generate(IndustrialApplication::WeldInspection);
    ASSERT_TRUE(result);
    EXPECT_TRUE(result.value().valid());
}

TEST(ApplicationJobIdGeneratorTest, PlatformGeneratorProducesValidFormatConcurrently) {
    OsApplicationJobIdGenerator generator;
    constexpr std::size_t kThreads = 4U;
    constexpr std::size_t kCallsPerThread = 8U;
    std::vector<std::thread> workers;
    std::vector<unsigned char> success(kThreads * kCallsPerThread, 0U);
    for (std::size_t thread_index = 0U; thread_index < kThreads; ++thread_index) {
        workers.emplace_back([&, thread_index] {
            for (std::size_t call = 0U; call < kCallsPerThread; ++call) {
                const auto result = generator.generate(
                    call % 2U == 0U ? IndustrialApplication::WeldInspection
                                    : IndustrialApplication::WeldingGuidance);
                success[thread_index * kCallsPerThread + call] =
                    static_cast<unsigned char>(
                        result && result.value().valid() &&
                        result.value().value().size() == 35U);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    EXPECT_TRUE(std::all_of(success.begin(), success.end(), [](const unsigned char value) {
        return value != 0U;
    }));
}

}  // namespace
}  // namespace iaisf::application
