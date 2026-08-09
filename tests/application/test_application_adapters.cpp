#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "iaisf/application/application_adapters.hpp"
#include "iaisf/application/application_artifacts.hpp"
#include "iaisf/application/local_artifact_catalog.hpp"

namespace iaisf::application {
namespace {

constexpr char kAgentExternalResult[] =
    R"({"status":"success","joint_values":[1,2,3,4,5,6],"JointValues":[6,5,4,3,2,1],"joint-values":[0],"tcp_pose":[1,2,3,4,5,6],"input_cloud":"E:/private/input.xyz","path":"E:/private/path","url":"https://private.invalid/result","command":"python private.py","stdout":"private stdout","stderr":"private stderr","visualization":{"html":"E:/private/result.html","unknown_object":{"secret":"value"}},"unknown_array":[{"future_field":"future value"}],"apparently_safe":"must not be copied","weld_points":[{"position":{"x":1,"y":2,"z":3},"joint_values":{"j1":1},"future_nested":[1,{"command-line":"private"}]}]})";

std::filesystem::path create_unique_adapter_fixture_directory() {
    static std::atomic<unsigned long> sequence{0};
    constexpr std::size_t kMaximumAttempts = 100U;
    const std::filesystem::path temp_root = std::filesystem::temp_directory_path();

    for (std::size_t attempt = 0; attempt < kMaximumAttempts; ++attempt) {
        const auto timestamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        const auto sequence_number =
            sequence.fetch_add(1, std::memory_order_relaxed);
        const std::filesystem::path candidate =
            temp_root /
            ("iaisf-mvp2-adapter-fixture-" + std::to_string(timestamp) + "-" +
             std::to_string(sequence_number) + "-" + std::to_string(attempt));

        std::error_code create_error;
        if (std::filesystem::create_directory(candidate, create_error)) {
            return candidate;
        }
        if (!create_error ||
            create_error == std::make_error_code(std::errc::file_exists)) {
            continue;
        }
        throw std::system_error{create_error,
                                "unable to create adapter fixture directory"};
    }

    throw std::runtime_error{"unable to allocate a unique adapter fixture directory"};
}

struct Fixture final {
    std::filesystem::path root = create_unique_adapter_fixture_directory();
    std::filesystem::path artifacts = root / "artifacts";
    std::filesystem::path scratch = root / "scratch";
    std::filesystem::path outputs = root / "outputs";
    std::filesystem::path project = root / "weld-agent";

    Fixture() {
        try {
            std::filesystem::create_directories(artifacts / "inputs" / "input-1");
            std::filesystem::create_directories(scratch);
            std::filesystem::create_directories(outputs);
            std::filesystem::create_directories(project);
            std::ofstream cloud(
                artifacts / "inputs" / "input-1" / "pointcloud.xyzf32le",
                std::ios::binary);
            const unsigned char zeros[12]{};
            cloud.write(reinterpret_cast<const char*>(zeros), sizeof(zeros));
            cloud.close();
            nlohmann::json manifest{
                {"artifact_id", "input-1"},
                {"sha256",
                 "15ec7bf0b50732b49f8228e07d24365338f9e3ab994b00af08e5a3bffe55fd8b"},
                {"size_bytes", 12U},
                {"kind", "input"},
                {"media_type", "application/vnd.iaisf.pointcloud.xyz-f32le"},
                {"coordinate_frame", "camera"},
                {"unit", "mm"},
                {"point_count", 1U}};
            std::ofstream manifest_file(artifacts / "inputs" / "input-1" /
                                        "artifact.json");
            manifest_file << manifest.dump();
        } catch (...) {
            std::error_code cleanup_error;
            std::filesystem::remove_all(root, cleanup_error);
            throw;
        }
    }

    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    ArtifactRef artifact() const {
        return {"input-1",
                "15ec7bf0b50732b49f8228e07d24365338f9e3ab994b00af08e5a3bffe55fd8b",
                12U, "input", "application/vnd.iaisf.pointcloud.xyz-f32le",
                std::string{"camera"}, std::string{"mm"}, 1U};
    }

    Result<ApplicationJobSnapshot> snapshot(const IndustrialApplication app,
                                            const ScenePhase phase,
                                            ApplicationSubmissionSpec submission,
                                            const std::string& id) const {
        auto job = ApplicationJobId::parse(id);
        if (!job) return Result<ApplicationJobSnapshot>::failure(job.error());
        return ApplicationJobSnapshot::create(ApplicationJobCreateRequest{
            job.value(), app, phase, std::move(submission),
            ApplicationJobTimePoint{}, {artifact()}});
    }
};

class FakeRunner final : public IProcessRunner {
public:
    enum class Kind { Ptv2, Ptv2InvalidCounts, Ptv2WrongTotal, Ptv2WeldExceedsTotal,
                      Ptv2ZeroWeld, Ptv2ZeroWeldMissingRatio,
                      Ptv2ZeroWeldNonzeroRatio, Ptv2ZeroWeldInvalidRatio,
                      Agent, AgentMinimal, Fail, Timeout, OutputLimit, Throw };
    explicit FakeRunner(Kind kind, std::filesystem::path project = {})
        : kind_(kind), project_(std::move(project)) {}

