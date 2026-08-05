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
#include "iaisf/logging/log_level.hpp"

namespace {

std::filesystem::path make_config(const std::string_view contents) {
    static std::atomic<unsigned long> sequence{0U};
    const auto directory = std::filesystem::temp_directory_path() /
                           ("iaisf-logging-config-" +
                            std::to_string(
                                std::chrono::steady_clock::now()
                                    .time_since_epoch()
                                    .count()) +
                            "-" + std::to_string(sequence.fetch_add(
                                              1U, std::memory_order_relaxed)));
    std::error_code error;
    if (!std::filesystem::create_directory(directory, error)) {
        throw std::runtime_error{"unable to create logging config directory"};
    }
    const auto path = directory / "config.json";
    std::ofstream output{path};
    output << contents;
    if (!output) {
        throw std::runtime_error{"unable to write logging config"};
    }
    return path;
}

class ScopedConfig final {
public:
    explicit ScopedConfig(const std::string_view contents)
        : path_(make_config(contents)) {}

    ~ScopedConfig() {
        std::error_code ignored;
        std::filesystem::remove_all(path_.parent_path(), ignored);
    }

    [[nodiscard]] const std::filesystem::path &path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void expect_rejected(const std::string_view contents) {
    ScopedConfig config{contents};
    const auto result = iaisf::load_app_config(config.path());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, iaisf::ErrorCode::ConfigError);
}

TEST(LoggingConfigTest, LegacyLevelOnlyConfigurationKeepsSafeDefaults) {
    ScopedConfig config{R"({"logging":{"level":"info"}})"};
    const auto result = iaisf::load_app_config(config.path());
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result.value().logging.level, iaisf::LogLevel::Info);
    EXPECT_TRUE(result.value().logging.console.enabled);
    EXPECT_FALSE(result.value().logging.file.enabled);
}

TEST(LoggingConfigTest, LoadsCompleteLoggingConfiguration) {
    ScopedConfig config{R"({
      "logging": {
        "level": "debug",
        "queue_capacity": 256,
        "reserved_critical_capacity": 16,
        "batch_size": 32,
        "flush_interval_ms": 250,
        "console": {"enabled": false},
        "file": {
          "enabled": true,
          "path": "logs/server.log",
          "max_file_bytes": 1048576,
          "max_archives": 5
        }
      }
    })"};
    const auto result = iaisf::load_app_config(config.path());
    ASSERT_TRUE(result) << result.error().message;
    const auto &logging = result.value().logging;
    EXPECT_EQ(logging.level, iaisf::LogLevel::Debug);
    EXPECT_EQ(logging.queue_capacity, 256U);
    EXPECT_EQ(logging.reserved_critical_capacity, 16U);
    EXPECT_EQ(logging.batch_size, 32U);
    EXPECT_EQ(logging.flush_interval_ms, 250U);
    EXPECT_FALSE(logging.console.enabled);
    EXPECT_TRUE(logging.file.enabled);
    EXPECT_EQ(logging.file.path, "logs/server.log");
    EXPECT_EQ(logging.file.max_file_bytes, 1048576U);
    EXPECT_EQ(logging.file.max_archives, 5U);
}

TEST(LoggingConfigTest, RejectsUnknownTypeAndRangeErrors) {
    expect_rejected(R"({"logging":{"unknown":true}})");
    expect_rejected(R"({"logging":{"queue_capacity":"10"}})");
    expect_rejected(R"({"logging":{"flush_interval_ms":-1}})");
    expect_rejected(R"({"logging":{"batch_size":0}})");
    expect_rejected(R"({"logging":{"reserved_critical_capacity":10,"queue_capacity":4}})");
}

TEST(LoggingConfigTest, RequiresASinkAndValidFilePath) {
    expect_rejected(R"({"logging":{"console":{"enabled":false}}})");
    expect_rejected(R"({"logging":{"file":{"enabled":true,"path":""}}})");
    expect_rejected(R"({"logging":{"file":{"enabled":true,"path":"bad\u0001path"}}})");
}

} // namespace
