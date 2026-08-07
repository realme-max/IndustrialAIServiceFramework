#include "iaisf/application/application_job_clock.hpp"

#include <chrono>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "detail/application_job_clock_detail.hpp"

namespace iaisf::application::detail {
namespace {

[[nodiscard]] Result<void> invalid_time() {
    return Result<void>::failure(make_error(
        ErrorCode::InvalidArgument,
        "application job time is before the Unix epoch"));
}

[[nodiscard]] Result<void> unrepresentable_time() {
    return Result<void>::failure(make_error(
        ErrorCode::ResourceExhausted,
        "application job time cannot be represented"));
}

template <typename Duration>
[[nodiscard]] Result<void> validate_duration(const Duration duration) {
    using Rep = typename Duration::rep;
    using Period = typename Duration::period;

    if constexpr (!std::is_integral_v<Rep>) {
        return unrepresentable_time();
    } else {
        if constexpr (std::is_signed_v<Rep>) {
            if (duration.count() < 0) {
                return invalid_time();
            }
        }

        using UnsignedRep = std::make_unsigned_t<Rep>;
        if constexpr (sizeof(UnsignedRep) > sizeof(std::uintmax_t)) {
            return unrepresentable_time();
        }

        constexpr auto period_num = Period::num;
        constexpr auto period_den = Period::den;
        constexpr auto uintmax_limit =
            std::numeric_limits<std::uintmax_t>::max();
        if constexpr (period_num <= 0 || period_den <= 0) {
            return unrepresentable_time();
        } else {
            constexpr auto numerator_limit = uintmax_limit / 1000U;
            if constexpr (static_cast<std::uintmax_t>(period_num) >
                          numerator_limit) {
                return unrepresentable_time();
            } else {
                constexpr auto milliseconds_numerator =
                    static_cast<std::uintmax_t>(period_num) * 1000U;
                constexpr auto milliseconds_denominator =
                    static_cast<std::uintmax_t>(period_den);
                constexpr auto maximum_milliseconds = static_cast<std::uintmax_t>(
                    std::numeric_limits<std::int64_t>::max());

                const auto count = static_cast<std::uintmax_t>(duration.count());
                const auto whole_units = count / milliseconds_denominator;
                if (whole_units >
                    maximum_milliseconds / milliseconds_numerator) {
                    return unrepresentable_time();
                }

                const auto whole_milliseconds =
                    whole_units * milliseconds_numerator;
                const auto remainder = count % milliseconds_denominator;
                if (remainder == 0U) {
                    return Result<void>::success();
                }
                if (remainder > uintmax_limit / milliseconds_numerator) {
                    return unrepresentable_time();
                }
                const auto fractional_milliseconds =
                    (remainder * milliseconds_numerator) /
                    milliseconds_denominator;
                if (fractional_milliseconds >
                    maximum_milliseconds - whole_milliseconds) {
                    return unrepresentable_time();
                }
                return Result<void>::success();
            }
        }
    }
}

}  // namespace

Result<void> validate_application_job_time_point(
    const ApplicationJobTimePoint value) {
    return validate_duration(value.time_since_epoch());
}

}  // namespace iaisf::application::detail

namespace iaisf::application {

Result<ApplicationJobTimePoint> SystemApplicationJobClock::now() const {
    const auto current = std::chrono::system_clock::now();
    const auto validation = detail::validate_application_job_time_point(current);
    if (!validation) {
        return Result<ApplicationJobTimePoint>::failure(validation.error());
    }
    return Result<ApplicationJobTimePoint>::success(current);
}

}  // namespace iaisf::application