    Result<ProcessResult> run(const ProcessSpec& spec) override {
        last_ = spec;
        if (kind_ == Kind::Fail) {
            return Result<ProcessResult>::success(ProcessResult{3, false, 2.0, "", ""});
        }
        if (kind_ == Kind::Timeout) {
            return Result<ProcessResult>::success(ProcessResult{0, true, 5.0, "", ""});
        }
        if (kind_ == Kind::OutputLimit) {
            return Result<ProcessResult>::failure(make_error(
                ErrorCode::ResourceExhausted, "process output exceeded its limit"));
        }
        if (kind_ == Kind::Throw) {
            throw std::runtime_error("fixture runner failure");
        }
        if (kind_ == Kind::Ptv2 || kind_ == Kind::Ptv2InvalidCounts ||
            kind_ == Kind::Ptv2WrongTotal || kind_ == Kind::Ptv2WeldExceedsTotal ||
            kind_ == Kind::Ptv2ZeroWeld || kind_ == Kind::Ptv2ZeroWeldMissingRatio ||
            kind_ == Kind::Ptv2ZeroWeldNonzeroRatio || kind_ == Kind::Ptv2ZeroWeldInvalidRatio) {
            const auto cloud = argument_after(spec.arguments, "--cloud");
            std::ifstream cloud_input(cloud);
            std::string line;
            ptv2_cloud_is_four_column = true;
            while (std::getline(cloud_input, line)) {
                std::istringstream parser(line);
                double x{};
                double y{};
                double z{};
                std::string label;
                std::string extra;
                if (!(parser >> x >> y >> z >> label) || (parser >> extra) ||
                    !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
                    label != "0") {
                    ptv2_cloud_is_four_column = false;
                    break;
                }
                ++ptv2_cloud_rows;
            }
            if (!cloud_input.eof() || !ptv2_cloud_is_four_column) {
                return Result<ProcessResult>::success(ProcessResult{9, false, 2.0, "", ""});
            }
            const auto output = argument_after(spec.arguments, "--output");
            std::filesystem::create_directories(output);
            std::ofstream result(output / "weld_result.json");
            if (kind_ == Kind::Ptv2InvalidCounts) {
                result << R"({"total_points":2,"weld_points":3,"weld_ratio":1.5,"length_mm":12.0,"inference_ms":3.0})";
            } else if (kind_ == Kind::Ptv2WrongTotal) {
                result << R"({"total_points":2,"weld_points":1,"weld_ratio":0.5,"length_mm":12.0,"inference_ms":3.0})";
            } else if (kind_ == Kind::Ptv2WeldExceedsTotal) {
                result << R"({"total_points":1,"weld_points":2,"weld_ratio":1.0,"length_mm":12.0,"inference_ms":3.0})";
            } else if (kind_ == Kind::Ptv2ZeroWeld) {
                result << R"({"total_points":1,"weld_points":0,"weld_ratio":0.0,"length_mm":0.0,"inference_ms":3.0})";
            } else if (kind_ == Kind::Ptv2ZeroWeldMissingRatio) {
                result << R"({"total_points":1,"weld_points":0,"length_mm":0.0,"inference_ms":3.0})";
            } else if (kind_ == Kind::Ptv2ZeroWeldNonzeroRatio) {
                result << R"({"total_points":1,"weld_points":0,"weld_ratio":0.1,"length_mm":0.0,"inference_ms":3.0})";
            } else if (kind_ == Kind::Ptv2ZeroWeldInvalidRatio) {
                result << R"({"total_points":1,"weld_points":0,"weld_ratio":"invalid","length_mm":0.0,"inference_ms":3.0})";
            } else {
                result << R"({"total_points":1,"weld_points":1,"weld_ratio":0.5,"length_mm":12.0,"inference_ms":3.0})";
            }
            std::ofstream ply(output / "weld_points.ply"); ply << "ply\n";
            std::ofstream prediction(output / "prediction.txt"); prediction << "0\n";
        } else {
            const auto task = argument_after(spec.arguments, "--task-id");
            const auto cloud = argument_after(spec.arguments, "--cloud");
            std::ifstream cloud_input(cloud);
            std::string line;
            agent_cloud_is_three_column = true;
            while (std::getline(cloud_input, line)) {
                std::istringstream parser(line);
                double x{};
                double y{};
                double z{};
                std::string extra;
                if (!(parser >> x >> y >> z) || (parser >> extra) ||
                    !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
                    agent_cloud_is_three_column = false;
                    break;
                }
            }
            if (!cloud_input.eof() || !agent_cloud_is_three_column) {
                return Result<ProcessResult>::success(ProcessResult{9, false, 2.0, "", ""});
            }
            const auto output = project_ / "tasks" / task / "output";
            const auto intermediate = project_ / "tasks" / task / "intermediate";
            std::filesystem::create_directories(output);
            std::filesystem::create_directories(intermediate);
            std::ofstream result(output / "final_result.json");
            result << (kind_ == Kind::AgentMinimal
                           ? R"({"status":"success"})"
                           : kAgentExternalResult);
            std::ofstream state(intermediate / "agent_state.json");
            state << R"({"status":"completed","safety":{"manual_confirmation_required":false}})";
            std::ofstream feature(intermediate / "weld_feature.json");
            if (argument_after(spec.arguments, "--weld-type") == "straight") {
                feature << R"({"coordinate":"fixture_frame","unit":"mm","start":[0,0,0],"end":[1,0,0],"x_axis":[1,0,0],"y_axis":[0,1,0],"z_axis":[0,0,1],"confidence":0.9})";
            } else {
                feature << R"({"coordinate":"fixture_frame","unit":"mm","start":[0,0,0],"end":[1,0,0],"corner":[0,1,0],"x_axis":[1,0,0],"y_axis":[0,1,0],"z_axis":[0,0,1],"confidence":0.9})";
            }
        }
        return Result<ProcessResult>::success(ProcessResult{0, false, 2.0, "", ""});
    }

