#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "iaisf/diagnostics/runtime_diagnostics.hpp"
#include "iaisf/http/http_router.hpp"

namespace iaisf::http {

[[nodiscard]] Result<void> register_diagnostics_route(
    HttpRouter& router,
    std::weak_ptr<const diagnostics::RuntimeDiagnostics> diagnostics,
    std::string endpoint,
    std::size_t maximum_response_bytes);

} // namespace iaisf::http
