#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "iaisf/api/task_http_api.hpp"
#include "iaisf/api/task_snapshot_json.hpp"
#include "iaisf/http/builtin_routes.hpp"
#include "iaisf/plugin/echo_plugin.hpp"
#include "iaisf/plugin/mock_vision_plugin.hpp"
#include "iaisf/plugin/plugin_task_adapter.hpp"

namespace {

class NullLogger final : public iaisf::ILogger {
public:
    void log(
        iaisf::LogLevel,
        std::string_view,
        std::string_view) override {}
};

enum class TestPluginMode {
    ImmediateSuccess,
    ReturnedFailure,
    StandardException,
    UnknownException,
    ValidationResource,
};

class TestPlugin final : public iaisf::plugin::IAlgorithmPlugin {
public:
    TestPlugin(std::string operation, const TestPluginMode mode)
        : operation_(std::move(operation)), mode_(mode) {}

    iaisf::plugin::PluginMetadata metadata() const override {
        return iaisf::plugin::PluginMetadata{
            operation_,
            "Task API Test Plugin",
            "1.0.0",
            "Test-only task API lifecycle behavior.",
            true,
            {"test"}};
    }

    iaisf::Result<void> validate_input(
        const nlohmann::json& input) const override {
        if (mode_ == TestPluginMode::ValidationResource) {
            return iaisf::Result<void>::failure(iaisf::make_error(
                iaisf::ErrorCode::ResourceExhausted,
                "private validation capacity detail"));
        }
        if (!input.is_object() || !input.empty()) {
            return iaisf::Result<void>::failure(iaisf::make_error(
                iaisf::ErrorCode::InvalidArgument,
                "test input must be an empty object"));
        }
        return iaisf::Result<void>::success();
    }

    iaisf::Result<nlohmann::json> execute(
        const nlohmann::json&) const override {
        switch (mode_) {
            case TestPluginMode::ImmediateSuccess:
                return iaisf::Result<nlohmann::json>::success(
                    {{"completed", true}});
            case TestPluginMode::ReturnedFailure:
                return iaisf::Result<nlohmann::json>::failure(
                    iaisf::make_error(
                        iaisf::ErrorCode::InternalError,
                        "private returned failure"));
            case TestPluginMode::StandardException:
                throw std::runtime_error{"private standard exception"};
            case TestPluginMode::UnknownException:
                throw 17;
            case TestPluginMode::ValidationResource:
                break;
        }
        return iaisf::Result<nlohmann::json>::failure(iaisf::make_error(
            iaisf::ErrorCode::InternalError,
            "unexpected test plugin mode"));
    }

private:
    std::string operation_;
    TestPluginMode mode_;
};

struct ApiBlockingState {
    ApiBlockingState()
        : started_future(started.get_future().share()),
          release_future(release.get_future().share()) {}

    void release_once() {
        if (!released.exchange(true, std::memory_order_acq_rel)) {
            release.set_value();
        }
    }

    std::promise<void> started;
    std::shared_future<void> started_future;
    std::promise<void> release;
    std::shared_future<void> release_future;
    std::atomic<bool> signalled{false};
    std::atomic<bool> released{false};
};

class ApiBlockingPlugin final : public iaisf::plugin::IAlgorithmPlugin {
public:
    explicit ApiBlockingPlugin(std::shared_ptr<ApiBlockingState> state)
        : state_(std::move(state)) {}

    iaisf::plugin::PluginMetadata metadata() const override {
        return iaisf::plugin::PluginMetadata{
            "test.block",
            "Task API Blocking Plugin",
            "1.0.0",
            "Test-only deterministic queue gate.",
            true,
            {"test"}};
    }

    iaisf::Result<void> validate_input(
        const nlohmann::json& input) const override {
        if (!input.is_object() || !input.empty()) {
            return iaisf::Result<void>::failure(iaisf::make_error(
                iaisf::ErrorCode::InvalidArgument,
                "blocking input must be empty"));
        }
        return iaisf::Result<void>::success();
    }

