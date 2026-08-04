#include "signal_queue.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <exception>
#include <new>
#include <utility>

#include <pthread.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "iaisf/core/error.hpp"
#include "iaisf/net/channel.hpp"
#include "iaisf/net/event_loop.hpp"
#include "system_error.hpp"

namespace iaisf::net::detail {
namespace {

constexpr std::array<int, 2U> kHandledSignals{SIGINT, SIGTERM};
constexpr std::size_t kReadBatchSize = 16U;

std::atomic<SignalQueue*>& process_signal_owner() noexcept {
    static std::atomic<SignalQueue*> owner{nullptr};
    return owner;
}

[[nodiscard]] Result<void> initialize_empty_set(
    sigset_t& set,
    const char* const operation) {
    if (::sigemptyset(&set) == 0) {
        return Result<void>::success();
    }
    const int error_number = errno;
    return Result<void>::failure(make_system_error(operation, error_number));
}

}  // namespace

SignalQueue::SignalQueue(
    EventLoop& owner,
    Callback before_loop_stop,
    ErrorNotification error_notification) noexcept
    : owner_(owner),
      before_loop_stop_(std::move(before_loop_stop)),
      error_notification_(std::move(error_notification)) {}

SignalQueue::~SignalQueue() noexcept {
    try {
        auto result = shutdown();
        if (!result) {
            notify_error("failed to shut down EventLoop signal queue");
        }
    } catch (...) {
        std::terminate();
    }
    if (signal_channel_ && signal_channel_->is_registered()) {
        std::terminate();
    }
}

Result<std::unique_ptr<SignalQueue>> SignalQueue::create(
    EventLoop& owner,
    Callback before_loop_stop,
    ErrorNotification error_notification) {
    if (!owner.is_in_loop_thread() ||
        owner.state() != EventLoop::State::Created) {
        return Result<std::unique_ptr<SignalQueue>>::failure(make_error(
            ErrorCode::InvalidState,
            "signal queue creation requires a Created owner EventLoop"));
    }
    if (!error_notification) {
        return Result<std::unique_ptr<SignalQueue>>::failure(make_error(
            ErrorCode::InvalidArgument,
            "signal error notification cannot be empty"));
    }

    try {
        auto queue = std::unique_ptr<SignalQueue>{new SignalQueue{
            owner,
            std::move(before_loop_stop),
            std::move(error_notification)}};
        auto result = queue->claim_process_owner();
        if (!result) {
            return Result<std::unique_ptr<SignalQueue>>::failure(
                std::move(result).error());
        }
        result = queue->block_signals();
        if (!result) {
            return Result<std::unique_ptr<SignalQueue>>::failure(
                std::move(result).error());
        }
        result = queue->initialize_descriptor();
        if (!result) {
            return Result<std::unique_ptr<SignalQueue>>::failure(
                std::move(result).error());
        }
        result = queue->initialize_channel();
        if (!result) {
            return Result<std::unique_ptr<SignalQueue>>::failure(
                std::move(result).error());
        }
        return Result<std::unique_ptr<SignalQueue>>::success(
            std::move(queue));
    } catch (const std::bad_alloc&) {
        return Result<std::unique_ptr<SignalQueue>>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate signal queue"));
    } catch (const std::exception&) {
        return Result<std::unique_ptr<SignalQueue>>::failure(make_error(
            ErrorCode::InternalError,
            "unable to create signal queue"));
    }
}

Result<void> SignalQueue::claim_process_owner() {
    SignalQueue* expected = nullptr;
    if (!process_signal_owner().compare_exchange_strong(
            expected,
            this,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "process already has a shutdown SignalQueue owner"));
    }
    process_owner_claimed_ = true;
    return Result<void>::success();
}

