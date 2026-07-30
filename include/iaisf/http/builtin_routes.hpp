#pragma once

#include "iaisf/core/result.hpp"
#include "iaisf/http/http_router.hpp"

namespace iaisf::http {

[[nodiscard]] Result<void> register_builtin_routes(HttpRouter& router);

}  // namespace iaisf::http