    iaisf::Result<nlohmann::json> execute(
        const nlohmann::json&) const override {
        if (!state_->signalled.exchange(true, std::memory_order_acq_rel)) {
            state_->started.set_value();
        }
        state_->release_future.wait();
        return iaisf::Result<nlohmann::json>::success(
            {{"released", true}});
    }

private:
    std::shared_ptr<ApiBlockingState> state_;
};

iaisf::http::HttpRequest request(
    std::string method,
    std::string target,
    std::string body = {},
    std::string content_type = "application/json") {
    iaisf::http::HttpRequest::Headers headers{
        {"host", "localhost"}};
    if (!content_type.empty()) {
        headers.push_back({"content-type", std::move(content_type)});
    }
    return iaisf::http::HttpRequest::create(
               std::move(method),
               std::move(target),
               std::move(headers),
               std::move(body),
               true)
        .value();
}

nlohmann::json body_json(const iaisf::http::HttpResponse& response) {
    return nlohmann::json::parse(response.body());
}

class TaskHttpApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        task_limits_ = std::make_unique<iaisf::task::TaskLimits>(
            iaisf::task::TaskLimits::create(
                3,
                256,
                512 * 1024,
                512 * 1024,
                1024,
                64,
                100000,
                512 * 1024)
                .value());
        plugin_manager_ = std::make_shared<iaisf::plugin::PluginManager>(
            iaisf::plugin::PluginLimits::create(
                128,
                128,
                128,
                64,
                1024,
                1024,
                512 * 1024,
                512 * 1024,
                64,
                100000,
                512 * 1024,
                64,
                128)
                .value());
        ASSERT_TRUE(plugin_manager_->register_plugin(
            std::make_shared<iaisf::plugin::EchoPlugin>()));
        ASSERT_TRUE(plugin_manager_->register_plugin(
            std::make_shared<iaisf::plugin::MockVisionPlugin>()));
        ASSERT_TRUE(plugin_manager_->register_plugin(
            std::make_shared<TestPlugin>(
                "test.immediate",
                TestPluginMode::ImmediateSuccess)));
        ASSERT_TRUE(plugin_manager_->register_plugin(
            std::make_shared<TestPlugin>(
                "test.return_failure",
                TestPluginMode::ReturnedFailure)));
        ASSERT_TRUE(plugin_manager_->register_plugin(
            std::make_shared<TestPlugin>(
                "test.standard_exception",
                TestPluginMode::StandardException)));
        ASSERT_TRUE(plugin_manager_->register_plugin(
            std::make_shared<TestPlugin>(
                "test.unknown_exception",
                TestPluginMode::UnknownException)));
        ASSERT_TRUE(plugin_manager_->register_plugin(
            std::make_shared<TestPlugin>(
                "test.validation_resource",
                TestPluginMode::ValidationResource)));
        blocking_state_ = std::make_shared<ApiBlockingState>();
        ASSERT_TRUE(plugin_manager_->register_plugin(
            std::make_shared<ApiBlockingPlugin>(blocking_state_)));
        ASSERT_TRUE(plugin_manager_->freeze());
        adapter_ =
            iaisf::plugin::PluginTaskAdapter::create(plugin_manager_).value();
        task_manager_ = iaisf::task::TaskManager::create(
                            {1U, 1U},
                            *task_limits_,
                            logger_,
                            adapter_->make_validator(),
                            adapter_->make_handler())
                            .value();
        api_ = iaisf::api::TaskHttpApi::create(
                   *task_manager_,
                   *plugin_manager_,
                   *task_limits_,
                   iaisf::http::HttpLimits::defaults())
                   .value();
        router_ = std::make_unique<iaisf::http::HttpRouter>();
        ASSERT_TRUE(iaisf::http::register_builtin_routes(*router_));
        ASSERT_TRUE(api_->register_routes(*router_));
        ASSERT_TRUE(router_->freeze());
    }

    void TearDown() override {
        if (blocking_state_) {
            blocking_state_->release_once();
        }
        if (task_manager_) {
            EXPECT_TRUE(task_manager_->shutdown());
        }
        router_.reset();
        api_.reset();
        task_manager_.reset();
        adapter_.reset();
        plugin_manager_.reset();
    }

    iaisf::http::HttpResponse dispatch(
        std::string method,
        std::string target,
        std::string body = {},
        std::string content_type = "application/json") {
        return router_
            ->dispatch(request(
                std::move(method),
                std::move(target),
                std::move(body),
                std::move(content_type)))
            .value();
    }

    NullLogger logger_;
    std::unique_ptr<iaisf::task::TaskLimits> task_limits_;
    std::shared_ptr<iaisf::plugin::PluginManager> plugin_manager_;
    std::shared_ptr<iaisf::plugin::PluginTaskAdapter> adapter_;
    std::unique_ptr<iaisf::task::TaskManager> task_manager_;
    iaisf::api::TaskHttpApi::Ptr api_;
    std::unique_ptr<iaisf::http::HttpRouter> router_;
    std::shared_ptr<ApiBlockingState> blocking_state_;
};

