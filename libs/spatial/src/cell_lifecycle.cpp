#include "geoworld/spatial/cell_lifecycle.hpp"

#include <algorithm>

namespace geoworld::spatial {

bool CellLifecycle::request_load(CellKey cell, std::uint64_t now_ms) {
    auto& record = cells_[cell];
    if (record.state != CellState::unloaded) {
        return false;
    }
    record = {CellState::loading, now_ms, 0};
    return true;
}

bool CellLifecycle::mark_loaded(CellKey cell, std::uint64_t now_ms) {
    const auto iterator = cells_.find(cell);
    if (iterator == cells_.end() || iterator->second.state != CellState::loading) {
        return false;
    }
    iterator->second = {CellState::loaded, now_ms, 0};
    return true;
}

bool CellLifecycle::activate(CellKey cell, std::uint64_t now_ms) {
    const auto iterator = cells_.find(cell);
    if (iterator == cells_.end()
        || (iterator->second.state != CellState::loaded
            && iterator->second.state != CellState::sleeping)) {
        return false;
    }
    iterator->second = {CellState::active, now_ms, 0};
    return true;
}

bool CellLifecycle::sleep(CellKey cell, std::uint64_t now_ms,
                          std::uint64_t unload_delay_ms) {
    const auto iterator = cells_.find(cell);
    if (iterator == cells_.end() || iterator->second.state != CellState::active) {
        return false;
    }
    iterator->second = {CellState::sleeping, now_ms, now_ms + unload_delay_ms};
    return true;
}

bool CellLifecycle::unload_if_due(CellKey cell, std::uint64_t now_ms) {
    const auto iterator = cells_.find(cell);
    if (iterator == cells_.end() || iterator->second.state != CellState::sleeping
        || now_ms < iterator->second.unload_after_ms) {
        return false;
    }
    cells_.erase(iterator);
    return true;
}

CellState CellLifecycle::state(CellKey cell) const noexcept {
    const auto iterator = cells_.find(cell);
    return iterator == cells_.end() ? CellState::unloaded : iterator->second.state;
}

std::vector<CellKey> CellLifecycle::known_cells() const {
    std::vector<CellKey> result;
    result.reserve(cells_.size());
    for (const auto& [cell, record] : cells_) {
        static_cast<void>(record);
        result.push_back(cell);
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<CellKey> CellLifecycle::prefetch(CellKey center, int radius) const {
    std::vector<CellKey> result;
    if (radius < 0) {
        return result;
    }
    for (auto y = -radius; y <= radius; ++y) {
        for (auto x = -radius; x <= radius; ++x) {
            result.push_back({center.x + x, center.y + y, center.level, center.z});
        }
    }
    return result;
}

} // namespace geoworld::spatial
