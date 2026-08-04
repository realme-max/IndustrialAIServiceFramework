#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "iaisf/app/application.hpp"

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

class ScopedApplicationConfig {
public:
    explicit ScopedApplicationConfig(const std::string_view contents) {
        directory_ = create_unique_temp_directory("iaisf-phase1-application");
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

    ~ScopedApplicationConfig() {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    ScopedApplicationConfig(const ScopedApplicationConfig&) = delete;
    ScopedApplicationConfig& operator=(const ScopedApplicationConfig&) = delete;

    [[nodiscard]] std::string path_string() const {
        return path_.string();
    }

private:
    std::filesystem::path directory_;
    std::filesystem::path path_;
};

TEST(ApplicationTest, VersionReturnsZeroAndStableOutput) {
    std::ostringstream output;
    std::ostringstream error;
    iaisf::Application application{output, error};

    EXPECT_EQ(application.run({"--version"}), 0);
    EXPECT_EQ(output.str(), "IndustrialAIServiceFramework 0.1.0\n");
    EXPECT_TRUE(error.str().empty());
}

TEST(ApplicationTest, HelpReturnsZeroAndContainsUsage) {
    std::ostringstream output;
    std::ostringstream error;
    iaisf::Application application{output, error};

    EXPECT_EQ(application.run({"--help"}), 0);
    EXPECT_NE(output.str().find("Usage:"), std::string::npos);
    EXPECT_NE(output.str().find("--config <path>"), std::string::npos);
    EXPECT_TRUE(error.str().empty());
}

TEST(ApplicationTest, NoArgumentsReturnsZeroWithoutStartingAService) {
    std::ostringstream output;
    std::ostringstream error;
    iaisf::Application application{output, error};

    EXPECT_EQ(application.run({}), 0);
    EXPECT_NE(output.str().find("0.1.0"), std::string::npos);
    EXPECT_NE(output.str().find("No service mode was selected"), std::string::npos);
    EXPECT_TRUE(error.str().empty());
}

TEST(ApplicationTest, RejectsUnknownArguments) {
    std::ostringstream output;
    std::ostringstream error;
    iaisf::Application application{output, error};

    EXPECT_EQ(application.run({"--unknown"}), 2);
    EXPECT_NE(error.str().find("argument error"), std::string::npos);
    EXPECT_NE(error.str().find("Usage:"), std::string::npos);
}

TEST(ApplicationTest, RejectsConfigWithoutAPath) {
    std::ostringstream output;
    std::ostringstream error;
    iaisf::Application application{output, error};

    EXPECT_EQ(application.run({"--config"}), 2);
    EXPECT_NE(error.str().find("requires exactly one"), std::string::npos);
}

TEST(ApplicationTest, AcceptsTheRealExampleConfiguration) {
    std::ostringstream output;
    std::ostringstream error;
    iaisf::Application application{output, error};

    EXPECT_EQ(
        application.run({"--config", IAISF_EXAMPLE_CONFIG_PATH}),
        0);
    EXPECT_NE(output.str().find("configuration validated"), std::string::npos);
    EXPECT_TRUE(error.str().empty());
}

TEST(ApplicationTest, ReportsValidationWhenConfiguredThresholdIsHigherThanInfo) {
    const ScopedApplicationConfig config{R"({"logging":{"level":"error"}})"};
    std::ostringstream output;
    std::ostringstream error;
    iaisf::Application application{output, error};

    EXPECT_EQ(application.run({"--config", config.path_string()}), 0);
    EXPECT_NE(output.str().find("[INFO]"), std::string::npos);
    EXPECT_NE(output.str().find("configuration validated"), std::string::npos);
    EXPECT_TRUE(error.str().empty());
}

TEST(ApplicationTest, ReportsMissingConfigurationWithoutThrowing) {
    const std::filesystem::path missing =
        std::filesystem::temp_directory_path() / "iaisf-app-missing-config.json";
    std::error_code ignored;
    std::filesystem::remove(missing, ignored);

    std::ostringstream output;
    std::ostringstream error;
    iaisf::Application application{output, error};
    int exit_code = 0;
    EXPECT_NO_THROW(exit_code = application.run({"--config", missing.string()}));
    EXPECT_EQ(exit_code, 1);
    EXPECT_NE(error.str().find("io_error"), std::string::npos);
    EXPECT_EQ(error.str().find(missing.string()), std::string::npos);
}

TEST(ApplicationTest, ReportsInvalidConfigurationWithoutThrowing) {
    const ScopedApplicationConfig config{R"({"runtime":{"worker_threads":0}})"};
    std::ostringstream output;
    std::ostringstream error;
    iaisf::Application application{output, error};
    int exit_code = 0;

    EXPECT_NO_THROW(
        exit_code = application.run({"--config", config.path_string()}));
    EXPECT_EQ(exit_code, 1);
    EXPECT_NE(error.str().find("config_error"), std::string::npos);
}

TEST(ApplicationTest, RejectsExtraAndConflictingArguments) {
    std::ostringstream output;
    std::ostringstream error;
    iaisf::Application application{output, error};

    EXPECT_EQ(application.run({"--config", "a.json", "extra"}), 2);
    EXPECT_EQ(application.run({"--version", "--config", "a.json"}), 2);
    EXPECT_EQ(application.run({"--help", "--config", "a.json"}), 2);
    EXPECT_EQ(application.run({"--serve"}), 2);
    EXPECT_EQ(application.run({"--serve", "a.json"}), 2);
    EXPECT_EQ(application.run({"--serve", "--config"}), 2);
}

#if defined(_WIN32)
TEST(ApplicationTest, ServeModeFailsClearlyWithoutLinuxRuntime) {
    std::ostringstream output;
    std::ostringstream error;
    iaisf::Application application{output, error};

    EXPECT_EQ(
        application.run({"--serve", "--config", IAISF_EXAMPLE_CONFIG_PATH}),
        1);
    EXPECT_NE(error.str().find("requires the Linux service runtime"),
              std::string::npos);
}
#endif

TEST(ApplicationTest, VersionAndHelpDoNotRequireAConfigurationFile) {
    std::ostringstream output;
    std::ostringstream error;
    iaisf::Application application{output, error};

    EXPECT_EQ(application.run({"--version"}), 0);
    EXPECT_EQ(application.run({"--help"}), 0);
    EXPECT_TRUE(error.str().empty());
}

}  // namespace