TEST_F(TaskHttpApiTest, EchoSubmissionReturnsAcceptedSafeEnvelope) {
    const auto response = dispatch(
        "POST",
        "/v1/tasks",
        R"({"operation":"echo","input":{"payload":{"x":1}}})");
    ASSERT_EQ(response.status(), iaisf::http::HttpStatus::Accepted);
    const auto body = body_json(response);
    EXPECT_TRUE(body["task_id"].is_string());
    EXPECT_EQ(body.size(), 2U);
    EXPECT_FALSE(body.contains("state"));
    EXPECT_EQ(body["status_url"], "/v1/tasks/" + body["task_id"].get<std::string>());
    EXPECT_FALSE(body.contains("input"));
}

TEST_F(
    TaskHttpApiTest,
    AcceptedResponseDoesNotClaimStateWhenTaskCanFinishImmediately) {
    const auto response = dispatch(
        "POST",
        "/v1/tasks",
        R"({"operation":"test.immediate","input":{}})");
    ASSERT_EQ(response.status(), iaisf::http::HttpStatus::Accepted);
    const auto body = body_json(response);
    EXPECT_EQ(body.size(), 2U);
    EXPECT_FALSE(body.contains("state"));
    EXPECT_EQ(
        body["status_url"],
        "/v1/tasks/" + body["task_id"].get<std::string>());
    ASSERT_TRUE(task_manager_->shutdown());
    EXPECT_EQ(
        body_json(dispatch(
            "GET",
            body["status_url"].get<std::string>()))["state"],
        "succeeded");
}

TEST_F(TaskHttpApiTest, MockVisionSubmissionIsAccepted) {
    const auto response = dispatch(
        "POST",
        "/v1/tasks",
        R"({"operation":"mock_vision.detect","input":{"image_id":"a","width":20,"height":10}})");
    EXPECT_EQ(response.status(), iaisf::http::HttpStatus::Accepted);
}

TEST_F(TaskHttpApiTest, AcceptsCaseInsensitiveJsonMediaType) {
    EXPECT_EQ(
        dispatch(
            "POST",
            "/v1/tasks",
            R"({"operation":"echo","input":{"payload":1}})",
            "Application/JSON")
            .status(),
        iaisf::http::HttpStatus::Accepted);
}

TEST_F(TaskHttpApiTest, AcceptsUtf8CharsetWithOws) {
    EXPECT_EQ(
        dispatch(
            "POST",
            "/v1/tasks",
            R"({"operation":"echo","input":{"payload":1}})",
            " application/json ; charset = UTF-8 ")
            .status(),
        iaisf::http::HttpStatus::Accepted);
}

TEST_F(TaskHttpApiTest, RejectsMissingContentType) {
    EXPECT_EQ(
        dispatch(
            "POST",
            "/v1/tasks",
            R"({"operation":"echo","input":{"payload":1}})",
            "")
            .status(),
        iaisf::http::HttpStatus::UnsupportedMediaType);
}

TEST_F(TaskHttpApiTest, RejectsWrongContentType) {
    EXPECT_EQ(
        dispatch("POST", "/v1/tasks", "{}", "text/plain").status(),
        iaisf::http::HttpStatus::UnsupportedMediaType);
}

TEST_F(TaskHttpApiTest, RejectsNonUtf8Charset) {
    EXPECT_EQ(
        dispatch(
            "POST",
            "/v1/tasks",
            "{}",
            "application/json; charset=latin1")
            .status(),
        iaisf::http::HttpStatus::UnsupportedMediaType);
}

