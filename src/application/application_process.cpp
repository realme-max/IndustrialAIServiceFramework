#include "iaisf/application/application_process.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include "iaisf/core/error.hpp"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace iaisf::application {
namespace {

constexpr std::size_t kMaxCapturedBytes = 64U * 1024U * 1024U;

template <typename T>
Result<T> failure(const ErrorCode code, const char* message) {
    return Result<T>::failure(make_error(code, message));
}

Result<void> validate_spec(const ProcessSpec& spec) {
    if (spec.executable.empty() || spec.arguments.size() > 128U ||
        spec.timeout <= std::chrono::milliseconds::zero() ||
        spec.timeout > std::chrono::hours{24} || spec.max_stdout_bytes == 0U ||
        spec.max_stdout_bytes > kMaxCapturedBytes || spec.max_stderr_bytes == 0U ||
        spec.max_stderr_bytes > kMaxCapturedBytes) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument, "process specification is invalid"));
    }
    if (!spec.working_directory.empty()) {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(
            spec.working_directory, error);
        if (error || std::filesystem::is_symlink(status) ||
            !std::filesystem::is_directory(status)) {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidArgument,
                "process working directory is invalid"));
        }
    }
    return Result<void>::success();
}

#if defined(_WIN32)
Result<std::wstring> utf8_to_wide(const std::string& value) {
    if (value.empty()) {
        return Result<std::wstring>::success({});
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        return failure<std::wstring>(ErrorCode::InvalidArgument,
                                     "process path encoding is invalid");
    }
    std::wstring output(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), output.data(), size) != size) {
        return failure<std::wstring>(ErrorCode::InvalidArgument,
                                     "process path encoding is invalid");
    }
    return Result<std::wstring>::success(std::move(output));
}

std::wstring quote_windows_argument(const std::wstring& argument) {
    if (argument.empty()) {
        return L"\"\"";
    }
    bool needs_quotes = false;
    for (const wchar_t character : argument) {
        if (character == L' ' || character == L'\t' || character == L'\n' ||
            character == L'\v' || character == L'\"') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes) {
        return argument;
    }
    std::wstring output{L'\"'};
    std::size_t slashes = 0U;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        if (character == L'\"') {
            output.append(slashes * 2U + 1U, L'\\');
            output.push_back(L'\"');
            slashes = 0U;
            continue;
        }
        output.append(slashes, L'\\');
        slashes = 0U;
        output.push_back(character);
    }
    output.append(slashes * 2U, L'\\');
    output.push_back(L'\"');
    return output;
}

Result<void> read_pipe(
    HANDLE pipe,
    std::string& output,
    const std::size_t maximum,
    bool& open) {
    while (open) {
        DWORD available = 0U;
        if (!PeekNamedPipe(pipe, nullptr, 0U, nullptr, &available, nullptr)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) {
                open = false;
                break;
            }
            return failure<void>(ErrorCode::SystemError,
                                 "process output pipe read failed");
        }
        if (available == 0U) {
            break;
        }
        const auto room = maximum - output.size();
        if (room == 0U || available > room) {
            return failure<void>(ErrorCode::ResourceExhausted,
                                 "process output exceeded its limit");
        }
        std::string chunk(static_cast<std::size_t>(available), '\0');
        DWORD read = 0U;
        if (!ReadFile(pipe, chunk.data(), available, &read, nullptr)) {
            return failure<void>(ErrorCode::SystemError,
                                 "process output pipe read failed");
        }
        output.append(chunk.data(), static_cast<std::size_t>(read));
        if (read == 0U) {
            open = false;
        }
    }
    return Result<void>::success();
}

