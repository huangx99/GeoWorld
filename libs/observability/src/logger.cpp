#include "geoworld/observability/logger.hpp"

#include <iostream>
#include <utility>

namespace geoworld::observability {
namespace {

std::string_view level_name(LogLevel level) {
    switch (level) {
    case LogLevel::trace: return "trace";
    case LogLevel::debug: return "debug";
    case LogLevel::info: return "info";
    case LogLevel::warning: return "warning";
    case LogLevel::error: return "error";
    case LogLevel::critical: return "critical";
    }
    return "unknown";
}

void console_sink(const LogRecord& record) {
    std::clog << "level=" << level_name(record.level)
              << " category=" << record.category;
    if (record.context.tick.has_value()) {
        std::clog << " tick=" << *record.context.tick;
    }
    if (record.context.world_id.has_value()) {
        std::clog << " wid=" << record.context.world_id->value;
    }
    std::clog << " message=\"" << record.message << "\"\n";
}

} // namespace

Logger::Logger(Sink sink) : sink_(sink ? std::move(sink) : Sink{console_sink}) {}

void Logger::write(LogLevel level, std::string_view category, std::string_view message,
                   LogContext context) const {
    sink_(LogRecord{level, std::string{category}, std::string{message}, std::move(context)});
}

} // namespace geoworld::observability
