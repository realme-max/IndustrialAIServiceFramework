#pragma once

#include <cstddef>
#include <string>

#include "iaisf/application/application_execution_result.hpp"
#include "iaisf/application/application_job.hpp"
#include "iaisf/core/result.hpp"

namespace iaisf::application {

inline constexpr std::size_t kMaxApplicationResultBodyBytes = 16U * 1024U;

[[nodiscard]] Result<std::string> application_execution_result_json(
    const ApplicationJobSnapshot& snapshot,
    std::size_t maximum_bytes = kMaxApplicationResultBodyBytes);

}  // namespace iaisf::application
