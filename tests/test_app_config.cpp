#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <gtest/gtest.h>

#include "iaisf/config/app_config.hpp"
#include "iaisf/core/error.hpp"
#include "iaisf/logging/log_level.hpp"

namespace {

std::filesystem::path create_unique_temp_directory(const std::string_view prefix) {
    static std::atomic<unsigned long> sequence{0};
    const std::filesystem::path temp_root = std::filesystem::temp_directory_path();

    for (std::size_t attempt = 0; attempt < 100U; ++attempt) {
        const auto timestamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        const auto sequence_number =
            sequence.fetch_add(1, std::memory_order_relaxed);
        const std::filesystem::path candidate =
            temp_root /
            (std::string{prefix} + "-" + std::to_string(timestamp) + "-" +
             std::to_string(sequence_number) + "-" + std::to_string(attempt));

        std::error_code create_error;
        if (std::filesystem::create_directory(candidate, create_error)) {
            return candidate;
        }
        if (create_error &&
            create_error != std::make_error_code(std::errc::file_exists)) {
            throw std::runtime_error{"unable to create temporary directory"};
        }
    }
    throw std::runtime_error{"unable to allocate a unique temporary directory"};
}

class ScopedConfigFile {
public:
    explicit ScopedConfigFile(const std::string_view contents) {
        directory_ = create_unique_temp_directory("iaisf-phase1-config");
        path_ = directory_ / "config.json";

        std::ofstream output{path_, std::ios::trunc};
        if (!output.is_open()) {
            throw std::runtime_error{"unable to create temporary configuration"};
        }
        output << contents;
        if (!output) {
            throw std::runtime_error{"unable to write temporary configuration"};
        }
    }

    ~ScopedConfigFile() {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    ScopedConfigFile(const ScopedConfigFile&) = delete;
    ScopedConfigFile& operator=(const ScopedConfigFile&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path directory_;
    std::filesystem::path path_;
};

void expect_config_error(const std::string_view json) {
    const ScopedConfigFile file{json};
    const auto result = iaisf::load_app_config(file.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, iaisf::ErrorCode::ConfigError);
    EXPECT_FALSE(result.error().message.empty());
}

TEST(AppConfigTest, ProvidesSafeDefaults) {
    const iaisf::AppConfig config = iaisf::default_app_config();

    EXPECT_EQ(config.service_name, "IndustrialAIServiceFramework");
    EXPECT_GE(config.worker_threads, 1U);
    EXPECT_LE(config.worker_threads, iaisf::kMaxWorkerThreads);
    EXPECT_EQ(config.task_queue_capacity, 1024U);
    EXPECT_EQ(config.log_level, iaisf::LogLevel::Info);
    EXPECT_TRUE(iaisf::validate_app_config(config));
}

TEST(AppConfigTest, LoadsTheRealExampleConfiguration) {
    const auto result =
        iaisf::load_app_config(std::filesystem::path{IAISF_EXAMPLE_CONFIG_PATH});

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result.value().service_name, "IndustrialAIServiceFramework");
    EXPECT_EQ(result.value().worker_threads, 4U);
    EXPECT_EQ(result.value().task_queue_capacity, 1024U);
    EXPECT_EQ(result.value().log_level, iaisf::LogLevel::Info);
}

TEST(AppConfigTest, LoadsACompleteValidConfiguration) {
    const ScopedConfigFile file{R"({
        "service": {"name": "factory-service"},
        "runtime": {"worker_threads": 8, "task_queue_capacity": 2048},
        "logging": {"level": "debug"}
    })"};

    const auto result = iaisf::load_app_config(file.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result.value().service_name, "factory-service");
    EXPECT_EQ(result.value().worker_threads, 8U);
    EXPECT_EQ(result.value().task_queue_capacity, 2048U);
    EXPECT_EQ(result.value().log_level, iaisf::LogLevel::Debug);
}

TEST(AppConfigTest, ReportsMissingFileAsIoErrorWithoutLeakingItsPath) {
    const auto missing =
        std::filesystem::temp_directory_path() / "iaisf-file-that-does-not-exist.json";
    std::error_code ignored;
    std::filesystem::remove(missing, ignored);

    const auto result = iaisf::load_app_config(missing);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, iaisf::ErrorCode::IoError);
    EXPECT_EQ(result.error().message.find(missing.string()), std::string::npos);
}

TEST(AppConfigTest, RejectsInvalidJsonAndNonObjectRoot) {
    expect_config_error("{not-json");
    expect_config_error(R"(["not", "an", "object"])");
}

TEST(AppConfigTest, RejectsGroupsWithWrongTypes) {
    expect_config_error(R"({"service": "bad"})");
    expect_config_error(R"({"service": null})");
    expect_config_error(R"({"runtime": []})");
    expect_config_error(R"({"runtime": true})");
    expect_config_error(R"({"logging": 3})");
    expect_config_error(R"({"logging": []})");
}

