#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "iaisf/logging/logger.hpp"
#include "iaisf/plugin/echo_plugin.hpp"
#include "iaisf/plugin/mock_vision_plugin.hpp"
#include "iaisf/plugin/plugin_task_adapter.hpp"
#include "iaisf/task/task_manager.hpp"

namespace {

using namespace std::chrono_literals;
using iaisf::ErrorCode;
using iaisf::ILogger;
using iaisf::LogLevel;
using iaisf::Result;
using iaisf::make_error;
using iaisf::plugin::EchoPlugin;
using iaisf::plugin::IAlgorithmPlugin;
using iaisf::plugin::MockVisionPlugin;
using iaisf::plugin::PluginLimits;
using iaisf::plugin::PluginManager;
using iaisf::plugin::PluginMetadata;
using iaisf::plugin::PluginTaskAdapter;
using iaisf::task::TaskLimits;
using iaisf::task::TaskManager;
using iaisf::task::TaskRequest;
using iaisf::task::TaskState;
using iaisf::task::ThreadPoolOptions;

class QuietLogger final : public ILogger {
public:
    void log(LogLevel, std::string_view, std::string_view) override {}
};

class FunctionPlugin final : public IAlgorithmPlugin {
public:
    using Validator =
        std::function<Result<void>(const nlohmann::json&)>;
    using Executor =
        std::function<Result<nlohmann::json>(const nlohmann::json&)>;

    FunctionPlugin(
        std::string operation,
        Validator validator,
        Executor executor)
        : metadata_{
              std::move(operation),
              "Adapter Test Plugin",
              "1.0.0",
              "Deterministic adapter test plugin.",
              false,
              {},
          },
          validator_(std::move(validator)),
          executor_(std::move(executor)) {}

    PluginMetadata metadata() const override {
        return metadata_;
    }

    Result<void> validate_input(
        const nlohmann::json& input) const override {
        return validator_(input);
    }

    Result<nlohmann::json> execute(
        const nlohmann::json& input) const override {
        return executor_(input);
    }

private:
    PluginMetadata metadata_;
    Validator validator_;
    Executor executor_;
};

std::shared_ptr<PluginManager> builtin_manager() {
    auto manager =
        std::make_shared<PluginManager>(PluginLimits::create().value());
    EXPECT_TRUE(manager->register_plugin(std::make_shared<EchoPlugin>()));
    EXPECT_TRUE(
        manager->register_plugin(std::make_shared<MockVisionPlugin>()));
    EXPECT_TRUE(manager->freeze());
    return manager;
}

std::shared_ptr<PluginTaskAdapter> adapter_for(
    const std::shared_ptr<PluginManager>& manager) {
    auto adapter = PluginTaskAdapter::create(manager);
    EXPECT_TRUE(adapter);
    return adapter ? std::move(adapter).value() : nullptr;
}

std::unique_ptr<TaskManager> task_manager_for(
    QuietLogger& logger,
    const std::shared_ptr<PluginTaskAdapter>& adapter,
    const ThreadPoolOptions options = ThreadPoolOptions{2, 32},
    TaskLimits limits = TaskLimits::create().value()) {
    auto manager = TaskManager::create(
        options,
        std::move(limits),
        logger,
        adapter->make_validator(),
        adapter->make_handler());
    EXPECT_TRUE(manager);
    return manager ? std::move(manager).value() : nullptr;
}

TEST(PluginTaskAdapterTest, RejectsNullAndUnfrozenManagers) {
    const auto null_adapter =
        PluginTaskAdapter::create(std::shared_ptr<PluginManager>{});
    ASSERT_FALSE(null_adapter);
    EXPECT_EQ(null_adapter.error().code, ErrorCode::InvalidArgument);

    const auto configuring =
        std::make_shared<PluginManager>(PluginLimits::create().value());
    const auto unfrozen = PluginTaskAdapter::create(configuring);
    ASSERT_FALSE(unfrozen);
    EXPECT_EQ(unfrozen.error().code, ErrorCode::InvalidState);
}

TEST(PluginTaskAdapterTest, CreatesFromFrozenManager) {
    const auto manager = builtin_manager();

    const auto adapter = PluginTaskAdapter::create(manager);

    EXPECT_TRUE(adapter);
}

TEST(PluginTaskAdapterTest, ValidatorUsesOperationAndStructuredNotFound) {
    const auto adapter = adapter_for(builtin_manager());
    ASSERT_NE(adapter, nullptr);

    const auto missing =
        adapter->validate_task(TaskRequest{"missing", {{"payload", 1}}});

    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound);
}

