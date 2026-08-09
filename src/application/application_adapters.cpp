#include "iaisf/application/application_adapters.hpp"
#include "iaisf/application/local_artifact_catalog.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <nlohmann/json.hpp>
#include <new>
#include <sstream>
#include <system_error>

#include "iaisf/core/error.hpp"

namespace iaisf::application {
namespace {

template <typename T>
Result<T> failure(ErrorCode code, const char* message) {
    return Result<T>::failure(make_error(code, message));
}

Result<nlohmann::json> read_json_file(const std::filesystem::path& path,
                                      const std::size_t maximum) {
    try {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error || std::filesystem::is_symlink(status) ||
            !std::filesystem::is_regular_file(status)) {
            return failure<nlohmann::json>(ErrorCode::NotFound,
                                           "adapter output file is unavailable");
        }
        const auto size = std::filesystem::file_size(path, error);
        if (error || size > maximum) {
            return failure<nlohmann::json>(ErrorCode::ResourceExhausted,
                                           "adapter JSON output exceeds its limit");
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) return failure<nlohmann::json>(ErrorCode::IoError,
                                                    "adapter JSON output cannot be opened");
        std::string text(static_cast<std::size_t>(size), '\0');
        input.read(text.data(), static_cast<std::streamsize>(text.size()));
        if (input.gcount() != static_cast<std::streamsize>(text.size())) {
            return failure<nlohmann::json>(ErrorCode::IoError,
                                           "adapter JSON output cannot be read");
        }
        return Result<nlohmann::json>::success(nlohmann::json::parse(text));
    } catch (const std::bad_alloc&) {
        return failure<nlohmann::json>(ErrorCode::ResourceExhausted,
                                       "adapter JSON output allocation failed");
    } catch (const nlohmann::json::exception&) {
        return failure<nlohmann::json>(ErrorCode::InvalidArgument,
                                       "adapter JSON output is invalid");
    } catch (const std::filesystem::filesystem_error&) {
        return failure<nlohmann::json>(ErrorCode::IoError,
                                       "adapter JSON output filesystem failure");
    }
}

Result<void> validate_snapshot_for(const ApplicationJobSnapshot& snapshot,
                                   const IndustrialApplication application,
                                   const ScenePhase phase) {
    const auto identity = validate_application_scene(application, phase);
    if (!identity) return identity;
    if (snapshot.application() != application || snapshot.scene_phase() != phase ||
        snapshot.input_artifacts().size() != 1U ||
        !snapshot.job_id().valid() ||
        !snapshot.submission().validate_for(application, phase)) {
        return failure<void>(ErrorCode::InvalidArgument,
                             "adapter snapshot is not valid for this application");
    }
    return Result<void>::success();
}

std::string path_text(const std::filesystem::path& path) {
    return path.u8string();
}

// WSL can start a Windows PE executable through execv, but that executable
// does not understand Linux's /mnt/<drive>/... spelling for its path
// arguments.  Convert only adapter-owned, known path arguments; ordinary
// request values never pass through this helper.
std::string windows_child_path_text(const std::filesystem::path& path) {
    const auto text = path_text(path);
#if !defined(_WIN32)
    if (text.size() >= 7U && text.rfind("/mnt/", 0U) == 0U &&
        ((text[5] >= 'a' && text[5] <= 'z') ||
         (text[5] >= 'A' && text[5] <= 'Z')) && text[6] == '/') {
        const std::filesystem::path lexical{text};
        for (const auto& component : lexical) {
            if (component == "..") {
                return text;
            }
        }
        std::string converted;
        converted.reserve(text.size() + 2U);
        converted.push_back(
            text[5] >= 'a' && text[5] <= 'z'
                ? static_cast<char>(text[5] - ('a' - 'A'))
                : text[5]);
        converted.append(":\\");
        for (std::size_t index = 7U; index < text.size(); ++index) {
            converted.push_back(text[index] == '/' ? '\\' : text[index]);
        }
        return converted;
    }
#endif
    return text;
}

std::optional<double> finite_number(const nlohmann::json& object,
                                    const char* key) {
    if (!object.contains(key) || !object.at(key).is_number()) return std::nullopt;
    const auto value = object.at(key).get<double>();
    return std::isfinite(value) ? std::optional<double>{value} : std::nullopt;
}

Result<ArtifactRef> register_required(LocalOutputArtifactRegistrar& registrar,
                                      const std::filesystem::path& path,
                                      std::string id,
                                      std::string kind,
                                      std::string media,
                                      std::optional<std::string> coordinate_frame = std::nullopt,
                                      std::optional<std::string> unit = std::nullopt,
                                      std::optional<std::uint64_t> point_count = std::nullopt) {
    return registrar.register_file(path, OutputArtifactSpec{
        std::move(id), std::move(kind), std::move(media),
        std::move(coordinate_frame), std::move(unit), point_count});
}

Result<bool> path_entry_exists(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error) {
        if (error == std::errc::no_such_file_or_directory) {
            return Result<bool>::success(false);
        }
        return failure<bool>(ErrorCode::IoError,
                             "controlled WeldAgent result path cannot be inspected");
    }
    return Result<bool>::success(status.type() != std::filesystem::file_type::not_found);
}