TEST_F(TaskHttpApiTest, RejectsExtraContentTypeParameter) {
    EXPECT_EQ(
        dispatch(
            "POST",
            "/v1/tasks",
            "{}",
            "application/json; charset=utf-8; profile=x")
            .status(),
        iaisf::http::HttpStatus::UnsupportedMediaType);
}

TEST_F(TaskHttpApiTest, RejectsMalformedJson) {
    EXPECT_EQ(
        dispatch("POST", "/v1/tasks", "{").status(),
        iaisf::http::HttpStatus::BadRequest);
}

TEST_F(TaskHttpApiTest, RejectsTrailingJsonBytes) {
    EXPECT_EQ(
        dispatch("POST", "/v1/tasks", "{} x").status(),
        iaisf::http::HttpStatus::BadRequest);
}

TEST_F(TaskHttpApiTest, RejectsInvalidUtf8JsonString) {
    std::string body{
        R"({"operation":"echo","input":{"payload":")"};
    body.push_back(static_cast<char>(0xFF));
    body.append(R"("}})");
    EXPECT_EQ(
        dispatch("POST", "/v1/tasks", std::move(body)).status(),
        iaisf::http::HttpStatus::BadRequest);
}

TEST_F(TaskHttpApiTest, RejectsNonObjectRoot) {
    EXPECT_EQ(
        dispatch("POST", "/v1/tasks", "[]").status(),
        iaisf::http::HttpStatus::BadRequest);
}

TEST_F(TaskHttpApiTest, RejectsMissingOperation) {
    EXPECT_EQ(
        dispatch("POST", "/v1/tasks", R"({"input":{}})").status(),
        iaisf::http::HttpStatus::BadRequest);
}

TEST_F(TaskHttpApiTest, RejectsMissingInput) {
    EXPECT_EQ(
        dispatch("POST", "/v1/tasks", R"({"operation":"echo"})").status(),
        iaisf::http::HttpStatus::BadRequest);
}

TEST_F(TaskHttpApiTest, RejectsNonStringOperation) {
    EXPECT_EQ(
        dispatch(
            "POST",
            "/v1/tasks",
            R"({"operation":1,"input":{}})")
            .status(),
        iaisf::http::HttpStatus::BadRequest);
}

TEST_F(TaskHttpApiTest, RejectsUnknownTopLevelField) {
    EXPECT_EQ(
        dispatch(
            "POST",
            "/v1/tasks",
            R"({"operation":"echo","input":{"payload":1},"extra":1})")
            .status(),
        iaisf::http::HttpStatus::BadRequest);
}

TEST_F(TaskHttpApiTest, UnknownOperationReturnsNotFound) {
    const auto response = dispatch(
        "POST",
        "/v1/tasks",
        R"({"operation":"missing","input":{}})");
    EXPECT_EQ(response.status(), iaisf::http::HttpStatus::NotFound);
    EXPECT_EQ(body_json(response)["error"]["code"], "operation_not_found");
}

TEST_F(TaskHttpApiTest, PluginValidationReturnsUnprocessableContent) {
    const auto response = dispatch(
        "POST",
        "/v1/tasks",
        R"({"operation":"echo","input":{}})");
    EXPECT_EQ(
        response.status(),
        iaisf::http::HttpStatus::UnprocessableContent);
    EXPECT_EQ(body_json(response)["error"]["code"], "validation_failed");
}

TEST_F(TaskHttpApiTest, PluginValidationResourceFailureStillReturns422) {
    const auto response = dispatch(
        "POST",
        "/v1/tasks",
        R"({"operation":"test.validation_resource","input":{}})");
    EXPECT_EQ(
        response.status(),
        iaisf::http::HttpStatus::UnprocessableContent);
    EXPECT_EQ(body_json(response)["error"]["code"], "validation_failed");
}

