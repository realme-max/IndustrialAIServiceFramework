#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <future>
#include <string>

#include "iaisf/application/application_process.hpp"

namespace iaisf::application {
namespace {

class RecordingRunner final : public IProcessRunner {
public:
    Result<ProcessResult> run(const ProcessSpec& spec) override {
        executable = spec.executable;
        arguments = spec.arguments;
        working_directory = spec.working_directory;
        return Result<ProcessResult>::success(ProcessResult{7, false, 1.0, "ok", ""});
    }

    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::filesystem::path working_directory;
};

TEST(ApplicationProcessTest, KeepsExecutableArgumentsAndWorkingDirectorySeparate) {
    RecordingRunner runner;
    const ProcessSpec spec{
        std::filesystem::path{"/bin/tool"}, {"--flag", "value with spaces"},
        std::filesystem::path{"/tmp/job"}, std::chrono::seconds{2}, 1024U, 2048U};
    const auto result = runner.run(spec);
    ASSERT_TRUE(result);
    EXPECT_EQ(runner.executable, "/bin/tool");
    ASSERT_EQ(runner.arguments.size(), 2U);
    EXPECT_EQ(runner.arguments[1], "value with spaces");
    EXPECT_EQ(runner.working_directory, "/tmp/job");
    EXPECT_EQ(result.value().exit_code, 7);
}

TEST(ApplicationProcessTest, LocalRunnerRejectsInvalidLimitsWithoutLaunching) {
    auto runner = LocalProcessRunner::create();
    ASSERT_TRUE(runner);
    ProcessSpec spec;
    spec.executable = std::filesystem::path{"/definitely/missing"};
    spec.timeout = std::chrono::milliseconds::zero();
    const auto result = runner.value()->run(spec);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

#if !defined(_WIN32)
TEST(ApplicationProcessTest, NativeProcessOutputAndEofReturnPromptly) {
    auto runner = LocalProcessRunner::create();
    ASSERT_TRUE(runner);
    const ProcessSpec spec{
        std::filesystem::path{"/usr/bin/printf"}, {"runner-ok"}, {},
        std::chrono::seconds{2}, 1024U, 1024U};
    auto future = std::async(std::launch::async, [&] {
        return runner.value()->run(spec);
    });
    ASSERT_EQ(future.wait_for(std::chrono::seconds{2}),
              std::future_status::ready);
    const auto result = future.get();
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().exit_code, 0);
    EXPECT_FALSE(result.value().timed_out);
    EXPECT_EQ(result.value().stdout_text, "runner-ok");
    EXPECT_TRUE(result.value().stderr_text.empty());
}
#endif

}  // namespace
}  // namespace iaisf::application
