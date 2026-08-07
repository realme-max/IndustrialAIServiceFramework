#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "iaisf/application/application_submission.hpp"
#include "iaisf/application/artifact_ref.hpp"

namespace iaisf::application {

enum class ApplicationContractErrorCategory {
    InvalidJson,
    InvalidRequest,
    ValidationFailed,
    PayloadTooLarge,
    ResourceFailure,
    InternalFailure,
};

[[nodiscard]] std::string_view to_string(
    ApplicationContractErrorCategory category) noexcept;

struct ApplicationContractError final {
    ApplicationContractErrorCategory category;
    std::string message;
};

template <typename T>
class ApplicationContractResult final {
public:
    static ApplicationContractResult success(T value) {
        return ApplicationContractResult(std::move(value));
    }
    static ApplicationContractResult failure(ApplicationContractError error) {
        return ApplicationContractResult(std::move(error));
    }

    [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return value_.has_value(); }
    [[nodiscard]] T& value() { return value_.value(); }
    [[nodiscard]] const T& value() const { return value_.value(); }
    [[nodiscard]] ApplicationContractError& error() { return error_.value(); }
    [[nodiscard]] const ApplicationContractError& error() const { return error_.value(); }

private:
    explicit ApplicationContractResult(T value)
        : value_(std::move(value)) {}
    explicit ApplicationContractResult(ApplicationContractError error)
        : error_(std::move(error)) {}

    std::optional<T> value_;
    std::optional<ApplicationContractError> error_;
};

struct ApplicationSubmitContract final {
    IndustrialApplication application;
    ScenePhase scene_phase;
    ApplicationSubmissionSpec submission;
    ArtifactRef input_artifact;
};

[[nodiscard]] ApplicationContractResult<ApplicationSubmitContract>
parse_weld_inspection_submit(std::string_view body);

[[nodiscard]] ApplicationContractResult<ApplicationSubmitContract>
parse_welding_guidance_submit(std::string_view body);

}  // namespace iaisf::application