TEST_F(TaskHttpApiTest, QueueFullUsesTyped503Mapping) {
    const auto first = dispatch(
        "POST",
        "/v1/tasks",
        R"({"operation":"test.block","input":{}})");
    ASSERT_EQ(first.status(), iaisf::http::HttpStatus::Accepted);
    ASSERT_EQ(
        blocking_state_->started_future.wait_for(std::chrono::seconds{5}),
        std::future_status::ready);
    EXPECT_EQ(
        dispatch(
            "POST",
            "/v1/tasks",
            R"({"operation":"test.block","input":{}})")
            .status(),
        iaisf::http::HttpStatus::Accepted);
    const auto rejected = dispatch(
        "POST",
        "/v1/tasks",
        R"({"operation":"test.block","input":{}})");
    EXPECT_EQ(
        rejected.status(),
        iaisf::http::HttpStatus::ServiceUnavailable);
    EXPECT_EQ(body_json(rejected)["error"]["code"], "queue_full");
    blocking_state_->release_once();
}

TEST_F(TaskHttpApiTest, RepositoryFullUsesDistinctTyped503Mapping) {
    for (int index = 0; index < 3; ++index) {
        const auto accepted = body_json(dispatch(
            "POST",
            "/v1/tasks",
            R"({"operation":"test.immediate","input":{}})"));
        const auto status_url =
            accepted["status_url"].get<std::string>();
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds{5};
        std::string state;
        while (std::chrono::steady_clock::now() < deadline) {
            state =
                body_json(dispatch("GET", status_url))
                    .value("state", "");
            if (state == "succeeded") {
                break;
            }
            std::this_thread::yield();
        }
        ASSERT_EQ(state, "succeeded");
    }

    const auto rejected = dispatch(
        "POST",
        "/v1/tasks",
        R"({"operation":"test.immediate","input":{}})");
    EXPECT_EQ(
        rejected.status(),
        iaisf::http::HttpStatus::ServiceUnavailable);
    EXPECT_EQ(body_json(rejected)["error"]["code"], "repository_full");
}

TEST_F(TaskHttpApiTest, StopAdmissionRejectsPostWith503) {
    api_->stop_admission();
    EXPECT_EQ(
        dispatch(
            "POST",
            "/v1/tasks",
            R"({"operation":"echo","input":{"payload":1}})")
            .status(),
        iaisf::http::HttpStatus::ServiceUnavailable);
}

TEST_F(TaskHttpApiTest, ManagerShutdownUsesTyped503Mapping) {
    ASSERT_TRUE(task_manager_->shutdown());
    const auto response = dispatch(
        "POST",
        "/v1/tasks",
        R"({"operation":"echo","input":{"payload":1}})");
    EXPECT_EQ(
        response.status(),
        iaisf::http::HttpStatus::ServiceUnavailable);
    EXPECT_EQ(body_json(response)["error"]["code"], "service_stopping");
}

TEST_F(TaskHttpApiTest, StoppingAdmissionStillAllowsExistingTaskQueries) {
    const auto accepted = body_json(dispatch(
        "POST",
        "/v1/tasks",
        R"({"operation":"echo","input":{"payload":1}})"));
    api_->stop_admission();
    EXPECT_EQ(
        dispatch(
            "POST",
            "/v1/tasks",
            R"({"operation":"echo","input":{"payload":2}})")
            .status(),
        iaisf::http::HttpStatus::ServiceUnavailable);
    EXPECT_EQ(
        dispatch(
            "GET",
            accepted["status_url"].get<std::string>())
            .status(),
        iaisf::http::HttpStatus::Ok);
}

TEST_F(TaskHttpApiTest, GetUnknownTaskReturns404) {
    EXPECT_EQ(
        dispatch("GET", "/v1/tasks/task-0000000000000999").status(),
        iaisf::http::HttpStatus::NotFound);
}

TEST_F(TaskHttpApiTest, GetRejectsZeroId) {
    EXPECT_EQ(
        dispatch("GET", "/v1/tasks/0").status(),
        iaisf::http::HttpStatus::BadRequest);
}

TEST_F(TaskHttpApiTest, GetRejectsLeadingZeroId) {
    EXPECT_EQ(
        dispatch("GET", "/v1/tasks/01").status(),
        iaisf::http::HttpStatus::BadRequest);
}

TEST_F(TaskHttpApiTest, GetRejectsSignedId) {
    EXPECT_EQ(
        dispatch("GET", "/v1/tasks/+1").status(),
        iaisf::http::HttpStatus::BadRequest);
}

