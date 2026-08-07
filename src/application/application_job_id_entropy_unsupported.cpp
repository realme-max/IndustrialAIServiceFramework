#include "detail/application_job_id_entropy.hpp"

namespace iaisf::application::detail {
namespace {

class UnsupportedEntropyReader final : public ApplicationJobIdEntropyReader {
public:
    [[nodiscard]] EntropyReadResult read(
        std::uint8_t* const,
        const std::size_t) override {
        return EntropyReadResult{EntropyReadStatus::Failed, 0U};
    }
};

}  // namespace

std::shared_ptr<ApplicationJobIdEntropyReader>
make_platform_job_id_entropy_reader() {
    return std::make_shared<UnsupportedEntropyReader>();
}

}  // namespace iaisf::application::detail
