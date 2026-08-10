#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "iaisf/application/application_executor.hpp"

namespace iaisf::application {
namespace {

constexpr char kZeroCloudSha[] =
    "15ec7bf0b50732b49f8228e07d24365338f9e3ab994b00af08e5a3bffe55fd8b";

std::filesystem::path unique_executor_fixture_root() {
    static std::atomic<unsigned long> sequence{0};
    const auto root = std::filesystem::temp_directory_path() /
        ("iaisf-executor-clock-" +
         std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
    std::error_code error;
    if (!std::filesystem::create_directory(root, error) || error) {
        throw std::system_error{error, "unable to create executor fixture"};
    }
    return root;
}

struct ExecutorFixture final {
    ExecutorFixture()
        : root(unique_executor_fixture_root()), artifacts(root / "artifacts"),
          scratch(root / "scratch"), outputs(root / "outputs") {
        std::filesystem::create_directories(artifacts / "inputs" / "input-1");
        std::filesystem::create_directories(scratch);
        std::filesystem::create_directories(outputs);
        std::ofstream cloud(
            artifacts / "inputs" / "input-1" / "pointcloud.xyzf32le",
            std::ios::binary);
        const unsigned char zeros[12]{};
        cloud.write(reinterpret_cast<const char*>(zeros), sizeof(zeros));
        std::ofstream manifest(
            artifacts / "inputs" / "input-1" / "artifact.json");
        manifest << R"({"artifact_id":"input-1","sha256":"15ec7bf0b50732b49f8228e07d24365338f9e3ab994b00af08e5a3bffe55fd8b","size_bytes":12,"kind":"input","media_type":"application/vnd.iaisf.pointcloud.xyz-f32le","coordinate_frame":"camera","unit":"mm","point_count":1})";
    }

    ~ExecutorFixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    ArtifactRef artifact() const {
        return {"input-1", kZeroCloudSha, 12U, "input",
                "application/vnd.iaisf.pointcloud.xyz-f32le",
                std::string{"camera"}, std::string{"mm"}, 1U};
    }

    std::filesystem::path root;
    std::filesystem::path artifacts;
    std::filesystem::path scratch;
    std::filesystem::path outputs;
};

class ScriptedClock final : public IApplicationJobClock {
public:
    struct Sample final {
        bool failure;
        ApplicationJobTimePoint value;
    };

    explicit ScriptedClock(std::vector<Sample> samples)
        : samples_(std::move(samples)) {}

    Result<ApplicationJobTimePoint> now() const override {
        const auto index = next_.fetch_add(1, std::memory_order_relaxed);
        const auto& sample = samples_[index < samples_.size()
                                          ? index
                                          : samples_.size() - 1U];
        if (sample.failure) {
            return Result<ApplicationJobTimePoint>::failure(
                make_error(ErrorCode::InternalError, "scripted clock failure"));
        }
        return Result<ApplicationJobTimePoint>::success(sample.value);
    }

private:
    std::vector<Sample> samples_;
    mutable std::atomic<std::size_t> next_{0U};
};

class ExecutorRunner final : public IProcessRunner {
public:
    explicit ExecutorRunner(const bool fail) : fail_(fail) {}

    Result<ProcessResult> run(const ProcessSpec& spec) override {
        if (fail_) {
            return Result<ProcessResult>::success(
                ProcessResult{3, false, 1.0, {}, {}});
        }
        std::filesystem::path output;
        for (std::size_t i = 0U; i + 1U < spec.arguments.size(); ++i) {
            if (spec.arguments[i] == "--output") {
                output = spec.arguments[i + 1U];
                break;
            }
        }
        std::error_code error;
        std::filesystem::create_directories(output, error);
        if (error) {
            return Result<ProcessResult>::failure(
                make_error(ErrorCode::IoError, "executor fixture output failed"));
        }
        std::ofstream result(output / "weld_result.json");
        result << R"({"total_points":1,"weld_points":1,"weld_ratio":0.5,"length_mm":1.0,"inference_ms":1.0})";
        std::ofstream points(output / "weld_points.ply");
        points << "ply\n";
        std::ofstream prediction(output / "prediction.txt");
        prediction << "0\n";
        return Result<ProcessResult>::success(ProcessResult{0, false, 1.0, {}, {}});
    }

private:
    bool fail_;
};

class CompleteConflictRepository final : public IApplicationJobRepository {
public:
    explicit CompleteConflictRepository(
        std::unique_ptr<InMemoryApplicationJobRepository> inner,
        const bool terminalize_on_complete_failure = false,
        const bool fail_complete = true)
        : inner_(std::move(inner)),
          terminalize_on_complete_failure_(terminalize_on_complete_failure),
          fail_complete_(fail_complete) {}

    ApplicationRepositoryResult<ApplicationJobSnapshot> create(
        const ApplicationJobCreateRequest& request) override {
        return inner_->create(request);
    }
    ApplicationRepositoryResult<ApplicationJobSnapshot> get(
        const ApplicationJobId& id, const IndustrialApplication application) const override {
        return inner_->get(id, application);
    }
    ApplicationRepositoryResult<ApplicationJobSnapshot> transition(
        const ApplicationJobId& id, const IndustrialApplication application,
        const std::uint64_t version, const ApplicationJobState state,
        const ApplicationJobTimePoint updated_at) override {
        if (state == ApplicationJobState::Failed && conflict_once_) {
            conflict_once_ = false;
            ++failed_transition_calls_;
            return ApplicationRepositoryResult<ApplicationJobSnapshot>::failure({
                ApplicationRepositoryFailure::VersionConflict,
                make_error(ErrorCode::InvalidState, "scripted version conflict")});
        }
        if (state == ApplicationJobState::Failed) {
            ++failed_transition_calls_;
        }
        return inner_->transition(id, application, version, state, updated_at);
    }
    ApplicationRepositoryResult<ApplicationJobSnapshot> complete(
        const ApplicationJobId& id, const IndustrialApplication application,
        const std::uint64_t version, const ApplicationJobState target_state,
        ApplicationExecutionResult result,
        const ApplicationJobTimePoint updated_at) override {
        ++complete_calls_;
        if (!fail_complete_) {
            return inner_->complete(id, application, version, target_state,
                                    std::move(result), updated_at);
        }
        if (terminalize_on_complete_failure_) {
            const auto terminal = inner_->complete(
                id, application, version, target_state, std::move(result),
                updated_at);
            if (!terminal) {
                return terminal;
            }
        }
        return ApplicationRepositoryResult<ApplicationJobSnapshot>::failure({
            ApplicationRepositoryFailure::InvalidArgument,
            make_error(ErrorCode::InvalidArgument, "scripted completion failure")});
    }
    ApplicationRepositoryResult<ApplicationJobSnapshot> erase_terminal(
        const ApplicationJobId& id, const IndustrialApplication application,
        const std::uint64_t version) override {
        return inner_->erase_terminal(id, application, version);
    }
    std::size_t size() const override { return inner_->size(); }
    std::size_t capacity() const noexcept override { return inner_->capacity(); }

    [[nodiscard]] std::size_t complete_calls() const noexcept {
        return complete_calls_;
    }
    [[nodiscard]] bool conflict_consumed() const noexcept {
        return !conflict_once_;
    }
    [[nodiscard]] std::size_t failed_transition_calls() const noexcept {
        return failed_transition_calls_;
    }

private:
    std::unique_ptr<InMemoryApplicationJobRepository> inner_;
    bool conflict_once_{true};
    bool terminalize_on_complete_failure_{false};
    bool fail_complete_{true};
    std::size_t complete_calls_{0U};
    std::size_t failed_transition_calls_{0U};
};

Result<ApplicationSubmissionSpec> executor_inspection_submission() {
    auto outputs = InspectionRequestedOutputs::create(true, true);
    if (!outputs) return Result<ApplicationSubmissionSpec>::failure(outputs.error());
    auto submission = WeldInspectionSubmission::create(outputs.value());
    if (!submission) return Result<ApplicationSubmissionSpec>::failure(submission.error());
    return ApplicationSubmissionSpec::create(submission.value());
}

Result<ApplicationJobSnapshot> create_queued_executor_job(
    IApplicationJobRepository& repository, const ApplicationJobTimePoint timestamp,
    const std::string& id) {
    auto parsed = ApplicationJobId::parse(id);
    if (!parsed) return Result<ApplicationJobSnapshot>::failure(parsed.error());
    auto submission = executor_inspection_submission();
    if (!submission) return Result<ApplicationJobSnapshot>::failure(submission.error());
    auto created = repository.create(ApplicationJobCreateRequest{
        parsed.value(), IndustrialApplication::WeldInspection,
        ScenePhase::PostWeld, submission.value(), ApplicationJobTimePoint{},
        {ArtifactRef{"input-1", kZeroCloudSha, 12U, "input",
                     "application/vnd.iaisf.pointcloud.xyz-f32le",
                     std::string{"camera"}, std::string{"mm"}, 1U}}});
    if (!created) return Result<ApplicationJobSnapshot>::failure(created.error().detail);
    auto queued = repository.transition(
        parsed.value(), IndustrialApplication::WeldInspection,
        created.value().version(), ApplicationJobState::Queued, timestamp);
    if (!queued) {
        return Result<ApplicationJobSnapshot>::failure(queued.error().detail);
    }
    return Result<ApplicationJobSnapshot>::success(queued.value());
}

Result<ApplicationJobSnapshot> wait_for_executor_terminal(
    IApplicationJobRepository& repository, const ApplicationJobId& id) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{3};
    for (;;) {
        auto current = repository.get(id, IndustrialApplication::WeldInspection);
        if (current && is_terminal(current.value().state())) {
            return Result<ApplicationJobSnapshot>::success(current.value());
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            ADD_FAILURE() << "executor did not reach a terminal state";
            return Result<ApplicationJobSnapshot>::failure(
                make_error(ErrorCode::InternalError,
                           "executor terminal wait timed out"));
        }
        std::this_thread::yield();
    }
}

Result<std::unique_ptr<Ptv2WeldInspectionAdapter>> make_executor_adapter(
    ExecutorFixture& fixture, const LocalArtifactResolver& resolver,
    IProcessRunner& runner) {
    return Ptv2WeldInspectionAdapter::create(
        {"ptv2", "engine.plan", "plugin.so", {}, fixture.scratch,
         fixture.outputs, std::chrono::seconds{5}, 1024U, 1024U, 4096U},
        resolver, runner);
}

TEST(ApplicationTaskManagerTest, BoundedQueueDrainsAndStops) {
    auto manager = ApplicationTaskManager::create(2U);
    ASSERT_TRUE(manager);
    std::promise<void> done;
    auto future = done.get_future();
    std::atomic<int> count{0};
    ASSERT_TRUE(manager.value()->submit([&] {
        count.fetch_add(1, std::memory_order_relaxed);
        done.set_value();
    }));
    EXPECT_EQ(future.wait_for(std::chrono::seconds{2}),
              std::future_status::ready);
    ASSERT_TRUE(manager.value()->shutdown());
    EXPECT_TRUE(manager.value()->stopped());
    EXPECT_EQ(count.load(std::memory_order_relaxed), 1);
}

TEST(ApplicationTaskManagerTest, RejectsAfterAdmissionStops) {
    auto manager = ApplicationTaskManager::create(1U);
    ASSERT_TRUE(manager);
    manager.value()->stop_admission();
    auto result = manager.value()->submit([] {});
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidState);
    ASSERT_TRUE(manager.value()->shutdown());
}

TEST(ApplicationExecutorTest, CompletionRollbackUsesRunningTimestampAndSucceeds) {
    ExecutorFixture fixture;
    auto resolver = LocalArtifactResolver::make(fixture.artifacts);
    ASSERT_TRUE(resolver);
    ExecutorRunner runner(false);
    auto adapter = make_executor_adapter(fixture, *resolver.value(), runner);
    ASSERT_TRUE(adapter);
    auto inner = InMemoryApplicationJobRepository::make(4U);
    ASSERT_TRUE(inner);
    CompleteConflictRepository repository(std::move(inner).value(), false, false);
    const auto queued = create_queued_executor_job(
        repository, ApplicationJobTimePoint{std::chrono::seconds{100}},
        "wi_0123456789abcdef0123456789abcdef");
    ASSERT_TRUE(queued);
    ScriptedClock clock({
        {false, ApplicationJobTimePoint{std::chrono::seconds{200}}},
        {false, ApplicationJobTimePoint{std::chrono::seconds{300}}},
        {false, ApplicationJobTimePoint{std::chrono::seconds{250}}},
    });
    auto executor = ApplicationExecutor::create(
        repository, adapter.value().get(), nullptr, clock, 2U);
    ASSERT_TRUE(executor);
    ASSERT_TRUE(executor.value()->submit(queued.value().job_id()));
    auto finished = wait_for_executor_terminal(
        repository, queued.value().job_id());
    ASSERT_TRUE(finished);
    EXPECT_EQ(finished.value().state(), ApplicationJobState::Succeeded);
    EXPECT_EQ(finished.value().updated_at(),
              ApplicationJobTimePoint{std::chrono::seconds{300}});
    EXPECT_EQ(finished.value().version(), 5U);
    ASSERT_NE(finished.value().execution_result(), nullptr);
    const auto& execution = *finished.value().execution_result();
    ASSERT_TRUE(std::holds_alternative<WeldInspectionResult>(execution));
    const auto& inspection = std::get<WeldInspectionResult>(execution);
    EXPECT_EQ(inspection.weld_point_count, 1U);
    EXPECT_DOUBLE_EQ(inspection.weld_ratio, 0.5);
    EXPECT_DOUBLE_EQ(inspection.length_mm, 1.0);
    EXPECT_EQ(inspection.output_artifacts.size(), 3U);
    EXPECT_EQ(inspection.quality_assessment, "not_implemented");
    EXPECT_EQ(repository.failed_transition_calls(), 0U);
    ASSERT_TRUE(executor.value()->shutdown());
}

TEST(ApplicationExecutorTest, ClampsRegressingClockForFailureFallback) {
    ExecutorFixture fixture;
    auto resolver = LocalArtifactResolver::make(fixture.artifacts);
    ASSERT_TRUE(resolver);
    ExecutorRunner runner(true);
    auto adapter = make_executor_adapter(fixture, *resolver.value(), runner);
    ASSERT_TRUE(adapter);
    auto repository = InMemoryApplicationJobRepository::make(4U);
    ASSERT_TRUE(repository);
    const auto queued = create_queued_executor_job(
        *repository.value(), ApplicationJobTimePoint{std::chrono::seconds{300}},
        "wi_1123456789abcdef0123456789abcdef");
    ASSERT_TRUE(queued);
    ScriptedClock clock({
        {false, ApplicationJobTimePoint{std::chrono::seconds{90}}},
        {false, ApplicationJobTimePoint{std::chrono::seconds{150}}},
        {false, ApplicationJobTimePoint{std::chrono::seconds{240}}},
    });
    auto executor = ApplicationExecutor::create(
        *repository.value(), adapter.value().get(), nullptr, clock, 2U);
    ASSERT_TRUE(executor);
    ASSERT_TRUE(executor.value()->submit(queued.value().job_id()));
    auto finished = wait_for_executor_terminal(
        *repository.value(), queued.value().job_id());
    ASSERT_TRUE(finished);
    EXPECT_EQ(finished.value().state(), ApplicationJobState::Failed);
    EXPECT_EQ(finished.value().updated_at(),
              ApplicationJobTimePoint{std::chrono::seconds{300}});
    ASSERT_TRUE(executor.value()->shutdown());
}

TEST(ApplicationExecutorTest, ClockFailureStillTerminalizesRunningJob) {
    ExecutorFixture fixture;
    auto resolver = LocalArtifactResolver::make(fixture.artifacts);
    ASSERT_TRUE(resolver);
    ExecutorRunner runner(true);
    auto adapter = make_executor_adapter(fixture, *resolver.value(), runner);
    ASSERT_TRUE(adapter);
    auto repository = InMemoryApplicationJobRepository::make(4U);
    ASSERT_TRUE(repository);
    const auto queued = create_queued_executor_job(
        *repository.value(), ApplicationJobTimePoint{std::chrono::seconds{100}},
        "wi_2123456789abcdef0123456789abcdef");
    ASSERT_TRUE(queued);
    ScriptedClock clock({
        {false, ApplicationJobTimePoint{std::chrono::seconds{100}}},
        {false, ApplicationJobTimePoint{std::chrono::seconds{200}}},
        {true, {}},
    });
    auto executor = ApplicationExecutor::create(
        *repository.value(), adapter.value().get(), nullptr, clock, 2U);
    ASSERT_TRUE(executor);
    ASSERT_TRUE(executor.value()->submit(queued.value().job_id()));
    auto finished = wait_for_executor_terminal(
        *repository.value(), queued.value().job_id());
    ASSERT_TRUE(finished);
    EXPECT_EQ(finished.value().state(), ApplicationJobState::Failed);
    EXPECT_EQ(finished.value().updated_at(),
              ApplicationJobTimePoint{std::chrono::seconds{200}});
    ASSERT_TRUE(executor.value()->shutdown());
}

TEST(ApplicationExecutorTest, RetriesFailureTransitionOnceAfterVersionConflict) {
    ExecutorFixture fixture;
    auto resolver = LocalArtifactResolver::make(fixture.artifacts);
    ASSERT_TRUE(resolver);
    ExecutorRunner runner(false);
    auto adapter = make_executor_adapter(fixture, *resolver.value(), runner);
    ASSERT_TRUE(adapter);
    auto inner = InMemoryApplicationJobRepository::make(4U);
    ASSERT_TRUE(inner);
    CompleteConflictRepository repository(std::move(inner).value());
    const auto queued = create_queued_executor_job(
        repository, ApplicationJobTimePoint{std::chrono::seconds{100}},
        "wi_3123456789abcdef0123456789abcdef");
    ASSERT_TRUE(queued);
    ScriptedClock clock({
        {false, ApplicationJobTimePoint{std::chrono::seconds{100}}},
        {false, ApplicationJobTimePoint{std::chrono::seconds{200}}},
        {false, ApplicationJobTimePoint{std::chrono::seconds{300}}},
    });
    auto executor = ApplicationExecutor::create(
        repository, adapter.value().get(), nullptr, clock, 2U);
    ASSERT_TRUE(executor);
    ASSERT_TRUE(executor.value()->submit(queued.value().job_id()));
    auto finished = wait_for_executor_terminal(repository, queued.value().job_id());
    ASSERT_TRUE(finished);
    EXPECT_EQ(finished.value().state(), ApplicationJobState::Failed);
    EXPECT_EQ(repository.complete_calls(), 1U);
    EXPECT_TRUE(repository.conflict_consumed());
    EXPECT_EQ(repository.failed_transition_calls(), 2U);
    ASSERT_TRUE(executor.value()->shutdown());
}

TEST(ApplicationExecutorTest, DoesNotOverwriteConcurrentTerminalState) {
    ExecutorFixture fixture;
    auto resolver = LocalArtifactResolver::make(fixture.artifacts);
    ASSERT_TRUE(resolver);
    ExecutorRunner runner(false);
    auto adapter = make_executor_adapter(fixture, *resolver.value(), runner);
    ASSERT_TRUE(adapter);
    auto inner = InMemoryApplicationJobRepository::make(4U);
    ASSERT_TRUE(inner);
    CompleteConflictRepository repository(std::move(inner).value(), true);
    const auto queued = create_queued_executor_job(
        repository, ApplicationJobTimePoint{std::chrono::seconds{100}},
        "wi_4123456789abcdef0123456789abcdef");
    ASSERT_TRUE(queued);
    ScriptedClock clock({
        {false, ApplicationJobTimePoint{std::chrono::seconds{100}}},
        {false, ApplicationJobTimePoint{std::chrono::seconds{200}}},
        {false, ApplicationJobTimePoint{std::chrono::seconds{300}}},
    });
    auto executor = ApplicationExecutor::create(
        repository, adapter.value().get(), nullptr, clock, 2U);
    ASSERT_TRUE(executor);
    ASSERT_TRUE(executor.value()->submit(queued.value().job_id()));
    auto finished = wait_for_executor_terminal(repository, queued.value().job_id());
    ASSERT_TRUE(finished);
    EXPECT_EQ(finished.value().state(), ApplicationJobState::Succeeded);
    EXPECT_EQ(repository.complete_calls(), 1U);
    EXPECT_EQ(repository.failed_transition_calls(), 0U);
    ASSERT_TRUE(executor.value()->shutdown());
}

}  // namespace
}  // namespace iaisf::application
