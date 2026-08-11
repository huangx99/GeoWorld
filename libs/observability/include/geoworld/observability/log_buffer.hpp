#pragma once

#include "geoworld/observability/logger.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace geoworld::observability {

class BoundedLogBuffer {
public:
    explicit BoundedLogBuffer(std::size_t capacity);

    [[nodiscard]] bool push(LogRecord record);
    [[nodiscard]] std::vector<LogRecord> drain();
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::uint64_t dropped() const noexcept;

private:
    std::size_t capacity_;
    std::deque<LogRecord> records_;
    std::uint64_t dropped_{};
};

} // namespace geoworld::observability