    ProcessSpec last_;
    bool ptv2_cloud_is_four_column{false};
    std::size_t ptv2_cloud_rows{0U};
    bool agent_cloud_is_three_column{false};

private:
    static std::filesystem::path argument_after(const std::vector<std::string>& args,
                                                const std::string& key) {
        for (std::size_t index = 0U; index + 1U < args.size(); ++index) {
            if (args[index] == key) return std::filesystem::path{args[index + 1U]};
        }
        return {};
    }

    Kind kind_;
    std::filesystem::path project_;
};

Result<ApplicationSubmissionSpec> inspection_submission() {
    auto outputs = InspectionRequestedOutputs::create(true, true);
    auto submission = WeldInspectionSubmission::create(outputs.value());
    return ApplicationSubmissionSpec::create(submission.value());
}

Result<ApplicationSubmissionSpec> guidance_submission(const WeldTypeMode mode,
                                                      const std::optional<RequestedWeldType> type) {
    auto request = WeldTypeRequest::create(mode, type);
    auto submission = WeldingGuidanceSubmission::create(request.value(),
                                                        HumanCheckpointPolicy::Required);
    return ApplicationSubmissionSpec::create(submission.value());
}

TEST(ApplicationAdapterTest, Ptv2ParsesFixtureAndRegistersArtifacts) {
    Fixture fixture;
    auto resolver = LocalArtifactResolver::make(fixture.artifacts);
    ASSERT_TRUE(resolver);
    FakeRunner runner(FakeRunner::Kind::Ptv2);
    auto adapter = Ptv2WeldInspectionAdapter::create(
        {"ptv2", "engine.plan", "plugin.so", {}, fixture.scratch, fixture.outputs,
         std::chrono::seconds{5}, 1024U, 1024U, 4096U}, *resolver.value(), runner);
    ASSERT_TRUE(adapter);
    auto snapshot = fixture.snapshot(IndustrialApplication::WeldInspection,
                                     ScenePhase::PostWeld, inspection_submission().value(), "job-ptv2");
    ASSERT_TRUE(snapshot);
    const auto result = adapter.value()->execute(snapshot.value());
    ASSERT_TRUE(result) << result.error().message;
    const auto& inspection = std::get<WeldInspectionResult>(result.value());
    EXPECT_EQ(inspection.weld_point_count, 1U);
    EXPECT_EQ(inspection.quality_assessment, "not_implemented");
    EXPECT_EQ(inspection.output_artifacts.size(), 3U);
    ASSERT_TRUE(inspection.weld_points.has_value());
    EXPECT_EQ(inspection.weld_points->coordinate_frame, std::optional<std::string>{"camera"});
    EXPECT_EQ(inspection.weld_points->unit, std::optional<std::string>{"mm"});
    EXPECT_EQ(inspection.weld_points->point_count, std::optional<std::uint64_t>{1U});
    ASSERT_TRUE(inspection.prediction.has_value());
    EXPECT_EQ(inspection.prediction->point_count, std::optional<std::uint64_t>{1U});
    EXPECT_TRUE(runner.ptv2_cloud_is_four_column);
    EXPECT_EQ(runner.ptv2_cloud_rows, 1U);
    EXPECT_FALSE(std::filesystem::exists(fixture.scratch / "jobs" / "job-ptv2" / "input"));
    EXPECT_EQ(runner.last_.arguments[0], "--engine");
    EXPECT_EQ(runner.last_.arguments[2], "--plugin");
    EXPECT_EQ(runner.last_.arguments[4], "--cloud");
    EXPECT_NE(runner.last_.arguments[5].find("pointcloud.ptv2.txt"), std::string::npos);
}

TEST(ApplicationAdapterTest, Ptv2RejectsInconsistentResultPointCounts) {
    Fixture fixture;
    auto resolver = LocalArtifactResolver::make(fixture.artifacts);
    ASSERT_TRUE(resolver);
    FakeRunner runner(FakeRunner::Kind::Ptv2InvalidCounts);
    auto adapter = Ptv2WeldInspectionAdapter::create(
        {"ptv2", "engine.plan", "plugin.so", {}, fixture.scratch, fixture.outputs,
         std::chrono::seconds{5}, 1024U, 1024U, 4096U}, *resolver.value(), runner);
    ASSERT_TRUE(adapter);
    auto snapshot = fixture.snapshot(IndustrialApplication::WeldInspection,
                                     ScenePhase::PostWeld, inspection_submission().value(), "job-invalid-counts");
    ASSERT_TRUE(snapshot);
    const auto result = adapter.value()->execute(snapshot.value());
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(ApplicationAdapterTest, Ptv2RejectsWrongTotalPointCount) {
    Fixture fixture;
    auto resolver = LocalArtifactResolver::make(fixture.artifacts);
    ASSERT_TRUE(resolver);
    FakeRunner runner(FakeRunner::Kind::Ptv2WrongTotal);
    auto adapter = Ptv2WeldInspectionAdapter::create(
        {"ptv2", "engine.plan", "plugin.so", {}, fixture.scratch, fixture.outputs,
         std::chrono::seconds{5}, 1024U, 1024U, 4096U}, *resolver.value(), runner);
    ASSERT_TRUE(adapter);
    auto snapshot = fixture.snapshot(IndustrialApplication::WeldInspection,
                                     ScenePhase::PostWeld, inspection_submission().value(), "job-wrong-total");
    ASSERT_TRUE(snapshot);
    const auto result = adapter.value()->execute(snapshot.value());
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(ApplicationAdapterTest, Ptv2RejectsWeldCountAboveTotal) {
    Fixture fixture;
    auto resolver = LocalArtifactResolver::make(fixture.artifacts);
    ASSERT_TRUE(resolver);
    FakeRunner runner(FakeRunner::Kind::Ptv2WeldExceedsTotal);
    auto adapter = Ptv2WeldInspectionAdapter::create(
        {"ptv2", "engine.plan", "plugin.so", {}, fixture.scratch, fixture.outputs,
         std::chrono::seconds{5}, 1024U, 1024U, 4096U}, *resolver.value(), runner);
    ASSERT_TRUE(adapter);
    auto snapshot = fixture.snapshot(IndustrialApplication::WeldInspection,
                                     ScenePhase::PostWeld, inspection_submission().value(), "job-weld-too-many");
    ASSERT_TRUE(snapshot);
    const auto result = adapter.value()->execute(snapshot.value());
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(ApplicationAdapterTest, Ptv2AllowsZeroWeldPoints) {
    Fixture fixture;
    auto resolver = LocalArtifactResolver::make(fixture.artifacts);
    ASSERT_TRUE(resolver);
    FakeRunner runner(FakeRunner::Kind::Ptv2ZeroWeld);
    auto adapter = Ptv2WeldInspectionAdapter::create(
        {"ptv2", "engine.plan", "plugin.so", {}, fixture.scratch, fixture.outputs,
         std::chrono::seconds{5}, 1024U, 1024U, 4096U}, *resolver.value(), runner);
    ASSERT_TRUE(adapter);
    auto snapshot = fixture.snapshot(IndustrialApplication::WeldInspection,
                                     ScenePhase::PostWeld, inspection_submission().value(), "job-zero-weld");
    ASSERT_TRUE(snapshot);
    const auto result = adapter.value()->execute(snapshot.value());
    ASSERT_TRUE(result) << result.error().message;
    const auto& inspection = std::get<WeldInspectionResult>(result.value());
    EXPECT_EQ(inspection.weld_point_count, 0U);
    EXPECT_DOUBLE_EQ(inspection.weld_ratio, 0.0);
    EXPECT_FALSE(inspection.weld_points.has_value());
    ASSERT_TRUE(inspection.prediction.has_value());
    EXPECT_EQ(inspection.output_artifacts.size(), 2U);
}

TEST(ApplicationAdapterTest, Ptv2RejectsZeroWeldWithoutRatio) {
    Fixture fixture;
    auto resolver = LocalArtifactResolver::make(fixture.artifacts);
    ASSERT_TRUE(resolver);
    FakeRunner runner(FakeRunner::Kind::Ptv2ZeroWeldMissingRatio);
    auto adapter = Ptv2WeldInspectionAdapter::create(
        {"ptv2", "engine.plan", "plugin.so", {}, fixture.scratch, fixture.outputs,
         std::chrono::seconds{5}, 1024U, 1024U, 4096U}, *resolver.value(), runner);
    ASSERT_TRUE(adapter);
    auto snapshot = fixture.snapshot(IndustrialApplication::WeldInspection,
                                     ScenePhase::PostWeld, inspection_submission().value(), "job-zero-missing-ratio");
    ASSERT_TRUE(snapshot);
    const auto result = adapter.value()->execute(snapshot.value());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(ApplicationAdapterTest, Ptv2RejectsZeroWeldWithNonzeroRatio) {
    Fixture fixture;
    auto resolver = LocalArtifactResolver::make(fixture.artifacts);
    ASSERT_TRUE(resolver);
    FakeRunner runner(FakeRunner::Kind::Ptv2ZeroWeldNonzeroRatio);
    auto adapter = Ptv2WeldInspectionAdapter::create(
        {"ptv2", "engine.plan", "plugin.so", {}, fixture.scratch, fixture.outputs,
         std::chrono::seconds{5}, 1024U, 1024U, 4096U}, *resolver.value(), runner);
    ASSERT_TRUE(adapter);
    auto snapshot = fixture.snapshot(IndustrialApplication::WeldInspection,
                                     ScenePhase::PostWeld, inspection_submission().value(), "job-zero-nonzero-ratio");
    ASSERT_TRUE(snapshot);
    const auto result = adapter.value()->execute(snapshot.value());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(ApplicationAdapterTest, Ptv2RejectsZeroWeldWithInvalidRatio) {
    Fixture fixture;
    auto resolver = LocalArtifactResolver::make(fixture.artifacts);
    ASSERT_TRUE(resolver);
    FakeRunner runner(FakeRunner::Kind::Ptv2ZeroWeldInvalidRatio);
    auto adapter = Ptv2WeldInspectionAdapter::create(
        {"ptv2", "engine.plan", "plugin.so", {}, fixture.scratch, fixture.outputs,
         std::chrono::seconds{5}, 1024U, 1024U, 4096U}, *resolver.value(), runner);
    ASSERT_TRUE(adapter);
    auto snapshot = fixture.snapshot(IndustrialApplication::WeldInspection,
                                     ScenePhase::PostWeld, inspection_submission().value(), "job-zero-invalid-ratio");
    ASSERT_TRUE(snapshot);
    const auto result = adapter.value()->execute(snapshot.value());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

#if !defined(_WIN32)
TEST(ApplicationAdapterTest, ConvertsKnownPtv2PathsForWslWindowsProcess) {
    Fixture fixture;
    auto resolver = LocalArtifactResolver::make(fixture.artifacts);
    ASSERT_TRUE(resolver);
    FakeRunner runner(FakeRunner::Kind::Fail);
    auto adapter = Ptv2WeldInspectionAdapter::create(
        {"/mnt/e/bin/weld_trt_demo.exe", "/mnt/e/models/engine.plan",
         "/mnt/e/models/plugin.dll", {}, fixture.scratch, fixture.outputs,
         std::chrono::seconds{5}, 1024U, 1024U, 4096U}, *resolver.value(), runner);
    ASSERT_TRUE(adapter);
    auto snapshot = fixture.snapshot(IndustrialApplication::WeldInspection,
                                     ScenePhase::PostWeld,
                                     inspection_submission().value(), "job-wsl-paths");
    ASSERT_TRUE(snapshot);
    EXPECT_FALSE(adapter.value()->execute(snapshot.value()));
    ASSERT_GE(runner.last_.arguments.size(), 8U);
    EXPECT_EQ(runner.last_.arguments[1], "E:\\models\\engine.plan");
    EXPECT_EQ(runner.last_.arguments[3], "E:\\models\\plugin.dll");
    EXPECT_NE(runner.last_.arguments[5].find("pointcloud.ptv2.txt"), std::string::npos);
    EXPECT_NE(runner.last_.arguments[7].find("ptv2"), std::string::npos);
}
#endif

TEST(ApplicationAdapterTest, GenericMaterializerRemainsThreeColumns) {
    Fixture fixture;
    auto resolver = LocalArtifactResolver::make(fixture.artifacts);
    ASSERT_TRUE(resolver);
    auto materializer = PointCloudTxtMaterializer::make(fixture.scratch);
    ASSERT_TRUE(materializer);
    const auto materialized = materializer.value()->materialize(
        *resolver.value(), fixture.artifact(), "job-three-column");
    ASSERT_TRUE(materialized);
    {
        std::ifstream input(materialized.value().text_path);
        std::string line;
        ASSERT_TRUE(std::getline(input, line));
        std::istringstream parser(line);
        double x{};
        double y{};
        double z{};
        std::string extra;
        EXPECT_TRUE(parser >> x >> y >> z);
        EXPECT_FALSE(parser >> extra);
        EXPECT_TRUE(std::isfinite(x) && std::isfinite(y) && std::isfinite(z));
    }
    EXPECT_TRUE(materializer.value()->cleanup("job-three-column"));
}

TEST(ApplicationAdapterTest, WeldAgentMapsRequestedTypeAndDoesNotEnableRobot) {
    Fixture fixture;
    auto resolver = LocalArtifactResolver::make(fixture.artifacts);
    ASSERT_TRUE(resolver);
    FakeRunner runner(FakeRunner::Kind::Agent, fixture.project);
    auto adapter = WeldAgentWeldingGuidanceAdapter::create(
        {"python", "agent_orchestrator.py", "tool.yaml", fixture.project, fixture.scratch,
         fixture.outputs, std::chrono::seconds{5}, 1024U, 1024U, 4096U}, *resolver.value(), runner);
    ASSERT_TRUE(adapter);
    auto snapshot = fixture.snapshot(IndustrialApplication::WeldingGuidance,
                                     ScenePhase::PreWeld,
                                     guidance_submission(WeldTypeMode::Requested,
                                                         RequestedWeldType::L).value(),
                                     "job-agent");
    ASSERT_TRUE(snapshot);
    const auto result = adapter.value()->execute(snapshot.value());
    ASSERT_TRUE(result);
    const auto& guidance = std::get<WeldingGuidanceResult>(result.value());
    EXPECT_EQ(guidance.weld_type, RequestedWeldType::L);
    EXPECT_EQ(guidance.disposition, GuidanceResultDisposition::WaitingHuman);
    EXPECT_FALSE(guidance.robot_execution_allowed);
    EXPECT_TRUE(runner.agent_cloud_is_three_column);
    EXPECT_TRUE(guidance.start.has_value());
    EXPECT_TRUE(guidance.end.has_value());
    EXPECT_TRUE(guidance.corner.has_value());
    EXPECT_TRUE(guidance.x_axis.has_value());
    EXPECT_TRUE(guidance.y_axis.has_value());
    EXPECT_TRUE(guidance.z_axis.has_value());
    ASSERT_TRUE(guidance.waiting_reason.has_value());
    EXPECT_FALSE(guidance.waiting_reason->empty());
    ASSERT_NE(std::find(runner.last_.arguments.begin(), runner.last_.arguments.end(), "l"),
              runner.last_.arguments.end());
    const auto external_result = fixture.project / "tasks" / "job-agent" /
                                 "output" / "final_result.json";
    const auto public_result = fixture.outputs / "jobs" / "job-agent" /
                               "weld_agent" / "final_result.json";
    const auto external_text = [&external_result] {
        std::ifstream input(external_result);
        return std::string{std::istreambuf_iterator<char>{input},
                           std::istreambuf_iterator<char>{}};
    }();
    const auto public_text = [&public_result] {
        std::ifstream input(public_result);
        return std::string{std::istreambuf_iterator<char>{input},
                           std::istreambuf_iterator<char>{}};
    }();
    EXPECT_EQ(external_text, kAgentExternalResult);
    const auto public_json = nlohmann::json::parse(public_text);
    ASSERT_TRUE(public_json.is_object());
    std::set<std::string> actual_keys;
    for (auto iterator = public_json.begin(); iterator != public_json.end(); ++iterator) {
        actual_keys.insert(iterator.key());
    }
    const std::set<std::string> expected_keys{
        "application", "confidence", "coordinate_frame", "corner", "disposition",
        "end", "job_id", "robot_execution_allowed", "schema_version", "start",
        "unit", "waiting_reason", "weld_type", "x_axis", "y_axis", "z_axis"};
    EXPECT_EQ(actual_keys, expected_keys);
    EXPECT_EQ(public_json.at("schema_version"), "1.0");
    EXPECT_EQ(public_json.at("job_id"), "job-agent");
    EXPECT_EQ(public_json.at("application"), "welding_guidance");
    EXPECT_EQ(public_json.at("weld_type"), "l");
    EXPECT_EQ(public_json.at("coordinate_frame"), "fixture_frame");
    EXPECT_EQ(public_json.at("unit"), "mm");
    EXPECT_EQ(public_json.at("start"), nlohmann::json::array({0.0, 0.0, 0.0}));
    EXPECT_EQ(public_json.at("end"), nlohmann::json::array({1.0, 0.0, 0.0}));
    EXPECT_EQ(public_json.at("corner"), nlohmann::json::array({0.0, 1.0, 0.0}));
    EXPECT_EQ(public_json.at("x_axis"), nlohmann::json::array({1.0, 0.0, 0.0}));
    EXPECT_EQ(public_json.at("y_axis"), nlohmann::json::array({0.0, 1.0, 0.0}));
    EXPECT_EQ(public_json.at("z_axis"), nlohmann::json::array({0.0, 0.0, 1.0}));
    EXPECT_DOUBLE_EQ(public_json.at("confidence").get<double>(), 0.9);
    EXPECT_EQ(public_json.at("disposition"), "waiting-human");
    EXPECT_TRUE(public_json.at("waiting_reason").is_string());
    EXPECT_EQ(public_json.at("robot_execution_allowed"), false);

    ASSERT_EQ(guidance.output_artifacts.size(), 1U);
    const auto& artifact = guidance.output_artifacts.front();
    EXPECT_EQ(artifact.artifact_id, "job-agent-agent-result");
    EXPECT_EQ(artifact.size_bytes, std::filesystem::file_size(public_result));
    const auto manifest_path = public_result.string() + ".artifact.json";
    std::ifstream manifest_input(manifest_path);
    const auto manifest = nlohmann::json::parse(manifest_input);
    EXPECT_EQ(manifest.at("sha256"), artifact.sha256);
    EXPECT_EQ(manifest.at("size_bytes"), artifact.size_bytes);
}

TEST(ApplicationAdapterTest, StraightPublicResultOmitsCornerAndUnknownFields) {
    Fixture fixture;
    auto resolver = LocalArtifactResolver::make(fixture.artifacts);
    ASSERT_TRUE(resolver);
    FakeRunner runner(FakeRunner::Kind::Agent, fixture.project);
    auto adapter = WeldAgentWeldingGuidanceAdapter::create(
        {"python", "agent_orchestrator.py", "tool.yaml", fixture.project,
         fixture.scratch, fixture.outputs, std::chrono::seconds{5}, 1024U,
         1024U, 4096U}, *resolver.value(), runner);
    ASSERT_TRUE(adapter);
    auto snapshot = fixture.snapshot(
        IndustrialApplication::WeldingGuidance, ScenePhase::PreWeld,
        guidance_submission(WeldTypeMode::Requested,
                            RequestedWeldType::Straight).value(),
        "job-agent-straight");
    ASSERT_TRUE(snapshot);
    const auto execution = adapter.value()->execute(snapshot.value());
    ASSERT_TRUE(execution) << execution.error().message;
    const auto& guidance = std::get<WeldingGuidanceResult>(execution.value());
    EXPECT_FALSE(guidance.corner.has_value());
    const auto public_result = fixture.outputs / "jobs" / "job-agent-straight" /
                               "weld_agent" / "final_result.json";
    std::ifstream input(public_result);
    const auto value = nlohmann::json::parse(input);
    EXPECT_FALSE(value.contains("corner"));
    EXPECT_EQ(value.at("weld_type"), "straight");
    EXPECT_EQ(value.at("job_id"), "job-agent-straight");
    EXPECT_FALSE(value.contains("joint_values"));
    EXPECT_FALSE(value.contains("JointValues"));
    EXPECT_FALSE(value.contains("joint-values"));
    EXPECT_FALSE(value.contains("tcp_pose"));
    EXPECT_FALSE(value.contains("weld_points"));
    EXPECT_FALSE(value.contains("apparently_safe"));
    EXPECT_FALSE(value.contains("unknown_array"));
}

TEST(ApplicationAdapterTest, PublicResultSizeLimitFailsWithoutOutputOrTemporary) {
    Fixture fixture;
    auto resolver = LocalArtifactResolver::make(fixture.artifacts);
    ASSERT_TRUE(resolver);
    FakeRunner runner(FakeRunner::Kind::AgentMinimal, fixture.project);
    auto adapter = WeldAgentWeldingGuidanceAdapter::create(
        {"python", "agent_orchestrator.py", "tool.yaml", fixture.project,
         fixture.scratch, fixture.outputs, std::chrono::seconds{5}, 1024U,
         1024U, 256U}, *resolver.value(), runner);
    ASSERT_TRUE(adapter);
    auto snapshot = fixture.snapshot(
        IndustrialApplication::WeldingGuidance, ScenePhase::PreWeld,
        guidance_submission(WeldTypeMode::Requested, RequestedWeldType::L).value(),
        "job-agent-oversize");
    ASSERT_TRUE(snapshot);
    const auto execution = adapter.value()->execute(snapshot.value());
    ASSERT_FALSE(execution);
    EXPECT_EQ(execution.error().code, ErrorCode::ResourceExhausted);
    const auto destination = fixture.outputs / "jobs" / "job-agent-oversize" /
                             "weld_agent" / "final_result.json";
    EXPECT_FALSE(std::filesystem::exists(destination));
    EXPECT_FALSE(std::filesystem::exists(destination.string() + ".tmp"));
    EXPECT_FALSE(std::filesystem::exists(destination.string() + ".artifact.json"));
}

TEST(ApplicationAdapterTest, ExistingPublicResultEntriesFailClosedWithoutOverwrite) {
    Fixture fixture;
    auto resolver = LocalArtifactResolver::make(fixture.artifacts);
    ASSERT_TRUE(resolver);

    const auto exercise = [&](const std::string& job, const bool temporary,
                              const bool directory, const bool symlink) {
        const auto destination = fixture.outputs / "jobs" / job / "weld_agent" /
                                 "final_result.json";
        const auto occupied = temporary
                                  ? std::filesystem::path{destination.string() + ".tmp"}
                                  : destination;
        std::filesystem::create_directories(occupied.parent_path());
        std::error_code error;
        if (symlink) {
            std::filesystem::create_symlink(
                occupied.parent_path() / "missing-target", occupied, error);
            ASSERT_FALSE(error) << error.message();
        } else if (directory) {
            std::filesystem::create_directory(occupied, error);
            ASSERT_FALSE(error) << error.message();
        } else {
            std::ofstream existing(occupied);
            existing << "existing";
        }
        FakeRunner runner(FakeRunner::Kind::Agent, fixture.project);
        auto adapter = WeldAgentWeldingGuidanceAdapter::create(
            {"python", "agent_orchestrator.py", "tool.yaml", fixture.project,
             fixture.scratch, fixture.outputs, std::chrono::seconds{5}, 1024U,
             1024U, 4096U}, *resolver.value(), runner);
        ASSERT_TRUE(adapter);
        auto snapshot = fixture.snapshot(
            IndustrialApplication::WeldingGuidance, ScenePhase::PreWeld,
            guidance_submission(WeldTypeMode::Requested, RequestedWeldType::L).value(),
            job);
        ASSERT_TRUE(snapshot);
        const auto execution = adapter.value()->execute(snapshot.value());
        ASSERT_FALSE(execution);
        EXPECT_EQ(execution.error().code, ErrorCode::InvalidState);
        const auto status = std::filesystem::symlink_status(occupied, error);
        ASSERT_FALSE(error);
        if (symlink) EXPECT_TRUE(std::filesystem::is_symlink(status));
        else if (directory) EXPECT_TRUE(std::filesystem::is_directory(status));
        else {
            std::ifstream existing(occupied);
            const std::string existing_text{
                std::istreambuf_iterator<char>{existing},
                std::istreambuf_iterator<char>{}};
            EXPECT_EQ(existing_text, "existing");
        }
    };

    exercise("job-destination-file", false, false, false);
    exercise("job-temporary-file", true, false, false);
    exercise("job-destination-directory", false, true, false);
    exercise("job-temporary-directory", true, true, false);
#if !defined(_WIN32)
    exercise("job-destination-symlink", false, false, true);
    exercise("job-temporary-symlink", true, false, true);
#endif
}

TEST(ApplicationAdapterTest, OutputDirectoryFailureDoesNotRegisterArtifact) {
    Fixture fixture;
    auto resolver = LocalArtifactResolver::make(fixture.artifacts);
    ASSERT_TRUE(resolver);
    auto catalog = LocalArtifactCatalog::make(fixture.artifacts, fixture.outputs, 16U);
    ASSERT_TRUE(catalog);
    std::filesystem::create_directories(fixture.outputs / "jobs");
    std::ofstream blocker(fixture.outputs / "jobs" / "job-write-failure");
    blocker << "not a directory";
    blocker.close();
    FakeRunner runner(FakeRunner::Kind::Agent, fixture.project);
    auto adapter = WeldAgentWeldingGuidanceAdapter::create(
        {"python", "agent_orchestrator.py", "tool.yaml", fixture.project,
         fixture.scratch, fixture.outputs, std::chrono::seconds{5}, 1024U,
         1024U, 4096U}, *resolver.value(), runner, catalog.value());
    ASSERT_TRUE(adapter);
    auto snapshot = fixture.snapshot(
        IndustrialApplication::WeldingGuidance, ScenePhase::PreWeld,
        guidance_submission(WeldTypeMode::Requested, RequestedWeldType::L).value(),
        "job-write-failure");
    ASSERT_TRUE(snapshot);
    const auto execution = adapter.value()->execute(snapshot.value());
    ASSERT_FALSE(execution);
    EXPECT_EQ(execution.error().code, ErrorCode::IoError);
    const auto found = catalog.value()->find("job-write-failure-agent-result");
    ASSERT_TRUE(found);
    EXPECT_FALSE(found.value().has_value());
}

TEST(ApplicationAdapterTest, NonzeroExternalExitIsFailure) {
    Fixture fixture;
    auto resolver = LocalArtifactResolver::make(fixture.artifacts);
    ASSERT_TRUE(resolver);
    FakeRunner runner(FakeRunner::Kind::Fail);
    auto adapter = Ptv2WeldInspectionAdapter::create(
        {"ptv2", "engine", "plugin", {}, fixture.scratch, fixture.outputs,
         std::chrono::seconds{5}, 1024U, 1024U, 4096U}, *resolver.value(), runner);
    ASSERT_TRUE(adapter);
    auto snapshot = fixture.snapshot(IndustrialApplication::WeldInspection,
                                     ScenePhase::PostWeld, inspection_submission().value(), "job-fail");
    ASSERT_TRUE(snapshot);
    const auto result = adapter.value()->execute(snapshot.value());
    ASSERT_FALSE(result);
}

TEST(ApplicationAdapterTest, TimeoutAndOutputLimitAreFailures) {
    Fixture fixture;
    auto resolver = LocalArtifactResolver::make(fixture.artifacts);
    ASSERT_TRUE(resolver);
    auto snapshot = fixture.snapshot(IndustrialApplication::WeldInspection,
                                     ScenePhase::PostWeld, inspection_submission().value(), "job-limits");
    ASSERT_TRUE(snapshot);
    FakeRunner timeout_runner(FakeRunner::Kind::Timeout);
    auto timeout_adapter = Ptv2WeldInspectionAdapter::create(
        {"ptv2", "engine", "plugin", {}, fixture.scratch, fixture.outputs,
         std::chrono::seconds{5}, 1024U, 1024U, 4096U}, *resolver.value(), timeout_runner);
    ASSERT_TRUE(timeout_adapter);
    EXPECT_FALSE(timeout_adapter.value()->execute(snapshot.value()));

    FakeRunner output_runner(FakeRunner::Kind::OutputLimit);
    auto output_adapter = Ptv2WeldInspectionAdapter::create(
        {"ptv2", "engine", "plugin", {}, fixture.scratch, fixture.outputs,
         std::chrono::seconds{5}, 1024U, 1024U, 4096U}, *resolver.value(), output_runner);
    ASSERT_TRUE(output_adapter);
    EXPECT_FALSE(output_adapter.value()->execute(snapshot.value()));
}

TEST(ApplicationAdapterTest, NonFinitePointCloudIsRejectedAndScratchIsCleaned) {
    Fixture fixture;
    const auto directory = fixture.artifacts / "inputs" / "input-nan";
    std::filesystem::create_directories(directory);
    const unsigned char bytes[12] = {0x00U, 0x00U, 0xc0U, 0x7fU, 0x00U, 0x00U,
                                     0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};
    std::ofstream cloud(directory / "pointcloud.xyzf32le", std::ios::binary);
    cloud.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
    cloud.close();
    nlohmann::json manifest{
        {"artifact_id", "input-nan"},
        {"sha256", "6c25aae419b5300847ce9d47bd8cea4ab7f4c53e3764abda54a098cbcec2dc19"},
        {"size_bytes", 12U}, {"kind", "input"},
        {"media_type", "application/vnd.iaisf.pointcloud.xyz-f32le"},
        {"coordinate_frame", ""}, {"unit", ""}, {"point_count", 1U}};
    std::ofstream manifest_file(directory / "artifact.json");
    manifest_file << manifest.dump();

    auto resolver = LocalArtifactResolver::make(fixture.artifacts);
    ASSERT_TRUE(resolver);
    auto materializer = PointCloudTxtMaterializer::make(fixture.scratch);
    ASSERT_TRUE(materializer);
    ArtifactRef artifact{"input-nan",
                         "6c25aae419b5300847ce9d47bd8cea4ab7f4c53e3764abda54a098cbcec2dc19",
                         12U,
                         "input",
                         "application/vnd.iaisf.pointcloud.xyz-f32le",
                         std::nullopt,
                         std::nullopt,
                         1U};
    EXPECT_FALSE(materializer.value()->materialize(*resolver.value(), artifact, "job-nan"));
    EXPECT_FALSE(std::filesystem::exists(fixture.scratch / "jobs" / "job-nan"));
}

TEST(ApplicationAdapterTest, RunnerExceptionCleansPrivateInputs) {
    Fixture fixture;
    auto resolver = LocalArtifactResolver::make(fixture.artifacts);
    ASSERT_TRUE(resolver);
    FakeRunner runner(FakeRunner::Kind::Throw);
    auto adapter = Ptv2WeldInspectionAdapter::create(
        {"ptv2", "engine", "plugin", {}, fixture.scratch, fixture.outputs,
         std::chrono::seconds{5}, 1024U, 1024U, 4096U}, *resolver.value(), runner);
    ASSERT_TRUE(adapter);
    auto snapshot = fixture.snapshot(IndustrialApplication::WeldInspection,
                                     ScenePhase::PostWeld, inspection_submission().value(),
                                     "job-throw");
    ASSERT_TRUE(snapshot);
    EXPECT_FALSE(adapter.value()->execute(snapshot.value()));
    EXPECT_FALSE(std::filesystem::exists(fixture.scratch / "jobs" / "job-throw" / "input"));
}

}  // namespace
}  // namespace iaisf::application
