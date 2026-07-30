#include <chrono>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "iaisf/task/task_executor.hpp"

namespace {

using namespace std::chrono_literals;
using iaisf::ErrorCode;
using iaisf::ILogger;
using iaisf::LogLevel;
using iaisf::Result;
using iaisf::make_error;
using iaisf::task::TaskExecutor;
using iaisf::task::TaskId;
using iaisf::task::TaskLimits;
using iaisf::task::TaskRepository;
using iaisf::task::TaskRequest;
using iaisf::task::TaskState;

class TestLogger final : public ILogger {
public:
    void log(
        LogLevel,
        std::string_view,
        const std::string_view message) override {
        if (throw_on_log) {
            throw std::runtime_error{"logger failure"};
        }
        std::lock_guard<std::mutex> lock(mutex);
        messages.emplace_back(message);
    }

    bool throw_on_log{false};
    std::mutex mutex;
    std::vector<std::string> messages;
};

TaskId queued_id(TaskRepository& repository, const TaskRequest& request) {
    auto created = repository.create_queued(request);
    EXPECT_TRUE(created);
    return created ? created.value() : TaskId{};
}

TEST(TaskExecutorTest, StoresSuccessfulHandlerResult) {
    TaskRepository repository{TaskLimits::create().value()};
    TestLogger logger;
    const TaskRequest request{"echo", {{"input", 7}}};
    const TaskId id = queued_id(repository, request);
    TaskExecutor executor{
        repository,
        logger,
        [](const TaskRequest& value) {
            return Result<nlohmann::json>::success(value.input);
        }};

    executor.execute(id, request);

    auto snapshot = repository.get_snapshot(id);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().state, TaskState::Succeeded);
    EXPECT_EQ(snapshot.value().result.value(), request.input);
}

TEST(TaskExecutorTest, StoresExplicitHandlerFailure) {
    TaskRepository repository{TaskLimits::create().value()};
    TestLogger logger;
    const TaskRequest request{"fail", {}};
    const TaskId id = queued_id(repository, request);
    TaskExecutor executor{
        repository,
        logger,
        [](const TaskRequest&) {
            return Result<nlohmann::json>::failure(
                make_error(ErrorCode::InvalidArgument, "controlled failure"));
        }};

    executor.execute(id, request);

    auto snapshot = repository.get_snapshot(id);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().state, TaskState::Failed);
    ASSERT_TRUE(snapshot.value().error.has_value());
    EXPECT_EQ(snapshot.value().error->message, "controlled failure");
}

TEST(TaskExecutorTest, IsolatesHandlerExceptionWithoutLeakingItsText) {
    TaskRepository repository{TaskLimits::create().value()};
    TestLogger logger;
    const TaskRequest request{"throw", {}};
    const TaskId id = queued_id(repository, request);
    TaskExecutor executor{
        repository,
        logger,
        [](const TaskRequest&) -> Result<nlohmann::json> {
            throw std::runtime_error{"secret handler detail"};
        }};

    executor.execute(id, request);

    auto snapshot = repository.get_snapshot(id);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().state, TaskState::Failed);
    ASSERT_TRUE(snapshot.value().error.has_value());
    EXPECT_EQ(snapshot.value().error->message, "task handler failed");
    EXPECT_EQ(executor.handler_exception_count(), 1U);
}

TEST(TaskExecutorTest, IsolatesUnknownHandlerException) {
    TaskRepository repository{TaskLimits::create().value()};
    TestLogger logger;
    const TaskRequest request{"throw-unknown", {}};
    const TaskId id = queued_id(repository, request);
    TaskExecutor executor{
        repository,
        logger,
        [](const TaskRequest&) -> Result<nlohmann::json> { throw 19; }};

    EXPECT_NO_THROW(executor.execute(id, request));

    const auto snapshot = repository.get_snapshot(id);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().state, TaskState::Failed);
    EXPECT_EQ(executor.handler_exception_count(), 1U);
}

TEST(TaskExecutorTest, OversizedHandlerResultBecomesFailure) {
    TaskRepository repository{TaskLimits::create(4, 32, 64, 8, 32).value()};
    TestLogger logger;
    const TaskRequest request{"large", {}};
    const TaskId id = queued_id(repository, request);
    TaskExecutor executor{
        repository,
        logger,
        [](const TaskRequest&) {
            return Result<nlohmann::json>::success("0123456789");
        }};

    executor.execute(id, request);

    auto snapshot = repository.get_snapshot(id);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().state, TaskState::Failed);
}

