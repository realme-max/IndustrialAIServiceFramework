#pragma once

#include <cstddef>
#include <string>

#include "iaisf/application/application_job.hpp"
#include "iaisf/core/result.hpp"

namespace iaisf::application {

inline constexpr std::size_t kMaxApplicationStatusBodyBytes = 16U * 1024U;

/** Produces the bounded public status body; it does not add HTTP headers. */
[[nodiscard]] Result<std::string> application_job_status_json(
    const ApplicationJobSnapshot& snapshot,
    std::size_t maximum_bytes = kMaxApplicationStatusBodyBytes);

}  // namespace iaisf::application
