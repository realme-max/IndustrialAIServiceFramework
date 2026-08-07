#include "detail/application_job_id_entropy.hpp"

#include <windows.h>
#include <bcrypt.h>

namespace iaisf::application::detail {
namespace {

class WindowsEntropyReader final : public ApplicationJobIdEntropyReader {
public:
    [[nodiscard]] EntropyReadResult read(
        std::uint8_t* const destination,
        const std::size_t maximum_bytes) override {
        const auto status = BCryptGenRandom(
            nullptr,
            destination,
            static_cast<ULONG>(maximum_bytes),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status != 0) {
            return EntropyReadResult{EntropyReadStatus::Failed, 0U};
        }
        return EntropyReadResult{EntropyReadStatus::Bytes, maximum_bytes};
    }
};

}  // namespace

std::shared_ptr<ApplicationJobIdEntropyReader>
make_platform_job_id_entropy_reader() {
    return std::make_shared<WindowsEntropyReader>();
}

}  // namespace iaisf::application::detail
