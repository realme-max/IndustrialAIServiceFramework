#include <csignal>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <pthread.h>
#include <unistd.h>

#include "iaisf/core/error.hpp"
#include "iaisf/logging/logger.hpp"
#include "iaisf/net/event_loop.hpp"
#include "signal_queue.hpp"

namespace {

class RecordingLogger final : public iaisf::ILogger {
public:
    void log(
        iaisf::LogLevel,
        std::string_view,
        const std::string_view message) override {
        std::lock_guard<std::mutex> lock{mutex_};
        messages_.emplace_back(message);
    }

    [[nodiscard]] bool contains(const std::string_view fragment) const {
        std::lock_guard<std::mutex> lock{mutex_};
        for (const auto& message : messages_) {
            if (message.find(fragment) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> messages_;
};

std::unique_ptr<iaisf::net::EventLoop> make_loop(
    iaisf::ILogger& logger) {
    auto result = iaisf::net::EventLoop::create(logger, 32U, 32U);
    EXPECT_TRUE(result);
    return result ? std::move(result).value() : nullptr;
}

int signal_mask_membership(const int signal_number) {
    sigset_t current{};
    const int query_result =
        ::pthread_sigmask(SIG_SETMASK, nullptr, &current);
    EXPECT_EQ(query_result, 0);
    if (query_result != 0) {
        return -1;
    }
    return ::sigismember(&current, signal_number);
}

void expect_signal_delivery_stops_loop(const int signal_number) {
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    const int original_int_mask = signal_mask_membership(SIGINT);
    const int original_term_mask = signal_mask_membership(SIGTERM);
    int callback_count = 0;
    bool callback_on_owner = false;
    ASSERT_TRUE(loop->enable_shutdown_signals([
        &loop,
        &callback_count,
        &callback_on_owner] {
        ++callback_count;
        callback_on_owner = loop->is_in_loop_thread();
    }));
    ASSERT_EQ(::kill(::getpid(), signal_number), 0);

    auto run_result = loop->run();

    ASSERT_TRUE(run_result);
    EXPECT_EQ(callback_count, 1);
    EXPECT_TRUE(callback_on_owner);
    EXPECT_EQ(loop->state(), iaisf::net::EventLoop::State::Stopped);
    const auto* queue =
        iaisf::net::detail::SignalQueueTestAccess::queue(*loop);
    ASSERT_NE(queue, nullptr);
    EXPECT_TRUE(
        iaisf::net::detail::SignalQueueTestAccess::shutdown_requested(
            *queue));
    EXPECT_FALSE(
        iaisf::net::detail::SignalQueueTestAccess::channel_registered(
            *queue));
    EXPECT_EQ(signal_mask_membership(SIGINT), original_int_mask);
    EXPECT_EQ(signal_mask_membership(SIGTERM), original_term_mask);
}

TEST(SignalQueueTest, CreatesNonblockingCloseOnExecEdgeTriggeredSource) {
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    ASSERT_TRUE(loop->enable_shutdown_signals());
    const auto* queue =
        iaisf::net::detail::SignalQueueTestAccess::queue(*loop);
    ASSERT_NE(queue, nullptr);
    const int descriptor =
        iaisf::net::detail::SignalQueueTestAccess::signal_fd(*queue);
    ASSERT_GE(descriptor, 0);
    const int status_flags = ::fcntl(descriptor, F_GETFL, 0);
    const int descriptor_flags = ::fcntl(descriptor, F_GETFD, 0);
    ASSERT_GE(status_flags, 0);
    ASSERT_GE(descriptor_flags, 0);
    EXPECT_NE(status_flags & O_NONBLOCK, 0);
    EXPECT_NE(descriptor_flags & FD_CLOEXEC, 0);
    EXPECT_TRUE(
        iaisf::net::detail::SignalQueueTestAccess::channel_registered(
            *queue));
    EXPECT_TRUE(
        iaisf::net::detail::SignalQueueTestAccess::channel_edge_triggered(
            *queue));
    loop->stop();
}

TEST(SignalQueueTest, SigtermStopsOnTheOwnerThread) {
    expect_signal_delivery_stops_loop(SIGTERM);
}

TEST(SignalQueueTest, SigintStopsOnTheOwnerThread) {
    expect_signal_delivery_stops_loop(SIGINT);
}

TEST(SignalQueueTest, MultipleSignalsRequestShutdownExactlyOnce) {
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    int callback_count = 0;
    ASSERT_TRUE(loop->enable_shutdown_signals(
        [&callback_count] { ++callback_count; }));
    ASSERT_EQ(::kill(::getpid(), SIGTERM), 0);
    ASSERT_EQ(::kill(::getpid(), SIGINT), 0);
    ASSERT_EQ(::kill(::getpid(), SIGTERM), 0);

    ASSERT_TRUE(loop->run());
    EXPECT_EQ(callback_count, 1);
    EXPECT_EQ(loop->state(), iaisf::net::EventLoop::State::Stopped);
}

TEST(SignalQueueTest, CallbackExceptionStillStopsFailClosed) {
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    ASSERT_TRUE(loop->enable_shutdown_signals([] {
        throw std::runtime_error{"expected signal callback failure"};
    }));
    ASSERT_EQ(::kill(::getpid(), SIGTERM), 0);

    ASSERT_TRUE(loop->run());
    EXPECT_EQ(loop->state(), iaisf::net::EventLoop::State::Stopped);
    EXPECT_TRUE(logger.contains("signal shutdown callback threw std::exception"));
}

TEST(SignalQueueTest, StopBeforeRunSkipsCallbackAndRestoresOwnedMask) {
    RecordingLogger logger;
    const int original_int_mask = signal_mask_membership(SIGINT);
    const int original_term_mask = signal_mask_membership(SIGTERM);
    int callback_count = 0;
    {
        auto loop = make_loop(logger);
        ASSERT_NE(loop, nullptr);
        ASSERT_TRUE(loop->enable_shutdown_signals(
            [&callback_count] { ++callback_count; }));
        loop->stop();
        loop->stop();
        EXPECT_EQ(loop->state(), iaisf::net::EventLoop::State::Stopped);
    }
    EXPECT_EQ(callback_count, 0);
    EXPECT_EQ(signal_mask_membership(SIGINT), original_int_mask);
    EXPECT_EQ(signal_mask_membership(SIGTERM), original_term_mask);
}

TEST(SignalQueueTest, PreservesSignalBitsBlockedBeforeEnable) {
    RecordingLogger logger;
    const int original_int_mask = signal_mask_membership(SIGINT);
    sigset_t int_mask{};
    ASSERT_EQ(::sigemptyset(&int_mask), 0);
    ASSERT_EQ(::sigaddset(&int_mask, SIGINT), 0);
    ASSERT_EQ(::pthread_sigmask(SIG_BLOCK, &int_mask, nullptr), 0);

    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    ASSERT_TRUE(loop->enable_shutdown_signals());
    ASSERT_EQ(::kill(::getpid(), SIGTERM), 0);
    ASSERT_TRUE(loop->run());
    EXPECT_EQ(signal_mask_membership(SIGINT), 1);

    if (original_int_mask == 0) {
        ASSERT_EQ(::pthread_sigmask(SIG_UNBLOCK, &int_mask, nullptr), 0);
    }
    EXPECT_EQ(signal_mask_membership(SIGINT), original_int_mask);
}

TEST(SignalQueueTest, RejectsNonOwnerAndDuplicateEnable) {
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    std::promise<iaisf::Result<void>> result_promise;
    auto result_future = result_promise.get_future();
    std::thread non_owner([&loop, &result_promise] {
        result_promise.set_value(loop->enable_shutdown_signals());
    });
    non_owner.join();
    auto non_owner_result = result_future.get();
    ASSERT_FALSE(non_owner_result);
    EXPECT_EQ(non_owner_result.error().code, iaisf::ErrorCode::InvalidState);

    ASSERT_TRUE(loop->enable_shutdown_signals());
    auto duplicate_result = loop->enable_shutdown_signals();
    ASSERT_FALSE(duplicate_result);
    EXPECT_EQ(duplicate_result.error().code, iaisf::ErrorCode::InvalidState);
    loop->stop();
}

TEST(SignalQueueTest, TwoEventLoopsCannotBothOwnShutdownSignals) {
    RecordingLogger logger;
    auto first_loop = make_loop(logger);
    auto second_loop = make_loop(logger);
    ASSERT_NE(first_loop, nullptr);
    ASSERT_NE(second_loop, nullptr);

    ASSERT_TRUE(first_loop->enable_shutdown_signals());
    auto second_result = second_loop->enable_shutdown_signals();
    ASSERT_FALSE(second_result);
    EXPECT_EQ(second_result.error().code, iaisf::ErrorCode::InvalidState);

    first_loop->stop();
    second_loop->stop();
}

}  // namespace
