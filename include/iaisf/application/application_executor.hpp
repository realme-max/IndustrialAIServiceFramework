#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "iaisf/application/application_adapters.hpp"
#include "iaisf/application/application_job_clock.hpp"
#include "iaisf/application/in_memory_application_job_repository.hpp"
#include "iaisf/core/result.hpp"

namespace iaisf::application {

/** A bounded, single-worker queue used only by the application runtime. */
class ApplicationTaskManager final {
public:
    static Result<std::unique_ptr<ApplicationTaskManager>> create(
        std::size_t capacity);
    ~ApplicationTaskManager() noexcept;

    ApplicationTaskManager(const ApplicationTaskManager&) = delete;
    ApplicationTaskManager& operator=(const ApplicationTaskManager&) = delete;

    Result<void> submit(std::function<void()> task);
    void stop_admission() noexcept;
    Result<void> shutdown();
    bool stopped() const noexcept;
    std::size_t pending() const noexcept;

private:
    explicit ApplicationTaskManager(std::size_t capacity);
    void run() noexcept;

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::function<void()>> queue_;
    bool accepting_{true};
    bool stopping_{false};
    bool stopped_{false};
    std::thread worker_;
};

/** Coordinates validated application snapshots and the two independent adapters. */
class ApplicationExecutor final {
public:
    static Result<std::unique_ptr<ApplicationExecutor>> create(
        IApplicationJobRepository& repository,
        Ptv2WeldInspectionAdapter* inspection_adapter,
        WeldAgentWeldingGuidanceAdapter* guidance_adapter,
        const IApplicationJobClock& clock,
        std::size_t queue_capacity);
    ~ApplicationExecutor() noexcept;

    ApplicationExecutor(const ApplicationExecutor&) = delete;
    ApplicationExecutor& operator=(const ApplicationExecutor&) = delete;

    Result<void> submit(const ApplicationJobId& job_id);
    void stop_admission() noexcept;
    Result<void> shutdown();
    bool stopped() const noexcept;

private:
    ApplicationExecutor(
        IApplicationJobRepository& repository,
        Ptv2WeldInspectionAdapter* inspection_adapter,
        WeldAgentWeldingGuidanceAdapter* guidance_adapter,
        const IApplicationJobClock& clock,
        std::unique_ptr<ApplicationTaskManager> tasks) noexcept;
    void execute(const ApplicationJobId& job_id) noexcept;
    [[nodiscard]] Result<ApplicationJobTimePoint> effective_timestamp(
        const ApplicationJobSnapshot& snapshot) const;
    [[nodiscard]] ApplicationJobTimePoint failure_timestamp(
        const ApplicationJobSnapshot& snapshot) const;
    [[nodiscard]] Result<void> fail_running(
        const ApplicationJobSnapshot& snapshot);

    IApplicationJobRepository& repository_;
    Ptv2WeldInspectionAdapter* const inspection_adapter_;
    WeldAgentWeldingGuidanceAdapter* const guidance_adapter_;
    const IApplicationJobClock& clock_;
    std::unique_ptr<ApplicationTaskManager> tasks_;
    std::atomic<bool> accepting_{true};
};

}  // namespace iaisf::application
