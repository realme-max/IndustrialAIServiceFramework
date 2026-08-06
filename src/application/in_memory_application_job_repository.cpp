#include "iaisf/application/in_memory_application_job_repository.hpp"

#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace iaisf::application {
namespace {

template <typename T>
[[nodiscard]] ApplicationRepositoryResult<T> failure(
    const ApplicationRepositoryFailure category,
    const ErrorCode code,
    const char* message) {
    return ApplicationRepositoryResult<T>::failure(
        ApplicationRepositoryError{category, make_error(code, message)});
}

[[nodiscard]] bool valid_application(
    const IndustrialApplication application) noexcept {
    switch (application) {
        case IndustrialApplication::WeldInspection:
        case IndustrialApplication::WeldingGuidance:
            return true;
    }
    return false;
}

[[nodiscard]] ApplicationRepositoryFailure creation_failure_category(
    const ErrorCode code) noexcept {
    return code == ErrorCode::ResourceExhausted ||
                   code == ErrorCode::InternalError
               ? ApplicationRepositoryFailure::InternalFailure
               : ApplicationRepositoryFailure::InvalidArgument;
}

}  // namespace

InMemoryApplicationJobRepository::InMemoryApplicationJobRepository(
    const std::size_t capacity) noexcept
    : capacity_(capacity) {}

ApplicationRepositoryResult<std::unique_ptr<InMemoryApplicationJobRepository>>
InMemoryApplicationJobRepository::make(const std::size_t capacity) {
    using RepositoryPointer = std::unique_ptr<InMemoryApplicationJobRepository>;
    if (capacity == 0U) {
        return failure<RepositoryPointer>(
            ApplicationRepositoryFailure::InvalidArgument,
            ErrorCode::InvalidArgument,
            "application job repository capacity must be positive");
    }
    try {
        // The validated factory is the only construction path; make_unique
        // cannot invoke this class's private constructor. Ownership is placed
        // in unique_ptr by the same full expression.
        return ApplicationRepositoryResult<RepositoryPointer>::success(
            RepositoryPointer{new InMemoryApplicationJobRepository{capacity}});
    } catch (const std::bad_alloc&) {
        return failure<RepositoryPointer>(
            ApplicationRepositoryFailure::InternalFailure,
            ErrorCode::ResourceExhausted,
            "unable to allocate application job repository");
    }
}

ApplicationRepositoryResult<ApplicationJobSnapshot>
InMemoryApplicationJobRepository::create(
    const ApplicationJobCreateRequest& request) {
    auto snapshot_result = ApplicationJobSnapshot::create(request);
    if (!snapshot_result) {
        return failure<ApplicationJobSnapshot>(
            creation_failure_category(snapshot_result.error().code),
            snapshot_result.error().code,
            "application job create request is invalid");
    }

    try {
        ApplicationJobSnapshot response = snapshot_result.value();
        std::lock_guard<std::mutex> lock(mutex_);
        if (records_.find(request.job_id) != records_.end()) {
            return failure<ApplicationJobSnapshot>(
                ApplicationRepositoryFailure::DuplicateId,
                ErrorCode::InvalidState,
                "application job id already exists");
        }
        if (records_.size() >= capacity_) {
            return failure<ApplicationJobSnapshot>(
                ApplicationRepositoryFailure::CapacityExceeded,
                ErrorCode::ResourceExhausted,
                "application job repository capacity has been reached");
        }
        const auto inserted = records_.emplace(
            request.job_id, std::move(snapshot_result).value());
        if (!inserted.second) {
            return failure<ApplicationJobSnapshot>(
                ApplicationRepositoryFailure::InternalFailure,
                ErrorCode::InternalError,
                "application job repository insertion failed");
        }
        return ApplicationRepositoryResult<ApplicationJobSnapshot>::success(
            std::move(response));
    } catch (const std::bad_alloc&) {
        return failure<ApplicationJobSnapshot>(
            ApplicationRepositoryFailure::InternalFailure,
            ErrorCode::ResourceExhausted,
            "unable to allocate application job repository storage");
    } catch (const std::length_error&) {
        return failure<ApplicationJobSnapshot>(
            ApplicationRepositoryFailure::InternalFailure,
            ErrorCode::ResourceExhausted,
            "application job repository exceeds the platform size limit");
    }
}

ApplicationRepositoryResult<ApplicationJobSnapshot>
InMemoryApplicationJobRepository::get(
    const ApplicationJobId& job_id,
    const IndustrialApplication application) const {
    if (!job_id.valid() || !valid_application(application)) {
        return failure<ApplicationJobSnapshot>(
            ApplicationRepositoryFailure::InvalidArgument,
            ErrorCode::InvalidArgument,
            "application is invalid");
    }
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = records_.find(job_id);
        if (found == records_.end() ||
            found->second.application() != application) {
            return failure<ApplicationJobSnapshot>(
                ApplicationRepositoryFailure::NotFound,
                ErrorCode::NotFound,
                "application job was not found");
        }
        return ApplicationRepositoryResult<ApplicationJobSnapshot>::success(
            found->second);
    } catch (const std::bad_alloc&) {
        return failure<ApplicationJobSnapshot>(
            ApplicationRepositoryFailure::InternalFailure,
            ErrorCode::ResourceExhausted,
            "unable to copy application job snapshot");
    } catch (const std::length_error&) {
        return failure<ApplicationJobSnapshot>(
            ApplicationRepositoryFailure::InternalFailure,
            ErrorCode::ResourceExhausted,
            "application job snapshot exceeds the platform size limit");
    }
}