TEST(PluginTaskAdapterTest, HandlerRevalidatesToPreventValidatorBypass) {
    const auto adapter = adapter_for(builtin_manager());
    ASSERT_NE(adapter, nullptr);
    const auto handler = adapter->make_handler();

    const auto result = handler(TaskRequest{"echo", {{"wrong", 1}}});

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InternalError);
    EXPECT_EQ(
        result.error().message,
        "plugin validation changed before execution");
}

TEST(PluginTaskAdapterTest, ChangedSecondValidationFailsWithoutExecution) {
    std::atomic<int> validation_calls{0};
    std::atomic<int> execution_calls{0};
    auto plugins =
        std::make_shared<PluginManager>(PluginLimits::create().value());
    ASSERT_TRUE(plugins->register_plugin(std::make_shared<FunctionPlugin>(
        "validation.changes",
        [&validation_calls](const nlohmann::json&) {
            const int call =
                validation_calls.fetch_add(1, std::memory_order_relaxed);
            if (call == 0) {
                return Result<void>::success();
            }
            return Result<void>::failure(make_error(
                ErrorCode::InvalidArgument,
                "mutable validation state"));
        },
        [&execution_calls](const nlohmann::json&) {
            execution_calls.fetch_add(1, std::memory_order_relaxed);
            return Result<nlohmann::json>::success({});
        })));
    ASSERT_TRUE(plugins->freeze());
    const auto adapter = adapter_for(plugins);
    QuietLogger logger;
    auto manager = task_manager_for(
        logger,
        adapter,
        ThreadPoolOptions{1, 2});
    ASSERT_NE(manager, nullptr);

    const auto submitted =
        manager->submit(TaskRequest{"validation.changes", {}});
    ASSERT_TRUE(submitted);
    ASSERT_TRUE(manager->shutdown());
    const auto snapshot = manager->get_snapshot(submitted.value());

    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().state, TaskState::Failed);
    ASSERT_TRUE(snapshot.value().error.has_value());
    EXPECT_EQ(
        snapshot.value().error->message,
        "plugin validation changed before execution");
    EXPECT_EQ(execution_calls.load(std::memory_order_relaxed), 0);
}

TEST(PluginTaskAdapterTest, EchoAndMockVisionCompleteThroughTaskRuntime) {
    QuietLogger logger;
    const auto adapter = adapter_for(builtin_manager());
    auto manager = task_manager_for(logger, adapter);
    ASSERT_NE(manager, nullptr);

    const auto echo =
        manager->submit(TaskRequest{"echo", {{"payload", {{"value", 7}}}}});
    const auto vision = manager->submit(TaskRequest{
        "mock_vision.detect",
        {{"image_id", "demo-001"}, {"width", 640}, {"height", 480}},
    });
    ASSERT_TRUE(echo);
    ASSERT_TRUE(vision);
    ASSERT_TRUE(manager->shutdown());

    const auto echo_snapshot = manager->get_snapshot(echo.value());
    const auto vision_snapshot = manager->get_snapshot(vision.value());
    ASSERT_TRUE(echo_snapshot);
    ASSERT_TRUE(vision_snapshot);
    EXPECT_EQ(echo_snapshot.value().state, TaskState::Succeeded);
    EXPECT_EQ(echo_snapshot.value().result->at("value"), 7);
    EXPECT_EQ(vision_snapshot.value().state, TaskState::Succeeded);
    EXPECT_TRUE(
        vision_snapshot.value().result->at("mock").get<bool>());
}