TEST(TaskExecutorTest, InvalidUtf8HandlerResultBecomesFailure) {
    TaskRepository repository{TaskLimits::create().value()};
    TestLogger logger;
    const TaskRequest request{"invalid-json", {}};
    const TaskId id = queued_id(repository, request);
    const std::string invalid_utf8(1, static_cast<char>(0xFF));
    TaskExecutor executor{
        repository,
        logger,
        [invalid_utf8](const TaskRequest&) {
            return Result<nlohmann::json>::success(
                nlohmann::json{invalid_utf8});
        }};

    executor.execute(id, request);

    const auto snapshot = repository.get_snapshot(id);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().state, TaskState::Failed);
    ASSERT_TRUE(snapshot.value().error.has_value());
    EXPECT_EQ(
        snapshot.value().error->message,
        "task handler produced an invalid or oversized result");
}

TEST(TaskExecutorTest, OversizedHandlerErrorIsSanitizedAndFails) {
    TaskRepository repository{
        TaskLimits::create(4, 32, 64, 64, 4).value()};
    TestLogger logger;
    const TaskRequest request{"fail-large", {}};
    const TaskId id = queued_id(repository, request);
    TaskExecutor executor{
        repository,
        logger,
        [](const TaskRequest&) {
            return Result<nlohmann::json>::failure(
                make_error(ErrorCode::InternalError, "sensitive failure detail"));
        }};

    executor.execute(id, request);

    const auto snapshot = repository.get_snapshot(id);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().state, TaskState::Failed);
    ASSERT_TRUE(snapshot.value().error.has_value());
    EXPECT_EQ(snapshot.value().error->message, "####");
}

TEST(TaskExecutorTest, DiscardsCompletionThatArrivesAfterTimeout) {
    TaskRepository repository{TaskLimits::create().value()};
    TestLogger logger;
    const TaskRequest request{"slow", {}};
    const TaskId id = queued_id(repository, request);
    std::promise<void> started;
    auto started_future = started.get_future();
    std::promise<void> release;
    auto release_future = release.get_future().share();
    TaskExecutor executor{
        repository,
        logger,
        [&started, release_future](const TaskRequest&) {
            started.set_value();
            release_future.wait();
            return Result<nlohmann::json>::success({{"late", true}});
        }};
    std::thread worker{[&] { executor.execute(id, request); }};

    ASSERT_EQ(started_future.wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(repository.mark_timed_out(
        id, make_error(ErrorCode::InvalidState, "deadline")));
    release.set_value();
    worker.join();

    auto snapshot = repository.get_snapshot(id);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().state, TaskState::TimedOut);
    EXPECT_FALSE(snapshot.value().result.has_value());
    EXPECT_EQ(executor.late_completion_count(), 1U);
}

