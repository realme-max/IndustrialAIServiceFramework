#include "iaisf/task/thread_pool.hpp"

#include <exception>
#include <new>
#include <utility>

namespace iaisf::task {
namespace {

constexpr std::size_t kMaximumWorkerCount = 256;
constexpr std::size_t kMaximumQueueCapacity = 1000000;

}  // namespace

thread_local const BoundedThreadPool* BoundedThreadPool::current_worker_pool_ = nullptr;

Result<std::unique_ptr<BoundedThreadPool>> BoundedThreadPool::create(
    const ThreadPoolOptions options) {
    if (options.worker_threads == 0 ||
        options.worker_threads > kMaximumWorkerCount) {
        return Result<std::unique_ptr<BoundedThreadPool>>::failure(make_error(
            ErrorCode::InvalidArgument,
            "thread pool worker count is outside the supported range"));
    }
    if (options.queue_capacity == 0 ||
        options.queue_capacity > kMaximumQueueCapacity) {
        return Result<std::unique_ptr<BoundedThreadPool>>::failure(make_error(
            ErrorCode::InvalidArgument,
            "thread pool queue capacity is outside the supported range"));
    }

    try {
        auto pool = std::make_unique<BoundedThreadPool>(ConstructionKey{}, options);
        auto started = pool->start();
        if (!started) {
            return Result<std::unique_ptr<BoundedThreadPool>>::failure(
                std::move(started).error());
        }
        return Result<std::unique_ptr<BoundedThreadPool>>::success(std::move(pool));
    } catch (const std::bad_alloc&) {
        return Result<std::unique_ptr<BoundedThreadPool>>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate the bounded thread pool"));
    } catch (const std::exception&) {
        return Result<std::unique_ptr<BoundedThreadPool>>::failure(make_error(
            ErrorCode::SystemError,
            "unable to construct the bounded thread pool"));
    }
}

BoundedThreadPool::BoundedThreadPool(
    ConstructionKey,
    const ThreadPoolOptions options)
    : options_(options) {}

BoundedThreadPool::~BoundedThreadPool() {
    const auto stopped = shutdown();
    if (!stopped) {
        std::terminate();
    }
}

Result<void> BoundedThreadPool::start() {
    try {
        workers_.reserve(options_.worker_threads);
        for (std::size_t index = 0; index < options_.worker_threads; ++index) {
            workers_.emplace_back([this] { worker_loop(); });
        }
        return Result<void>::success();
    } catch (const std::exception&) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = State::ShuttingDown;
        }
        work_available_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = State::Stopped;
        }
        stopped_.notify_all();
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to create all bounded thread pool workers"));
    }
}

Result<void> BoundedThreadPool::try_submit(WorkItem work) {
    if (!work) {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidArgument, "work item must not be empty"));
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::Running) {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidState,
                "thread pool is not accepting work"));
        }
        if (queue_.size() >= options_.queue_capacity) {
            return Result<void>::failure(make_error(
                ErrorCode::ResourceExhausted,
                "thread pool queue capacity has been reached"));
        }
        try {
            queue_.push_back(std::move(work));
        } catch (const std::bad_alloc&) {
            return Result<void>::failure(make_error(
                ErrorCode::ResourceExhausted,
                "unable to allocate thread pool queue storage"));
        }
    }
    work_available_.notify_one();
    return Result<void>::success();
}

Result<void> BoundedThreadPool::shutdown() {
    if (is_worker_thread()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "a worker cannot shut down its own thread pool"));
    }

    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (state_ == State::Stopped) {
            return Result<void>::success();
        }
        if (state_ == State::ShuttingDown) {
            stopped_.wait(lock, [this] { return state_ == State::Stopped; });
            return Result<void>::success();
        }
        state_ = State::ShuttingDown;
    }
    work_available_.notify_all();

    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = State::Stopped;
    }
    stopped_.notify_all();
    return Result<void>::success();
}

bool BoundedThreadPool::accepting() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::Running;
}

bool BoundedThreadPool::running() const {
    return accepting();
}

bool BoundedThreadPool::stopped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::Stopped;
}

bool BoundedThreadPool::is_worker_thread() const noexcept {
    return current_worker_pool_ == this;
}

std::size_t BoundedThreadPool::pending_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

std::size_t BoundedThreadPool::worker_count() const noexcept {
    return options_.worker_threads;
}

std::size_t BoundedThreadPool::task_exception_count() const noexcept {
    return task_exception_count_.load(std::memory_order_relaxed);
}

std::size_t BoundedThreadPool::unhandled_exception_count() const noexcept {
    return task_exception_count();
}

void BoundedThreadPool::worker_loop() noexcept {
    current_worker_pool_ = this;
    for (;;) {
        WorkItem work;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            work_available_.wait(
                lock,
                [this] { return !queue_.empty() || state_ != State::Running; });
            if (queue_.empty()) {
                current_worker_pool_ = nullptr;
                return;
            }
            work = std::move(queue_.front());
            queue_.pop_front();
        }

        try {
            work();
        } catch (...) {
            task_exception_count_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

}  // namespace iaisf::task
