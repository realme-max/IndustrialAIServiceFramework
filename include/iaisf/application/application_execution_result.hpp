#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "iaisf/application/application_identity.hpp"
#include "iaisf/application/application_submission.hpp"
#include "iaisf/application/artifact_ref.hpp"
#include "iaisf/core/result.hpp"

namespace iaisf::application {

struct ApplicationPoint3 final {
    double x{};
    double y{};
    double z{};
};

enum class GuidanceResultDisposition {
    Completed,
    WaitingHuman,
};

struct WeldInspectionResult final {
    std::vector<ArtifactRef> output_artifacts;
    std::optional<ArtifactRef> weld_points;
    std::optional<ArtifactRef> prediction;
    std::uint64_t weld_point_count{};
    double weld_ratio{};
    double length_mm{};
    double inference_time_ms{};
    double total_time_ms{};
    std::string quality_assessment{"not_implemented"};
};

struct WeldingGuidanceResult final {
    std::vector<ArtifactRef> output_artifacts;
    RequestedWeldType weld_type{RequestedWeldType::Straight};
    std::string coordinate_frame;
    std::string unit{"mm"};
    std::optional<ApplicationPoint3> start;
    std::optional<ApplicationPoint3> end;
    std::optional<ApplicationPoint3> corner;
    std::optional<ApplicationPoint3> x_axis;
    std::optional<ApplicationPoint3> y_axis;
    std::optional<ApplicationPoint3> z_axis;
    std::optional<double> confidence;
    GuidanceResultDisposition disposition{GuidanceResultDisposition::Completed};
    std::optional<std::string> waiting_reason;
    bool robot_execution_allowed{false};
};

using ApplicationExecutionResult =
    std::variant<WeldInspectionResult, WeldingGuidanceResult>;

[[nodiscard]] std::string_view to_string(
    GuidanceResultDisposition disposition) noexcept;

/** Validates a result against its application and bounded domain contract. */
[[nodiscard]] Result<void> validate_execution_result(
    const ApplicationExecutionResult& result,
    IndustrialApplication application);

}  // namespace iaisf::application
