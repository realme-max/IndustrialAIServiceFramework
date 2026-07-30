#include <algorithm>
#include <array>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "iaisf/core/error.hpp"
#include "iaisf/logging/console_logger.hpp"
#include "iaisf/logging/log_level.hpp"

namespace {

TEST(LogLevelTest, ParsesAllSupportedLowercaseNames) {
    const std::array<std::pair<std::string_view, iaisf::LogLevel>, 5> cases{{
        {"trace", iaisf::LogLevel::Trace},
        {"debug", iaisf::LogLevel::Debug},
        {"info", iaisf::LogLevel::Info},
        {"warn", iaisf::LogLevel::Warn},
        {"error", iaisf::LogLevel::Error},
    }};

    for (const auto& [text, expected] : cases) {
        const auto parsed = iaisf::parse_log_level(text);
        ASSERT_TRUE(parsed.has_value()) << text;
        EXPECT_EQ(parsed.value(), expected);
    }
}

TEST(LogLevelTest, ConvertsAllLevelsAndRejectsInvalidText) {
    EXPECT_EQ(iaisf::to_string(iaisf::LogLevel::Trace), "TRACE");
    EXPECT_EQ(iaisf::to_string(iaisf::LogLevel::Debug), "DEBUG");
    EXPECT_EQ(iaisf::to_string(iaisf::LogLevel::Info), "INFO");
    EXPECT_EQ(iaisf::to_string(iaisf::LogLevel::Warn), "WARN");
    EXPECT_EQ(iaisf::to_string(iaisf::LogLevel::Error), "ERROR");
    EXPECT_EQ(
        iaisf::to_string(static_cast<iaisf::LogLevel>(999)),
        "UNKNOWN");

    const auto invalid = iaisf::parse_log_level("INFO");
    ASSERT_FALSE(invalid.has_value());
    EXPECT_EQ(invalid.error().code, iaisf::ErrorCode::InvalidArgument);
}

std::string emit_all_levels(const iaisf::LogLevel threshold) {
    std::ostringstream output;
    iaisf::ConsoleLogger logger{threshold, output};
    logger.log(iaisf::LogLevel::Trace, "test", "trace-message");
    logger.log(iaisf::LogLevel::Debug, "test", "debug-message");
    logger.log(iaisf::LogLevel::Info, "test", "info-message");
    logger.log(iaisf::LogLevel::Warn, "test", "warn-message");
    logger.log(iaisf::LogLevel::Error, "test", "error-message");
    return output.str();
}

TEST(ConsoleLoggerTest, AppliesEverySupportedThreshold) {
    const std::string trace_output = emit_all_levels(iaisf::LogLevel::Trace);
    EXPECT_NE(trace_output.find("trace-message"), std::string::npos);
    EXPECT_NE(trace_output.find("debug-message"), std::string::npos);
    EXPECT_NE(trace_output.find("info-message"), std::string::npos);
    EXPECT_NE(trace_output.find("warn-message"), std::string::npos);
    EXPECT_NE(trace_output.find("error-message"), std::string::npos);

    const std::string debug_output = emit_all_levels(iaisf::LogLevel::Debug);
    EXPECT_EQ(debug_output.find("trace-message"), std::string::npos);
    EXPECT_NE(debug_output.find("debug-message"), std::string::npos);

    const std::string info_output = emit_all_levels(iaisf::LogLevel::Info);
    EXPECT_EQ(info_output.find("debug-message"), std::string::npos);
    EXPECT_NE(info_output.find("info-message"), std::string::npos);

    const std::string warn_output = emit_all_levels(iaisf::LogLevel::Warn);
    EXPECT_EQ(warn_output.find("info-message"), std::string::npos);
    EXPECT_NE(warn_output.find("warn-message"), std::string::npos);

    const std::string error_output = emit_all_levels(iaisf::LogLevel::Error);
    EXPECT_EQ(error_output.find("warn-message"), std::string::npos);
    EXPECT_NE(error_output.find("error-message"), std::string::npos);
}

TEST(ConsoleLoggerTest, EmitsCompleteSingleLineRecords) {
    std::ostringstream output;
    iaisf::ConsoleLogger logger{iaisf::LogLevel::Info, output};
    logger.log(iaisf::LogLevel::Info, "Application", "configuration validated");

    const std::string record = output.str();
    EXPECT_NE(record.find("[INFO]"), std::string::npos);
    EXPECT_NE(record.find("[Application]"), std::string::npos);
    EXPECT_NE(record.find("configuration validated"), std::string::npos);
    ASSERT_FALSE(record.empty());
    EXPECT_EQ(record.back(), '\n');
    EXPECT_EQ(std::count(record.begin(), record.end(), '\n'), 1);
}

TEST(ConsoleLoggerTest, DoesNotGlueConsecutiveRecordsTogether) {
    std::ostringstream output;
    iaisf::ConsoleLogger logger{iaisf::LogLevel::Trace, output};
    logger.log(iaisf::LogLevel::Info, "first", "one");
    logger.log(iaisf::LogLevel::Warn, "second", "two");

    const std::string records = output.str();
    EXPECT_EQ(std::count(records.begin(), records.end(), '\n'), 2);
    EXPECT_NE(records.find("one\n"), std::string::npos);
    EXPECT_NE(records.find("two\n"), std::string::npos);
}

TEST(ConsoleLoggerTest, SanitizesEmbeddedLineBreaks) {
    std::ostringstream output;
    iaisf::ConsoleLogger logger{iaisf::LogLevel::Info, output};
    logger.log(iaisf::LogLevel::Info, "component\nname", "line1\r\nline2");

    const std::string record = output.str();
    EXPECT_EQ(std::count(record.begin(), record.end(), '\n'), 1);
    EXPECT_NE(record.find("component\\nname"), std::string::npos);
    EXPECT_NE(record.find("line1\\r\\nline2"), std::string::npos);
}

TEST(ConsoleLoggerTest, AllowsThresholdChanges) {
    std::ostringstream output;
    iaisf::ConsoleLogger logger{iaisf::LogLevel::Error, output};
    logger.log(iaisf::LogLevel::Info, "test", "hidden");
    logger.set_threshold(iaisf::LogLevel::Info);
    logger.log(iaisf::LogLevel::Info, "test", "visible");

    EXPECT_EQ(logger.threshold(), iaisf::LogLevel::Info);
    EXPECT_EQ(output.str().find("hidden"), std::string::npos);
    EXPECT_NE(output.str().find("visible"), std::string::npos);
}

}  // namespace