nlohmann::json point_json(const ApplicationPoint3& point) {
    return nlohmann::json::array({point.x, point.y, point.z});
}

nlohmann::json public_weld_agent_result_json(
    const ApplicationJobSnapshot& snapshot,
    const WeldingGuidanceResult& result) {
    nlohmann::json value{
        {"schema_version", "1.0"},
        {"job_id", std::string{snapshot.job_id().value()}},
        {"application", "welding_guidance"},
        {"weld_type", std::string{to_string(result.weld_type)}},
        {"coordinate_frame", result.coordinate_frame},
        {"unit", result.unit},
        {"disposition", std::string{to_string(result.disposition)}},
        {"robot_execution_allowed", false}};
    if (result.start) value["start"] = point_json(*result.start);
    if (result.end) value["end"] = point_json(*result.end);
    if (result.corner) value["corner"] = point_json(*result.corner);
    if (result.x_axis) value["x_axis"] = point_json(*result.x_axis);
    if (result.y_axis) value["y_axis"] = point_json(*result.y_axis);
    if (result.z_axis) value["z_axis"] = point_json(*result.z_axis);
    if (result.confidence) value["confidence"] = *result.confidence;
    if (result.waiting_reason) value["waiting_reason"] = *result.waiting_reason;
    return value;
}

Result<void> write_public_weld_agent_result(const nlohmann::json& value,
                                            const std::filesystem::path& destination,
                                            const std::size_t maximum) {
    auto temporary = destination;
    temporary += ".tmp";
    bool temporary_created = false;
    try {
        if (maximum == 0U) {
            return failure<void>(ErrorCode::ResourceExhausted,
                                 "public WeldAgent result limit is invalid");
        }
        const auto serialized = value.dump();
        if (serialized.size() > maximum) {
            return failure<void>(ErrorCode::ResourceExhausted,
                                 "public WeldAgent result exceeds its limit");
        }
        std::error_code error;
        std::filesystem::create_directories(destination.parent_path(), error);
        if (error) return failure<void>(ErrorCode::IoError,
                                        "controlled output directory cannot be created");
        const auto destination_exists = path_entry_exists(destination);
        if (!destination_exists) return Result<void>::failure(destination_exists.error());
        const auto temporary_exists = path_entry_exists(temporary);
        if (!temporary_exists) return Result<void>::failure(temporary_exists.error());
        if (destination_exists.value() || temporary_exists.value()) {
            return failure<void>(ErrorCode::InvalidState,
                                 "controlled WeldAgent result already exists");
        }
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            std::filesystem::remove(temporary, error);
            return failure<void>(ErrorCode::IoError,
                                 "controlled WeldAgent result cannot be opened");
        }
        temporary_created = true;
        output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
        output.flush();
        output.close();
        if (!output) {
            std::filesystem::remove(temporary, error);
            temporary_created = false;
            return failure<void>(ErrorCode::IoError,
                                 "controlled WeldAgent result cannot be written");
        }
        const auto temporary_status = std::filesystem::symlink_status(temporary, error);
        if (error || std::filesystem::is_symlink(temporary_status) ||
            !std::filesystem::is_regular_file(temporary_status)) {
            std::filesystem::remove(temporary, error);
            temporary_created = false;
            return failure<void>(ErrorCode::IoError,
                                 "controlled WeldAgent result temporary is invalid");
        }
        const auto written_size = std::filesystem::file_size(temporary, error);
        if (error || written_size != serialized.size() || written_size > maximum) {
            std::filesystem::remove(temporary, error);
            temporary_created = false;
            return failure<void>(ErrorCode::IoError,
                                 "controlled WeldAgent result size is invalid");
        }
        const auto destination_before_commit = path_entry_exists(destination);
        if (!destination_before_commit) {
            std::filesystem::remove(temporary, error);
            temporary_created = false;
            return Result<void>::failure(destination_before_commit.error());
        }
        if (destination_before_commit.value()) {
            std::filesystem::remove(temporary, error);
            temporary_created = false;
            return failure<void>(ErrorCode::InvalidState,
                                 "controlled WeldAgent result already exists");
        }
        std::filesystem::rename(temporary, destination, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            temporary_created = false;
            return failure<void>(ErrorCode::IoError,
                                 "controlled WeldAgent result cannot be committed");
        }
        temporary_created = false;
        return Result<void>::success();
    } catch (const std::filesystem::filesystem_error&) {
        if (temporary_created) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
        }
        return failure<void>(ErrorCode::IoError,
                             "controlled WeldAgent result filesystem failure");
    } catch (const nlohmann::json::exception&) {
        if (temporary_created) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
        }
        return failure<void>(ErrorCode::InternalError,
                             "WeldAgent public result cannot be serialized");
    } catch (const std::bad_alloc&) {
        if (temporary_created) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
        }
        return failure<void>(ErrorCode::ResourceExhausted,
                             "WeldAgent public result allocation failed");
    } catch (const std::exception&) {
        if (temporary_created) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
        }
        return failure<void>(ErrorCode::InternalError,
                             "WeldAgent public result generation failed");
    } catch (...) {
        if (temporary_created) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
        }
        return failure<void>(ErrorCode::InternalError,
                             "WeldAgent public result generation failed");
    }
}

