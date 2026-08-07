#include "iaisf/application/application_job_id_generator.hpp"

#include <array>
#include <new>
#include <stdexcept>
#include <utility>

#include "detail/application_job_id_entropy.hpp"

namespace iaisf::application {
namespace {

constexpr std::size_t kEntropyBytes = 16U;
constexpr std::size_t kCanonicalBytes = 35U;
constexpr char kHex[] = "0123456789abcdef";

[[nodiscard]] bool is_valid_failure_category(
    const ApplicationJobIdGenerationFailure category) noexcept {
    switch (category) {
        case ApplicationJobIdGenerationFailure::InvalidApplication:
        case ApplicationJobIdGenerationFailure::EntropyUnavailable:
        case ApplicationJobIdGenerationFailure::ResourceFailure:
        case ApplicationJobIdGenerationFailure::InternalFailure:
            return true;
    }
    return false;
}

[[nodiscard]] const char* default_failure_message(
    const ApplicationJobIdGenerationFailure category) noexcept {
    switch (category) {
        case ApplicationJobIdGenerationFailure::InvalidApplication:
            return "application is invalid";
        case ApplicationJobIdGenerationFailure::EntropyUnavailable:
            return "application job id entropy is unavailable";
        case ApplicationJobIdGenerationFailure::ResourceFailure:
            return "unable to allocate application job id";
        case ApplicationJobIdGenerationFailure::InternalFailure:
            return "application job id generation failed";
    }
    return "application job id generation failed";
}

[[nodiscard]] ApplicationJobIdGenerationError normalize_error(
    ApplicationJobIdGenerationError error) {
    if (!is_valid_failure_category(error.category)) {
        error.category = ApplicationJobIdGenerationFailure::InternalFailure;
    }
    if (error.message.empty() ||
        error.message.size() >
            ApplicationJobIdGenerationResult::kMaxErrorMessageBytes) {
        error.message = default_failure_message(error.category);
    }
    return error;
}

[[nodiscard]] ApplicationJobIdGenerationResult failure(
    const ApplicationJobIdGenerationFailure category,
    const char* const message) {
    return ApplicationJobIdGenerationResult::failure(
        ApplicationJobIdGenerationError{category, std::string{message}});
}

[[nodiscard]] std::string canonical_text(
    const IndustrialApplication application,
    const std::array<std::uint8_t, kEntropyBytes>& entropy) {
    std::array<char, kCanonicalBytes> text{};
    const char* prefix = nullptr;
    switch (application) {
        case IndustrialApplication::WeldInspection:
            prefix = "wi_";
            break;
        case IndustrialApplication::WeldingGuidance:
            prefix = "wg_";
            break;
    }

    if (prefix == nullptr) {
        return {};
    }
    text[0] = prefix[0];
    text[1] = prefix[1];
    text[2] = prefix[2];
    for (std::size_t index = 0U; index < entropy.size(); ++index) {
        const std::size_t output = 3U + index * 2U;
        text[output] = kHex[(entropy[index] >> 4U) & 0x0FU];
        text[output + 1U] = kHex[entropy[index] & 0x0FU];
    }
    return std::string{text.data(), text.size()};
}

enum class EntropyFillStatus {
    Complete,
    Unavailable,
    ContractViolation,
};

[[nodiscard]] EntropyFillStatus fill_entropy(
    detail::ApplicationJobIdEntropyReader& reader,
    std::array<std::uint8_t, kEntropyBytes>& entropy) {
    std::size_t offset = 0U;
    while (offset < entropy.size()) {
        const auto read = reader.read(
            entropy.data() + offset,
            entropy.size() - offset);
        switch (read.status) {
            case detail::EntropyReadStatus::Bytes:
                if (read.bytes == 0U || read.bytes > entropy.size() - offset) {
                    return EntropyFillStatus::ContractViolation;
                }
                offset += read.bytes;
                break;
            case detail::EntropyReadStatus::Interrupted:
                break;
            case detail::EntropyReadStatus::Failed:
            case detail::EntropyReadStatus::EndOfStream:
                return EntropyFillStatus::Unavailable;
        }
    }
    return EntropyFillStatus::Complete;
}

}  // namespace

struct OsApplicationJobIdGenerator::Impl {
    explicit Impl(std::shared_ptr<detail::ApplicationJobIdEntropyReader> source)
        : entropy(std::move(source)) {}

