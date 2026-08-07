#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "detail/application_job_id_entropy.hpp"

namespace iaisf::application::detail {
namespace {

class ScriptedGetrandomInvoker final : public LinuxGetrandomInvoker {
public:
    struct Event {
        std::ptrdiff_t return_value;
        int error_number;
        std::vector<std::uint8_t> bytes;
    };

    explicit ScriptedGetrandomInvoker(std::vector<Event> events)
        : events_(std::move(events)) {}

    [[nodiscard]] LinuxGetrandomResult invoke(
        std::uint8_t* const destination,
        const std::size_t maximum_bytes) override {
        if (index_ >= events_.size()) {
            return LinuxGetrandomResult{-1, EIO};
        }
        const auto& event = events_[index_++];
        if (event.return_value > 0) {
            const auto bytes_to_copy = std::min(
                static_cast<std::size_t>(event.return_value),
                std::min(maximum_bytes, event.bytes.size()));
            if (bytes_to_copy > 0U) {
                std::memcpy(
                    destination,
                    event.bytes.data(),
                    bytes_to_copy);
            }
        }
        return LinuxGetrandomResult{event.return_value, event.error_number};
    }

private:
    std::vector<Event> events_;
    std::size_t index_{0U};
};

[[nodiscard]] ApplicationJobIdGenerationResult generate_from_events(
    std::vector<ScriptedGetrandomInvoker::Event> events) {
    auto invoker = std::make_shared<ScriptedGetrandomInvoker>(
        std::move(events));
    auto reader = make_linux_test_job_id_entropy_reader(std::move(invoker));
    return generate_with_entropy_reader(
        IndustrialApplication::WeldInspection,
        *reader);
}

TEST(ApplicationJobIdLinuxEntropyTest, MapsGetrandomResultsWithoutSystemCalls) {
    const auto interrupted_then_complete = generate_from_events({
        {-1, EINTR, {}},
        {-1, EINTR, {}},
        {16, 0, std::vector<std::uint8_t>(16U, 0x11U)},
    });
    ASSERT_TRUE(interrupted_then_complete);

    const auto short_reads = generate_from_events({
        {5, 0, std::vector<std::uint8_t>(5U, 0x22U)},
        {11, 0, std::vector<std::uint8_t>(11U, 0x33U)},
    });
    ASSERT_TRUE(short_reads);

    const auto end_of_stream = generate_from_events({{0, 0, {}}});
    ASSERT_FALSE(end_of_stream);
    EXPECT_EQ(
        end_of_stream.error().category,
        ApplicationJobIdGenerationFailure::EntropyUnavailable);

    const auto failed = generate_from_events({{-1, EIO, {}}});
    ASSERT_FALSE(failed);
    EXPECT_EQ(
        failed.error().category,
        ApplicationJobIdGenerationFailure::EntropyUnavailable);
}

}  // namespace
}  // namespace iaisf::application::detail
