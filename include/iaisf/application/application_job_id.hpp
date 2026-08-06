#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include "iaisf/core/result.hpp"

namespace iaisf::application {

class ApplicationJobId {
public:
    static constexpr std::size_t kMaxBytes = 64U;

    [[nodiscard]] static Result<ApplicationJobId> parse(std::string_view text);

    ApplicationJobId(const ApplicationJobId&) = default;
    ApplicationJobId& operator=(const ApplicationJobId& other);

    /** Copy-preserving move: both source and destination retain the same id. */
    ApplicationJobId(ApplicationJobId&& other);
    ApplicationJobId& operator=(ApplicationJobId&& other);

    void swap(ApplicationJobId& other) noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::string_view value() const noexcept;

    friend bool operator==(
        const ApplicationJobId& lhs,
        const ApplicationJobId& rhs) noexcept {
        return lhs.value_ == rhs.value_;
    }

    friend bool operator!=(
        const ApplicationJobId& lhs,
        const ApplicationJobId& rhs) noexcept {
        return !(lhs == rhs);
    }

private:
    explicit ApplicationJobId(std::string value) : value_(std::move(value)) {}

    std::string value_;
};

}  // namespace iaisf::application

namespace std {

template <>
struct hash<iaisf::application::ApplicationJobId> {
    [[nodiscard]] size_t operator()(
        const iaisf::application::ApplicationJobId& id) const noexcept {
        return hash<string_view>{}(id.value());
    }
};

}  // namespace std
