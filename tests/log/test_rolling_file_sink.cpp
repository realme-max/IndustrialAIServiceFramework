#include "iaisf/logging/async_logger.hpp"
#include "iaisf/logging/console_sink.hpp"
#include "iaisf/logging/rolling_file_sink.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace iaisf {
namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path() /
                ("iaisf-rolling-" + std::to_string(stamp));
        std::error_code error;
        std::filesystem::create_directories(root_, error);
        if (error != std::error_code{}) {
            throw std::runtime_error{"unable to create temporary directory"};
        }
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return root_;
    }

private:
    std::filesystem::path root_;
};

LogRecord record(const std::uint64_t sequence) {
    LogRecord value;
    value.sequence = sequence;
    value.level = LogLevel::Info;
    value.component = "test";
    value.message = "message";
    return value;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::in | std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

Result<std::unique_ptr<RollingFileSink>> make_sink(
    const std::filesystem::path& path,
    const std::uintmax_t max_file_bytes,
    const std::size_t max_archives) {
    auto options = RollingFileSinkOptions::create(path, max_file_bytes, max_archives);
    if (!options) {
        return Result<std::unique_ptr<RollingFileSink>>::failure(options.error());
    }
    return RollingFileSink::create(std::move(options.value()));
}

class FailingSink final : public ILogSink {
public:
    Result<void> write(
        const LogRecord& /*record*/,
        const std::string_view /*formatted_record*/) override {
        return Result<void>::failure(
            make_error(ErrorCode::IoError, "intentional sink failure"));
    }

    Result<void> flush() override {
        return Result<void>::failure(
            make_error(ErrorCode::IoError, "intentional sink flush failure"));
    }
};

TEST(RollingFileSinkTest, CreatesAndFlushesFile) {
    TemporaryDirectory directory;
    const auto file = directory.path() / "server.log";
    auto sink_result = make_sink(file, 64U, 2U);
    ASSERT_TRUE(sink_result.has_value());
    auto sink = std::move(sink_result.value());

    ASSERT_TRUE(sink->write(record(1U), "first\n").has_value());
    ASSERT_TRUE(sink->flush().has_value());
    EXPECT_EQ(read_file(file), "first\n");
}

TEST(RollingFileSinkTest, AppendsToExistingFile) {
    TemporaryDirectory directory;
    const auto file = directory.path() / "server.log";
    {
        std::ofstream output{file, std::ios::binary};
        output << "old\n";
    }

    auto sink_result = make_sink(file, 64U, 2U);
    ASSERT_TRUE(sink_result.has_value());
    auto sink = std::move(sink_result.value());
    ASSERT_TRUE(sink->write(record(1U), "new\n").has_value());
    ASSERT_TRUE(sink->flush().has_value());
    EXPECT_EQ(read_file(file), "old\nnew\n");
}

TEST(RollingFileSinkTest, RotatesOnlyAfterTheExactSizeBoundary) {
    TemporaryDirectory directory;
    const auto file = directory.path() / "server.log";
    auto sink_result = make_sink(file, 6U, 2U);
    ASSERT_TRUE(sink_result.has_value());
    auto sink = std::move(sink_result.value());

    ASSERT_TRUE(sink->write(record(1U), "123456").has_value());
    EXPECT_FALSE(std::filesystem::exists(file.string() + ".1"));
    ASSERT_TRUE(sink->write(record(2U), "7").has_value());
    ASSERT_TRUE(sink->flush().has_value());
    EXPECT_EQ(read_file(file), "7");
    EXPECT_EQ(read_file(file.string() + ".1"), "123456");
}

TEST(RollingFileSinkTest, RotatesInOrderAndDeletesOldestArchive) {
    TemporaryDirectory directory;
    const auto file = directory.path() / "server.log";
    auto sink_result = make_sink(file, 1U, 3U);
    ASSERT_TRUE(sink_result.has_value());
    auto sink = std::move(sink_result.value());

    ASSERT_TRUE(sink->write(record(1U), "A").has_value());
    ASSERT_TRUE(sink->write(record(2U), "B").has_value());
    ASSERT_TRUE(sink->write(record(3U), "C").has_value());
    ASSERT_TRUE(sink->write(record(4U), "D").has_value());
    ASSERT_TRUE(sink->write(record(5U), "E").has_value());
    ASSERT_TRUE(sink->flush().has_value());

    EXPECT_EQ(read_file(file), "E");
    EXPECT_EQ(read_file(file.string() + ".1"), "D");
    EXPECT_EQ(read_file(file.string() + ".2"), "C");
    EXPECT_EQ(read_file(file.string() + ".3"), "B");
    EXPECT_FALSE(std::filesystem::exists(file.string() + ".4"));
}

TEST(RollingFileSinkTest, ShiftsPreexistingArchivesBeforeRotation) {
    TemporaryDirectory directory;
    const auto file = directory.path() / "server.log";
    {
        std::ofstream current{file, std::ios::binary};
        current << "current";
        std::ofstream archive_one{file.string() + ".1", std::ios::binary};
        archive_one << "archive-one";
        std::ofstream archive_two{file.string() + ".2", std::ios::binary};
        archive_two << "archive-two";
    }

    auto sink_result = make_sink(file, 7U, 2U);
    ASSERT_TRUE(sink_result.has_value());
    auto sink = std::move(sink_result.value());
    ASSERT_TRUE(sink->write(record(1U), "next").has_value());
    ASSERT_TRUE(sink->flush().has_value());

    EXPECT_EQ(read_file(file), "next");
    EXPECT_EQ(read_file(file.string() + ".1"), "current");
    EXPECT_EQ(read_file(file.string() + ".2"), "archive-one");
    EXPECT_FALSE(std::filesystem::exists(file.string() + ".3"));
}

TEST(RollingFileSinkTest, ZeroArchivesTruncatesAtTheSizeBoundary) {
    TemporaryDirectory directory;
    const auto file = directory.path() / "server.log";
    auto sink_result = make_sink(file, 3U, 0U);
    ASSERT_TRUE(sink_result.has_value());
    auto sink = std::move(sink_result.value());
    ASSERT_TRUE(sink->write(record(1U), "old").has_value());
    ASSERT_TRUE(sink->write(record(2U), "new").has_value());
    ASSERT_TRUE(sink->flush().has_value());

    EXPECT_EQ(read_file(file), "new");
    EXPECT_FALSE(std::filesystem::exists(file.string() + ".1"));
}

TEST(RollingFileSinkTest, ReportsOpenFailure) {
    TemporaryDirectory directory;
    const auto file = directory.path() / "missing" / "server.log";
    auto sink_result = make_sink(file, 64U, 1U);
    EXPECT_FALSE(sink_result.has_value());
    ASSERT_FALSE(sink_result.has_value());
    EXPECT_EQ(sink_result.error().code, ErrorCode::IoError);
}

TEST(RollingFileSinkTest, ReportsRenameFailureWithoutLosingCurrentFile) {
    TemporaryDirectory directory;
    const auto file = directory.path() / "server.log";
    auto sink_result = make_sink(file, 1U, 1U);
    ASSERT_TRUE(sink_result.has_value());
    auto sink = std::move(sink_result.value());
    ASSERT_TRUE(sink->write(record(1U), "A").has_value());

    std::error_code error;
    const auto blocker = std::filesystem::path{file.string() + ".1"} / "block";
    std::filesystem::create_directories(blocker, error);
    ASSERT_FALSE(error);

    const auto result = sink->write(record(2U), "B");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::IoError);
    ASSERT_TRUE(sink->flush().has_value());
    EXPECT_EQ(read_file(file), "A");
}

