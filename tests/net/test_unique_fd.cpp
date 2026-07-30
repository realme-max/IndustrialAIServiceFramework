#include <cerrno>
#include <type_traits>
#include <utility>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "iaisf/net/unique_fd.hpp"

namespace {

int make_event_fd() {
    return ::eventfd(0U, EFD_NONBLOCK | EFD_CLOEXEC);
}

static_assert(!std::is_copy_constructible_v<iaisf::net::UniqueFd>);
static_assert(!std::is_copy_assignable_v<iaisf::net::UniqueFd>);
static_assert(std::is_nothrow_move_constructible_v<iaisf::net::UniqueFd>);
static_assert(std::is_nothrow_move_assignable_v<iaisf::net::UniqueFd>);
static_assert(std::is_nothrow_destructible_v<iaisf::net::UniqueFd>);

TEST(UniqueFdTest, DefaultAndInvalidDescriptorsAreEmpty) {
    iaisf::net::UniqueFd empty;
    iaisf::net::UniqueFd invalid{-1};

    EXPECT_FALSE(empty.valid());
    EXPECT_FALSE(static_cast<bool>(empty));
    EXPECT_EQ(empty.get(), -1);
    EXPECT_FALSE(invalid.valid());
}

TEST(UniqueFdTest, DestructorClosesOwnedDescriptor) {
    const int descriptor = make_event_fd();
    ASSERT_GE(descriptor, 0);
    {
        iaisf::net::UniqueFd owned{descriptor};
        EXPECT_TRUE(owned.valid());
    }

    errno = 0;
    EXPECT_EQ(::fcntl(descriptor, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}

TEST(UniqueFdTest, MoveConstructorTransfersOwnership) {
    const int descriptor = make_event_fd();
    ASSERT_GE(descriptor, 0);
    iaisf::net::UniqueFd source{descriptor};

    iaisf::net::UniqueFd destination{std::move(source)};

    EXPECT_FALSE(source.valid());
    EXPECT_EQ(destination.get(), descriptor);
}

TEST(UniqueFdTest, MoveAssignmentClosesOldAndTransfersNewDescriptor) {
    const int old_descriptor = make_event_fd();
    const int new_descriptor = make_event_fd();
    ASSERT_GE(old_descriptor, 0);
    ASSERT_GE(new_descriptor, 0);
    iaisf::net::UniqueFd destination{old_descriptor};
    iaisf::net::UniqueFd source{new_descriptor};

    destination = std::move(source);

    EXPECT_FALSE(source.valid());
    EXPECT_EQ(destination.get(), new_descriptor);
    errno = 0;
    EXPECT_EQ(::fcntl(old_descriptor, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}

TEST(UniqueFdTest, ReleaseLeavesDescriptorOpen) {
    const int descriptor = make_event_fd();
    ASSERT_GE(descriptor, 0);
    iaisf::net::UniqueFd owned{descriptor};

    const int released = owned.release();

    EXPECT_FALSE(owned.valid());
    EXPECT_EQ(released, descriptor);
    EXPECT_NE(::fcntl(released, F_GETFD), -1);
    EXPECT_EQ(::close(released), 0);
}

TEST(UniqueFdTest, ResetClosesOldAndOwnsNewDescriptor) {
    const int old_descriptor = make_event_fd();
    const int new_descriptor = make_event_fd();
    ASSERT_GE(old_descriptor, 0);
    ASSERT_GE(new_descriptor, 0);
    iaisf::net::UniqueFd owned{old_descriptor};

    owned.reset(new_descriptor);

    EXPECT_EQ(owned.get(), new_descriptor);
    errno = 0;
    EXPECT_EQ(::fcntl(old_descriptor, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}

TEST(UniqueFdTest, ResetToSameDescriptorDoesNotCloseIt) {
    const int descriptor = make_event_fd();
    ASSERT_GE(descriptor, 0);
    iaisf::net::UniqueFd owned{descriptor};

    owned.reset(descriptor);

    EXPECT_EQ(owned.get(), descriptor);
    EXPECT_NE(::fcntl(descriptor, F_GETFD), -1);
}

TEST(UniqueFdTest, SwapExchangesOwnership) {
    const int first_descriptor = make_event_fd();
    const int second_descriptor = make_event_fd();
    ASSERT_GE(first_descriptor, 0);
    ASSERT_GE(second_descriptor, 0);
    iaisf::net::UniqueFd first{first_descriptor};
    iaisf::net::UniqueFd second{second_descriptor};

    swap(first, second);

    EXPECT_EQ(first.get(), second_descriptor);
    EXPECT_EQ(second.get(), first_descriptor);
}

}  // namespace