// The generic materializer deliberately emits three columns.  The current
// PTV2 loader still requires a fourth loader-compatible label column, so the
// adapter creates a private, short-lived bridge file without changing the
// generic artifact contract.  The fixed zero is only a compatibility value;
// it is not ground truth and is not consumed as a model feature.
Result<std::filesystem::path> materialize_ptv2_cloud(
    const MaterializedPointCloud& materialized, const std::uint64_t expected_points) {
    try {
        std::error_code error;
        const auto source_status = std::filesystem::symlink_status(
            materialized.text_path, error);
        if (error || std::filesystem::is_symlink(source_status) ||
            !std::filesystem::is_regular_file(source_status)) {
            return failure<std::filesystem::path>(
                ErrorCode::InvalidArgument, "PTV2 materialized input is unavailable");
        }
        if (materialized.point_count != expected_points) {
            return failure<std::filesystem::path>(
                ErrorCode::InvalidArgument, "PTV2 point count metadata is inconsistent");
        }
        const auto parent = materialized.text_path.parent_path();
        const auto bridge = parent / "pointcloud.ptv2.txt";
        const auto temporary = bridge.string() + ".tmp";
        const bool bridge_exists = std::filesystem::exists(bridge, error);
        if (error) {
            return failure<std::filesystem::path>(
                ErrorCode::IoError, "PTV2 bridge input status cannot be checked");
        }
        if (bridge_exists) {
            return failure<std::filesystem::path>(
                ErrorCode::InvalidState, "PTV2 bridge input already exists");
        }
        error.clear();
        const bool temporary_exists = std::filesystem::exists(temporary, error);
        if (error) {
            return failure<std::filesystem::path>(
                ErrorCode::IoError, "PTV2 bridge temporary status cannot be checked");
        }
        if (temporary_exists) {
            return failure<std::filesystem::path>(
                ErrorCode::InvalidState, "PTV2 bridge temporary input already exists");
        }

        std::ifstream input(materialized.text_path);
        std::ofstream output(temporary, std::ios::trunc | std::ios::binary);
        if (!input || !output) {
            return failure<std::filesystem::path>(
                ErrorCode::IoError, "PTV2 bridge input cannot be opened");
        }
        output.imbue(std::locale::classic());
        std::string line;
        std::uint64_t rows = 0U;
        while (std::getline(input, line)) {
            std::istringstream parser(line);
            parser.imbue(std::locale::classic());
            double x{};
            double y{};
            double z{};
            std::string extra;
            if (!(parser >> x >> y >> z) || (parser >> extra) ||
                !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
                std::filesystem::remove(temporary, error);
                return failure<std::filesystem::path>(
                    ErrorCode::InvalidArgument,
                    "PTV2 bridge input must contain three finite coordinates per line");
            }
            if (rows == std::numeric_limits<std::uint64_t>::max()) {
                std::filesystem::remove(temporary, error);
                return failure<std::filesystem::path>(
                    ErrorCode::ResourceExhausted, "PTV2 bridge point count overflow");
            }
            output << std::setprecision(std::numeric_limits<double>::max_digits10)
                   << x << ' ' << y << ' ' << z << " 0\n";
            if (!output) {
                std::filesystem::remove(temporary, error);
                return failure<std::filesystem::path>(
                    ErrorCode::IoError, "PTV2 bridge input cannot be written");
            }
            ++rows;
        }
        if (!input.eof()) {
            std::filesystem::remove(temporary, error);
            return failure<std::filesystem::path>(
                ErrorCode::IoError, "PTV2 bridge input cannot be read");
        }
        if (rows != expected_points) {
            std::filesystem::remove(temporary, error);
            return failure<std::filesystem::path>(
                ErrorCode::InvalidArgument, "PTV2 bridge point count does not match artifact");
        }
        output.flush();
        output.close();
        if (!output) {
            std::filesystem::remove(temporary, error);
            return failure<std::filesystem::path>(
                ErrorCode::IoError, "PTV2 bridge input cannot be committed");
        }
        std::filesystem::rename(temporary, bridge, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            return failure<std::filesystem::path>(
                ErrorCode::IoError, "PTV2 bridge input cannot be committed");
        }
        return Result<std::filesystem::path>::success(bridge);
    } catch (const std::bad_alloc&) {
        return failure<std::filesystem::path>(
            ErrorCode::ResourceExhausted, "PTV2 bridge allocation failed");
    } catch (const std::filesystem::filesystem_error&) {
        return failure<std::filesystem::path>(
            ErrorCode::IoError, "PTV2 bridge filesystem failure");
    }
}

