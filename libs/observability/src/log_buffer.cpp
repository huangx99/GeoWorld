#include "geoworld/observability/log_buffer.hpp"

#include <utility>

namespace geoworld::observability {

BoundedLogBuffer::BoundedLogBuffer(std::size_t capacity) : capacity_(capacity) {}

bool BoundedLogBuffer::push(LogRecord record) {
    if (records_.size() >= capacity_) {
        ++dropped_;
        return false;
    }
    records_.push_back(std::move(record));
    return true;
}

std::vector<LogRecord> BoundedLogBuffer::drain() {
    std::vector<LogRecord> result;
    result.reserve(records_.size());
    while (!records_.empty()) {
        result.push_back(std::move(records_.front()));
        records_.pop_front();
    }
    return result;
}

std::size_t BoundedLogBuffer::size() const noexcept { return records_.size(); }
std::uint64_t BoundedLogBuffer::dropped() const noexcept { return dropped_; }

} // namespace geoworld::observability
