#pragma once

#include "iaisf/application/application_job.hpp"
#include "iaisf/core/result.hpp"

namespace iaisf::application {

class IApplicationJobClock {
public:
    virtual ~IApplicationJobClock() = default;

    [[nodiscard]] virtual Result<ApplicationJobTimePoint> now() const = 0;
};

/** Reads system_clock once per call and rejects unrepresentable timestamps. */
class SystemApplicationJobClock final : public IApplicationJobClock {
public:
    [[nodiscard]] Result<ApplicationJobTimePoint> now() const override;
};

}  // namespace iaisf::application