Result<RequestedWeldType> map_weld_type(const WeldTypeRequest& request) {
    if (request.mode() == WeldTypeMode::Auto) {
        return Result<RequestedWeldType>::success(RequestedWeldType::Straight);
    }
    if (!request.requested_type().has_value()) {
        return failure<RequestedWeldType>(ErrorCode::InvalidArgument,
                                          "requested weld type is missing");
    }
    return Result<RequestedWeldType>::success(*request.requested_type());
}

std::optional<ApplicationPoint3> point_field(const nlohmann::json& object,
                                             const char* key) {
    if (!object.contains(key) || !object.at(key).is_array() ||
        object.at(key).size() != 3U) return std::nullopt;
    const auto& array = object.at(key);
    if (!array[0].is_number() || !array[1].is_number() || !array[2].is_number()) {
        return std::nullopt;
    }
    ApplicationPoint3 point{array[0].get<double>(), array[1].get<double>(),
                            array[2].get<double>()};
    return std::isfinite(point.x) && std::isfinite(point.y) &&
                   std::isfinite(point.z)
               ? std::optional<ApplicationPoint3>{point}
               : std::nullopt;
}

}  // namespace

Result<std::unique_ptr<Ptv2WeldInspectionAdapter>>
Ptv2WeldInspectionAdapter::create(Ptv2AdapterOptions options,
                                   const LocalArtifactResolver& resolver,
                                   IProcessRunner& runner,
                                   std::shared_ptr<LocalArtifactCatalog> catalog) {
    if (options.executable.empty() || options.engine.empty() || options.plugin.empty() ||
        options.scratch_root.empty() || options.output_root.empty()) {
        return failure<std::unique_ptr<Ptv2WeldInspectionAdapter>>(
            ErrorCode::InvalidArgument, "PTV2 adapter options are incomplete");
    }
    auto materializer = PointCloudTxtMaterializer::make(options.scratch_root);
    if (!materializer) return Result<std::unique_ptr<Ptv2WeldInspectionAdapter>>::failure(
        materializer.error());
    auto registrar = LocalOutputArtifactRegistrar::make(options.output_root, catalog);
    if (!registrar) return Result<std::unique_ptr<Ptv2WeldInspectionAdapter>>::failure(
        registrar.error());
    try {
        return Result<std::unique_ptr<Ptv2WeldInspectionAdapter>>::success(
            std::unique_ptr<Ptv2WeldInspectionAdapter>{new Ptv2WeldInspectionAdapter{
                std::move(options), resolver, runner, std::move(materializer.value()),
                std::move(registrar.value())}});
    } catch (const std::bad_alloc&) {
        return failure<std::unique_ptr<Ptv2WeldInspectionAdapter>>(
            ErrorCode::ResourceExhausted, "PTV2 adapter allocation failed");
    }
}