TEST(AppConfigTest, RejectsFieldsWithWrongTypes) {
    expect_config_error(R"({"service": {"name": 9}})");
    expect_config_error(R"({"runtime": {"worker_threads": "4"}})");
    expect_config_error(R"({"runtime": {"worker_threads": 4.5}})");
    expect_config_error(R"({"runtime": {"worker_threads": null}})");
    expect_config_error(R"({"runtime": {"worker_threads": true}})");
    expect_config_error(R"({"runtime": {"task_queue_capacity": "4"}})");
    expect_config_error(R"({"runtime": {"task_queue_capacity": 4.5}})");
    expect_config_error(R"({"runtime": {"task_queue_capacity": null}})");
    expect_config_error(R"({"runtime": {"task_queue_capacity": false}})");
    expect_config_error(R"({"logging": {"level": true}})");
}

TEST(AppConfigTest, RejectsInvalidServiceNames) {
    expect_config_error(R"({"service": {"name": ""}})");
    expect_config_error(R"({"service": {"name": "   "}})");
    expect_config_error(
        std::string{R"({"service": {"name": ")"} +
        std::string(iaisf::kMaxServiceNameLength + 1U, 'x') + R"("}})");
    std::string escaped_multibyte_name;
    for (std::size_t index = 0; index < 43U; ++index) {
        escaped_multibyte_name += R"(\u5de5)";
    }
    expect_config_error(
        std::string{R"({"service": {"name": ")"} +
        escaped_multibyte_name + R"("}})");
    expect_config_error(R"({"service": {"name": "line\u0001break"}})");
}

TEST(AppConfigTest, RejectsInvalidWorkerThreadCounts) {
    expect_config_error(R"({"runtime": {"worker_threads": 0}})");
    expect_config_error(R"({"runtime": {"worker_threads": -1}})");
    expect_config_error(R"({"runtime": {"worker_threads": 257}})");
    expect_config_error(
        R"({"runtime": {"worker_threads": 18446744073709551616}})");
}

TEST(AppConfigTest, RejectsInvalidQueueCapacities) {
    expect_config_error(R"({"runtime": {"task_queue_capacity": 0}})");
    expect_config_error(R"({"runtime": {"task_queue_capacity": -1}})");
    expect_config_error(R"({"runtime": {"task_queue_capacity": 1000001}})");
}

TEST(AppConfigTest, RequiresStrictLowercaseLogLevel) {
    expect_config_error(R"({"logging": {"level": "INFO"}})");
    expect_config_error(R"({"logging": {"level": "verbose"}})");
}

TEST(AppConfigTest, RejectsUnknownFieldsAtEveryLevel) {
    expect_config_error(R"({"unknown": {}})");
    expect_config_error(R"({"service": {"typo": "value"}})");
    expect_config_error(R"({"runtime": {"workers": 4}})");
    expect_config_error(R"({"logging": {"format": "json"}})");
}

TEST(AppConfigTest, UsesDefaultsForMissingGroupsAndFields) {
    const ScopedConfigFile empty_root{"{}"};
    const auto default_result = iaisf::load_app_config(empty_root.path());
    ASSERT_TRUE(default_result.has_value()) << default_result.error().message;

    const iaisf::AppConfig defaults = iaisf::default_app_config();
    EXPECT_EQ(default_result.value().service_name, defaults.service_name);
    EXPECT_EQ(default_result.value().worker_threads, defaults.worker_threads);
    EXPECT_EQ(
        default_result.value().task_queue_capacity,
        defaults.task_queue_capacity);
    EXPECT_EQ(default_result.value().log_level, defaults.log_level);

    const ScopedConfigFile partial{R"({"runtime": {"worker_threads": 2}})"};
    const auto partial_result = iaisf::load_app_config(partial.path());
    ASSERT_TRUE(partial_result.has_value()) << partial_result.error().message;
    EXPECT_EQ(partial_result.value().worker_threads, 2U);
    EXPECT_EQ(
        partial_result.value().task_queue_capacity,
        defaults.task_queue_capacity);
}

TEST(AppConfigTest, ProducesStableResultsAcrossRepeatedLoads) {
    const ScopedConfigFile file{R"({
        "service": {"name": "stable"},
        "runtime": {"worker_threads": 3, "task_queue_capacity": 99},
        "logging": {"level": "warn"}
    })"};

    const auto first = iaisf::load_app_config(file.path());
    const auto second = iaisf::load_app_config(file.path());
    ASSERT_TRUE(first.has_value()) << first.error().message;
    ASSERT_TRUE(second.has_value()) << second.error().message;
    EXPECT_EQ(first.value().service_name, second.value().service_name);
    EXPECT_EQ(first.value().worker_threads, second.value().worker_threads);
    EXPECT_EQ(
        first.value().task_queue_capacity,
        second.value().task_queue_capacity);
    EXPECT_EQ(first.value().log_level, second.value().log_level);
}

}  // namespace
