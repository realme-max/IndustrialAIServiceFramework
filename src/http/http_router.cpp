#include "iaisf/http/http_router.hpp"

#include <algorithm>
#include <new>
#include <string_view>
#include <utility>

namespace iaisf::http {
namespace {

[[nodiscard]] bool is_token_character(unsigned char value) noexcept {
    if ((value >= static_cast<unsigned char>('a') &&
         value <= static_cast<unsigned char>('z')) ||
        (value >= static_cast<unsigned char>('A') &&
         value <= static_cast<unsigned char>('Z')) ||
        (value >= static_cast<unsigned char>('0') &&
         value <= static_cast<unsigned char>('9'))) {
        return true;
    }
    constexpr std::string_view kPunctuation{"!#$%&'*+-.^_`|~"};
    return kPunctuation.find(static_cast<char>(value)) != std::string_view::npos;
}

[[nodiscard]] bool valid_method(std::string_view method) noexcept {
    return !method.empty() &&
           std::all_of(method.begin(), method.end(), [](char character) {
               return is_token_character(static_cast<unsigned char>(character));
           });
}

[[nodiscard]] bool valid_path(std::string_view path) noexcept {
    if (path.empty() || path.front() != '/' ||
        path.find('?') != std::string_view::npos ||
        path.find('#') != std::string_view::npos) {
        return false;
    }
    return std::none_of(path.begin(), path.end(), [](char character) {
        const auto value = static_cast<unsigned char>(character);
        return value <= 0x20U || value == 0x7FU || character == '\\';
    });
}

}  // namespace

HttpRouter::HttpRouter(HttpLimits limits) : limits_(std::move(limits)) {}

Result<void> HttpRouter::add_route(
    std::string method,
    std::string path,
    Handler handler) {
    if (frozen_) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "HTTP router is frozen"));
    }
    if (!valid_method(method) || !valid_path(path) || !handler) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "HTTP route method, path or handler is invalid"));
    }
    if (route_count() == limits_.max_routes()) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "HTTP route capacity is exhausted"));
    }
    const auto duplicate = std::find_if(
        routes_.begin(),
        routes_.end(),
        [&method, &path](const Route& route) {
            return route.method == method && route.path == path;
        });
    if (duplicate != routes_.end()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "duplicate HTTP route"));
    }
    try {
        routes_.push_back(
            Route{std::move(method), std::move(path), std::move(handler)});
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "HTTP route allocation failed"));
    }
    return Result<void>::success();
}

Result<void> HttpRouter::add_terminal_parameter_route(
    std::string method,
    std::string prefix,
    TerminalParameterHandler handler) {
    if (frozen_) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "HTTP router is frozen"));
    }
    if (!valid_method(method) || !valid_path(prefix) || prefix.back() != '/' ||
        !handler) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "HTTP parameter route method, prefix or handler is invalid"));
    }
    if (route_count() == limits_.max_routes()) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "HTTP route capacity is exhausted"));
    }
    const auto duplicate = std::find_if(
        terminal_parameter_routes_.begin(),
        terminal_parameter_routes_.end(),
        [&method, &prefix](const TerminalParameterRoute& route) {
            return route.method == method && route.prefix == prefix;
        });
    if (duplicate != terminal_parameter_routes_.end()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "duplicate HTTP parameter route"));
    }
    try {
        terminal_parameter_routes_.push_back(TerminalParameterRoute{
            std::move(method),
            std::move(prefix),
            std::move(handler)});
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "HTTP parameter route allocation failed"));
    }
    return Result<void>::success();
}

Result<void> HttpRouter::set_routing_error_handler(
    RoutingErrorHandler handler) {
    if (frozen_) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "HTTP router is frozen"));
    }
    if (!handler) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "HTTP routing error handler must not be empty"));
    }
    if (routing_error_handler_) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "HTTP routing error handler is already registered"));
    }
    try {
        routing_error_handler_ = std::move(handler);
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "HTTP routing error handler allocation failed"));
    }
    return Result<void>::success();
}

Result<void> HttpRouter::freeze() {
    if (frozen_) {
        return Result<void>::success();
    }
    frozen_ = true;
    return Result<void>::success();
}