Result<void> SignalQueue::block_signals() {
    auto result = initialize_empty_set(handled_mask_, "sigemptyset handled mask");
    if (!result) {
        return result;
    }
    result = initialize_empty_set(owned_mask_, "sigemptyset owned mask");
    if (!result) {
        return result;
    }
    for (const int signal_number : kHandledSignals) {
        if (::sigaddset(&handled_mask_, signal_number) != 0) {
            const int error_number = errno;
            return Result<void>::failure(
                make_system_error("sigaddset handled signal", error_number));
        }
    }

    sigset_t previous_mask{};
    const int block_error =
        ::pthread_sigmask(SIG_BLOCK, &handled_mask_, &previous_mask);
    if (block_error != 0) {
        return Result<void>::failure(
            make_system_error("pthread_sigmask block", block_error));
    }
    mask_installed_ = true;

    for (const int signal_number : kHandledSignals) {
        const int membership = ::sigismember(&previous_mask, signal_number);
        if (membership < 0) {
            const int error_number = errno;
            return Result<void>::failure(
                make_system_error("sigismember previous mask", error_number));
        }
        if (membership == 0 &&
            ::sigaddset(&owned_mask_, signal_number) != 0) {
            const int error_number = errno;
            return Result<void>::failure(
                make_system_error("sigaddset owned signal", error_number));
        }
    }
    return Result<void>::success();
}

Result<void> SignalQueue::initialize_descriptor() {
    const int descriptor = ::signalfd(
        -1,
        &handled_mask_,
        SFD_NONBLOCK | SFD_CLOEXEC);
    if (descriptor < 0) {
        const int error_number = errno;
        return Result<void>::failure(
            make_system_error("signalfd", error_number));
    }
    signal_fd_.reset(descriptor);
    return Result<void>::success();
}

Result<void> SignalQueue::initialize_channel() {
    try {
        signal_channel_ = std::make_unique<Channel>(owner_, signal_fd_.get());
        signal_channel_->set_read_callback([this] { handle_readable(); });
        signal_channel_->set_error_callback(
            [this] { handle_descriptor_failure(); });
        signal_channel_->set_close_callback(
            [this] { handle_descriptor_failure(); });
        signal_channel_->enable_reading();
        signal_channel_->set_edge_triggered(true);
        return signal_channel_->update();
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate signal channel"));
    } catch (const std::exception&) {
        return Result<void>::failure(make_error(
            ErrorCode::InternalError,
            "unable to initialize signal channel"));
    }
}

Result<void> SignalQueue::drain_descriptor(
    const bool request_stop) {
    std::array<signalfd_siginfo, kReadBatchSize> records{};
    bool handled_signal_seen = false;
    for (;;) {
        const ssize_t bytes_read = ::read(
            signal_fd_.get(),
            records.data(),
            sizeof(records));
        if (bytes_read > 0) {
            const auto unsigned_bytes = static_cast<std::size_t>(bytes_read);
            if (unsigned_bytes % sizeof(signalfd_siginfo) != 0U) {
                return Result<void>::failure(make_error(
                    ErrorCode::SystemError,
                    "signalfd returned an incomplete signal record"));
            }
            const std::size_t record_count =
                unsigned_bytes / sizeof(signalfd_siginfo);
            for (std::size_t index = 0U; index < record_count; ++index) {
                const std::uint32_t signal_number = records[index].ssi_signo;
                if (signal_number != static_cast<std::uint32_t>(SIGINT) &&
                    signal_number != static_cast<std::uint32_t>(SIGTERM)) {
                    return Result<void>::failure(make_error(
                        ErrorCode::SystemError,
                        "signalfd returned an unexpected signal"));
                }
                handled_signal_seen = true;
            }
            continue;
        }
        if (bytes_read == 0) {
            return Result<void>::failure(make_error(
                ErrorCode::SystemError,
                "signalfd returned end of file"));
        }

        const int error_number = errno;
        if (error_number == EINTR) {
            continue;
        }
        if (error_number == EAGAIN || error_number == EWOULDBLOCK) {
            if (request_stop && handled_signal_seen) {
                request_shutdown(nullptr);
            }
            return Result<void>::success();
        }
        return Result<void>::failure(
            make_system_error("signalfd read", error_number));
    }
}

