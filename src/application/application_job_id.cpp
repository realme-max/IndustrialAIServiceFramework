#include "iaisf/application/application_job_id.hpp"

#include <new>
#include <stdexcept>

#include "iaisf/core/error.hpp"

namespace iaisf::application {
namespace {

[[nodiscard]] bool ascii_alnum(const unsigned char value) noexcept {
    return (value >= static_cast<unsigned char>('0') &&
            value <= static_cast<unsigned char>('9')) ||
           (value >= static_cast<unsigned char>('A') &&
            value <= static_cast<unsigned char>('Z')) ||
           (value >= static_cast<unsigned char>('a') &&
            value <= static_cast<unsigned char>('z'));
}

[[nodiscard]] bool valid_text(const std::string_view text) noexcept {
    if (text.empty() || text.size() > ApplicationJobId::kMaxBytes ||
        !ascii_alnum(static_cast<unsigned char>(text.front()))) {
        return false;
    }
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (!ascii_alnum(byte) && character != '_' && character != '-') {
            return false;
        }
    }
    return true;
}

}  // namespace

Result<ApplicationJobId> ApplicationJobId::parse(const std::string_view text) {
    if (!valid_text(text)) {
        return Result<ApplicationJobId>::failure(make_error(
            ErrorCode::InvalidArgument,
            "application job id is invalid"));
    }
    try {
        return Result<ApplicationJobId>::success(
            ApplicationJobId{std::string{text}});
    } catch (const std::bad_alloc&) {
        return Result<ApplicationJobId>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate application job id"));
    } catch (const std::length_error&) {
        return Result<ApplicationJobId>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "application job id exceeds the platform size limit"));
    }
}

ApplicationJobId::ApplicationJobId(ApplicationJobId&& other)
    : value_(other.value_) {}

ApplicationJobId& ApplicationJobId::operator=(const ApplicationJobId& other) {
    if (this != &other) {
        ApplicationJobId copy{other};
        swap(copy);
    }
    return *this;
}

ApplicationJobId& ApplicationJobId::operator=(ApplicationJobId&& other) {
    if (this != &other) {
        *this = static_cast<const ApplicationJobId&>(other);
    }
    return *this;
}

void ApplicationJobId::swap(ApplicationJobId& other) noexcept {
    value_.swap(other.value_);
}

bool ApplicationJobId::valid() const noexcept {
    return valid_text(value_);
}

std::string_view ApplicationJobId::value() const noexcept {
    return value_;
}

}  // namespace iaisf::application
