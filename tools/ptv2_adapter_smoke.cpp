#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "iaisf/application/application_adapters.hpp"
#include "iaisf/application/application_job.hpp"
#include "iaisf/application/application_submission.hpp"

namespace iaisf::application {
namespace {

std::optional<std::string> environment(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t length = 0U;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) return std::nullopt;
    std::string result{value, length == 0U ? 0U : length - 1U};
    std::free(value);
    return result;
#else
    const auto* value = std::getenv(name);
    if (value == nullptr) return std::nullopt;
    return std::string{value};
#endif
}

Result<std::filesystem::path> required_path(const char* name) {
    const auto value = environment(name);
    if (!value.has_value() || value->empty()) {
        return Result<std::filesystem::path>::failure(
            make_error(ErrorCode::InvalidArgument, "PTV2 smoke environment is incomplete"));
    }
    return Result<std::filesystem::path>::success(std::filesystem::path{*value});
}

Result<ArtifactRef> read_artifact(const std::filesystem::path& root,
                                  const std::string& artifact_id) {
    try {
        std::ifstream input(root / "inputs" / artifact_id / "artifact.json");
        if (!input) {
            return Result<ArtifactRef>::failure(
                make_error(ErrorCode::NotFound, "PTV2 smoke artifact manifest is unavailable"));
        }
        const auto json = nlohmann::json::parse(input);
        ArtifactRef artifact{
            json.at("artifact_id").get<std::string>(),
            json.at("sha256").get<std::string>(),
            json.at("size_bytes").get<std::uint64_t>(),
            json.at("kind").get<std::string>(),
            json.at("media_type").get<std::string>(),
            std::nullopt,
            std::nullopt,
            json.at("point_count").get<std::uint64_t>()};
        if (json.contains("coordinate_frame")) {
            artifact.coordinate_frame = json.at("coordinate_frame").get<std::string>();
        }
        if (json.contains("unit")) {
            artifact.unit = json.at("unit").get<std::string>();
        }
        const auto valid = validate_artifact_ref(artifact);
        if (!valid) return Result<ArtifactRef>::failure(valid.error());
        return Result<ArtifactRef>::success(std::move(artifact));
    } catch (const std::exception&) {
        return Result<ArtifactRef>::failure(
            make_error(ErrorCode::InvalidArgument, "PTV2 smoke artifact manifest is invalid"));
    }
}

}  // namespace
}  // namespace iaisf::application

int main() {
    using namespace iaisf::application;

    const auto executable = required_path("IAISF_PTV2_EXECUTABLE");
    const auto engine = required_path("IAISF_PTV2_ENGINE");
    const auto plugin = required_path("IAISF_PTV2_PLUGIN");
    const auto artifact_root = required_path("IAISF_PTV2_ARTIFACT_ROOT");
    const auto scratch_root = required_path("IAISF_PTV2_SCRATCH_ROOT");
    const auto output_root = required_path("IAISF_PTV2_OUTPUT_ROOT");
    const auto working_directory = required_path("IAISF_PTV2_WORKING_DIRECTORY");
    const auto artifact_id_value = environment("IAISF_PTV2_ARTIFACT_ID");
    if (!executable || !engine || !plugin || !artifact_root || !scratch_root ||
        !output_root || !working_directory || !artifact_id_value.has_value() ||
        artifact_id_value->empty()) {
        std::cerr << "PTV2 smoke requires configured local paths and artifact id\n";
        return 2;
    }

    auto resolver = LocalArtifactResolver::make(artifact_root.value());
    if (!resolver) {
        std::cerr << "PTV2 smoke resolver creation failed\n";
        return 1;
    }
    auto artifact = read_artifact(artifact_root.value(), *artifact_id_value);
    if (!artifact) {
        std::cerr << "PTV2 smoke artifact validation failed\n";
        return 1;
    }
    auto outputs = InspectionRequestedOutputs::create(true, true);
    if (!outputs) return 1;
    auto submission = WeldInspectionSubmission::create(outputs.value());
    if (!submission) return 1;
    auto spec = ApplicationSubmissionSpec::create(submission.value());
    if (!spec) return 1;
    auto id = ApplicationJobId::parse("mvp2-ptv2-real");
    if (!id) return 1;
    auto snapshot = ApplicationJobSnapshot::create(ApplicationJobCreateRequest{
        id.value(), IndustrialApplication::WeldInspection, ScenePhase::PostWeld,
        spec.value(), std::chrono::system_clock::now(), {artifact.value()}});
    if (!snapshot) return 1;
    auto runner = LocalProcessRunner::create();
    if (!runner) return 1;
    auto adapter = Ptv2WeldInspectionAdapter::create(
        {executable.value(), engine.value(), plugin.value(), working_directory.value(),
         scratch_root.value(), output_root.value(), std::chrono::minutes{5},
         4U * 1024U * 1024U, 4U * 1024U * 1024U, 256U * 1024U},
        *resolver.value(), *runner.value());
    if (!adapter) {
        std::cerr << "PTV2 smoke adapter creation failed\n";
        return 1;
    }
    const auto result = adapter.value()->execute(snapshot.value());
    if (!result) {
        std::cerr << "PTV2 adapter-mediated smoke failed\n";
        return 1;
    }
    const auto* inspection = std::get_if<WeldInspectionResult>(&result.value());
    if (inspection == nullptr || inspection->quality_assessment != "not_implemented" ||
        inspection->output_artifacts.size() != 3U) {
        std::cerr << "PTV2 adapter-mediated result validation failed\n";
        return 1;
    }
    std::cout << "ptv2_adapter_smoke exit=0 total_weld_points="
              << inspection->weld_point_count << " weld_ratio=" << inspection->weld_ratio
              << " length_mm=" << inspection->length_mm
              << " inference_ms=" << inspection->inference_time_ms
              << " artifacts=" << inspection->output_artifacts.size()
              << " quality_assessment=not_implemented\n";
    for (const auto& output : inspection->output_artifacts) {
        std::cout << "artifact=" << output.artifact_id << " size=" << output.size_bytes
                  << " sha256=" << output.sha256 << '\n';
    }
    return 0;
}