TEST(PluginTaskAdapterTest, ValidationFailuresAllocateNoTaskOrQueueEntry) {
    QuietLogger logger;
    const auto adapter = adapter_for(builtin_manager());
    auto manager = task_manager_for(logger, adapter);
    ASSERT_NE(manager, nullptr);

    const std::vector<TaskRequest> invalid{
        {"missing", {{"payload", 1}}},
        {"echo", {{"wrong", 1}}},
        {
            "mock_vision.detect",
            {{"image_id", "demo"}, {"width", 0}, {"height", 480}},
        },
    };
    for (const auto& request : invalid) {
        const auto result = manager->submit(request);
        EXPECT_FALSE(result);
        EXPECT_EQ(manager->repository_size(), 0U);
        EXPECT_EQ(manager->pending_count(), 0U);
    }
    EXPECT_TRUE(manager->shutdown());
}

TEST(PluginTaskAdapterTest, ValidationExceptionCreatesNoTaskRecord) {
    auto plugins =
        std::make_shared<PluginManager>(PluginLimits::create().value());
    ASSERT_TRUE(plugins->register_plugin(std::make_shared<FunctionPlugin>(
        "validation.throw",
        [](const nlohmann::json&) -> Result<void> {
            throw std::runtime_error("validation secret");
        },
        [](const nlohmann::json&) {
            return Result<nlohmann::json>::success({});
        })));
    ASSERT_TRUE(plugins->freeze());
    const auto adapter = adapter_for(plugins);
    QuietLogger logger;
    auto manager = task_manager_for(logger, adapter);
    ASSERT_NE(manager, nullptr);

    const auto submitted =
        manager->submit(TaskRequest{"validation.throw", {}});

    ASSERT_FALSE(submitted);
    EXPECT_EQ(submitted.error().code, ErrorCode::InternalError);
    EXPECT_EQ(submitted.error().message, "plugin validation failed");
    EXPECT_EQ(manager->repository_size(), 0U);
    EXPECT_EQ(manager->pending_count(), 0U);
    EXPECT_TRUE(manager->shutdown());
}

TEST(PluginTaskAdapterTest, ExecutionExceptionFailsTaskAndNextTaskSucceeds) {
    auto plugins =
        std::make_shared<PluginManager>(PluginLimits::create().value());
    ASSERT_TRUE(plugins->register_plugin(std::make_shared<FunctionPlugin>(
        "execution.throw",
        [](const nlohmann::json&) { return Result<void>::success(); },
        [](const nlohmann::json&) -> Result<nlohmann::json> {
            throw std::runtime_error("C:\\private\\model.engine");
        })));
    ASSERT_TRUE(plugins->register_plugin(std::make_shared<EchoPlugin>()));
    ASSERT_TRUE(plugins->freeze());
    const auto adapter = adapter_for(plugins);
    QuietLogger logger;
    auto manager = task_manager_for(
        logger,
        adapter,
        ThreadPoolOptions{1, 4});
    ASSERT_NE(manager, nullptr);

    const auto failed =
        manager->submit(TaskRequest{"execution.throw", {}});
    const auto healthy =
        manager->submit(TaskRequest{"echo", {{"payload", "ok"}}});
    ASSERT_TRUE(failed);
    ASSERT_TRUE(healthy);
    ASSERT_TRUE(manager->shutdown());

    const auto failure = manager->get_snapshot(failed.value());
    const auto success = manager->get_snapshot(healthy.value());
    ASSERT_TRUE(failure);
    ASSERT_TRUE(success);
    EXPECT_EQ(failure.value().state, TaskState::Failed);
    ASSERT_TRUE(failure.value().error.has_value());
    EXPECT_EQ(failure.value().error->message, "plugin execution failed");
    EXPECT_EQ(
        failure.value().error->message.find("private"),
        std::string::npos);
    EXPECT_EQ(success.value().state, TaskState::Succeeded);
}

