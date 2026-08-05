#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <gtest/gtest.h>

#include "iaisf/app/application.hpp"

#if defined(__linux__) && defined(IAISF_SERVER_PATH)
#include <cerrno>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

std::filesystem::path make_directory(const std::string_view prefix) {
    static std::atomic<unsigned long> sequence{0U};
    const auto directory = std::filesystem::temp_directory_path() /
                           (std::string{prefix} + "-" +
                            std::to_string(
                                std::chrono::steady_clock::now()
                                    .time_since_epoch()
                                    .count()) +
                            "-" + std::to_string(sequence.fetch_add(
                                              1U, std::memory_order_relaxed)));
    std::error_code error;
    if (!std::filesystem::create_directory(directory, error)) {
        throw std::runtime_error{"unable to create application test directory"};
    }
    return directory;
}

class ScopedConfig final {
public:
    explicit ScopedConfig(const std::string_view contents)
        : directory_(make_directory("iaisf-application-logging")),
          path_(directory_ / "config.json") {
        std::ofstream output{path_};
        output << contents;
        if (!output) {
            throw std::runtime_error{"unable to write application test config"};
        }
    }

    ~ScopedConfig() {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    [[nodiscard]] const std::filesystem::path &path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path directory_;
    std::filesystem::path path_;
};

TEST(ApplicationLoggingTest, ConfigModeValidatesWithoutCreatingFileSink) {
    const auto directory = make_directory("iaisf-config-only");
    const auto config_path = directory / "config.json";
    const auto log_path = directory / "logs" / "server.log";
    std::ofstream output{config_path};
    output << R"({"logging":{"file":{"enabled":true,"path":")"
            << log_path.generic_string()
            << R"("}}})";
    ASSERT_TRUE(output);
    output.close();

    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    iaisf::Application application{stdout_stream, stderr_stream};
    EXPECT_EQ(application.run({"--config", config_path.string()}), 0)
        << stderr_stream.str();
    EXPECT_FALSE(std::filesystem::exists(log_path));

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

#if defined(__linux__) && defined(IAISF_SERVER_PATH)

class ChildProcess final {
public:
    explicit ChildProcess(const std::filesystem::path &config) {
        int descriptors[2]{};
        if (::pipe(descriptors) != 0) {
            throw std::runtime_error{"pipe failed"};
        }
        read_fd_ = descriptors[0];
        const int write_fd = descriptors[1];
        pid_ = ::fork();
        if (pid_ < 0) {
            ::close(read_fd_);
            ::close(write_fd);
            throw std::runtime_error{"fork failed"};
        }
        if (pid_ == 0) {
            (void)::dup2(write_fd, STDOUT_FILENO);
            (void)::dup2(write_fd, STDERR_FILENO);
            ::close(read_fd_);
            ::close(write_fd);
            const auto path = config.string();
            ::execl(IAISF_SERVER_PATH, IAISF_SERVER_PATH, "--serve", "--config",
                    path.c_str(), static_cast<char *>(nullptr));
            ::_exit(127);
        }
        ::close(write_fd);
    }

    ~ChildProcess() {
        if (read_fd_ >= 0) {
            ::close(read_fd_);
        }
        if (pid_ > 0 && !waited_) {
            (void)::kill(pid_, SIGKILL);
            int status = 0;
            (void)::waitpid(pid_, &status, 0);
        }
    }

    [[nodiscard]] bool wait_for_start(std::string &output) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (output.find("service started") == std::string::npos) {
            const auto remaining = std::chrono::duration_cast<
                std::chrono::milliseconds>(deadline -
                                           std::chrono::steady_clock::now());
            if (remaining <= std::chrono::milliseconds::zero()) {
                return false;
            }
            pollfd descriptor{read_fd_, POLLIN, 0};
            const int timeout = static_cast<int>(remaining.count());
            if (::poll(&descriptor, 1, timeout) <= 0) {
                return false;
            }
            char buffer[512]{};
            const auto count = ::read(read_fd_, buffer, sizeof(buffer));
            if (count <= 0) {
                return false;
            }
            output.append(buffer, static_cast<std::size_t>(count));
        }
        return true;
    }

    [[nodiscard]] int terminate_and_wait() {
        if (::kill(pid_, SIGTERM) != 0) {
            return -1;
        }
        int status = 0;
        while (::waitpid(pid_, &status, 0) < 0) {
            if (errno != EINTR) {
                return -1;
            }
        }
        waited_ = true;
        return status;
    }

    [[nodiscard]] int wait_for_exit() {
        int status = 0;
        while (::waitpid(pid_, &status, 0) < 0) {
            if (errno != EINTR) {
                return -1;
            }
        }
        waited_ = true;
        return status;
    }

    [[nodiscard]] std::string read_all() {
        std::string output;
        char buffer[512]{};
        for (;;) {
            const auto count = ::read(read_fd_, buffer, sizeof(buffer));
            if (count <= 0) {
                break;
            }
            output.append(buffer, static_cast<std::size_t>(count));
        }
        return output;
    }

private:
    pid_t pid_{-1};
    int read_fd_{-1};
    bool waited_{false};
};

TEST(ApplicationLoggingTest, ServeCreatesAsyncLoggerAndFlushesOnSigterm) {
    const auto directory = make_directory("iaisf-serve-logging");
    const auto config_path = directory / "config.json";
    const auto log_path = directory / "server.log";
    std::ofstream output{config_path};
    output << R"({
      "server":{"host":"127.0.0.1","port":0,
        "reactor":{"max_events":64,"pending_callback_capacity":64,"max_timers":16},
        "tcp":{"max_connections":8}},
      "runtime":{"worker_threads":1,"task_queue_capacity":8},
      "logging":{"file":{"enabled":true,"path":")"
            << log_path.generic_string() << R"("}}
    })";
    ASSERT_TRUE(output);
    output.close();

    ChildProcess child{config_path};
    std::string process_output;
    ASSERT_TRUE(child.wait_for_start(process_output)) << process_output;
    const int status = child.terminate_and_wait();
    ASSERT_NE(status, -1);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);

    std::ifstream log{log_path};
    ASSERT_TRUE(log.is_open());
    const std::string contents{std::istreambuf_iterator<char>{log},
                               std::istreambuf_iterator<char>{}};
    EXPECT_NE(contents.find("service started"), std::string::npos);

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

TEST(ApplicationLoggingTest, LoggerCreationFailurePreventsServiceStart) {
    const auto directory = make_directory("iaisf-serve-logging-failure");
    const auto config_path = directory / "config.json";
    const auto log_path = directory / "missing" / "server.log";
    std::ofstream output{config_path};
    output << R"({"server":{"host":"127.0.0.1","port":0},
      "logging":{"file":{"enabled":true,"path":")"
            << log_path.generic_string() << R"("}}})";
    ASSERT_TRUE(output);
    output.close();

    ChildProcess child{config_path};
    const int status = child.wait_for_exit();
    ASSERT_NE(status, -1);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 1);
    EXPECT_NE(child.read_all().find("logging startup failed"), std::string::npos);

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

#endif

} // namespace
