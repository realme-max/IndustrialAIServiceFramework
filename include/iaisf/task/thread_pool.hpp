#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "iaisf/core/result.hpp"

namespace iaisf::task {

struct ThreadPoolOptions {
    std::size_t worker_threads{4};
    std::size_t queue_capacity{1024};
};

/**
 * Fixed-size worker pool with a bounded, non-blocking submission queue.
 *
 * All public methods are thread-safe. Work items run outside the queue lock.
 * shutdown() stops admission, drains accepted work, and joins every worker.
 */
class BoundedThreadPool {
    struct ConstructionKey {
        explicit ConstructionKey() = default;
    };

public:
    using WorkItem = std::function<void()>;
    static constexpr std::size_t kMaximumWorkerCount = 256U;
    static constexpr std::size_t kMaximumQueueCapacity = 1'000'000U;

    [[nodiscard]] static Result<void> validate_options(
        ThreadPoolOptions options);
    [[nodiscard]] static Result<std::unique_ptr<BoundedThreadPool>> create(
        ThreadPoolOptions options);

    BoundedThreadPool(ConstructionKey, ThreadPoolOptions options);
    ~BoundedThreadPool();

    BoundedThreadPool(const BoundedThreadPool&) = delete;
    BoundedThreadPool& operator=(const BoundedThreadPool&) = delete;
    BoundedThreadPool(BoundedThreadPool&&) = delete;
    BoundedThreadPool& operator=(BoundedThreadPool&&) = delete;

    [[nodiscard]] Result<void> try_submit(WorkItem work);
    [[nodiscard]] Result<void> shutdown();

    [[nodiscard]] bool accepting() const;
    [[nodiscard]] bool running() const;
    [[nodiscard]] bool stopped() const;
    [[nodiscard]] bool is_worker_thread() const noexcept;
    [[nodiscard]] std::size_t pending_count() const;
    [[nodiscard]] std::size_t worker_count() const noexcept;
    [[nodiscard]] std::size_t task_exception_count() const noexcept;
    [[nodiscard]] std::size_t unhandled_exception_count() const noexcept;

private:
    enum class State {
        Running,
        ShuttingDown,
        Stopped,
    };

    [[nodiscard]] Result<void> start();
    void worker_loop() noexcept;

    ThreadPoolOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable work_available_;
    std::condition_variable stopped_;
    std::deque<WorkItem> queue_;
    std::vector<std::thread> workers_;
    State state_{State::Running};
    std::atomic<std::size_t> task_exception_count_{0};

    static thread_local const BoundedThreadPool* current_worker_pool_;
};

}  // namespace iaisf::task