TEST(TaskExecutorTest, CountsLateSuccessAfterTimedOutRecordWasErased) {
    TaskRepository repository{TaskLimits::create().value()};
    TestLogger logger;
    const TaskRequest request{"slow", {}};
    const TaskId id = queued_id(repository, request);
    std::promise<void> started;
    auto started_future = started.get_future();
    std::promise<void> release;
    auto release_future = release.get_future().share();
    TaskExecutor executor{
        repository,
        logger,
        [&started, release_future](const TaskRequest& value) {
            if (value.operation == "slow") {
                started.set_value();
                release_future.wait();
            }
            return Result<nlohmann::json>::success({{"ok", true}});
        }};
    std::thread worker{[&] { executor.execute(id, request); }};

    ASSERT_EQ(started_future.wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(repository.mark_timed_out(id));
    ASSERT_TRUE(repository.erase_terminal(id));
    release.set_value();
    worker.join();

    EXPECT_EQ(repository.get_snapshot(id).error().code, ErrorCode::NotFound);
    EXPECT_EQ(executor.late_completion_count(), 1U);

    const TaskRequest next_request{"next", {}};
    const TaskId next_id = queued_id(repository, next_request);
    executor.execute(next_id, next_request);
    EXPECT_EQ(
        repository.get_snapshot(next_id).value().state,
        TaskState::Succeeded);
}

TEST(TaskExecutorTest, CountsLateFailureAfterTimedOutRecordWasErased) {
    TaskRepository repository{TaskLimits::create().value()};
    TestLogger logger;
    const TaskRequest request{"slow-failure", {}};
    const TaskId id = queued_id(repository, request);
    std::promise<void> started;
    auto started_future = started.get_future();
    std::promise<void> release;
    auto release_future = release.get_future().share();
    TaskExecutor executor{
        repository,
        logger,
        [&started, release_future](const TaskRequest&) {
            started.set_value();
            release_future.wait();
            return Result<nlohmann::json>::failure(
                make_error(ErrorCode::InternalError, "late failure"));
        }};
    std::thread worker{[&] { executor.execute(id, request); }};

    ASSERT_EQ(started_future.wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(repository.mark_timed_out(id));
    ASSERT_TRUE(repository.erase_terminal(id));
    release.set_value();
    worker.join();

    EXPECT_EQ(repository.get_snapshot(id).error().code, ErrorCode::NotFound);
    EXPECT_EQ(executor.late_completion_count(), 1U);
}

TEST(TaskExecutorTest, LoggerExceptionIsContainedAndObservable) {
    TaskRepository repository{TaskLimits::create().value()};
    TestLogger logger;
    logger.throw_on_log = true;
    const TaskRequest request{"throw", {}};
    const TaskId id = queued_id(repository, request);
    TaskExecutor executor{
        repository,
        logger,
        [](const TaskRequest&) -> Result<nlohmann::json> {
            throw std::runtime_error{"handler"};
        }};

    EXPECT_NO_THROW(executor.execute(id, request));
    EXPECT_GE(executor.logger_failure_count(), 1U);
    auto snapshot = repository.get_snapshot(id);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().state, TaskState::Failed);
}

TEST(TaskExecutorTest, SuccessTerminalWinsAgainstLaterTimeout) {
    TaskRepository repository{TaskLimits::create().value()};
    TestLogger logger;
    const TaskRequest request{"success", {}};
    const TaskId id = queued_id(repository, request);
    TaskExecutor executor{
        repository,
        logger,
        [](const TaskRequest&) {
            return Result<nlohmann::json>::success({{"ok", true}});
        }};

    executor.execute(id, request);
    const auto timeout = repository.mark_timed_out(id);

    ASSERT_TRUE(timeout);
    EXPECT_EQ(timeout.value(), iaisf::task::TransitionOutcome::AlreadyTerminal);
    EXPECT_EQ(repository.get_snapshot(id).value().state, TaskState::Succeeded);
}

TEST(TaskExecutorTest, FailedTerminalWinsAgainstLaterTimeout) {
    TaskRepository repository{TaskLimits::create().value()};
    TestLogger logger;
    const TaskRequest request{"failure", {}};
    const TaskId id = queued_id(repository, request);
    TaskExecutor executor{
        repository,
        logger,
        [](const TaskRequest&) {
            return Result<nlohmann::json>::failure(
                make_error(ErrorCode::InternalError, "expected failure"));
        }};

    executor.execute(id, request);
    const auto timeout = repository.mark_timed_out(id);

    ASSERT_TRUE(timeout);
    EXPECT_EQ(timeout.value(), iaisf::task::TransitionOutcome::AlreadyTerminal);
    EXPECT_EQ(repository.get_snapshot(id).value().state, TaskState::Failed);
}

TEST(TaskExecutorTest, HandlerRunsWithoutRepositoryMutexHeld) {
    TaskRepository repository{TaskLimits::create().value()};
    TestLogger logger;
    const TaskRequest request{"inspect", {}};
    const TaskId id = queued_id(repository, request);
    TaskExecutor executor{
        repository,
        logger,
        [&repository, id](const TaskRequest&) {
            const auto snapshot = repository.get_snapshot(id);
            if (!snapshot || snapshot.value().state != TaskState::Running) {
                return Result<nlohmann::json>::failure(
                    make_error(ErrorCode::InternalError, "repository unavailable"));
            }
            return Result<nlohmann::json>::success({{"unlocked", true}});
        }};

    executor.execute(id, request);

    EXPECT_EQ(repository.get_snapshot(id).value().state, TaskState::Succeeded);
}

}  // namespace