    std::shared_ptr<detail::ApplicationJobIdEntropyReader> entropy;
};

std::string_view to_string(const ApplicationJobIdGenerationFailure failure) noexcept {
    switch (failure) {
        case ApplicationJobIdGenerationFailure::InvalidApplication:
            return "invalid_application";
        case ApplicationJobIdGenerationFailure::EntropyUnavailable:
            return "entropy_unavailable";
        case ApplicationJobIdGenerationFailure::ResourceFailure:
            return "resource_failure";
        case ApplicationJobIdGenerationFailure::InternalFailure:
            return "internal_failure";
    }
    return "unknown";
}

ApplicationJobIdGenerationResult::ApplicationJobIdGenerationResult(
    ApplicationJobId value)
    : storage_(std::in_place_index<0>, std::move(value)) {}

ApplicationJobIdGenerationResult::ApplicationJobIdGenerationResult(
    ApplicationJobIdGenerationError error)
    : storage_(std::in_place_index<1>, normalize_error(std::move(error))) {}

ApplicationJobIdGenerationResult ApplicationJobIdGenerationResult::success(
    ApplicationJobId value) {
    return ApplicationJobIdGenerationResult{std::move(value)};
}

ApplicationJobIdGenerationResult ApplicationJobIdGenerationResult::failure(
    ApplicationJobIdGenerationError error) {
    return ApplicationJobIdGenerationResult{std::move(error)};
}

bool ApplicationJobIdGenerationResult::has_value() const noexcept {
    return storage_.index() == 0U;
}

ApplicationJobId& ApplicationJobIdGenerationResult::value() {
    ensure_value();
    return std::get<0>(storage_);
}

const ApplicationJobId& ApplicationJobIdGenerationResult::value() const {
    ensure_value();
    return std::get<0>(storage_);
}

ApplicationJobIdGenerationError& ApplicationJobIdGenerationResult::error() {
    ensure_error();
    return std::get<1>(storage_);
}

const ApplicationJobIdGenerationError&
ApplicationJobIdGenerationResult::error() const {
    ensure_error();
    return std::get<1>(storage_);
}

void ApplicationJobIdGenerationResult::ensure_value() const {
    if (!has_value()) {
        throw std::logic_error{
            "ApplicationJobIdGenerationResult does not contain a value"};
    }
}

void ApplicationJobIdGenerationResult::ensure_error() const {
    if (has_value()) {
        throw std::logic_error{
            "ApplicationJobIdGenerationResult does not contain an error"};
    }
}

namespace detail {

ApplicationJobIdGenerationResult generate_with_entropy_reader(
    const IndustrialApplication application,
    ApplicationJobIdEntropyReader& reader) {
    switch (application) {
        case IndustrialApplication::WeldInspection:
        case IndustrialApplication::WeldingGuidance:
            break;
        default:
            return failure(
                ApplicationJobIdGenerationFailure::InvalidApplication,
                "application is invalid");
    }

    std::array<std::uint8_t, kEntropyBytes> entropy{};
    switch (fill_entropy(reader, entropy)) {
        case EntropyFillStatus::Unavailable:
            return failure(
                ApplicationJobIdGenerationFailure::EntropyUnavailable,
                "application job id entropy is unavailable");
        case EntropyFillStatus::ContractViolation:
            return failure(
                ApplicationJobIdGenerationFailure::InternalFailure,
                "application job id generation failed");
        case EntropyFillStatus::Complete:
            break;
    }

    const auto parsed = ApplicationJobId::parse(
        canonical_text(application, entropy));
    if (!parsed) {
        const auto category =
            parsed.error().code == ErrorCode::ResourceExhausted
                ? ApplicationJobIdGenerationFailure::ResourceFailure
                : ApplicationJobIdGenerationFailure::InternalFailure;
        return failure(
            category,
            category == ApplicationJobIdGenerationFailure::ResourceFailure
                ? "unable to allocate application job id"
                : "application job id generation failed");
    }
    return ApplicationJobIdGenerationResult::success(parsed.value());
}

}  // namespace detail

OsApplicationJobIdGenerator::OsApplicationJobIdGenerator()
    : impl_(std::make_unique<Impl>(
          detail::make_platform_job_id_entropy_reader())) {}

OsApplicationJobIdGenerator::~OsApplicationJobIdGenerator() = default;

ApplicationJobIdGenerationResult OsApplicationJobIdGenerator::generate(
    const IndustrialApplication application) {
    if (impl_ == nullptr || impl_->entropy == nullptr) {
        return failure(
            ApplicationJobIdGenerationFailure::InternalFailure,
            "application job id generation failed");
    }

    try {
        return detail::generate_with_entropy_reader(
            application,
            *impl_->entropy);
    } catch (const std::bad_alloc&) {
        return failure(
            ApplicationJobIdGenerationFailure::ResourceFailure,
            "unable to allocate application job id");
    } catch (const std::length_error&) {
        return failure(
            ApplicationJobIdGenerationFailure::ResourceFailure,
            "unable to allocate application job id");
    } catch (const std::exception&) {
        return failure(
            ApplicationJobIdGenerationFailure::InternalFailure,
            "application job id generation failed");
    } catch (...) {
        return failure(
            ApplicationJobIdGenerationFailure::InternalFailure,
            "application job id generation failed");
    }
}

}  // namespace iaisf::application
