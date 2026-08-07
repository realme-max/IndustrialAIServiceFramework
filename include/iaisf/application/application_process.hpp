#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "iaisf/core/result.hpp"

namespace iaisf::application {

/** Parameters for one non-shell child-process invocation. */
struct ProcessSpec final {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::filesystem::path working_directory;
    std::chrono::milliseconds timeout{std::chrono::seconds{300}};
    std::size_t max_stdout_bytes{4U * 1024U * 1024U};
    std::size_t max_stderr_bytes{4U * 1024U * 1024U};
};

struct ProcessResult final {
    int exit_code{-1};
    bool timed_out{false};
    double elapsed_ms{0.0};
    std::string stdout_text;
    std::string stderr_text;
};

class IProcessRunner {
public:
    virtual ~IProcessRunner() = default;
    [[nodiscard]] virtual Result<ProcessResult> run(
        const ProcessSpec& spec) = 0;
};

/** Cross-platform local runner. It never invokes a shell. */
class LocalProcessRunner final : public IProcessRunner {
public:
    [[nodiscard]] static Result<std::unique_ptr<LocalProcessRunner>> create();

    LocalProcessRunner(const LocalProcessRunner&) = delete;
    LocalProcessRunner& operator=(const LocalProcessRunner&) = delete;

    [[nodiscard]] Result<ProcessResult> run(
        const ProcessSpec& spec) override;

private:
    LocalProcessRunner() = default;
};

}  // namespace iaisf::application
