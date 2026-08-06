#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

#include "iaisf/application/application_job.hpp"
#include "iaisf/core/error.hpp"

namespace iaisf::application {

enum class ApplicationRepositoryFailure {
    InvalidArgument,
    DuplicateId,
    NotFound,
    CapacityExceeded,
    VersionConflict,
    VersionExhausted,
    InvalidTransition,
    InvalidTimestamp,
    InternalFailure,
};

[[nodiscard]] std::string_view to_string(
    ApplicationRepositoryFailure failure) noexcept;

struct ApplicationRepositoryError {
    ApplicationRepositoryFailure category;
    Error detail;
};

template <typename T>
class ApplicationRepositoryResult {
public:
    [[nodiscard]] static ApplicationRepositoryResult success(T value) {
        return ApplicationRepositoryResult{
            std::in_place_index<0>, std::move(value)};
    }

    [[nodiscard]] static ApplicationRepositoryResult failure(
        ApplicationRepositoryError error) {
        return ApplicationRepositoryResult{
            std::in_place_index<1>, std::move(error)};
    }

    [[nodiscard]] bool has_value() const noexcept {
        return storage_.index() == 0U;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    [[nodiscard]] T& value() & {
        ensure_value();
        return std::get<0>(storage_);
    }

    [[nodiscard]] const T& value() const& {
        ensure_value();
        return std::get<0>(storage_);
    }

    [[nodiscard]] T&& value() && {
        ensure_value();
        return std::move(std::get<0>(storage_));
    }

    [[nodiscard]] ApplicationRepositoryError& error() & {
        ensure_error();
        return std::get<1>(storage_);
    }

    [[nodiscard]] const ApplicationRepositoryError& error() const& {
        ensure_error();
        return std::get<1>(storage_);
    }

private:
    using Storage = std::variant<T, ApplicationRepositoryError>;

    template <std::size_t Index, typename Value>
    explicit ApplicationRepositoryResult(
        std::in_place_index_t<Index> index,
        Value&& value)
        : storage_(index, std::forward<Value>(value)) {}

    void ensure_value() const {
        if (!has_value()) {
            throw std::logic_error{
                "ApplicationRepositoryResult does not contain a value"};
        }
    }

    void ensure_error() const {
        if (has_value()) {
            throw std::logic_error{
                "ApplicationRepositoryResult does not contain an error"};
        }
    }

    Storage storage_;
};

/**
 * Protocol-independent repository contract with optimistic concurrency.
 *
 * Implementations return value snapshots and structured failures. Supplying
 * an application that differs from the stored job must be indistinguishable
 * from an unknown id so one application cannot probe the other's jobs.
 */
class IApplicationJobRepository {
public:
    virtual ~IApplicationJobRepository() = default;

    [[nodiscard]] virtual ApplicationRepositoryResult<ApplicationJobSnapshot>
    create(const ApplicationJobCreateRequest& request) = 0;

    [[nodiscard]] virtual ApplicationRepositoryResult<ApplicationJobSnapshot>
    get(
        const ApplicationJobId& job_id,
        IndustrialApplication application) const = 0;

    [[nodiscard]] virtual ApplicationRepositoryResult<ApplicationJobSnapshot>
    transition(
        const ApplicationJobId& job_id,
        IndustrialApplication application,
        std::uint64_t expected_version,
        ApplicationJobState target_state,
        ApplicationJobTimePoint updated_at) = 0;

    [[nodiscard]] virtual ApplicationRepositoryResult<ApplicationJobSnapshot>
    erase_terminal(
        const ApplicationJobId& job_id,
        IndustrialApplication application,
        std::uint64_t expected_version) = 0;

    [[nodiscard]] virtual std::size_t size() const = 0;
    [[nodiscard]] virtual std::size_t capacity() const noexcept = 0;
};

}  // namespace iaisf::application
