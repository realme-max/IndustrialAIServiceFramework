#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <future>
#include <string>

#include "iaisf/application/application_process.hpp"

#if !defined(_WIN32)
#include <cerrno>
#include <csignal>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

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

TEST(ApplicationProcessTest, PreservesWorkingDirectoryArgumentsAndExitStatus) {
    auto runner = LocalProcessRunner::create();
    ASSERT_TRUE(runner);
    const ProcessSpec spec{
        std::filesystem::path{"/bin/sh"},
        {"-c", "printf '%s:%s' \"$1\" \"$PWD\"", "sh", "runner-arg"},
        std::filesystem::path{"/tmp"}, std::chrono::seconds{2}, 1024U, 1024U};
    const auto result = runner.value()->run(spec);
    ASSERT_TRUE(result);
    EXPECT_FALSE(result.value().timed_out);
    EXPECT_EQ(result.value().exit_code, 0);
    EXPECT_EQ(result.value().stdout_text, "runner-arg:/tmp");
}

TEST(ApplicationProcessTest, MissingExecutableReturnsItsExecFailureExitCode) {
    auto runner = LocalProcessRunner::create();
    ASSERT_TRUE(runner);
    const ProcessSpec spec{
        std::filesystem::path{"/definitely/missing/iaisf-process"}, {}, {},
        std::chrono::seconds{2}, 1024U, 1024U};
    const auto result = runner.value()->run(spec);
    ASSERT_TRUE(result);
    EXPECT_FALSE(result.value().timed_out);
    EXPECT_EQ(result.value().exit_code, 127);
}

TEST(ApplicationProcessTest, NonZeroExitIsReturnedWithoutRunnerFailure) {
    auto runner = LocalProcessRunner::create();
    ASSERT_TRUE(runner);
    const ProcessSpec spec{
        std::filesystem::path{"/bin/sh"}, {"-c", "exit 7"}, {},
        std::chrono::seconds{2}, 1024U, 1024U};
    const auto result = runner.value()->run(spec);
    ASSERT_TRUE(result);
    EXPECT_FALSE(result.value().timed_out);
    EXPECT_EQ(result.value().exit_code, 7);
}

TEST(ApplicationProcessTest, TimeoutReapsChildAndReturnsWithinBound) {
    auto runner = LocalProcessRunner::create();
    ASSERT_TRUE(runner);
    const ProcessSpec spec{
        std::filesystem::path{"/bin/sh"}, {"-c", "sleep 5"}, {},
        std::chrono::milliseconds{100}, 1024U, 1024U};
    const auto started = std::chrono::steady_clock::now();
    const auto result = runner.value()->run(spec);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    ASSERT_TRUE(result);
    EXPECT_TRUE(result.value().timed_out);
    EXPECT_LT(elapsed, std::chrono::seconds{2});
    int status = 0;
    errno = 0;
    EXPECT_EQ(::waitpid(-1, &status, WNOHANG), -1);
    EXPECT_EQ(errno, ECHILD);
}

TEST(ApplicationProcessTest, DescendantHoldingPipeCannotExtendRunnerTimeout) {
    auto runner = LocalProcessRunner::create();
    ASSERT_TRUE(runner);
    const ProcessSpec spec{
        std::filesystem::path{"/bin/sh"}, {"-c", "sleep 1 & exit 0"}, {},
        std::chrono::milliseconds{100}, 1024U, 1024U};
    const auto started = std::chrono::steady_clock::now();
    const auto result = runner.value()->run(spec);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    ASSERT_TRUE(result);
    EXPECT_TRUE(result.value().timed_out);
    EXPECT_LT(elapsed, std::chrono::seconds{2});
    EXPECT_EQ(result.value().exit_code, 0);
}

