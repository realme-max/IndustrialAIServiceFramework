#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

#include "iaisf/application/application_identity.hpp"
#include "iaisf/application/application_job_id.hpp"

namespace iaisf::application {

enum class ApplicationJobIdGenerationFailure {
    InvalidApplication,
    EntropyUnavailable,
    ResourceFailure,
    InternalFailure,
};

[[nodiscard]] std::string_view to_string(
    ApplicationJobIdGenerationFailure failure) noexcept;

struct ApplicationJobIdGenerationError {
    ApplicationJobIdGenerationFailure category;
    std::string message;
};

/** Result with a stable, machine-readable generation failure category. */
class ApplicationJobIdGenerationResult final {
public:
    static constexpr std::size_t kMaxErrorMessageBytes = 128U;

    [[nodiscard]] static ApplicationJobIdGenerationResult success(
        ApplicationJobId value);
    [[nodiscard]] static ApplicationJobIdGenerationResult failure(
        ApplicationJobIdGenerationError error);

    [[nodiscard]] bool has_value() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    ApplicationJobIdGenerationResult(const ApplicationJobIdGenerationResult&) = default;
    ApplicationJobIdGenerationResult& operator=(
        const ApplicationJobIdGenerationResult&) = default;
    ApplicationJobIdGenerationResult(ApplicationJobIdGenerationResult&&) = delete;
    ApplicationJobIdGenerationResult& operator=(
        ApplicationJobIdGenerationResult&&) = delete;

    [[nodiscard]] ApplicationJobId& value();
    [[nodiscard]] const ApplicationJobId& value() const;
    [[nodiscard]] ApplicationJobIdGenerationError& error();
    [[nodiscard]] const ApplicationJobIdGenerationError& error() const;

private:
    explicit ApplicationJobIdGenerationResult(ApplicationJobId value);
    explicit ApplicationJobIdGenerationResult(
        ApplicationJobIdGenerationError error);

    void ensure_value() const;
    void ensure_error() const;

    std::variant<ApplicationJobId, ApplicationJobIdGenerationError> storage_;
};

class IApplicationJobIdGenerator {
public:
    virtual ~IApplicationJobIdGenerator() = default;

    [[nodiscard]] virtual ApplicationJobIdGenerationResult generate(
        IndustrialApplication application) = 0;
};

/**
 * Generates canonical IDs from the operating system cryptographic RNG.
 *
 * The implementation is stateless apart from its private entropy source and
 * may be called concurrently by multiple threads.
 */
class OsApplicationJobIdGenerator final : public IApplicationJobIdGenerator {
public:
    OsApplicationJobIdGenerator();
    ~OsApplicationJobIdGenerator();

    OsApplicationJobIdGenerator(const OsApplicationJobIdGenerator&) = delete;
    OsApplicationJobIdGenerator& operator=(
        const OsApplicationJobIdGenerator&) = delete;
    OsApplicationJobIdGenerator(OsApplicationJobIdGenerator&&) = delete;
    OsApplicationJobIdGenerator& operator=(OsApplicationJobIdGenerator&&) = delete;

    [[nodiscard]] ApplicationJobIdGenerationResult generate(
        IndustrialApplication application) override;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

}  // namespace iaisf::application