TEST(PluginTaskAdapterTest, TaskResultLimitStillControlsPluginOutput) {
    auto plugins =
        std::make_shared<PluginManager>(PluginLimits::create().value());
    ASSERT_TRUE(plugins->register_plugin(std::make_shared<FunctionPlugin>(
        "output.large",
        [](const nlohmann::json&) { return Result<void>::success(); },
        [](const nlohmann::json&) {
            return Result<nlohmann::json>::success(
                {{"value", std::string(512, 'x')}});
        })));
    ASSERT_TRUE(plugins->freeze());
    const auto adapter = adapter_for(plugins);
    QuietLogger logger;
    auto manager = task_manager_for(
        logger,
        adapter,
        ThreadPoolOptions{1, 2},
        TaskLimits::create(8, 128, 1024, 64, 128).value());
    ASSERT_NE(manager, nullptr);

    const auto submitted = manager->submit(TaskRequest{"output.large", {}});
    ASSERT_TRUE(submitted);
    ASSERT_TRUE(manager->shutdown());
    const auto snapshot = manager->get_snapshot(submitted.value());

    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().state, TaskState::Failed);
    EXPECT_FALSE(snapshot.value().result.has_value());
}

TEST(PluginTaskAdapterTest, BuiltinOutputsUseLimitBeforeRepositorySuccess) {
    const auto plugin_limits = PluginLimits::create(
        2, 64, 64, 64, 128, 128, 1024, 6).value();
    auto plugins = std::make_shared<PluginManager>(plugin_limits);
    ASSERT_TRUE(plugins->register_plugin(std::make_shared<EchoPlugin>()));
    ASSERT_TRUE(
        plugins->register_plugin(std::make_shared<MockVisionPlugin>()));
    ASSERT_TRUE(plugins->freeze());
    const auto adapter = adapter_for(plugins);
    QuietLogger logger;
    auto manager = task_manager_for(
        logger,
        adapter,
        ThreadPoolOptions{1, 2},
        TaskLimits::create(8, 128, 1024, 1024, 128).value());
    ASSERT_NE(manager, nullptr);

    const auto echo =
        manager->submit(TaskRequest{"echo", {{"payload", "value"}}});
    const auto vision = manager->submit(TaskRequest{
        "mock_vision.detect",
        {{"image_id", "demo"}, {"width", 640}, {"height", 480}},
    });
    ASSERT_TRUE(echo);
    ASSERT_TRUE(vision);
    ASSERT_TRUE(manager->shutdown());
    for (const auto id : {echo.value(), vision.value()}) {
        const auto snapshot = manager->get_snapshot(id);
        ASSERT_TRUE(snapshot);
        EXPECT_EQ(snapshot.value().state, TaskState::Failed);
        EXPECT_FALSE(snapshot.value().result.has_value());
        ASSERT_TRUE(snapshot.value().error.has_value());
        EXPECT_EQ(
            snapshot.value().error->code,
            ErrorCode::ResourceExhausted);
    }
}

