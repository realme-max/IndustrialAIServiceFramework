#pragma once

#include <filesystem>

#include "iaisf/core/result.hpp"

namespace iaisf::plugin::detail {

/** Resolves a relative plugin library without following untrusted links. */
class SafePathResolver final {
public:
    [[nodiscard]] static Result<std::filesystem::path> canonical_root(
        const std::filesystem::path& root);

    [[nodiscard]] static Result<std::filesystem::path> resolve(
        const std::filesystem::path& root,
        const std::filesystem::path& relative_library);
};

}  // namespace iaisf::plugin::detail
