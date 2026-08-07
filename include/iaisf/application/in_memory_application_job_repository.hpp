#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "iaisf/application/application_job_repository.hpp"

namespace iaisf::application {

class ApplicationJobRepositoryTestAccess;

/**
 * Thread-safe bounded metadata repository.
 *
 * It starts no threads, performs no I/O, never evicts automatically, and does
 * not own or delete any ArtifactRef payload. Public operations serialize one
 * record transaction under the internal mutex; no callback or user code runs
 * while that mutex is held.
 */
class InMemoryApplicationJobRepository final
    : public IApplicationJobRepository {
public:
    [[nodiscard]] static ApplicationRepositoryResult<
        std::unique_ptr<InMemoryApplicationJobRepository>>
    make(std::size_t capacity);

    InMemoryApplicationJobRepository(
        const InMemoryApplicationJobRepository&) = delete;
    InMemoryApplicationJobRepository& operator=(
        const InMemoryApplicationJobRepository&) = delete;

    [[nodiscard]] ApplicationRepositoryResult<ApplicationJobSnapshot> create(
        const ApplicationJobCreateRequest& request) override;

    [[nodiscard]] ApplicationRepositoryResult<ApplicationJobSnapshot> get(
        const ApplicationJobId& job_id,
        IndustrialApplication application) const override;

    [[nodiscard]] ApplicationRepositoryResult<ApplicationJobSnapshot> transition(
        const ApplicationJobId& job_id,
        IndustrialApplication application,
        std::uint64_t expected_version,
        ApplicationJobState target_state,
        ApplicationJobTimePoint updated_at) override;

    [[nodiscard]] ApplicationRepositoryResult<ApplicationJobSnapshot> complete(
        const ApplicationJobId& job_id,
        IndustrialApplication application,
        std::uint64_t expected_version,
        ApplicationJobState target_state,
        ApplicationExecutionResult result,
        ApplicationJobTimePoint updated_at) override;

    [[nodiscard]] ApplicationRepositoryResult<ApplicationJobSnapshot>
    erase_terminal(
        const ApplicationJobId& job_id,
        IndustrialApplication application,
        std::uint64_t expected_version) override;

    [[nodiscard]] std::size_t size() const override;
    [[nodiscard]] std::size_t capacity() const noexcept override;

private:
    friend class ApplicationJobRepositoryTestAccess;

    explicit InMemoryApplicationJobRepository(std::size_t capacity) noexcept;

    using Records = std::unordered_map<ApplicationJobId, ApplicationJobSnapshot>;

    std::size_t capacity_;
    mutable std::mutex mutex_;
    Records records_;
};

}  // namespace iaisf::application
