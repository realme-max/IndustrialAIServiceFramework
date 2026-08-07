#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include "iaisf/application/application_job_id_generator.hpp"

namespace iaisf::application::detail {

enum class EntropyReadStatus {
    Bytes,
    Interrupted,
    Failed,
    EndOfStream,
};

struct EntropyReadResult {
    EntropyReadStatus status;
    std::size_t bytes;
};

class ApplicationJobIdEntropyReader {
public:
    virtual ~ApplicationJobIdEntropyReader() = default;

    [[nodiscard]] virtual EntropyReadResult read(
        std::uint8_t* destination,
        std::size_t maximum_bytes) = 0;
};

[[nodiscard]] std::shared_ptr<ApplicationJobIdEntropyReader>
make_platform_job_id_entropy_reader();

/** Source-private coordinator used by the platform generator and tests. */
[[nodiscard]] ApplicationJobIdGenerationResult generate_with_entropy_reader(
    IndustrialApplication application,
    ApplicationJobIdEntropyReader& reader);

#if defined(__linux__)
struct LinuxGetrandomResult {
    std::ptrdiff_t return_value;
    int error_number;
};

class LinuxGetrandomInvoker {
public:
    virtual ~LinuxGetrandomInvoker() = default;

    [[nodiscard]] virtual LinuxGetrandomResult invoke(
        std::uint8_t* destination,
        std::size_t maximum_bytes) = 0;
};

[[nodiscard]] std::shared_ptr<ApplicationJobIdEntropyReader>
make_linux_test_job_id_entropy_reader(
    std::shared_ptr<LinuxGetrandomInvoker> invoker);
#endif

}  // namespace iaisf::application::detail