Ptv2WeldInspectionAdapter::Ptv2WeldInspectionAdapter(
    Ptv2AdapterOptions options, const LocalArtifactResolver& resolver,
    IProcessRunner& runner, std::unique_ptr<PointCloudTxtMaterializer> materializer,
    std::unique_ptr<LocalOutputArtifactRegistrar> registrar)
    : options_(std::move(options)), resolver_(resolver), runner_(runner),
      materializer_(std::move(materializer)), registrar_(std::move(registrar)) {}

Result<ApplicationExecutionResult>
Ptv2WeldInspectionAdapter::execute(const ApplicationJobSnapshot& snapshot) {
    const auto valid = validate_snapshot_for(snapshot, IndustrialApplication::WeldInspection,
                                             ScenePhase::PostWeld);
    if (!valid) return Result<ApplicationExecutionResult>::failure(valid.error());
    if (snapshot.submission().inspection() == nullptr) {
        return failure<ApplicationExecutionResult>(ErrorCode::InvalidArgument,
                                                   "inspection submission is unavailable");
    }
    const std::string job{snapshot.job_id().value()};
    const auto materialized = materializer_->materialize(
        resolver_, snapshot.input_artifacts().front(), job);
    if (!materialized) return Result<ApplicationExecutionResult>::failure(materialized.error());
    const auto bridged = materialize_ptv2_cloud(
        materialized.value(), snapshot.input_artifacts().front().point_count.value());
    if (!bridged) {
        (void)materializer_->cleanup(job);
        return Result<ApplicationExecutionResult>::failure(bridged.error());
    }
    const auto output_dir = options_.output_root / "jobs" / job / "ptv2";
    try {
        std::error_code error;
        std::filesystem::create_directories(output_dir, error);
        if (error) {
            (void)materializer_->cleanup(job);
            return failure<ApplicationExecutionResult>(
                ErrorCode::IoError, "PTV2 output directory cannot be created");
        }
        ProcessSpec spec;
        spec.executable = options_.executable;
        spec.working_directory = options_.working_directory;
        spec.timeout = options_.timeout;
        spec.max_stdout_bytes = options_.max_stdout_bytes;
        spec.max_stderr_bytes = options_.max_stderr_bytes;
        spec.arguments = {"--engine", windows_child_path_text(options_.engine),
                          "--plugin", windows_child_path_text(options_.plugin),
                          "--cloud", windows_child_path_text(bridged.value()),
                          "--output", windows_child_path_text(output_dir)};
        const auto process = runner_.run(spec);
        (void)materializer_->cleanup(job);
        if (!process) return Result<ApplicationExecutionResult>::failure(process.error());
        if (process.value().timed_out) return failure<ApplicationExecutionResult>(
            ErrorCode::ResourceExhausted, "PTV2 process timed out");
        if (process.value().exit_code != 0) return failure<ApplicationExecutionResult>(
            ErrorCode::SystemError, "PTV2 process failed");
        const auto parsed = read_json_file(output_dir / "weld_result.json",
                                           options_.max_result_json_bytes);
        if (!parsed) return Result<ApplicationExecutionResult>::failure(parsed.error());
        const auto& json = parsed.value();
        if (!json.is_object() || !json.contains("total_points") ||
            !json.at("total_points").is_number_unsigned() ||
            !json.contains("weld_points") ||
            !json.at("weld_points").is_number_unsigned()) {
            return failure<ApplicationExecutionResult>(ErrorCode::InvalidArgument,
                                                       "PTV2 result schema is invalid");
        }
        const auto input_points = snapshot.input_artifacts().front().point_count;
        const auto total_points = json.at("total_points").get<std::uint64_t>();
        const auto weld_points = json.at("weld_points").get<std::uint64_t>();
        if (!input_points.has_value() || total_points != *input_points ||
            weld_points > total_points) {
            return failure<ApplicationExecutionResult>(
                ErrorCode::InvalidArgument, "PTV2 result point counts are invalid");
        }
        const auto weld_ratio = finite_number(json, "weld_ratio");
        if (!weld_ratio.has_value() || *weld_ratio < 0.0 || *weld_ratio > 1.0 ||
            (weld_points == 0U && *weld_ratio != 0.0)) {
            return failure<ApplicationExecutionResult>(
                ErrorCode::InvalidArgument, "PTV2 weld ratio is invalid");
        }
        WeldInspectionResult result;
        result.weld_point_count = weld_points;
        result.weld_ratio = *weld_ratio;
        result.length_mm = finite_number(json, "length_mm").value_or(-1.0);
        result.inference_time_ms = finite_number(json, "inference_ms").value_or(0.0);
        result.total_time_ms = finite_number(json, "total_ms").value_or(process.value().elapsed_ms);
        result.quality_assessment = "not_implemented";
        auto weld = register_required(*registrar_, output_dir / "weld_result.json",
                                      job + "-weld-result", "weld_result", "application/json");
        if (!weld) return Result<ApplicationExecutionResult>::failure(weld.error());
        result.output_artifacts.push_back(weld.value());
        const auto ply = output_dir / "weld_points.ply";
        if (weld_points > 0U && std::filesystem::exists(ply)) {
            auto ref = register_required(*registrar_, ply, job + "-weld-points", "weld_points",
                                         "application/vnd.iaisf.pointcloud.ply",
                                         snapshot.input_artifacts().front().coordinate_frame,
                                         snapshot.input_artifacts().front().unit,
                                         weld_points);
            if (!ref) return Result<ApplicationExecutionResult>::failure(ref.error());
            result.weld_points = ref.value(); result.output_artifacts.push_back(ref.value());
        }
        const auto prediction = output_dir / "prediction.txt";
        if (std::filesystem::exists(prediction)) {
            auto ref = register_required(*registrar_, prediction, job + "-prediction", "prediction",
                                         "text/plain",
                                         snapshot.input_artifacts().front().coordinate_frame,
                                         snapshot.input_artifacts().front().unit,
                                         total_points);
            if (!ref) return Result<ApplicationExecutionResult>::failure(ref.error());
            result.prediction = ref.value(); result.output_artifacts.push_back(ref.value());
        }
        const auto checked = validate_execution_result(result, IndustrialApplication::WeldInspection);
        if (!checked) return Result<ApplicationExecutionResult>::failure(checked.error());
        return Result<ApplicationExecutionResult>::success(std::move(result));
    } catch (const std::bad_alloc&) {
        (void)materializer_->cleanup(job);
        return failure<ApplicationExecutionResult>(ErrorCode::ResourceExhausted,
                                                   "PTV2 adapter allocation failed");
    } catch (const std::exception&) {
        (void)materializer_->cleanup(job);
        return failure<ApplicationExecutionResult>(ErrorCode::InternalError,
                                                   "PTV2 adapter failed");
    } catch (...) {
        (void)materializer_->cleanup(job);
        return failure<ApplicationExecutionResult>(ErrorCode::InternalError,
                                                   "PTV2 adapter failed");
    }
}

