#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>
#include <mutex>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "iaisf/application/application_http_api.hpp"
#include "iaisf/application/application_process.hpp"

namespace iaisf::application {
namespace {

class FixedIdGenerator final : public IApplicationJobIdGenerator {
public:
    ApplicationJobIdGenerationResult generate(
        const IndustrialApplication) override {
        return ApplicationJobIdGenerationResult::success(id_);
    }

private:
    ApplicationJobId id_{ApplicationJobId::parse(
                              "wi_0123456789abcdef0123456789abcdef")
                              .value()};
};

class FixedClock final : public IApplicationJobClock {
public:
    Result<ApplicationJobTimePoint> now() const override {
        return Result<ApplicationJobTimePoint>::success(
            ApplicationJobTimePoint{std::chrono::seconds{100}});
    }
};

class BlockingPtv2Runner final : public IProcessRunner {
public:
    Result<ProcessResult> run(const ProcessSpec& spec) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            started_ = true;
        }
        started_condition_.notify_all();
        {
            std::unique_lock<std::mutex> lock(mutex_);
            release_condition_.wait(lock, [this] { return released_; });
        }
        const auto output = argument_after(spec, "--output");
        std::error_code error;
        std::filesystem::create_directories(output, error);
        if (error) {
            return Result<ProcessResult>::failure(make_error(
                ErrorCode::InternalError, "fixture output directory failed"));
        }
        std::ofstream result(output / "weld_result.json");
        result << R"({"weld_points":1,"weld_ratio":0.5,"length_mm":12.0,"inference_ms":3.0})";
        std::ofstream points(output / "weld_points.ply");
        points << "ply\n";
        std::ofstream prediction(output / "prediction.txt");
        prediction << "0\n";
        {
            std::lock_guard<std::mutex> lock(mutex_);
            finished_ = true;
        }
        finished_condition_.notify_all();
        return Result<ProcessResult>::success(ProcessResult{0, false, 1.0, {}, {}});
    }

    bool wait_started() {
        std::unique_lock<std::mutex> lock(mutex_);
        return started_condition_.wait_for(
            lock, std::chrono::seconds{2}, [this] { return started_; });
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        release_condition_.notify_all();
    }

    bool wait_finished() {
        std::unique_lock<std::mutex> lock(mutex_);
        return finished_condition_.wait_for(
            lock, std::chrono::seconds{2}, [this] { return finished_; });
    }

private:
    static std::filesystem::path argument_after(
        const ProcessSpec& spec, const std::string& key) {
        for (std::size_t i = 0U; i + 1U < spec.arguments.size(); ++i) {
            if (spec.arguments[i] == key) {
                return spec.arguments[i + 1U];
            }
        }
        return {};
    }

    std::mutex mutex_;
    std::condition_variable started_condition_;
    std::condition_variable release_condition_;
    std::condition_variable finished_condition_;
    bool started_{false};
    bool released_{false};
    bool finished_{false};
};

struct Fixture final {
    Fixture()
        : root(std::filesystem::temp_directory_path() /
               "iaisf-application-http-api-fixture"),
          artifacts(root / "artifacts"),
          scratch(root / "scratch"),
          outputs(root / "outputs") {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(
            artifacts / "inputs" / "input-1");
        std::filesystem::create_directories(scratch);
        std::filesystem::create_directories(outputs);
        std::ofstream cloud(
            artifacts / "inputs" / "input-1" / "pointcloud.xyzf32le",
            std::ios::binary);
        const unsigned char zeros[12]{};
        cloud.write(reinterpret_cast<const char*>(zeros), sizeof(zeros));
        std::ofstream manifest(
            artifacts / "inputs" / "input-1" / "artifact.json");
        manifest << R"({"artifact_id":"input-1","sha256":"15ec7bf0b50732b49f8228e07d24365338f9e3ab994b00af08e5a3bffe55fd8b","size_bytes":12,"kind":"point_cloud","media_type":"application/vnd.iaisf.pointcloud.xyz-f32le","coordinate_frame":"camera","unit":"mm","point_count":1})";
    }

    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    std::filesystem::path root;
    std::filesystem::path artifacts;
    std::filesystem::path scratch;
    std::filesystem::path outputs;
};