ApplicationRepositoryResult<ApplicationJobSnapshot>
InMemoryApplicationJobRepository::transition(
    const ApplicationJobId& job_id,
    const IndustrialApplication application,
    const std::uint64_t expected_version,
    const ApplicationJobState target_state,
    const ApplicationJobTimePoint updated_at) {
    if (!job_id.valid() || !valid_application(application) ||
        expected_version == 0U) {
        return failure<ApplicationJobSnapshot>(
            ApplicationRepositoryFailure::InvalidArgument,
            ErrorCode::InvalidArgument,
            "application job transition argument is invalid");
    }

    try {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = records_.find(job_id);
        if (found == records_.end() ||
            found->second.application() != application) {
            return failure<ApplicationJobSnapshot>(
                ApplicationRepositoryFailure::NotFound,
                ErrorCode::NotFound,
                "application job was not found");
        }
        const auto& current = found->second;
        if (current.version() != expected_version) {
            return failure<ApplicationJobSnapshot>(
                ApplicationRepositoryFailure::VersionConflict,
                ErrorCode::InvalidState,
                "application job version does not match");
        }
        if (updated_at < current.updated_at()) {
            return failure<ApplicationJobSnapshot>(
                ApplicationRepositoryFailure::InvalidTimestamp,
                ErrorCode::InvalidArgument,
                "application job timestamp cannot move backwards");
        }
        if (current.version() == std::numeric_limits<std::uint64_t>::max()) {
            return failure<ApplicationJobSnapshot>(
                ApplicationRepositoryFailure::VersionExhausted,
                ErrorCode::ResourceExhausted,
                "application job version space is exhausted");
        }
        const auto valid = validate_transition(
            current.state(), target_state, current.application());
        if (!valid) {
            return failure<ApplicationJobSnapshot>(
                ApplicationRepositoryFailure::InvalidTransition,
                ErrorCode::InvalidState,
                "application job state transition is invalid");
        }

        auto updated_result = current.transitioned(target_state, updated_at);
        if (!updated_result) {
            return failure<ApplicationJobSnapshot>(
                ApplicationRepositoryFailure::InternalFailure,
                updated_result.error().code,
                "unable to create transitioned application job snapshot");
        }
        ApplicationJobSnapshot response = updated_result.value();
        found->second.state_ = target_state;
        ++found->second.version_;
        found->second.updated_at_ = updated_at;
        return ApplicationRepositoryResult<ApplicationJobSnapshot>::success(
            std::move(response));
    } catch (const std::bad_alloc&) {
        return failure<ApplicationJobSnapshot>(
            ApplicationRepositoryFailure::InternalFailure,
            ErrorCode::ResourceExhausted,
            "unable to allocate transitioned application job snapshot");
    } catch (const std::length_error&) {
        return failure<ApplicationJobSnapshot>(
            ApplicationRepositoryFailure::InternalFailure,
            ErrorCode::ResourceExhausted,
            "transitioned application job snapshot exceeds the platform size limit");
    }
}

ApplicationRepositoryResult<ApplicationJobSnapshot>
InMemoryApplicationJobRepository::erase_terminal(
    const ApplicationJobId& job_id,
    const IndustrialApplication application,
    const std::uint64_t expected_version) {
    if (!job_id.valid() || !valid_application(application) ||
        expected_version == 0U) {
        return failure<ApplicationJobSnapshot>(
            ApplicationRepositoryFailure::InvalidArgument,
            ErrorCode::InvalidArgument,
            "application job erase argument is invalid");
    }
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = records_.find(job_id);
        if (found == records_.end() ||
            found->second.application() != application) {
            return failure<ApplicationJobSnapshot>(
                ApplicationRepositoryFailure::NotFound,
                ErrorCode::NotFound,
                "application job was not found");
        }
        if (found->second.version() != expected_version) {
            return failure<ApplicationJobSnapshot>(
                ApplicationRepositoryFailure::VersionConflict,
                ErrorCode::InvalidState,
                "application job version does not match");
        }
        if (!is_terminal(found->second.state())) {
            return failure<ApplicationJobSnapshot>(
                ApplicationRepositoryFailure::InvalidTransition,
                ErrorCode::InvalidState,
                "only a terminal application job can be erased");
        }
        ApplicationJobSnapshot response = found->second;
        records_.erase(found);
        return ApplicationRepositoryResult<ApplicationJobSnapshot>::success(
            std::move(response));
    } catch (const std::bad_alloc&) {
        return failure<ApplicationJobSnapshot>(
            ApplicationRepositoryFailure::InternalFailure,
            ErrorCode::ResourceExhausted,
            "unable to copy erased application job snapshot");
    } catch (const std::length_error&) {
        return failure<ApplicationJobSnapshot>(
            ApplicationRepositoryFailure::InternalFailure,
            ErrorCode::ResourceExhausted,
            "erased application job snapshot exceeds the platform size limit");
    }
}

std::size_t InMemoryApplicationJobRepository::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.size();
}

std::size_t InMemoryApplicationJobRepository::capacity() const noexcept {
    return capacity_;
}

}  // namespace iaisf::application
