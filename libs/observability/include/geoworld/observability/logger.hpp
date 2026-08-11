#pragma once

#include "geoworld/foundation/ids.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace geoworld::observability {

enum class LogLevel { trace, debug, info, warning, error, critical };

struct LogContext {
    std::optional<std::uint64_t> tick;
    std::optional<foundation::WorldId> world_id;
};

struct LogRecord {
    LogLevel level{LogLevel::info};
    std::string category;
    std::string message;
    LogContext context;
};

class Logger {
public:
    using Sink = std::function<void(const LogRecord&)>;

    explicit Logger(Sink sink = {});
    void write(LogLevel level, std::string_view category, std::string_view message,
               LogContext context = {}) const;

private:
    Sink sink_;
};

} // namespace geoworld::observability