Result<ProcessResult> run_windows(const ProcessSpec& spec) {
    auto executable = utf8_to_wide(spec.executable.u8string());
    if (!executable) {
        return Result<ProcessResult>::failure(executable.error());
    }
    auto working_directory = utf8_to_wide(
        spec.working_directory.empty() ? std::string{} :
                                         spec.working_directory.u8string());
    if (!working_directory) {
        return Result<ProcessResult>::failure(working_directory.error());
    }
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stderr_read = nullptr;
    HANDLE stderr_write = nullptr;
    HANDLE stdin_handle = CreateFileW(
        L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (stdin_handle == INVALID_HANDLE_VALUE ||
        !CreatePipe(&stdout_read, &stdout_write, &security, 0U) ||
        !CreatePipe(&stderr_read, &stderr_write, &security, 0U)) {
        if (stdin_handle != INVALID_HANDLE_VALUE) CloseHandle(stdin_handle);
        if (stdout_read != nullptr) CloseHandle(stdout_read);
        if (stdout_write != nullptr) CloseHandle(stdout_write);
        if (stderr_read != nullptr) CloseHandle(stderr_read);
        if (stderr_write != nullptr) CloseHandle(stderr_write);
        return failure<ProcessResult>(ErrorCode::SystemError,
                                      "process pipe creation failed");
    }
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0U);
    SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0U);

    std::wstring command_line = quote_windows_argument(executable.value());
    for (const auto& argument : spec.arguments) {
        auto wide_argument = utf8_to_wide(argument);
        if (!wide_argument) {
            CloseHandle(stdin_handle); CloseHandle(stdout_read);
            CloseHandle(stdout_write); CloseHandle(stderr_read);
            CloseHandle(stderr_write);
            return Result<ProcessResult>::failure(wide_argument.error());
        }
        command_line.push_back(L' ');
        command_line += quote_windows_argument(wide_argument.value());
    }
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdin_handle;
    startup.hStdOutput = stdout_write;
    startup.hStdError = stderr_write;
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        executable.value().c_str(), mutable_command.data(), nullptr, nullptr,
        TRUE, CREATE_NO_WINDOW, nullptr,
        working_directory.value().empty() ? nullptr : working_directory.value().c_str(),
        &startup, &process);
    CloseHandle(stdin_handle);
    CloseHandle(stdout_write);
    CloseHandle(stderr_write);
    if (!created) {
        CloseHandle(stdout_read); CloseHandle(stderr_read);
        return failure<ProcessResult>(ErrorCode::NotFound,
                                      "configured process could not be started");
    }

    ProcessResult result;
    const auto started = std::chrono::steady_clock::now();
    bool stdout_open = true;
    bool stderr_open = true;
    bool output_failed = false;
    while (stdout_open || stderr_open ||
           WaitForSingleObject(process.hProcess, 0U) == WAIT_TIMEOUT) {
        auto read_stdout = read_pipe(
            stdout_read, result.stdout_text, spec.max_stdout_bytes, stdout_open);
        auto read_stderr = read_pipe(
            stderr_read, result.stderr_text, spec.max_stderr_bytes, stderr_open);
        if (!read_stdout || !read_stderr) {
            output_failed = true;
            TerminateProcess(process.hProcess, 1U);
            break;
        }
        if (std::chrono::steady_clock::now() - started >= spec.timeout) {
            result.timed_out = true;
            TerminateProcess(process.hProcess, 1U);
            break;
        }
        WaitForSingleObject(process.hProcess, 10U);
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    read_pipe(stdout_read, result.stdout_text, spec.max_stdout_bytes, stdout_open);
    read_pipe(stderr_read, result.stderr_text, spec.max_stderr_bytes, stderr_open);
    DWORD exit_code = 1U;
    GetExitCodeProcess(process.hProcess, &exit_code);
    result.exit_code = static_cast<int>(exit_code);
    result.elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    CloseHandle(stdout_read); CloseHandle(stderr_read);
    if (output_failed) {
        return failure<ProcessResult>(ErrorCode::ResourceExhausted,
                                      "process output exceeded its limit");
    }
    return Result<ProcessResult>::success(std::move(result));
}
#else
class Fd final {
public:
    explicit Fd(const int value = -1) noexcept : value_(value) {}
    ~Fd() { if (value_ >= 0) ::close(value_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const noexcept { return value_; }
    void close() noexcept {
        if (value_ >= 0) {
            (void)::close(value_);
            value_ = -1;
        }
    }
private:
    int value_;
};

Result<void> drain_fd(
    const int fd,
    std::string& output,
    const std::size_t maximum,
    bool& open) {
    std::array<char, 8192U> buffer{};
    while (open) {
        const auto count = ::read(fd, buffer.data(), buffer.size());
        if (count > 0) {
            if (static_cast<std::size_t>(count) > maximum - output.size()) {
                return failure<void>(ErrorCode::ResourceExhausted,
                                     "process output exceeded its limit");
            }
            output.append(buffer.data(), static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) { open = false; break; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        return failure<void>(ErrorCode::SystemError,
                             "process output pipe read failed");
    }
    return Result<void>::success();
}

Result<ProcessResult> run_linux(const ProcessSpec& spec) {
    int stdout_pipe[2]{};
    int stderr_pipe[2]{};
    const auto create_pipe = [](int (&pipe_fds)[2]) {
        if (::pipe(pipe_fds) != 0) {
            return false;
        }
        if (::fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC) != 0 ||
            ::fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC) != 0) {
            (void)::close(pipe_fds[0]);
            (void)::close(pipe_fds[1]);
            pipe_fds[0] = -1;
            pipe_fds[1] = -1;
            return false;
        }
        return true;
    };
    if (!create_pipe(stdout_pipe)) {
        return failure<ProcessResult>(ErrorCode::SystemError,
                                      "process pipe creation failed");
    }
    if (!create_pipe(stderr_pipe)) {
        (void)::close(stdout_pipe[0]);
        (void)::close(stdout_pipe[1]);
        return failure<ProcessResult>(ErrorCode::SystemError,
                                      "process pipe creation failed");
    }
    Fd stdout_read(stdout_pipe[0]); Fd stdout_write(stdout_pipe[1]);
    Fd stderr_read(stderr_pipe[0]); Fd stderr_write(stderr_pipe[1]);
    const int flags = fcntl(stdout_read.get(), F_GETFL, 0);
    const int error_flags = fcntl(stderr_read.get(), F_GETFL, 0);
    if (flags < 0 || error_flags < 0 ||
        fcntl(stdout_read.get(), F_SETFL, flags | O_NONBLOCK) < 0 ||
        fcntl(stderr_read.get(), F_SETFL, error_flags | O_NONBLOCK) < 0) {
        return failure<ProcessResult>(ErrorCode::SystemError,
                                      "process pipe setup failed");
    }

    // Everything reachable from the child after fork must already exist.  In
    // particular, do not project paths or grow argv in the child: a service
    // may be multi-threaded and the child can inherit a locked allocator.
    const std::string executable = spec.executable.string();
    const std::string working_directory = spec.working_directory.empty()
                                               ? std::string{}
                                               : spec.working_directory.string();
    const bool has_working_directory = !working_directory.empty();
    std::vector<char*> child_argv;
    child_argv.reserve(spec.arguments.size() + 2U);
    child_argv.push_back(const_cast<char*>(executable.c_str()));
    for (const auto& argument : spec.arguments) {
        child_argv.push_back(const_cast<char*>(argument.c_str()));
    }
    child_argv.push_back(nullptr);
    const char* executable_c = executable.c_str();
    const char* working_directory_c = working_directory.c_str();
    char** child_argv_data = child_argv.data();

    const int stdout_read_fd = stdout_read.get();
    const int stdout_write_fd = stdout_write.get();
    const int stderr_read_fd = stderr_read.get();
    const int stderr_write_fd = stderr_write.get();
    const pid_t child = ::fork();
    if (child < 0) {
        return failure<ProcessResult>(ErrorCode::SystemError,
                                      "process creation failed");
    }
    if (child == 0) {
        if (::dup2(stdout_write_fd, STDOUT_FILENO) < 0 ||
            ::dup2(stderr_write_fd, STDERR_FILENO) < 0) {
            ::_exit(126);
        }
        if (stdout_read_fd != STDOUT_FILENO &&
            stdout_read_fd != STDERR_FILENO) {
            (void)::close(stdout_read_fd);
        }
        if (stdout_write_fd != STDOUT_FILENO &&
            stdout_write_fd != STDERR_FILENO) {
            (void)::close(stdout_write_fd);
        }
        if (stderr_read_fd != STDOUT_FILENO &&
            stderr_read_fd != STDERR_FILENO) {
            (void)::close(stderr_read_fd);
        }
        if (stderr_write_fd != STDOUT_FILENO &&
            stderr_write_fd != STDERR_FILENO) {
            (void)::close(stderr_write_fd);
        }
        if (has_working_directory) {
            if (::chdir(working_directory_c) != 0) {
                ::_exit(126);
            }
        }
        ::execv(executable_c, child_argv_data);
        ::_exit(127);
    }
    // The parent must close its write ends before doing any other work.  EOF
    // on the read ends is otherwise dependent on this process's own handles.
    stdout_write.close();
    stderr_write.close();
    ProcessResult result;
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + spec.timeout;
    bool stdout_open = true; bool stderr_open = true;
    enum class ChildState { Running, Reaped };
    ChildState child_state = ChildState::Running;
    bool kill_sent = false;
    bool failed = false;
    ErrorCode failure_code = ErrorCode::InternalError;
    int wait_status = 0;
    const auto record_failure = [&](const ErrorCode code) {
        if (!failed) {
            failed = true;
            failure_code = code;
        }
    };
    const auto kill_child = [&] {
        if (child_state == ChildState::Running && !kill_sent) {
            if (::kill(child, SIGKILL) != 0 && errno != ESRCH) {
                record_failure(ErrorCode::SystemError);
            }
            kill_sent = true;
        }
    };
    const auto reap_after_kill = [&] {
        if (child_state != ChildState::Running) {
            return;
        }
        // A blocking reap is used after termination was requested to prevent
        // a direct-child zombie.  SIGKILL normally makes the child promptly
        // waitable, but user space cannot provide an absolute deadline for an
        // uninterruptible kernel state.  EINTR is the only retryable result.
        for (;;) {
            const pid_t waited_pid = ::waitpid(child, &wait_status, 0);
            if (waited_pid == child) {
                child_state = ChildState::Reaped;
                return;
            }
            if (waited_pid < 0 && errno == EINTR) {
                continue;
            }
            record_failure(ErrorCode::SystemError);
            return;
        }
    };
    while (true) {
        auto read_stdout = drain_fd(
            stdout_read.get(), result.stdout_text, spec.max_stdout_bytes, stdout_open);
        auto read_stderr = drain_fd(
            stderr_read.get(), result.stderr_text, spec.max_stderr_bytes, stderr_open);
        if (!read_stdout) {
            record_failure(read_stdout.error().code);
        } else if (!read_stderr) {
            record_failure(read_stderr.error().code);
        }
        if (failed) {
            kill_child();
            break;
        }
        if (child_state == ChildState::Running) {
            const pid_t waited_pid = ::waitpid(child, &wait_status, WNOHANG);
            if (waited_pid == child) {
                child_state = ChildState::Reaped;
            } else if (waited_pid < 0 && errno != EINTR) {
                record_failure(ErrorCode::SystemError);
                kill_child();
            }
        }
        if (failed) {
            break;
        }
        if (child_state == ChildState::Reaped && !stdout_open && !stderr_open) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            kill_child();
            break;
        }
        struct pollfd fds[2] = {
            {stdout_read.get(), static_cast<short>(stdout_open ? POLLIN : 0), 0},
            {stderr_read.get(), static_cast<short>(stderr_open ? POLLIN : 0), 0}};
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const int wait_ms = static_cast<int>(std::max<std::int64_t>(
            1, std::min<std::int64_t>(10, remaining.count())));
        const int poll_result = ::poll(fds, 2U, wait_ms);
        if (poll_result < 0 && errno != EINTR) {
            record_failure(ErrorCode::SystemError);
            kill_child();
            break;
        }
    }
    if (child_state == ChildState::Running) {
        kill_child();
        reap_after_kill();
    }
    if (child_state == ChildState::Reaped) {
        result.exit_code = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status)
                                                  : 128 + WTERMSIG(wait_status);
    }
    result.elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    if (failed) {
        if (failure_code == ErrorCode::ResourceExhausted) {
            return failure<ProcessResult>(
                ErrorCode::ResourceExhausted, "process output exceeded its limit");
        }
        return failure<ProcessResult>(ErrorCode::SystemError,
                                      "process wait or pipe operation failed");
    }
    return Result<ProcessResult>::success(std::move(result));
}
#endif

}  // namespace

Result<std::unique_ptr<LocalProcessRunner>> LocalProcessRunner::create() {
    return Result<std::unique_ptr<LocalProcessRunner>>::success(
        std::unique_ptr<LocalProcessRunner>{new LocalProcessRunner{}});
}

Result<ProcessResult> LocalProcessRunner::run(const ProcessSpec& spec) {
    const auto valid = validate_spec(spec);
    if (!valid) {
        return Result<ProcessResult>::failure(valid.error());
    }
    try {
#if defined(_WIN32)
        return run_windows(spec);
#else
        return run_linux(spec);
#endif
    } catch (const std::bad_alloc&) {
        return failure<ProcessResult>(ErrorCode::ResourceExhausted,
                                      "process runner allocation failed");
    } catch (const std::exception&) {
        return failure<ProcessResult>(ErrorCode::InternalError,
                                      "process runner failed");
    }
}

}  // namespace iaisf::application
