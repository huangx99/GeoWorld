#include "geoworld/observability/logger.hpp"
#include "geoworld/observability/metrics.hpp"
#include "geoworld/observability/log_buffer.hpp"

#include <vector>

int main() {
    std::vector<geoworld::observability::LogRecord> records;
    geoworld::observability::Logger logger{
        [&records](const auto& record) { records.push_back(record); }
    };
    logger.write(geoworld::observability::LogLevel::info, "test", "ready",
                 geoworld::observability::LogContext{7, geoworld::foundation::WorldId{9}});
    if (records.size() != 1) {
        return 1;
    }
    const auto& record = records.front();
    if (record.category != "test" || record.context.tick != 7
        || record.context.world_id != geoworld::foundation::WorldId{9}) {
        return 1;
    }

    geoworld::observability::MetricsRecorder metrics;
    std::uint64_t metric_tick = 0;
    metrics.set_sink([&metric_tick](const auto& metric) { metric_tick = metric.tick; });
    metrics.record({7, 21'000, 9'000, 4'000, 2'000, 12, 1, 4, true});
    const auto latest = metrics.latest();
    if (!latest.has_value() || latest->tick != 7 || metric_tick != 7 || metrics.overload_count() != 1) {
        return 1;
    }

    geoworld::observability::BoundedLogBuffer buffer{1};
    if (!buffer.push(records.front()) || buffer.push(records.front()) || buffer.dropped() != 1) {
        return 1;
    }
    return buffer.drain().size() == 1 && buffer.size() == 0 ? 0 : 1;
}
