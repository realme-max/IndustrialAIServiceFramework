#include "iaisf/application/application_job_repository.hpp"

namespace iaisf::application {

std::string_view to_string(const ApplicationRepositoryFailure failure) noexcept {
    switch (failure) {
        case ApplicationRepositoryFailure::InvalidArgument:
            return "invalid_argument";
        case ApplicationRepositoryFailure::DuplicateId:
            return "duplicate_id";
        case ApplicationRepositoryFailure::NotFound:
            return "not_found";
        case ApplicationRepositoryFailure::CapacityExceeded:
            return "capacity_exceeded";
        case ApplicationRepositoryFailure::VersionConflict:
            return "version_conflict";
        case ApplicationRepositoryFailure::VersionExhausted:
            return "version_exhausted";
        case ApplicationRepositoryFailure::InvalidTransition:
            return "invalid_transition";
        case ApplicationRepositoryFailure::InvalidTimestamp:
            return "invalid_timestamp";
        case ApplicationRepositoryFailure::InternalFailure:
            return "internal_failure";
    }
    return "unknown";
}

}  // namespace iaisf::application
