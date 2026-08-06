#pragma once

#include <filesystem>
#include <string_view>

#include "iaisf/core/result.hpp"

namespace iaisf::plugin::detail {

using NativeModuleHandle = void*;

/** Move-only RAII wrapper for one native shared-library handle. */
class DynamicModule final {
public:
    [[nodiscard]] static Result<DynamicModule> open(
        const std::filesystem::path& path);

    DynamicModule(const DynamicModule&) = delete;
    DynamicModule& operator=(const DynamicModule&) = delete;

    DynamicModule(DynamicModule&& other) noexcept;
    DynamicModule& operator=(DynamicModule&& other) noexcept;

    ~DynamicModule() noexcept;

    [[nodiscard]] Result<void*> resolve_symbol(
        std::string_view symbol) const;

    [[nodiscard]] bool loaded() const noexcept;

    /** Releases the native handle and reports a platform unload failure. */
    [[nodiscard]] bool close() noexcept;

private:
    explicit DynamicModule(NativeModuleHandle handle) noexcept;

    NativeModuleHandle handle_{nullptr};
};

}  // namespace iaisf::plugin::detail