TEST(ApplicationProcessTest, DistinguishesStdoutAndStderrLimitFailures) {
    auto runner = LocalProcessRunner::create();
    ASSERT_TRUE(runner);
    const ProcessSpec stdout_spec{
        std::filesystem::path{"/usr/bin/yes"}, {"x"}, {},
        std::chrono::seconds{2}, 1024U, 1024U};
    const auto stdout_result = runner.value()->run(stdout_spec);
    ASSERT_FALSE(stdout_result);
    EXPECT_EQ(stdout_result.error().code, ErrorCode::ResourceExhausted);

    const ProcessSpec stderr_spec{
        std::filesystem::path{"/bin/sh"}, {"-c", "yes x >&2"}, {},
        std::chrono::seconds{2}, 1024U, 1024U};
    const auto stderr_result = runner.value()->run(stderr_spec);
    ASSERT_FALSE(stderr_result);
    EXPECT_EQ(stderr_result.error().code, ErrorCode::ResourceExhausted);
}

TEST(ApplicationProcessTest, CapturesBothOutputStreamsAndClosesPipes) {
    auto runner = LocalProcessRunner::create();
    ASSERT_TRUE(runner);
    const ProcessSpec spec{
        std::filesystem::path{"/bin/sh"},
        {"-c", "printf out; printf err >&2"}, {},
        std::chrono::seconds{2}, 1024U, 1024U};
    const auto result = runner.value()->run(spec);
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().stdout_text, "out");
    EXPECT_EQ(result.value().stderr_text, "err");
}

TEST(ApplicationProcessTest, RetriesInterruptedWaitAndPoll) {
    auto runner = LocalProcessRunner::create();
    ASSERT_TRUE(runner);
    struct sigaction action{};
    struct sigaction previous{};
    action.sa_handler = [](int) {};
    sigemptyset(&action.sa_mask);
    ASSERT_EQ(::sigaction(SIGALRM, &action, &previous), 0);
    struct itimerval timer{};
    timer.it_value.tv_usec = 1000;
    timer.it_interval.tv_usec = 1000;
    ASSERT_EQ(::setitimer(ITIMER_REAL, &timer, nullptr), 0);
    const ProcessSpec spec{
        std::filesystem::path{"/bin/sh"}, {"-c", "sleep 0.05; printf ok"}, {},
        std::chrono::seconds{2}, 1024U, 1024U};
    const auto result = runner.value()->run(spec);
    timer = {};
    ASSERT_EQ(::setitimer(ITIMER_REAL, &timer, nullptr), 0);
    ASSERT_EQ(::sigaction(SIGALRM, &previous, nullptr), 0);
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().exit_code, 0);
    EXPECT_EQ(result.value().stdout_text, "ok");
}

TEST(ApplicationProcessTest, RepeatedShortProcessesLeaveNoDirectChildren) {
    auto runner = LocalProcessRunner::create();
    ASSERT_TRUE(runner);
    for (int index = 0; index < 32; ++index) {
        const ProcessSpec spec{
            std::filesystem::path{"/usr/bin/true"}, {}, {},
            std::chrono::seconds{2}, 1024U, 1024U};
        const auto result = runner.value()->run(spec);
        ASSERT_TRUE(result);
        EXPECT_EQ(result.value().exit_code, 0);
    }
    int status = 0;
    errno = 0;
    EXPECT_EQ(::waitpid(-1, &status, WNOHANG), -1);
    EXPECT_EQ(errno, ECHILD);
}

TEST(ApplicationProcessTest, SupportsConcurrentRunnerCalls) {
    auto runner = LocalProcessRunner::create();
    ASSERT_TRUE(runner);
    std::vector<std::future<bool>> futures;
    for (int worker = 0; worker < 4; ++worker) {
        futures.push_back(std::async(std::launch::async, [&runner] {
            for (int iteration = 0; iteration < 8; ++iteration) {
                const ProcessSpec spec{
                    std::filesystem::path{"/usr/bin/true"}, {}, {},
                    std::chrono::seconds{2}, 1024U, 1024U};
                const auto result = runner.value()->run(spec);
                if (!result || result.value().exit_code != 0) {
                    return false;
                }
            }
            return true;
        }));
    }
    for (auto& future : futures) {
        EXPECT_TRUE(future.get());
    }
}
#endif

}  // namespace
}  // namespace iaisf::application