TEST_F(TaskHttpApiTest, GetRejectsOverflowId) {
    EXPECT_EQ(
        dispatch("GET", "/v1/tasks/18446744073709551616").status(),
        iaisf::http::HttpStatus::BadRequest);
}

TEST_F(TaskHttpApiTest, GetAcceptsMaximumCanonicalIdBeforeRepositoryLookup) {
    const auto maximum_id =
        iaisf::task::TaskId{
            std::numeric_limits<std::uint64_t>::max()}
            .to_string();
    const auto response = dispatch("GET", "/v1/tasks/" + maximum_id);
    EXPECT_EQ(response.status(), iaisf::http::HttpStatus::NotFound);
    EXPECT_EQ(body_json(response)["error"]["code"], "task_not_found");
}

TEST_F(TaskHttpApiTest, ExtraPathSegmentDoesNotMatch) {
    EXPECT_EQ(
        dispatch("GET", "/v1/tasks/1/extra").status(),
        iaisf::http::HttpStatus::NotFound);
}

TEST_F(TaskHttpApiTest, EmptyIdPathDoesNotMatch) {
    EXPECT_EQ(
        dispatch("GET", "/v1/tasks/").status(),
        iaisf::http::HttpStatus::NotFound);
}

TEST_F(TaskHttpApiTest, MethodMismatchReturns405WithAllow) {
    const auto response =
        dispatch("POST", "/v1/tasks/task-0000000000000001");
    EXPECT_EQ(
        response.status(),
        iaisf::http::HttpStatus::MethodNotAllowed);
    const auto allow = std::find_if(
        response.headers().begin(),
        response.headers().end(),
        [](const iaisf::http::HttpHeader& header) {
            return header.name == "Allow";
        });
    ASSERT_NE(allow, response.headers().end());
    EXPECT_EQ(allow->value, "GET");
}

TEST_F(TaskHttpApiTest, SubmittedTaskCanBeQueriedWithoutInputDisclosure) {
    const auto accepted = body_json(dispatch(
        "POST",
        "/v1/tasks",
        R"({"operation":"echo","input":{"payload":"secret"}})"));
    const auto status_url = accepted["status_url"].get<std::string>();
    EXPECT_EQ(status_url, "/v1/tasks/task-0000000000000001");
    const auto response = dispatch("GET", status_url);
    ASSERT_EQ(response.status(), iaisf::http::HttpStatus::Ok)
        << response.body();
    const auto body = body_json(response);
    EXPECT_FALSE(body.contains("input"));
    EXPECT_TRUE(body.contains("state"));
}

TEST_F(TaskHttpApiTest, EchoCompletionReturnsSucceededResult) {
    const auto accepted = body_json(dispatch(
        "POST",
        "/v1/tasks",
        R"({"operation":"echo","input":{"payload":{"answer":42}}})"));
    ASSERT_TRUE(task_manager_->shutdown());
    const auto response = dispatch(
        "GET",
        accepted["status_url"].get<std::string>());
    ASSERT_EQ(response.status(), iaisf::http::HttpStatus::Ok);
    const auto body = body_json(response);
    EXPECT_EQ(body["state"], "succeeded");
    EXPECT_EQ(body["result"]["answer"], 42);
    EXPECT_FALSE(body.contains("error"));
}

struct FailedTaskCase {
    const char* operation;
};

class FailedTaskHttpApiTest
    : public TaskHttpApiTest,
      public ::testing::WithParamInterface<FailedTaskCase> {};

