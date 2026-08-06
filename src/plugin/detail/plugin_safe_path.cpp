#include "iaisf/plugin/detail/plugin_safe_path.hpp"

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

#include "iaisf/core/error.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace iaisf::plugin::detail {
namespace {

Result<std::filesystem::path> path_failure(
    const ErrorCode code,
    const char* const message) {
    return Result<std::filesystem::path>::failure(make_error(code, message));
}

bool valid_utf8(const std::string_view value) noexcept {
    std::size_t index = 0U;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        std::size_t width = 0U;
        std::uint32_t code_point = 0U;
        if (first <= 0x7FU) {
            width = 1U;
            code_point = first;
        } else if (first >= 0xC2U && first <= 0xDFU) {
            width = 2U;
            code_point = first & 0x1FU;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            width = 3U;
            code_point = first & 0x0FU;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            width = 4U;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (index + width > value.size()) {
            return false;
        }
        for (std::size_t offset = 1U; offset < width; ++offset) {
            const auto continuation =
                static_cast<unsigned char>(value[index + offset]);
            if ((continuation & 0xC0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (continuation & 0x3FU);
        }
        if ((width == 3U && code_point < 0x800U) ||
            (width == 4U && code_point < 0x10000U) ||
            code_point > 0x10FFFFU ||
            (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
            return false;
        }
        index += width;
    }
    return true;
}

Result<std::string> path_utf8(const std::filesystem::path& path) {
    try {
        auto value = path.u8string();
        if (!valid_utf8(value)) {
            return Result<std::string>::failure(make_error(
                ErrorCode::InvalidArgument,
                "plugin path is not valid UTF-8"));
        }
        return Result<std::string>::success(std::move(value));
    } catch (...) {
        return Result<std::string>::failure(make_error(
            ErrorCode::InvalidArgument,
            "plugin path cannot be converted to UTF-8"));
    }
}

bool contains_control(const std::string_view value) noexcept {
    for (const auto byte : value) {
        const auto code = static_cast<unsigned char>(byte);
        if (code < 0x20U || code == 0x7FU) {
            return true;
        }
    }
    return false;
}

bool reparse_or_symlink(const std::filesystem::path& path) noexcept {
#if defined(_WIN32)
    try {
        const auto attributes = ::GetFileAttributesW(path.wstring().c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
               (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
    } catch (...) {
        return true;
    }
#else
    std::error_code error;
    return std::filesystem::is_symlink(
        std::filesystem::symlink_status(path, error));
#endif
}

bool has_reparse_component(const std::filesystem::path& path) noexcept {
    std::filesystem::path current;
    try {
        for (const auto& component : path) {
            current /= component;
            std::error_code error;
            const auto status =
                std::filesystem::symlink_status(current, error);
            if (error || !std::filesystem::exists(status)) {
                continue;
            }
            if (reparse_or_symlink(current)) {
                return true;
            }
        }
    } catch (...) {
        return true;
    }
    return false;
}

Result<void> validate_relative(const std::filesystem::path& relative) {
    auto utf8 = path_utf8(relative);
    if (!utf8) {
        return Result<void>::failure(std::move(utf8).error());
    }
    const auto& raw = utf8.value();
    if (raw.empty() || relative.is_absolute() || relative.has_root_name() ||
        relative.has_root_directory() || raw.find('\\') != std::string::npos ||
        contains_control(raw)) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "plugin library path must be a safe relative UTF-8 path"));
    }
    if (raw.size() >= 2U &&
        std::isalpha(static_cast<unsigned char>(raw.front())) != 0 &&
        raw[1] == ':') {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "plugin library drive paths are not allowed"));
    }
    std::size_t begin = 0U;
    while (begin <= raw.size()) {
        const auto end = raw.find('/', begin);
        const auto length = end == std::string::npos ? raw.size() - begin
                                                     : end - begin;
        const std::string_view component{raw.data() + begin, length};
        if (component.empty() || component == "." || component == "..") {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidArgument,
                "plugin library path contains a forbidden component"));
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1U;
    }
    return Result<void>::success();
}

}  // namespace

Result<std::filesystem::path> SafePathResolver::canonical_root(
    const std::filesystem::path& root) {
    if (root.empty()) {
        return path_failure(ErrorCode::InvalidArgument, "plugin root is empty");
    }
    auto root_utf8 = path_utf8(root);
    if (!root_utf8) {
        return path_failure(
            root_utf8.error().code,
            root_utf8.error().message.c_str());
    }
    if (contains_control(root_utf8.value()) || has_reparse_component(root)) {
        return path_failure(
            ErrorCode::InvalidArgument,
            "plugin root contains a symlink or reparse point");
    }
    std::error_code error;
    const auto status = std::filesystem::symlink_status(root, error);
    if (error || !std::filesystem::exists(status)) {
        return path_failure(ErrorCode::NotFound, "plugin root was not found");
    }
    const auto canonical = std::filesystem::canonical(root, error);
    if (error || !std::filesystem::is_directory(canonical, error) || error) {
        return path_failure(
            ErrorCode::InvalidArgument,
            "plugin root is not a directory");
    }
    return Result<std::filesystem::path>::success(canonical);
}

Result<std::filesystem::path> SafePathResolver::resolve(
    const std::filesystem::path& root,
    const std::filesystem::path& relative_library) {
    auto root_result = canonical_root(root);
    if (!root_result) {
        return root_result;
    }
    auto relative_valid = validate_relative(relative_library);
    if (!relative_valid) {
        return path_failure(
            relative_valid.error().code,
            relative_valid.error().message.c_str());
    }

    const auto canonical_root_path = root_result.value();
    const auto candidate_input = canonical_root_path / relative_library;
    if (has_reparse_component(candidate_input)) {
        return path_failure(
            ErrorCode::InvalidArgument,
            "plugin library contains a symlink or reparse point");
    }
    std::error_code error;
    const auto candidate = std::filesystem::canonical(candidate_input, error);
    if (error || !std::filesystem::is_regular_file(candidate, error) || error) {
        return path_failure(
            ErrorCode::NotFound,
            "plugin library was not found");
    }
    const auto relative = candidate.lexically_relative(canonical_root_path);
    if (relative.empty() || relative.is_absolute() ||
        relative.begin() == relative.end() ||
        *relative.begin() == std::filesystem::path("..")) {
        return path_failure(
            ErrorCode::InvalidArgument,
            "plugin library escapes its root");
    }
    return Result<std::filesystem::path>::success(candidate);
}

}  // namespace iaisf::plugin::detail