void SignalQueue::handle_readable() noexcept {
    try {
        auto result = drain_descriptor(true);
        if (!result) {
            request_shutdown("failed to drain EventLoop signal fd");
        }
    } catch (...) {
        request_shutdown("failed to drain EventLoop signal fd");
    }
}

void SignalQueue::handle_descriptor_failure() noexcept {
    request_shutdown("EventLoop signal fd reported an error");
}

void SignalQueue::request_shutdown(const char* const message) noexcept {
    if (message != nullptr) {
        notify_error(message);
    }
    if (shutdown_requested_ || shutdown_) {
        owner_.stop();
        return;
    }
    shutdown_requested_ = true;
    if (before_loop_stop_) {
        try {
            before_loop_stop_();
        } catch (const std::exception&) {
            notify_error("signal shutdown callback threw std::exception");
        } catch (...) {
            notify_error("signal shutdown callback threw an unknown exception");
        }
    }
    owner_.stop();
}

Result<void> SignalQueue::restore_owned_mask() {
    if (!mask_installed_) {
        return Result<void>::success();
    }
    const int unblock_error =
        ::pthread_sigmask(SIG_UNBLOCK, &owned_mask_, nullptr);
    if (unblock_error != 0) {
        return Result<void>::failure(
            make_system_error("pthread_sigmask unblock", unblock_error));
    }
    mask_installed_ = false;
    return Result<void>::success();
}

void SignalQueue::release_process_owner() noexcept {
    if (!process_owner_claimed_) {
        return;
    }
    SignalQueue* expected = this;
    const bool released = process_signal_owner().compare_exchange_strong(
        expected,
        nullptr,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
    if (!released) {
        std::terminate();
    }
    process_owner_claimed_ = false;
}

Result<void> SignalQueue::shutdown() {
    if (shutdown_) {
        return Result<void>::success();
    }
    if (!owner_.is_in_loop_thread()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "signal queue shutdown requires the EventLoop owner thread"));
    }
    if (owner_.dispatching_active_channels()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "signal queue shutdown must follow the active Channel batch"));
    }

    if (signal_channel_ && signal_channel_->is_registered()) {
        auto remove_result = signal_channel_->remove();
        if (!remove_result) {
            return remove_result;
        }
    }
    signal_channel_.reset();

    if (signal_fd_.valid()) {
        auto drain_result = drain_descriptor(false);
        if (!drain_result) {
            signal_fd_.reset();
            before_loop_stop_ = {};
            mask_restore_safe_ = false;
            shutdown_ = true;
            release_process_owner();
            return drain_result;
        }
        signal_fd_.reset();
    }

    if (mask_restore_safe_) {
        auto restore_result = restore_owned_mask();
        if (!restore_result) {
            before_loop_stop_ = {};
            shutdown_ = true;
            release_process_owner();
            return restore_result;
        }
    }
    before_loop_stop_ = {};
    shutdown_ = true;
    release_process_owner();
    return Result<void>::success();
}

void SignalQueue::notify_error(const char* const message) noexcept {
    if (!error_notification_) {
        return;
    }
    try {
        error_notification_(message);
    } catch (...) {
        owner_.stop();
    }
}

const SignalQueue* SignalQueueTestAccess::queue(
    const EventLoop& loop) noexcept {
    return loop.signal_queue_.get();
}

int SignalQueueTestAccess::signal_fd(const SignalQueue& queue) noexcept {
    return queue.signal_fd_.get();
}

bool SignalQueueTestAccess::channel_registered(
    const SignalQueue& queue) noexcept {
    return queue.signal_channel_ && queue.signal_channel_->is_registered();
}

bool SignalQueueTestAccess::channel_edge_triggered(
    const SignalQueue& queue) noexcept {
    return queue.signal_channel_ && queue.signal_channel_->edge_triggered();
}

bool SignalQueueTestAccess::shutdown_requested(
    const SignalQueue& queue) noexcept {
    return queue.shutdown_requested_;
}

}  // namespace iaisf::net::detail