http::HttpRequest request(
    std::string method, std::string target, std::string body = {}) {
    auto result = http::HttpRequest::create(
        std::move(method), std::move(target),
        {http::HttpHeader{"content-type", "application/json"}},
        std::move(body), true);
    return std::move(result).value();
}

TEST(ApplicationHttpApiTest, SubmitStatusAndResultUseIndependentApplicationRoute) {
    Fixture fixture;
    auto resolver = LocalArtifactResolver::make(fixture.artifacts);
    ASSERT_TRUE(resolver);
    BlockingPtv2Runner runner;
    auto adapter = Ptv2WeldInspectionAdapter::create(
        {"ptv2", "engine.plan", "plugin.so", {}, fixture.scratch,
         fixture.outputs, std::chrono::seconds{5}, 1024U, 1024U, 4096U},
        *resolver.value(), runner);
    ASSERT_TRUE(adapter);
    auto repository = InMemoryApplicationJobRepository::make(8U);
    ASSERT_TRUE(repository);
    FixedClock clock;
    auto executor = ApplicationExecutor::create(
        *repository.value(), adapter.value().get(), nullptr, clock, 4U);
    ASSERT_TRUE(executor);
    FixedIdGenerator ids;
    auto api = ApplicationHttpApi::create(
        *repository.value(), *executor.value(), ids, clock,
        http::HttpLimits::defaults());
    ASSERT_TRUE(api);
    http::HttpRouter router;
    ASSERT_TRUE(api.value()->register_routes(router));
    ASSERT_TRUE(router.freeze());

    const auto body =
        R"({"schema_version":"1.0","input_artifacts":[{"artifact_id":"input-1","sha256":"15ec7bf0b50732b49f8228e07d24365338f9e3ab994b00af08e5a3bffe55fd8b","size_bytes":12,"kind":"point_cloud","media_type":"application/vnd.iaisf.pointcloud.xyz-f32le","coordinate_frame":"camera","unit":"mm","point_count":1}],"requested_outputs":["segmentation"]})";
    auto submitted = router.dispatch(request(
        "POST", "/api/weld-inspection/v1/jobs", body));
    ASSERT_TRUE(submitted);
    EXPECT_EQ(submitted.value().status(), http::HttpStatus::Accepted);
    const auto envelope = nlohmann::json::parse(submitted.value().body());
    ASSERT_EQ(envelope.at("job_id"), "wi_0123456789abcdef0123456789abcdef");

    ASSERT_TRUE(runner.wait_started());
    auto status = router.dispatch(request(
        "GET", "/api/weld-inspection/v1/jobs/wi_0123456789abcdef0123456789abcdef"));
    ASSERT_TRUE(status);
    EXPECT_EQ(status.value().status(), http::HttpStatus::Ok);
    const auto status_json = nlohmann::json::parse(status.value().body());
    ASSERT_TRUE(status_json.at("state").is_string());
    EXPECT_EQ(status_json.at("state").get<std::string>(), "running");

    runner.release();
    ASSERT_TRUE(runner.wait_finished());
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{2};
    for (;;) {
        auto snapshot = repository.value()->get(
            ApplicationJobId::parse("wi_0123456789abcdef0123456789abcdef").value(),
            IndustrialApplication::WeldInspection);
        if (snapshot && snapshot.value().state() == ApplicationJobState::Succeeded) {
            break;
        }
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        std::this_thread::yield();
    }
    auto result = router.dispatch(request(
        "GET", "/api/weld-inspection/v1/results/wi_0123456789abcdef0123456789abcdef"));
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().status(), http::HttpStatus::Ok);
    const auto result_json = nlohmann::json::parse(result.value().body());
    EXPECT_EQ(result_json.at("state"), "succeeded");
    EXPECT_EQ(result_json.at("quality_assessment"), "not_implemented");
    ASSERT_TRUE(executor.value()->shutdown());
}

}  // namespace
}  // namespace iaisf::application
