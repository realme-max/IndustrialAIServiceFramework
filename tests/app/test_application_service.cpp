#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

class ScopedConfigFile final {
  public:
    ScopedConfigFile() {
        path_ = std::filesystem::temp_directory_path() /
                ("iaisf-serve-" + std::to_string(::getpid()) + ".json");
        std::ofstream output{path_, std::ios::trunc};
        output << R"({
          "service":{"name":"application-integration"},
          "server":{
            "host":"127.0.0.1",
            "port":0,
            "reactor":{"max_events":64,"pending_callback_capacity":64,
                       "max_timers":16},
            "tcp":{"max_connections":8}
          },
          "runtime":{"worker_threads":1,"task_queue_capacity":8}
        })";
        if (!output) {
            throw std::runtime_error{"unable to write application test config"};
        }
    }

    ~ScopedConfigFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path &path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

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
            const std::string path = config.string();
            ::execl(IAISF_SERVER_PATH, IAISF_SERVER_PATH, "--serve", "--config",
                    path.c_str(), static_cast<char *>(nullptr));
            ::_exit(127);
        }
        ::close(write_fd);
    }

    ChildProcess(const ChildProcess &) = delete;
    ChildProcess &operator=(const ChildProcess &) = delete;

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
        while (output.find("service started on") == std::string::npos) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
            if (remaining <= std::chrono::milliseconds::zero()) {
                return false;
            }
            pollfd descriptor{read_fd_, POLLIN, 0};
            const int timeout = static_cast<int>(remaining.count());
            const int poll_result = ::poll(&descriptor, 1, timeout);
            if (poll_result <= 0) {
                return false;
            }
            char buffer[512]{};
            const ssize_t count = ::read(read_fd_, buffer, sizeof(buffer));
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

  private:
    pid_t pid_{-1};
    int read_fd_{-1};
    bool waited_{false};
};

TEST(ApplicationServiceTest, ServeStartsAndSigtermPerformsFullShutdown) {
    const ScopedConfigFile config;
    ChildProcess child{config.path()};
    std::string output;
    ASSERT_TRUE(child.wait_for_start(output)) << output;

    const int status = child.terminate_and_wait();
    ASSERT_NE(status, -1);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

} // namespace