Result<HttpResponse> HttpRouter::dispatch(
    const HttpRequest& request) const {
    if (!frozen_) {
        return Result<HttpResponse>::failure(make_error(
            ErrorCode::InvalidState,
            "HTTP router must be frozen before dispatch"));
    }

    try {
        const auto route = std::find_if(
            routes_.begin(),
            routes_.end(),
            [&request](const Route& candidate) {
                return candidate.method == request.method() &&
                       candidate.path == request.path();
            });
        if (route != routes_.end()) {
            try {
                auto result = route->handler(request);
                if (result) {
                    auto validation = result.value().validate(limits_);
                    if (!validation) {
                        return Result<HttpResponse>::success(
                            HttpResponse::error(
                                HttpStatus::InternalServerError,
                                true));
                    }
                    return result;
                }
                return Result<HttpResponse>::success(
                    HttpResponse::error(
                        HttpStatus::InternalServerError,
                        true));
            } catch (...) {
                return Result<HttpResponse>::success(
                    HttpResponse::error(
                        HttpStatus::InternalServerError,
                        true));
            }
        }

        const TerminalParameterRoute* parameter_route = nullptr;
        std::string parameter;
        for (const auto& candidate : terminal_parameter_routes_) {
            if (request.path().size() <= candidate.prefix.size() ||
                request.path().compare(
                    0U,
                    candidate.prefix.size(),
                    candidate.prefix) != 0) {
                continue;
            }
            const std::string_view suffix{
                request.path().data() + candidate.prefix.size(),
                request.path().size() - candidate.prefix.size()};
            if (suffix.find('/') != std::string_view::npos) {
                continue;
            }
            if (candidate.method == request.method()) {
                parameter_route = &candidate;
                parameter.assign(suffix);
                break;
            }
        }
        if (parameter_route != nullptr) {
            try {
                auto result = parameter_route->handler(request, parameter);
                if (result && result.value().validate(limits_)) {
                    return result;
                }
            } catch (...) {
            }
            return Result<HttpResponse>::success(
                HttpResponse::error(HttpStatus::InternalServerError, true));
        }

        std::vector<std::string> methods;
        for (const auto& candidate : routes_) {
            if (candidate.path == request.path()) {
                methods.push_back(candidate.method);
            }
        }
        for (const auto& candidate : terminal_parameter_routes_) {
            if (request.path().size() <= candidate.prefix.size() ||
                request.path().compare(
                    0U,
                    candidate.prefix.size(),
                    candidate.prefix) != 0) {
                continue;
            }
            const std::string_view suffix{
                request.path().data() + candidate.prefix.size(),
                request.path().size() - candidate.prefix.size()};
            if (suffix.find('/') == std::string_view::npos) {
                methods.push_back(candidate.method);
            }
        }
        if (methods.empty()) {
            if (routing_error_handler_) {
                try {
                    auto custom = routing_error_handler_(
                        HttpStatus::NotFound,
                        request);
                    if (custom && custom.value().validate(limits_)) {
                        return custom;
                    }
                } catch (...) {
                }
                return Result<HttpResponse>::success(
                    HttpResponse::error(
                        HttpStatus::InternalServerError,
                        true));
            }
            return Result<HttpResponse>::success(
                HttpResponse::error(HttpStatus::NotFound, false));
        }

        std::sort(methods.begin(), methods.end());
        methods.erase(std::unique(methods.begin(), methods.end()), methods.end());
        std::string allow;
        for (const auto& method : methods) {
            if (!allow.empty()) {
                allow.append(", ");
            }
            allow.append(method);
        }
        HttpResponse response;
        if (routing_error_handler_) {
            try {
                auto custom = routing_error_handler_(
                    HttpStatus::MethodNotAllowed,
                    request);
                if (!custom) {
                    return Result<HttpResponse>::success(
                        HttpResponse::error(
                            HttpStatus::InternalServerError,
                            true));
                }
                response = std::move(custom).value();
            } catch (...) {
                return Result<HttpResponse>::success(
                    HttpResponse::error(
                        HttpStatus::InternalServerError,
                        true));
            }
        } else {
            response =
                HttpResponse::error(HttpStatus::MethodNotAllowed, false);
        }
        auto header_result = response.set_header("Allow", std::move(allow));
        if (!header_result) {
            return Result<HttpResponse>::failure(
                std::move(header_result).error());
        }
        auto response_validation = response.validate(limits_);
        if (!response_validation) {
            return Result<HttpResponse>::success(
                HttpResponse::error(
                    HttpStatus::InternalServerError,
                    true));
        }
        return Result<HttpResponse>::success(std::move(response));
    } catch (const std::bad_alloc&) {
        return Result<HttpResponse>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "HTTP router allocation failed"));
    } catch (...) {
        return Result<HttpResponse>::failure(make_error(
            ErrorCode::InternalError,
            "HTTP router internal failure"));
    }
}

bool HttpRouter::frozen() const noexcept {
    return frozen_;
}
std::size_t HttpRouter::route_count() const noexcept {
    return routes_.size() + terminal_parameter_routes_.size();
}

}  // namespace iaisf::http
