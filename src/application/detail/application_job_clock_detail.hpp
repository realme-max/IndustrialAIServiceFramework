#pragma once

#include "iaisf/application/application_job.hpp"
#include "iaisf/core/result.hpp"

namespace iaisf::application::detail {

[[nodiscard]] Result<void> validate_application_job_time_point(
    ApplicationJobTimePoint value);

}  // namespace iaisf::application::detail
