#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

#include "iaisf/application/application_identity.hpp"
#include "iaisf/core/result.hpp"

namespace iaisf::application {

enum class ApplicationSubmissionKind {
    WeldInspection,
    WeldingGuidance,
};

enum class WeldTypeMode {
    Auto,
    Requested,
};

enum class RequestedWeldType {
    Straight,
    Corner,
    L,
};

enum class HumanCheckpointPolicy {
    Required,
    NotRequired,
};

[[nodiscard]] std::string_view to_string(
    ApplicationSubmissionKind kind) noexcept;
[[nodiscard]] std::string_view to_string(WeldTypeMode mode) noexcept;
[[nodiscard]] std::string_view to_string(RequestedWeldType type) noexcept;
[[nodiscard]] std::string_view to_string(
    HumanCheckpointPolicy policy) noexcept;

/**
 * Canonical, allocation-free set of inspection outputs.
 *
 * A bitmask makes output order unobservable in the Domain value: the two
 * JSON spellings [segmentation, geometry] and [geometry, segmentation] map to
 * the same validated value in the later protocol layer.
 */
class InspectionRequestedOutputs final {
public:
    [[nodiscard]] static Result<InspectionRequestedOutputs> create(
        bool segmentation,
        bool geometry);

    [[nodiscard]] bool requests_segmentation() const noexcept;
    [[nodiscard]] bool requests_geometry() const noexcept;

    friend bool operator==(
        const InspectionRequestedOutputs& lhs,
        const InspectionRequestedOutputs& rhs) noexcept {
        return lhs.mask_ == rhs.mask_;
    }

    friend bool operator!=(
        const InspectionRequestedOutputs& lhs,
        const InspectionRequestedOutputs& rhs) noexcept {
        return !(lhs == rhs);
    }

private:
    explicit InspectionRequestedOutputs(const std::uint8_t mask) noexcept
        : mask_(mask) {}

    std::uint8_t mask_;
};

class WeldInspectionSubmission final {
public:
    [[nodiscard]] static Result<WeldInspectionSubmission> create(
        InspectionRequestedOutputs outputs);

    [[nodiscard]] const InspectionRequestedOutputs& outputs() const noexcept;

    friend bool operator==(
        const WeldInspectionSubmission& lhs,
        const WeldInspectionSubmission& rhs) noexcept {
        return lhs.outputs_ == rhs.outputs_;
    }

    friend bool operator!=(
        const WeldInspectionSubmission& lhs,
        const WeldInspectionSubmission& rhs) noexcept {
        return !(lhs == rhs);
    }

private:
    explicit WeldInspectionSubmission(
        InspectionRequestedOutputs outputs) noexcept
        : outputs_(outputs) {}

    InspectionRequestedOutputs outputs_;
};

class WeldTypeRequest final {
public:
    [[nodiscard]] static Result<WeldTypeRequest> create(
        WeldTypeMode mode,
        std::optional<RequestedWeldType> requested_type);

    [[nodiscard]] WeldTypeMode mode() const noexcept;
    [[nodiscard]] std::optional<RequestedWeldType> requested_type()
        const noexcept;

    friend bool operator==(
        const WeldTypeRequest& lhs,
        const WeldTypeRequest& rhs) noexcept {
        return lhs.mode_ == rhs.mode_ &&
               lhs.requested_type_ == rhs.requested_type_;
    }

    friend bool operator!=(
        const WeldTypeRequest& lhs,
        const WeldTypeRequest& rhs) noexcept {
        return !(lhs == rhs);
    }

private:
    WeldTypeRequest(
        const WeldTypeMode mode,
        const std::optional<RequestedWeldType> requested_type) noexcept
        : mode_(mode), requested_type_(requested_type) {}

    WeldTypeMode mode_;
    std::optional<RequestedWeldType> requested_type_;
};

class WeldingGuidanceSubmission final {
public:
    [[nodiscard]] static Result<WeldingGuidanceSubmission> create(
        WeldTypeRequest weld_type,
        HumanCheckpointPolicy human_checkpoint);

    [[nodiscard]] const WeldTypeRequest& weld_type() const noexcept;
    [[nodiscard]] HumanCheckpointPolicy human_checkpoint() const noexcept;

    friend bool operator==(
        const WeldingGuidanceSubmission& lhs,
        const WeldingGuidanceSubmission& rhs) noexcept {
        return lhs.weld_type_ == rhs.weld_type_ &&
               lhs.human_checkpoint_ == rhs.human_checkpoint_;
    }

    friend bool operator!=(
        const WeldingGuidanceSubmission& lhs,
        const WeldingGuidanceSubmission& rhs) noexcept {
        return !(lhs == rhs);
    }

private:
    WeldingGuidanceSubmission(
        WeldTypeRequest weld_type,
        const HumanCheckpointPolicy human_checkpoint) noexcept
        : weld_type_(weld_type), human_checkpoint_(human_checkpoint) {}

    WeldTypeRequest weld_type_;
    HumanCheckpointPolicy human_checkpoint_;
};

/**
 * Validated, immutable application-specific submission value.
 *
 * The variant is deliberately private. Callers can only obtain an instance
 * through one of the checked factories and can inspect only the matching
 * alternative.
 */
class ApplicationSubmissionSpec final {
public:
    [[nodiscard]] static Result<ApplicationSubmissionSpec> create(
        WeldInspectionSubmission inspection);
    [[nodiscard]] static Result<ApplicationSubmissionSpec> create(
        WeldingGuidanceSubmission guidance);

    [[nodiscard]] ApplicationSubmissionKind kind() const noexcept;
    [[nodiscard]] const WeldInspectionSubmission* inspection() const noexcept;
    [[nodiscard]] const WeldingGuidanceSubmission* guidance() const noexcept;

    /** Validates the application/scene/spec cross-product. */
    [[nodiscard]] Result<void> validate_for(
        IndustrialApplication application,
        ScenePhase scene_phase) const;

    friend bool operator==(
        const ApplicationSubmissionSpec& lhs,
        const ApplicationSubmissionSpec& rhs) noexcept {
        return lhs.value_ == rhs.value_;
    }

    friend bool operator!=(
        const ApplicationSubmissionSpec& lhs,
        const ApplicationSubmissionSpec& rhs) noexcept {
        return !(lhs == rhs);
    }

private:
    using Value = std::variant<WeldInspectionSubmission, WeldingGuidanceSubmission>;

    explicit ApplicationSubmissionSpec(Value value) noexcept
        : value_(std::move(value)) {}

    Value value_;
};

}  // namespace iaisf::application
