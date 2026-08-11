#include "geoworld/spatial/cell_grid.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace geoworld::spatial {

std::size_t CellKeyHash::operator()(const CellKey& key) const noexcept {
    auto value = static_cast<std::uint64_t>(key.x) * 0x9E3779B185EBCA87ULL;
    value ^= static_cast<std::uint64_t>(key.y) + 0xC2B2AE3D27D4EB4FULL + (value << 6U) + (value >> 2U);
    value ^= static_cast<std::uint64_t>(key.level) * 0x165667B19E3779F9ULL;
    value ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.z))
        * 0xD6E8FEB86659FD93ULL;
    return static_cast<std::size_t>(value ^ (value >> 32U));
}

CellGrid::CellGrid(double base_cell_size_meters, std::uint8_t max_level) noexcept
    : base_cell_size_meters_(base_cell_size_meters > 0.0 ? base_cell_size_meters : 1.0),
      max_level_(max_level) {}

double CellGrid::cell_size(std::uint8_t level) const noexcept {
    if (level > max_level_) {
        level = max_level_;
    }
    return std::ldexp(base_cell_size_meters_, level);
}

CellKey CellGrid::cell_for(Enu position, std::uint8_t level) const noexcept {
    if (level > max_level_) {
        level = max_level_;
    }
    const auto size = cell_size(level);
    return {
        static_cast<std::int64_t>(std::floor(position.east / size)),
        static_cast<std::int64_t>(std::floor(position.north / size)),
        level,
        0
    };
}

CellKey CellGrid::cell_for_2_5d(Enu position, std::uint8_t level,
                                double floor_height_meters) const noexcept {
    if (level > max_level_) {
        level = max_level_;
    }
    const auto size = cell_size(level);
    const auto floor_size = floor_height_meters > 0.0 ? floor_height_meters : 1.0;
    return {
        static_cast<std::int64_t>(std::floor(position.east / size)),
        static_cast<std::int64_t>(std::floor(position.north / size)),
        level,
        static_cast<std::int32_t>(std::floor(position.up / floor_size))
    };
}

CellKey CellGrid::parent(CellKey child) const noexcept {
    if (child.level >= max_level_) {
        return child;
    }
    return {
        static_cast<std::int64_t>(std::floor(static_cast<double>(child.x) / 2.0)),
        static_cast<std::int64_t>(std::floor(static_cast<double>(child.y) / 2.0)),
        static_cast<std::uint8_t>(child.level + 1),
        child.z
    };
}

std::vector<CellKey> CellGrid::neighbors(CellKey cell) const {
    std::vector<CellKey> result;
    result.reserve(8);
    for (std::int64_t dy = -1; dy <= 1; ++dy) {
        for (std::int64_t dx = -1; dx <= 1; ++dx) {
            if (dx != 0 || dy != 0) {
                result.push_back({cell.x + dx, cell.y + dy, cell.level, cell.z});
            }
        }
    }
    return result;
}

DynamicCellIndex::DynamicCellIndex(CellGrid grid, std::uint8_t level,
                                   double floor_height_meters)
    : grid_(std::move(grid)), level_(level),
      floor_height_meters_(floor_height_meters > 0.0 ? floor_height_meters : 0.0) {}

std::optional<CellMigration> DynamicCellIndex::upsert(foundation::WorldId id, Enu position) {
    if (!id.valid()) {
        return std::nullopt;
    }
    const auto current = floor_height_meters_ > 0.0
        ? grid_.cell_for_2_5d(position, level_, floor_height_meters_)
        : grid_.cell_for(position, level_);
    const auto existing = locations_.find(id);
    if (existing == locations_.end()) {
        locations_.emplace(id, current);
        members_[current].insert(id);
        return std::nullopt;
    }
    if (existing->second == current) {
        return std::nullopt;
    }

    const auto previous = existing->second;
    members_[previous].erase(id);
    members_[current].insert(id);
    existing->second = current;
    return CellMigration{id, previous, current};
}

bool DynamicCellIndex::erase(foundation::WorldId id) {
    const auto iterator = locations_.find(id);
    if (iterator == locations_.end()) {
        return false;
    }
    members_[iterator->second].erase(id);
    locations_.erase(iterator);
    return true;
}

std::vector<foundation::WorldId> DynamicCellIndex::members(CellKey cell) const {
    const auto iterator = members_.find(cell);
    if (iterator == members_.end()) {
        return {};
    }
    return {iterator->second.begin(), iterator->second.end()};
}

std::vector<foundation::WorldId> DynamicCellIndex::query_candidates(Enu minimum,
                                                                     Enu maximum) const {
    const auto min_x = std::min(minimum.east, maximum.east);
    const auto min_y = std::min(minimum.north, maximum.north);
    const auto max_x = std::max(minimum.east, maximum.east);
    const auto max_y = std::max(minimum.north, maximum.north);
    const auto first = grid_.cell_for({min_x, min_y, 0}, level_);
    const auto last = grid_.cell_for({max_x, max_y, 0}, level_);
    const auto first_2_5d = floor_height_meters_ > 0.0
        ? grid_.cell_for_2_5d({min_x, min_y, std::min(minimum.up, maximum.up)}, level_,
                              floor_height_meters_)
        : first;
    const auto last_2_5d = floor_height_meters_ > 0.0
        ? grid_.cell_for_2_5d({max_x, max_y, std::max(minimum.up, maximum.up)}, level_,
                              floor_height_meters_)
        : last;
    std::unordered_set<foundation::WorldId, foundation::WorldIdHash> result_set;
    for (auto z = first_2_5d.z; z <= last_2_5d.z; ++z) {
        for (auto y = first.y; y <= last.y; ++y) {
            for (auto x = first.x; x <= last.x; ++x) {
                const auto iterator = members_.find({x, y, level_, z});
                if (iterator != members_.end()) {
                    result_set.insert(iterator->second.begin(), iterator->second.end());
                }
            }
        }
    }
    std::vector<foundation::WorldId> result{result_set.begin(), result_set.end()};
    std::sort(result.begin(), result.end());
    return result;
}

std::optional<CellKey> DynamicCellIndex::cell_of(foundation::WorldId id) const {
    const auto iterator = locations_.find(id);
    return iterator == locations_.end() ? std::nullopt : std::optional{iterator->second};
}

std::size_t DynamicCellIndex::size() const noexcept { return locations_.size(); }

} // namespace geoworld::spatial