TEST(PluginTaskAdapterTest, AcceptedTaskOwnsInputSnapshotAfterCallerMutation) {
    std::atomic<int> validation_calls{0};
    std::promise<void> worker_validation_entered;
    auto entered = worker_validation_entered.get_future();
    std::promise<void> release_worker;
    auto release = release_worker.get_future().share();
    auto plugins =
        std::make_shared<PluginManager>(PluginLimits::create().value());
    ASSERT_TRUE(plugins->register_plugin(std::make_shared<FunctionPlugin>(
        "snapshot",
        [
            &validation_calls,
            &worker_validation_entered,
            release
        ](const nlohmann::json& input) {
            const int call =
                validation_calls.fetch_add(1, std::memory_order_relaxed);
            if (call == 1) {
                worker_validation_entered.set_value();
                release.wait();
            }
            if (input.at("value") != "original") {
                return Result<void>::failure(make_error(
                    ErrorCode::InvalidArgument,
                    "input snapshot changed"));
            }
            return Result<void>::success();
        },
        [](const nlohmann::json& input) {
            return Result<nlohmann::json>::success(input);
        })));
    ASSERT_TRUE(plugins->freeze());
    const auto adapter = adapter_for(plugins);
    QuietLogger logger;
    auto manager = task_manager_for(
        logger,
        adapter,
        ThreadPoolOptions{1, 2});
    ASSERT_NE(manager, nullptr);
    TaskRequest request{"snapshot", {{"value", "original"}}};

    const auto submitted = manager->submit(request);
    ASSERT_TRUE(submitted);
    const auto entered_status = entered.wait_for(2s);
    if (entered_status != std::future_status::ready) {
        release_worker.set_value();
        const auto stopped = manager->shutdown();
        EXPECT_TRUE(stopped);
        FAIL() << "worker validation did not start before the deadline";
        return;
    }
    request.operation = "mutated";
    request.input["value"] = "mutated";
    release_worker.set_value();
    ASSERT_TRUE(manager->shutdown());
    const auto snapshot = manager->get_snapshot(submitted.value());

    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().state, TaskState::Succeeded);
    ASSERT_TRUE(snapshot.value().result.has_value());
    EXPECT_EQ(snapshot.value().result->at("value"), "original");
}

TEST(PluginTaskAdapterTest, MultipleWorkersExecuteBothOperationsWithUniqueIds) {
    QuietLogger logger;
    const auto adapter = adapter_for(builtin_manager());
    auto manager = task_manager_for(
        logger,
        adapter,
        ThreadPoolOptions{4, 64},
        TaskLimits::create(64).value());
    ASSERT_NE(manager, nullptr);
    std::vector<iaisf::task::TaskId> ids;
    for (int index = 0; index < 20; ++index) {
        auto submitted = index % 2 == 0
                             ? manager->submit(TaskRequest{
                                   "echo",
                                   {{"payload", index}},
                               })
                             : manager->submit(TaskRequest{
                                   "mock_vision.detect",
                                   {
                                       {"image_id", "image-" + std::to_string(index)},
                                       {"width", 640},
                                       {"height", 480},
                                   },
                               });
        ASSERT_TRUE(submitted);
        ids.push_back(submitted.value());
    }
    ASSERT_TRUE(manager->shutdown());

    std::set<std::uint64_t> unique_ids;
    for (const auto id : ids) {
        unique_ids.insert(id.value());
        const auto snapshot = manager->get_snapshot(id);
        ASSERT_TRUE(snapshot);
        EXPECT_EQ(snapshot.value().state, TaskState::Succeeded);
    }
    EXPECT_EQ(unique_ids.size(), ids.size());
}

TEST(
    PluginTaskAdapterTest,
    GeneratedClosuresRetainManagerAndPluginWithoutRetainingAdapter) {
    auto plugins =
        std::make_shared<PluginManager>(PluginLimits::create().value());
    std::weak_ptr<PluginManager> weak_manager = plugins;
    auto plugin = std::make_shared<EchoPlugin>();
    std::weak_ptr<EchoPlugin> weak_plugin = plugin;
    ASSERT_TRUE(plugins->register_plugin(plugin));
    plugin.reset();
    ASSERT_TRUE(plugins->freeze());
    auto adapter = adapter_for(plugins);
    std::weak_ptr<PluginTaskAdapter> weak_adapter = adapter;
    auto validator = adapter->make_validator();
    auto handler = adapter->make_handler();
    adapter.reset();
    EXPECT_TRUE(weak_adapter.expired());
    plugins.reset();
    EXPECT_FALSE(weak_manager.expired());
    EXPECT_FALSE(weak_plugin.expired());

    QuietLogger logger;
    auto manager_result = TaskManager::create(
        ThreadPoolOptions{1, 2},
        TaskLimits::create().value(),
        logger,
        std::move(validator),
        std::move(handler));
    ASSERT_TRUE(manager_result);
    auto manager = std::move(manager_result).value();
    const auto submitted =
        manager->submit(TaskRequest{"echo", {{"payload", "alive"}}});
    ASSERT_TRUE(submitted);
    ASSERT_TRUE(manager->shutdown());
    EXPECT_EQ(
        manager->get_snapshot(submitted.value()).value().state,
        TaskState::Succeeded);
    manager.reset();
    EXPECT_TRUE(weak_manager.expired());
    EXPECT_TRUE(weak_plugin.expired());
}