TEST(RollingFileSinkTest, ReportsWriteFailure) {
#if defined(_WIN32)
    GTEST_SKIP() << "Windows ACL fault injection is not deterministic in this test environment";
#else
    auto sink_result = make_sink("/dev/full", 64U, 1U);
    ASSERT_TRUE(sink_result.has_value());
    auto sink = std::move(sink_result.value());
    const auto result = sink->write(record(1U), "write-failure\n");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::IoError);
#endif
}

TEST(RollingFileSinkTest, ReportsFlushFailure) {
#if defined(_WIN32)
    GTEST_SKIP() << "Windows ACL fault injection is not deterministic in this test environment";
#else
    auto sink_result = make_sink("/dev/full", 64U, 1U);
    ASSERT_TRUE(sink_result.has_value());
    auto sink = std::move(sink_result.value());
    ASSERT_FALSE(sink->write(record(1U), "flush-failure\n").has_value());
    const auto result = sink->flush();
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::IoError);
#endif
}

TEST(RollingFileSinkTest, FailureOfOneSinkDoesNotStopOtherSinks) {
    TemporaryDirectory directory;
    const auto file = directory.path() / "server.log";
    auto file_sink_result = make_sink(file, 64U, 1U);
    ASSERT_TRUE(file_sink_result.has_value());

    std::ostringstream console_output;
    std::vector<std::unique_ptr<ILogSink>> sinks;
    sinks.push_back(std::move(file_sink_result.value()));
    sinks.push_back(std::make_unique<FailingSink>());
    sinks.push_back(std::make_unique<ConsoleSink>(console_output));
    auto options = AsyncLoggerOptions::create(16U, 0U, 4U, 64U, 256U, 1024U, LogLevel::Trace);
    ASSERT_TRUE(options.has_value());
    auto logger_result = AsyncLogger::create(std::move(options.value()), std::move(sinks));
    ASSERT_TRUE(logger_result.has_value());
    auto logger = std::move(logger_result.value());

    logger->log(LogLevel::Info, "file", "survives sink failure");
    const auto shutdown = logger->shutdown();
    EXPECT_FALSE(shutdown.has_value());
    logger.reset();

    EXPECT_NE(read_file(file).find("survives sink failure"), std::string::npos);
    EXPECT_NE(console_output.str().find("survives sink failure"), std::string::npos);
}

}  // namespace
}  // namespace iaisf
