#pragma once

#include <nlohmann/json.hpp>

#include "iaisf/core/result.hpp"
#include "iaisf/task/task_types.hpp"

namespace iaisf::api {

/**
 * Produces the public task view. Input, timestamps and runtime internals are
 * intentionally absent.
 */
[[nodiscard]] Result<nlohmann::json> task_snapshot_to_json(
    const task::TaskSnapshot& snapshot);

}  // namespace iaisf::api