TEST_P(FailedTaskHttpApiTest, GetReturns200WithStableSafeFailedView) {
    const auto accepted = body_json(dispatch(
        "POST",
        "/v1/tasks",
        std::string{"{\"operation\":\""} + GetParam().operation +
            "\",\"input\":{}}"));
    ASSERT_TRUE(task_manager_->shutdown());
    const auto response = dispatch(
        "GET",
        accepted["status_url"].get<std::string>());
    ASSERT_EQ(response.status(), iaisf::http::HttpStatus::Ok);
    const auto body = body_json(response);
    EXPECT_EQ(body["state"], "failed");
    EXPECT_EQ(body["error"]["code"], "internal_error");
    EXPECT_EQ(body["error"]["message"], "task execution failed");
    EXPECT_FALSE(body.contains("result"));
    EXPECT_EQ(response.body().find("private"), std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(
    FailureKinds,
    FailedTaskHttpApiTest,
    ::testing::Values(
        FailedTaskCase{"test.return_failure"},
        FailedTaskCase{"test.standard_exception"},
        FailedTaskCase{"test.unknown_exception"}));

TEST_F(TaskHttpApiTest, RouterHandlersDoNotKeepApiAlive) {
    const std::weak_ptr<iaisf::api::TaskHttpApi> weak = api_;
    api_.reset();
    EXPECT_TRUE(weak.expired());
    const auto response = router_->dispatch(request(
        "POST",
        "/v1/tasks",
        R"({"operation":"echo","input":{"payload":1}})"));
    ASSERT_TRUE(response);
    EXPECT_EQ(
        response.value().status(),
        iaisf::http::HttpStatus::ServiceUnavailable);
}

TEST(TaskSnapshotJsonTest, QueuedViewContainsOnlyIdentityOperationAndState) {
    iaisf::task::TaskSnapshot snapshot;
    snapshot.id = iaisf::task::TaskId{7U};
    snapshot.operation = "echo";
    auto view = iaisf::api::task_snapshot_to_json(snapshot);
    ASSERT_TRUE(view);
    EXPECT_EQ(view.value().size(), 3U);
    EXPECT_EQ(view.value()["state"], "queued");
}

TEST(TaskSnapshotJsonTest, SucceededViewIncludesResultOnly) {
    iaisf::task::TaskSnapshot snapshot;
    snapshot.id = iaisf::task::TaskId{7U};
    snapshot.operation = "echo";
    snapshot.state = iaisf::task::TaskState::Succeeded;
    snapshot.result = nlohmann::json{{"ok", true}};
    auto view = iaisf::api::task_snapshot_to_json(snapshot);
    ASSERT_TRUE(view);
    EXPECT_TRUE(view.value().contains("result"));
    EXPECT_FALSE(view.value().contains("error"));
}

TEST(TaskSnapshotJsonTest, FailedViewIncludesSafeErrorOnly) {
    iaisf::task::TaskSnapshot snapshot;
    snapshot.id = iaisf::task::TaskId{7U};
    snapshot.operation = "echo";
    snapshot.state = iaisf::task::TaskState::Failed;
    snapshot.error = iaisf::make_error(
        iaisf::ErrorCode::InternalError,
        "private plugin error");
    auto view = iaisf::api::task_snapshot_to_json(snapshot);
    ASSERT_TRUE(view);
    EXPECT_EQ(view.value()["error"]["code"], "internal_error");
    EXPECT_EQ(view.value()["error"]["message"], "task execution failed");
    EXPECT_FALSE(view.value().contains("result"));
}

TEST(TaskSnapshotJsonTest, RejectsEveryImpossibleStatePayloadCombination) {
    iaisf::task::TaskSnapshot snapshot;
    snapshot.id = iaisf::task::TaskId{7U};
    snapshot.operation = "echo";

    snapshot.result = nlohmann::json{{"unexpected", true}};
    EXPECT_FALSE(iaisf::api::task_snapshot_to_json(snapshot));
    snapshot.result.reset();
    snapshot.error = iaisf::make_error(
        iaisf::ErrorCode::InternalError,
        "unexpected");
    EXPECT_FALSE(iaisf::api::task_snapshot_to_json(snapshot));

    snapshot.state = iaisf::task::TaskState::Succeeded;
    EXPECT_FALSE(iaisf::api::task_snapshot_to_json(snapshot));
    snapshot.error.reset();
    EXPECT_FALSE(iaisf::api::task_snapshot_to_json(snapshot));
    snapshot.result = nlohmann::json{{"ok", true}};
    EXPECT_TRUE(iaisf::api::task_snapshot_to_json(snapshot));

    snapshot.state = iaisf::task::TaskState::Failed;
    EXPECT_FALSE(iaisf::api::task_snapshot_to_json(snapshot));
    snapshot.result.reset();
    snapshot.error = iaisf::make_error(
        iaisf::ErrorCode::InternalError,
        "failed");
    EXPECT_TRUE(iaisf::api::task_snapshot_to_json(snapshot));
}

TEST(TaskSnapshotJsonTest, InvalidIdFailsClosed) {
    iaisf::task::TaskSnapshot snapshot;
    snapshot.operation = "echo";
    EXPECT_FALSE(iaisf::api::task_snapshot_to_json(snapshot));
}

TEST(TaskApiLimitsTest, RejectsNonPositiveAndExcessiveLimits) {
    EXPECT_FALSE(iaisf::api::TaskApiLimits::create(0, 10));
    EXPECT_FALSE(iaisf::api::TaskApiLimits::create(10, 0));
    EXPECT_FALSE(iaisf::api::TaskApiLimits::create(65537, 10));
}

TEST(TaskApiLimitsTest, DefaultsProvideBoundedStatusAndErrorStrings) {
    const auto limits = iaisf::api::TaskApiLimits::defaults();
    EXPECT_GE(
        limits.max_status_url_bytes(),
        std::string_view{"/v1/tasks/"}.size() +
            iaisf::task::TaskId::kMaximumTextBytes);
    EXPECT_GT(limits.max_error_message_bytes(), 0U);
    EXPECT_LE(limits.max_error_message_bytes(), 64U * 1024U);
}

TEST(TaskApiLimitsTest, ExactCrossLayerEnvelopeCapacityIsInclusive) {
    const auto task_limits = iaisf::task::TaskLimits::create(
        8,
        8,
        10,
        11,
        64,
        8,
        32,
        10)
                                 .value();
    const auto plugin_limits = iaisf::plugin::PluginLimits::create(
        8,
        8,
        32,
        16,
        64,
        64,
        10,
        11,
        8,
        32,
        10,
        8,
        16)
                                   .value();
    const auto api_limits = iaisf::api::TaskApiLimits::create(16, 64).value();
    const std::string operation(8U, 'a');
    const std::string task_id =
        iaisf::task::TaskId{
            std::numeric_limits<std::uint64_t>::max()}
            .to_string();
    const auto request_bytes = nlohmann::json{
        {"input", "12345678"},
        {"operation", operation}}
                                   .dump()
                                   .size();
    const auto success_bytes = nlohmann::json{
        {"operation", operation},
        {"result", "123456789"},
        {"state", "succeeded"},
        {"task_id", task_id}}
                                   .dump()
                                   .size();
    const auto failure_bytes = nlohmann::json{
        {"error",
         {{"code", "internal_error"},
          {"message", "task execution failed"}}},
        {"operation", operation},
        {"state", "failed"},
        {"task_id", task_id}}
                                   .dump()
                                   .size();
    const auto api_error_bytes = nlohmann::json{
        {"error",
         {{"code", "internal_error"},
          {"message", std::string(16U, 'x')}}}}
                                     .dump()
                                     .size();
    const auto response_bytes =
        std::max({success_bytes, failure_bytes, api_error_bytes});
    const auto exact_http = iaisf::http::HttpLimits::create(
        512,
        32,
        128,
        256,
        1024,
        16,
        static_cast<std::int64_t>(request_bytes),
        static_cast<std::int64_t>(response_bytes),
        4,
        4)
                                .value();
    EXPECT_TRUE(iaisf::api::validate_task_api_capacity(
        task_limits,
        plugin_limits,
        exact_http,
        api_limits));

    const auto short_response = iaisf::http::HttpLimits::create(
        512,
        32,
        128,
        256,
        1024,
        16,
        static_cast<std::int64_t>(request_bytes),
        static_cast<std::int64_t>(response_bytes - 1U),
        4,
        4)
                                    .value();
    EXPECT_FALSE(iaisf::api::validate_task_api_capacity(
        task_limits,
        plugin_limits,
        short_response,
        api_limits));

    const auto short_request = iaisf::http::HttpLimits::create(
        512,
        32,
        128,
        256,
        1024,
        16,
        static_cast<std::int64_t>(request_bytes - 1U),
        static_cast<std::int64_t>(response_bytes),
        4,
        4)
                                   .value();
    EXPECT_FALSE(iaisf::api::validate_task_api_capacity(
        task_limits,
        plugin_limits,
        short_request,
        api_limits));
}

}  // namespace