Result<std::unique_ptr<WeldAgentWeldingGuidanceAdapter>>
WeldAgentWeldingGuidanceAdapter::create(WeldAgentAdapterOptions options,
                                        const LocalArtifactResolver& resolver,
                                        IProcessRunner& runner,
                                        std::shared_ptr<LocalArtifactCatalog> catalog) {
    if (options.python_executable.empty() || options.orchestrator.empty() ||
        options.project_root.empty() || options.tool_config.empty() ||
        options.scratch_root.empty() || options.output_root.empty()) {
        return failure<std::unique_ptr<WeldAgentWeldingGuidanceAdapter>>(
            ErrorCode::InvalidArgument, "WeldAgent adapter options are incomplete");
    }
    auto materializer = PointCloudTxtMaterializer::make(options.scratch_root);
    if (!materializer) return Result<std::unique_ptr<WeldAgentWeldingGuidanceAdapter>>::failure(
        materializer.error());
    auto registrar = LocalOutputArtifactRegistrar::make(options.output_root, catalog);
    if (!registrar) return Result<std::unique_ptr<WeldAgentWeldingGuidanceAdapter>>::failure(
        registrar.error());
    try {
        return Result<std::unique_ptr<WeldAgentWeldingGuidanceAdapter>>::success(
            std::unique_ptr<WeldAgentWeldingGuidanceAdapter>{new WeldAgentWeldingGuidanceAdapter{
                std::move(options), resolver, runner, std::move(materializer.value()),
                std::move(registrar.value())}});
    } catch (const std::bad_alloc&) {
        return failure<std::unique_ptr<WeldAgentWeldingGuidanceAdapter>>(
            ErrorCode::ResourceExhausted, "WeldAgent adapter allocation failed");
    }
}

