#include "detail/application_job_id_entropy.hpp"

#include <cerrno>
#include <sys/random.h>

#include <utility>

namespace iaisf::application::detail {
namespace {

class SystemLinuxGetrandomInvoker final : public LinuxGetrandomInvoker {
public:
    [[nodiscard]] LinuxGetrandomResult invoke(
        std::uint8_t* const destination,
        const std::size_t maximum_bytes) override {
        errno = 0;
        const auto result = ::getrandom(destination, maximum_bytes, 0U);
        return LinuxGetrandomResult{
            static_cast<std::ptrdiff_t>(result),
            errno};
    }
};

class LinuxEntropyReader final : public ApplicationJobIdEntropyReader {
public:
    explicit LinuxEntropyReader(
        std::shared_ptr<LinuxGetrandomInvoker> invoker)
        : invoker_(std::move(invoker)) {}

    [[nodiscard]] EntropyReadResult read(
        std::uint8_t* const destination,
        const std::size_t maximum_bytes) override {
        if (invoker_ == nullptr) {
            return EntropyReadResult{EntropyReadStatus::Failed, 0U};
        }

        const auto result = invoker_->invoke(destination, maximum_bytes);
        if (result.return_value > 0) {
            return EntropyReadResult{
                EntropyReadStatus::Bytes,
                static_cast<std::size_t>(result.return_value)};
        }
        if (result.return_value == 0) {
            return EntropyReadResult{EntropyReadStatus::EndOfStream, 0U};
        }
        if (result.error_number == EINTR) {
            return EntropyReadResult{EntropyReadStatus::Interrupted, 0U};
        }
        return EntropyReadResult{EntropyReadStatus::Failed, 0U};
    }

private:
    std::shared_ptr<LinuxGetrandomInvoker> invoker_;
};

}  // namespace

std::shared_ptr<ApplicationJobIdEntropyReader>
make_platform_job_id_entropy_reader() {
    return std::make_shared<LinuxEntropyReader>(
        std::make_shared<SystemLinuxGetrandomInvoker>());
}

std::shared_ptr<ApplicationJobIdEntropyReader>
make_linux_test_job_id_entropy_reader(
    std::shared_ptr<LinuxGetrandomInvoker> invoker) {
    return std::make_shared<LinuxEntropyReader>(std::move(invoker));
}

}  // namespace iaisf::application::detail
