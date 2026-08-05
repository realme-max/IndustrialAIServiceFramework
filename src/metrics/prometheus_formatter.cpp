#include "iaisf/metrics/prometheus_formatter.hpp"

#include <iomanip>
#include <locale>
#include <new>
#include <sstream>
#include <utility>

namespace iaisf::metrics {
namespace {

void append_type(std::ostringstream& output,
                 const std::string& name,
                 const char* const type) {
    output << "# TYPE " << name << ' ' << type << '\n';
}

void append_histogram(std::ostringstream& output,
                      const HistogramSnapshot& histogram) {
    append_type(output, histogram.name, "histogram");
    output << histogram.name << "_count " << histogram.count << '\n';
    output << histogram.name << "_sum " << std::setprecision(17)
           << histogram.sum << '\n';
    output << histogram.name << "_min " << std::setprecision(17)
           << histogram.min << '\n';
    output << histogram.name << "_max " << std::setprecision(17)
           << histogram.max << '\n';
}

}  // namespace

Result<std::string> PrometheusFormatter::format(
    const MetricsSnapshot& snapshot) {
    try {
        std::ostringstream output;
        output.imbue(std::locale::classic());
        for (const auto& counter : snapshot.counters) {
            append_type(output, counter.name, "counter");
            output << counter.name << ' ' << counter.value << '\n';
        }
        for (const auto& gauge : snapshot.gauges) {
            append_type(output, gauge.name, "gauge");
            output << gauge.name << ' ' << gauge.value << '\n';
        }
        for (const auto& histogram : snapshot.histograms) {
            append_histogram(output, histogram);
        }
        return Result<std::string>::success(output.str());
    } catch (const std::bad_alloc&) {
        return Result<std::string>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "metrics formatting allocation failed"));
    } catch (...) {
        return Result<std::string>::failure(make_error(
            ErrorCode::InternalError,
            "metrics formatting failed"));
    }
}

}  // namespace iaisf::metrics
