#include <cstddef>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

#include "iaisf/core/error.hpp"
#include "iaisf/net/tcp/buffer.hpp"

namespace {

using iaisf::net::tcp::Buffer;

static_assert(!std::is_copy_constructible_v<Buffer>);
static_assert(!std::is_copy_assignable_v<Buffer>);
static_assert(std::is_nothrow_move_constructible_v<Buffer>);
static_assert(std::is_nothrow_move_assignable_v<Buffer>);

Buffer make_buffer(
    const std::size_t initial_capacity,
    const std::size_t maximum_capacity) {
    auto result = Buffer::create(initial_capacity, maximum_capacity);
    EXPECT_TRUE(result);
    return result ? std::move(result).value()
                  : std::move(Buffer::create(1U, 1U)).value();
}

TEST(BufferTest, RejectsInvalidCapacityRelationships) {
    auto zero_maximum = Buffer::create(0U, 0U);
    auto initial_too_large = Buffer::create(9U, 8U);

    ASSERT_FALSE(zero_maximum);
    ASSERT_FALSE(initial_too_large);
    EXPECT_EQ(
        zero_maximum.error().code,
        iaisf::ErrorCode::InvalidArgument);
    EXPECT_EQ(
        initial_too_large.error().code,
        iaisf::ErrorCode::InvalidArgument);
}

TEST(BufferTest, EmptyOperationsAreNoOps) {
    auto buffer = make_buffer(0U, 8U);

    EXPECT_TRUE(buffer.empty());
    EXPECT_TRUE(buffer.append(nullptr, 0U));
    EXPECT_TRUE(buffer.retrieve(0U));
    auto text = buffer.retrieve_as_string(0U);

    ASSERT_TRUE(text);
    EXPECT_TRUE(text.value().empty());
    EXPECT_EQ(buffer.readable_bytes(), 0U);
}

TEST(BufferTest, PreservesBinaryBytesIncludingNul) {
    auto buffer = make_buffer(2U, 16U);
    const std::string bytes{"a\0b\0c", 5U};

    ASSERT_TRUE(buffer.append(bytes));
    auto result = buffer.retrieve_as_string(bytes.size());

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), bytes);
    EXPECT_TRUE(buffer.empty());
}

TEST(BufferTest, RetrieveAdvancesWithoutChangingCapacity) {
    auto buffer = make_buffer(8U, 8U);
    ASSERT_TRUE(buffer.append("abcdef"));
    const std::size_t original_capacity = buffer.capacity();

    ASSERT_TRUE(buffer.retrieve(4U));

    EXPECT_EQ(buffer.readable_bytes(), 2U);
    EXPECT_EQ(buffer.prependable_bytes(), 4U);
    EXPECT_EQ(buffer.reader_index(), 4U);
    EXPECT_EQ(buffer.writer_index(), 6U);
    EXPECT_EQ(buffer.capacity(), original_capacity);
    EXPECT_EQ(std::string(buffer.peek(), buffer.readable_bytes()), "ef");
}

TEST(BufferTest, ExplicitCompactPreservesOrderAndResetsReaderIndex) {
    auto buffer = make_buffer(8U, 8U);
    ASSERT_TRUE(buffer.append("abcdef"));
    ASSERT_TRUE(buffer.retrieve(2U));

    buffer.compact();

    EXPECT_EQ(buffer.reader_index(), 0U);
    EXPECT_EQ(buffer.writer_index(), 4U);
    EXPECT_EQ(buffer.prependable_bytes(), 0U);
    EXPECT_EQ(std::string(buffer.peek(), buffer.readable_bytes()), "cdef");
}

TEST(BufferTest, EnsureWritableReusesSpaceWithoutChangingReadableData) {
    auto buffer = make_buffer(8U, 16U);
    ASSERT_TRUE(buffer.append("abcdef"));
    ASSERT_TRUE(buffer.retrieve(4U));

    ASSERT_TRUE(buffer.ensure_writable(6U));

    EXPECT_GE(buffer.writable_bytes(), 6U);
    EXPECT_EQ(std::string(buffer.peek(), buffer.readable_bytes()), "ef");
}

TEST(BufferTest, MultiplePartialRetrievesAndRetrieveAllRemainConstantTime) {
    auto buffer = make_buffer(16U, 16U);
    ASSERT_TRUE(buffer.append("0123456789"));
    ASSERT_TRUE(buffer.retrieve(2U));
    ASSERT_TRUE(buffer.retrieve(3U));

    EXPECT_EQ(std::string(buffer.peek(), buffer.readable_bytes()), "56789");
    buffer.retrieve_all();
    EXPECT_TRUE(buffer.empty());
    EXPECT_EQ(buffer.reader_index(), 0U);
    EXPECT_EQ(buffer.writer_index(), 0U);
}