TEST(PluginTaskAdapterTest, ClosuresReleaseDependenciesWithoutAnyTask) {
    auto plugins =
        std::make_shared<PluginManager>(PluginLimits::create().value());
    std::weak_ptr<PluginManager> weak_manager = plugins;
    auto plugin = std::make_shared<EchoPlugin>();
    std::weak_ptr<EchoPlugin> weak_plugin = plugin;
    ASSERT_TRUE(plugins->register_plugin(plugin));
    plugin.reset();
    ASSERT_TRUE(plugins->freeze());
    auto adapter = adapter_for(plugins);
    std::weak_ptr<PluginTaskAdapter> weak_adapter = adapter;
    auto validator = adapter->make_validator();
    auto handler = adapter->make_handler();

    adapter.reset();
    plugins.reset();
    EXPECT_TRUE(weak_adapter.expired());
    EXPECT_FALSE(weak_manager.expired());
    EXPECT_FALSE(weak_plugin.expired());

    validator = {};
    EXPECT_FALSE(weak_manager.expired());
    handler = {};
    EXPECT_TRUE(weak_manager.expired());
    EXPECT_TRUE(weak_plugin.expired());
}

TEST(
    PluginTaskAdapterTest,
    ValidationAndExecutionFailurePathsReleaseDependencies) {
    auto plugins =
        std::make_shared<PluginManager>(PluginLimits::create().value());
    std::weak_ptr<PluginManager> weak_manager = plugins;
    auto plugin = std::make_shared<FunctionPlugin>(
        "lifetime.failure",
        [](const nlohmann::json& input) {
            if (input.value("reject", false)) {
                return Result<void>::failure(make_error(
                    ErrorCode::InvalidArgument,
                    "rejected"));
            }
            return Result<void>::success();
        },
        [](const nlohmann::json&) {
            return Result<nlohmann::json>::failure(make_error(
                ErrorCode::InternalError,
                "unsafe plugin detail"));
        });
    std::weak_ptr<FunctionPlugin> weak_plugin = plugin;
    ASSERT_TRUE(plugins->register_plugin(plugin));
    plugin.reset();
    ASSERT_TRUE(plugins->freeze());
    auto adapter = adapter_for(plugins);
    std::weak_ptr<PluginTaskAdapter> weak_adapter = adapter;
    QuietLogger logger;
    auto manager = task_manager_for(logger, adapter);
    ASSERT_NE(manager, nullptr);

    adapter.reset();
    plugins.reset();
    EXPECT_TRUE(weak_adapter.expired());
    EXPECT_FALSE(weak_manager.expired());
    EXPECT_FALSE(weak_plugin.expired());

    const auto rejected = manager->submit(
        TaskRequest{"lifetime.failure", {{"reject", true}}});
    EXPECT_FALSE(rejected);
    const auto failed = manager->submit(
        TaskRequest{"lifetime.failure", {{"reject", false}}});
    ASSERT_TRUE(failed);
    ASSERT_TRUE(manager->shutdown());
    const auto snapshot = manager->get_snapshot(failed.value());
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().state, TaskState::Failed);

    manager.reset();
    EXPECT_TRUE(weak_manager.expired());
    EXPECT_TRUE(weak_plugin.expired());
}

}  // namespace
