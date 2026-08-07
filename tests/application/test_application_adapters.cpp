#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#include "iaisf/application/application_adapters.hpp"
#include "iaisf/application/application_artifacts.hpp"

namespace iaisf::application {
namespace {

struct Fixture final {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
                                  ("iaisf-mvp2-adapter-fixture-" +
                                   std::to_string(std::chrono::steady_clock::now()
                                                      .time_since_epoch().count()));
    std::filesystem::path artifacts = root / "artifacts";
    std::filesystem::path scratch = root / "scratch";
    std::filesystem::path outputs = root / "outputs";
    std::filesystem::path project = root / "weld-agent";

    Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(artifacts / "inputs" / "input-1");
        std::filesystem::create_directories(scratch);
        std::filesystem::create_directories(outputs);
        std::filesystem::create_directories(project);
        std::ofstream cloud(artifacts / "inputs" / "input-1" / "pointcloud.xyzf32le",
                            std::ios::binary);
        const unsigned char zeros[12]{};
        cloud.write(reinterpret_cast<const char*>(zeros), sizeof(zeros));
        cloud.close();
        nlohmann::json manifest{
            {"artifact_id", "input-1"},
            {"sha256", "15ec7bf0b50732b49f8228e07d24365338f9e3ab994b00af08e5a3bffe55fd8b"},
            {"size_bytes", 12U}, {"kind", "input"},
            {"media_type", "application/vnd.iaisf.pointcloud.xyz-f32le"},
            {"coordinate_frame", ""}, {"unit", ""}, {"point_count", 1U}};
        std::ofstream manifest_file(artifacts / "inputs" / "input-1" / "artifact.json");
        manifest_file << manifest.dump();
    }

    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    ArtifactRef artifact() const {
        return {"input-1",
                "15ec7bf0b50732b49f8228e07d24365338f9e3ab994b00af08e5a3bffe55fd8b",
                12U, "input", "application/vnd.iaisf.pointcloud.xyz-f32le",
                std::nullopt, std::nullopt, 1U};
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
    enum class Kind { Ptv2, Agent, Fail, Timeout, OutputLimit, Throw };
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
        if (kind_ == Kind::Ptv2) {
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
            result << R"({"weld_points":1,"weld_ratio":0.5,"length_mm":12.0,"inference_ms":3.0})";
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
            std::ofstream result(output / "final_result.json"); result << R"({"status":"success"})";
            std::ofstream state(intermediate / "agent_state.json");
            state << R"({"status":"completed","safety":{"manual_confirmation_required":false}})";
            std::ofstream feature(intermediate / "weld_feature.json");
            feature << R"({"coordinate":"fixture_frame","unit":"mm","start":[0,0,0],"end":[1,0,0],"corner":[0,1,0],"x_axis":[1,0,0],"y_axis":[0,1,0],"z_axis":[0,0,1],"confidence":0.9})";
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
    EXPECT_TRUE(runner.ptv2_cloud_is_four_column);
    EXPECT_EQ(runner.ptv2_cloud_rows, 1U);
    EXPECT_FALSE(std::filesystem::exists(fixture.scratch / "jobs" / "job-ptv2" / "input"));
    EXPECT_EQ(runner.last_.arguments[0], "--engine");
    EXPECT_EQ(runner.last_.arguments[2], "--plugin");
    EXPECT_EQ(runner.last_.arguments[4], "--cloud");
    EXPECT_NE(runner.last_.arguments[5].find("pointcloud.ptv2.txt"), std::string::npos);
}

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
    ASSERT_NE(std::find(runner.last_.arguments.begin(), runner.last_.arguments.end(), "l"),
              runner.last_.arguments.end());
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