TEST(BufferTest, ReusesReclaimedFrontBeforeGrowing) {
    auto buffer = make_buffer(8U, 8U);
    ASSERT_TRUE(buffer.append("abcdef"));
    ASSERT_TRUE(buffer.retrieve(4U));

    ASSERT_TRUE(buffer.append("WXYZ"));

    EXPECT_EQ(buffer.capacity(), 8U);
    auto result = buffer.retrieve_as_string(6U);
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), "efWXYZ");
}

TEST(BufferTest, GrowsOnlyUpToMaximumCapacity) {
    auto buffer = make_buffer(2U, 8U);

    ASSERT_TRUE(buffer.append("12345678"));
    EXPECT_EQ(buffer.capacity(), 8U);
    auto overflow = buffer.append("9");

    ASSERT_FALSE(overflow);
    EXPECT_EQ(overflow.error().code, iaisf::ErrorCode::ResourceExhausted);
    EXPECT_EQ(buffer.readable_bytes(), 8U);
    EXPECT_EQ(
        std::string(buffer.peek(), buffer.readable_bytes()),
        "12345678");
}

TEST(BufferTest, FailedAppendPreservesReadableBytesAndIndexes) {
    auto buffer = make_buffer(8U, 8U);
    ASSERT_TRUE(buffer.append("abcdef"));
    ASSERT_TRUE(buffer.retrieve(2U));
    const std::size_t reader_before = buffer.reader_index();
    const std::size_t writer_before = buffer.writer_index();
    const std::string bytes_before{
        buffer.peek(),
        buffer.readable_bytes()};

    auto result = buffer.append("12345");

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, iaisf::ErrorCode::ResourceExhausted);
    EXPECT_EQ(buffer.reader_index(), reader_before);
    EXPECT_EQ(buffer.writer_index(), writer_before);
    EXPECT_EQ(
        std::string(buffer.peek(), buffer.readable_bytes()),
        bytes_before);
}

TEST(BufferTest, UsesOverflowSafeCapacityCheck) {
    auto buffer = make_buffer(4U, 8U);
    const char byte = 'x';
    ASSERT_TRUE(buffer.append(&byte, 1U));

    auto result = buffer.append(
        &byte,
        std::numeric_limits<std::size_t>::max());
    auto ensure_result =
        buffer.ensure_writable(std::numeric_limits<std::size_t>::max());

    ASSERT_FALSE(result);
    ASSERT_FALSE(ensure_result);
    EXPECT_EQ(result.error().code, iaisf::ErrorCode::ResourceExhausted);
    EXPECT_EQ(
        ensure_result.error().code,
        iaisf::ErrorCode::ResourceExhausted);
    EXPECT_EQ(buffer.readable_bytes(), 1U);
}

TEST(BufferTest, RejectsNullNonEmptyAppendAndExcessRetrieve) {
    auto buffer = make_buffer(4U, 8U);

    auto null_append = buffer.append(nullptr, 1U);
    auto excessive_retrieve = buffer.retrieve(1U);
    auto excessive_string = buffer.retrieve_as_string(1U);

    ASSERT_FALSE(null_append);
    ASSERT_FALSE(excessive_retrieve);
    ASSERT_FALSE(excessive_string);
    EXPECT_EQ(null_append.error().code, iaisf::ErrorCode::InvalidArgument);
    EXPECT_EQ(
        excessive_retrieve.error().code,
        iaisf::ErrorCode::InvalidArgument);
    EXPECT_EQ(
        excessive_string.error().code,
        iaisf::ErrorCode::InvalidArgument);
}

TEST(BufferTest, MoveTransfersBytesAndLimits) {
    auto source = make_buffer(4U, 16U);
    ASSERT_TRUE(source.append("payload"));

    Buffer destination{std::move(source)};

    EXPECT_EQ(destination.maximum_capacity(), 16U);
    auto value = destination.retrieve_as_string(7U);
    ASSERT_TRUE(value);
    EXPECT_EQ(value.value(), "payload");
    EXPECT_TRUE(source.empty());
    EXPECT_EQ(source.maximum_capacity(), 0U);
    auto moved_from_append = source.append("x");
    ASSERT_FALSE(moved_from_append);
    EXPECT_EQ(
        moved_from_append.error().code,
        iaisf::ErrorCode::ResourceExhausted);
}

}  // namespace
