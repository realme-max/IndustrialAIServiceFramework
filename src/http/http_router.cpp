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
    if (routes_.size() == limits_.max_routes()) {
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

        std::vector<std::string> methods;
        for (const auto& candidate : routes_) {
            if (candidate.path == request.path()) {
                methods.push_back(candidate.method);
            }
        }
        if (methods.empty()) {
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
        auto response =
            HttpResponse::error(HttpStatus::MethodNotAllowed, false);
        auto header_result = response.set_header("Allow", std::move(allow));
        if (!header_result) {
            return Result<HttpResponse>::failure(
                std::move(header_result).error());
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
    return routes_.size();
}

}  // namespace iaisf::http