WeldAgentWeldingGuidanceAdapter::WeldAgentWeldingGuidanceAdapter(
    WeldAgentAdapterOptions options, const LocalArtifactResolver& resolver,
    IProcessRunner& runner, std::unique_ptr<PointCloudTxtMaterializer> materializer,
    std::unique_ptr<LocalOutputArtifactRegistrar> registrar)
    : options_(std::move(options)), resolver_(resolver), runner_(runner),
      materializer_(std::move(materializer)), registrar_(std::move(registrar)) {}

Result<ApplicationExecutionResult>
WeldAgentWeldingGuidanceAdapter::execute(const ApplicationJobSnapshot& snapshot) {
    const auto valid = validate_snapshot_for(snapshot, IndustrialApplication::WeldingGuidance,
                                             ScenePhase::PreWeld);
    if (!valid) return Result<ApplicationExecutionResult>::failure(valid.error());
    const auto* guidance = snapshot.submission().guidance();
    if (guidance == nullptr) return failure<ApplicationExecutionResult>(
        ErrorCode::InvalidArgument, "guidance submission is unavailable");
    const std::string job{snapshot.job_id().value()};
    const auto type = map_weld_type(guidance->weld_type());
    if (!type) return Result<ApplicationExecutionResult>::failure(type.error());
    const auto materialized = materializer_->materialize(
        resolver_, snapshot.input_artifacts().front(), job);
    if (!materialized) return Result<ApplicationExecutionResult>::failure(materialized.error());
    const auto external_task = options_.project_root / "tasks" / job;
    try {
        ProcessSpec spec;
        spec.executable = options_.python_executable;
        spec.working_directory = options_.project_root;
        spec.timeout = options_.timeout;
        spec.max_stdout_bytes = options_.max_stdout_bytes;
        spec.max_stderr_bytes = options_.max_stderr_bytes;
        const std::string weld_type_arg = guidance->weld_type().mode() == WeldTypeMode::Auto
                                              ? std::string{"auto"}
                                              : type.value() == RequestedWeldType::L
                                                    ? std::string{"L"}
                                                    : std::string{to_string(type.value())};
        spec.arguments = {windows_child_path_text(options_.orchestrator), "--mode", "pointcloud", "--task-id",
                          job, "--cloud", windows_child_path_text(materialized.value().text_path), "--weld-type",
                          weld_type_arg, "--tool-config",
                          windows_child_path_text(options_.tool_config), "--no-open-view"};
        const auto process = runner_.run(spec);
        (void)materializer_->cleanup(job);
        if (!process) return Result<ApplicationExecutionResult>::failure(process.error());
        if (process.value().timed_out) return failure<ApplicationExecutionResult>(
            ErrorCode::ResourceExhausted, "WeldAgent process timed out");
        if (process.value().exit_code != 0) return failure<ApplicationExecutionResult>(
            ErrorCode::SystemError, "WeldAgent process failed");
        const auto final_path = external_task / "output" / "final_result.json";
        const auto feature_path = external_task / "intermediate" / "weld_feature.json";
        const auto state_path = external_task / "intermediate" / "agent_state.json";
        const auto final_json = read_json_file(final_path, options_.max_json_bytes);
        const auto feature_json = read_json_file(feature_path, options_.max_json_bytes);
        const auto state_json = read_json_file(state_path, options_.max_json_bytes);
        if (!final_json || !feature_json || !state_json) {
            return failure<ApplicationExecutionResult>(ErrorCode::InvalidArgument,
                                                       "WeldAgent result files are incomplete");
        }
        const auto status = final_json.value().value("status", std::string{});
        if (status != "success" && status != "waiting_human") {
            return failure<ApplicationExecutionResult>(ErrorCode::InvalidArgument,
                                                       "WeldAgent result status is invalid");
        }
        if (!state_json.value().is_object()) {
            return failure<ApplicationExecutionResult>(ErrorCode::InvalidArgument,
                                                       "WeldAgent state schema is invalid");
        }
        const auto state_status = state_json.value().value("status", std::string{});
        if (state_status != "completed" && state_status != "waiting_human") {
            return failure<ApplicationExecutionResult>(ErrorCode::InvalidArgument,
                                                       "WeldAgent state status is invalid");
        }
        bool state_requires_human = false;
        if (state_json.value().contains("safety")) {
            const auto& safety = state_json.value().at("safety");
            if (!safety.is_object() ||
                (safety.contains("manual_confirmation_required") &&
                 !safety.at("manual_confirmation_required").is_boolean())) {
                return failure<ApplicationExecutionResult>(
                    ErrorCode::InvalidArgument, "WeldAgent safety schema is invalid");
            }
            state_requires_human = safety.value("manual_confirmation_required", false);
            if (state_requires_human && state_json.value().contains("last_decision")) {
                const auto& decision = state_json.value().at("last_decision");
                if (!decision.is_object() ||
                    !decision.contains("decision") ||
                    !decision.at("decision").is_string()) {
                    return failure<ApplicationExecutionResult>(
                        ErrorCode::InvalidArgument,
                        "WeldAgent safety decision schema is invalid");
                }
                const auto action = decision.at("decision").get<std::string>();
                if (action == "continue" || action == "continue_with_warning") {
                    // WeldAgent may require confirmation before a later robot-use
                    // workflow while explicitly allowing analysis to complete.
                    // NotRequired applies only to this analysis result; robot
                    // execution remains forbidden below.
                    state_requires_human = false;
                } else if (action != "stop") {
                    return failure<ApplicationExecutionResult>(
                        ErrorCode::InvalidArgument,
                        "WeldAgent safety decision is invalid");
                }
            }
        }
        WeldingGuidanceResult result;
        result.weld_type = type.value();
        result.coordinate_frame = feature_json.value().value("coordinate", std::string{});
        result.unit = feature_json.value().value("unit", std::string{"mm"});
        result.start = point_field(feature_json.value(), "start");
        result.end = point_field(feature_json.value(), "end");
        result.corner = point_field(feature_json.value(), "corner");
        result.x_axis = point_field(feature_json.value(), "x_axis");
        result.y_axis = point_field(feature_json.value(), "y_axis");
        result.z_axis = point_field(feature_json.value(), "z_axis");
        result.confidence = finite_number(feature_json.value(), "confidence");
        if (status == "waiting_human" || state_status == "waiting_human" ||
            state_requires_human ||
            guidance->human_checkpoint() == HumanCheckpointPolicy::Required) {
            result.disposition = GuidanceResultDisposition::WaitingHuman;
            result.waiting_reason = std::string{"human review is required"};
        }
        result.robot_execution_allowed = false;
        // The public validator requires at least one valid output artifact.  Use
        // an already-validated input ArtifactRef only in this transient copy so
        // the guidance payload is checked before any public file is generated.
        // The input ArtifactRef is never published as an output.
        auto validation_candidate = result;
        validation_candidate.output_artifacts.push_back(
            snapshot.input_artifacts().front());
        const auto checked_without_artifact = validate_execution_result(
            validation_candidate, IndustrialApplication::WeldingGuidance);
        if (!checked_without_artifact) {
            return Result<ApplicationExecutionResult>::failure(
                checked_without_artifact.error());
        }
        const auto controlled = options_.output_root / "jobs" / job / "weld_agent";
        const auto copied_final = controlled / "final_result.json";
        const auto public_json = public_weld_agent_result_json(snapshot, result);
        auto copied = write_public_weld_agent_result(
            public_json, copied_final, options_.max_json_bytes);
        if (!copied) return Result<ApplicationExecutionResult>::failure(copied.error());
        auto final_ref = register_required(*registrar_, copied_final,
                                           job + "-agent-result", "agent_result",
                                           "application/json");
        if (!final_ref) {
            std::error_code ignored;
            std::filesystem::remove(copied_final, ignored);
            return Result<ApplicationExecutionResult>::failure(final_ref.error());
        }
        result.output_artifacts.push_back(final_ref.value());
        const auto checked = validate_execution_result(
            result, IndustrialApplication::WeldingGuidance);
        if (!checked) return Result<ApplicationExecutionResult>::failure(checked.error());
        return Result<ApplicationExecutionResult>::success(std::move(result));
    } catch (const std::bad_alloc&) {
        (void)materializer_->cleanup(job);
        return failure<ApplicationExecutionResult>(ErrorCode::ResourceExhausted,
                                                   "WeldAgent adapter allocation failed");
    } catch (const std::exception&) {
        (void)materializer_->cleanup(job);
        return failure<ApplicationExecutionResult>(ErrorCode::InternalError,
                                                   "WeldAgent adapter failed");
    }
}

}  // namespace iaisf::application
