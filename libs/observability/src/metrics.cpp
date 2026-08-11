#include "geoworld/observability/metrics.hpp"

namespace geoworld::observability {

void MetricsRecorder::set_sink(Sink sink) { sink_ = std::move(sink); }

void MetricsRecorder::record(TickMetrics metrics) {
    overload_count_ += metrics.overloaded ? 1 : 0;
    latest_ = metrics;
    if (sink_) {
        sink_(*latest_);
    }
}

std::optional<TickMetrics> MetricsRecorder::latest() const noexcept { return latest_; }

std::uint64_t MetricsRecorder::overload_count() const noexcept { return overload_count_; }

} // namespace geoworld::observability
