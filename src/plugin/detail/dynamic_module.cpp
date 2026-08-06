#include "iaisf/plugin/detail/dynamic_module.hpp"

#include <new>
#include <string>
#include <utility>

#include "iaisf/core/error.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace iaisf::plugin::detail {
namespace {

Result<DynamicModule> open_failure(const ErrorCode code, const char* message) {
    return Result<DynamicModule>::failure(make_error(code, message));
}

}  // namespace

Result<DynamicModule> DynamicModule::open(
    const std::filesystem::path& path) {
    if (path.empty()) {
        return open_failure(
            ErrorCode::InvalidArgument,
            "dynamic module path is empty");
    }

#if defined(_WIN32)
    try {
        // filesystem::path stores the Windows native UTF-16 representation;
        // LoadLibraryExW therefore never depends on the process code page.
        const auto wide_path = path.wstring();
        const auto handle = ::LoadLibraryExW(
            wide_path.c_str(),
            nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (handle == nullptr) {
            return open_failure(
                ErrorCode::SystemError,
                "dynamic module load failed");
        }
        return Result<DynamicModule>::success(
            DynamicModule{reinterpret_cast<NativeModuleHandle>(handle)});
    } catch (const std::bad_alloc&) {
        return open_failure(
            ErrorCode::ResourceExhausted,
            "dynamic module path allocation failed");
    } catch (...) {
        return open_failure(
            ErrorCode::SystemError,
            "dynamic module load failed");
    }
#else
    try {
        const std::string native_path = path.string();
        (void)::dlerror();
        void* const handle =
            ::dlopen(native_path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) {
            (void)::dlerror();
            return open_failure(
                ErrorCode::SystemError,
                "dynamic module load failed");
        }
        return Result<DynamicModule>::success(DynamicModule{handle});
    } catch (const std::bad_alloc&) {
        return open_failure(
            ErrorCode::ResourceExhausted,
            "dynamic module path allocation failed");
    } catch (...) {
        return open_failure(
            ErrorCode::SystemError,
            "dynamic module load failed");
    }
#endif
}

DynamicModule::DynamicModule(const NativeModuleHandle handle) noexcept
    : handle_(handle) {}

DynamicModule::DynamicModule(DynamicModule&& other) noexcept
    : handle_(other.handle_) {
    other.handle_ = nullptr;
}

DynamicModule& DynamicModule::operator=(DynamicModule&& other) noexcept {
    if (this != &other) {
        (void)close();
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

DynamicModule::~DynamicModule() noexcept {
    (void)close();
}

Result<void*> DynamicModule::resolve_symbol(
    const std::string_view symbol) const {
    if (symbol.empty()) {
        return Result<void*>::failure(make_error(
            ErrorCode::InvalidArgument,
            "dynamic module symbol is empty"));
    }
    if (!loaded()) {
        return Result<void*>::failure(make_error(
            ErrorCode::InvalidState,
            "dynamic module is not loaded"));
    }

    try {
        const std::string symbol_name{symbol};
#if defined(_WIN32)
        auto* const handle = static_cast<HMODULE>(handle_);
        auto* const address = ::GetProcAddress(handle, symbol_name.c_str());
        if (address == nullptr) {
            return Result<void*>::failure(make_error(
                ErrorCode::NotFound,
                "dynamic module symbol was not found"));
        }
        return Result<void*>::success(reinterpret_cast<void*>(address));
#else
        (void)::dlerror();
        void* const address = ::dlsym(handle_, symbol_name.c_str());
        const char* const error = ::dlerror();
        if (error != nullptr || address == nullptr) {
            return Result<void*>::failure(make_error(
                ErrorCode::NotFound,
                "dynamic module symbol was not found"));
        }
        return Result<void*>::success(address);
#endif
    } catch (const std::bad_alloc&) {
        return Result<void*>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "dynamic module symbol allocation failed"));
    } catch (...) {
        return Result<void*>::failure(make_error(
            ErrorCode::InternalError,
            "dynamic module symbol lookup failed"));
    }
}

bool DynamicModule::loaded() const noexcept {
    return handle_ != nullptr;
}

bool DynamicModule::close() noexcept {
    if (handle_ == nullptr) {
        return true;
    }
    bool success = true;
#if defined(_WIN32)
    success = ::FreeLibrary(static_cast<HMODULE>(handle_)) != FALSE;
#else
    success = ::dlclose(handle_) == 0;
#endif
    handle_ = nullptr;
    return success;
}

}  // namespace iaisf::plugin::detail
