#include "geoworld/spatial/streaming.hpp"

#include <algorithm>
#include <limits>
#include <utility>
#include <unordered_set>

namespace geoworld::spatial {

StreamingPlanner::StreamingPlanner(CellGrid grid) noexcept : grid_(std::move(grid)) {}

StreamingPlanner::StreamingPlanner(CellGrid grid, CellLifecycle& lifecycle) noexcept
    : grid_(std::move(grid)), lifecycle_(&lifecycle) {}

StreamingPlan StreamingPlanner::plan(const std::vector<StreamingSource>& sources) const {
    static const CellLifecycle empty_lifecycle;
    return plan(sources, lifecycle_ != nullptr ? *lifecycle_ : empty_lifecycle);
}

StreamingPlan StreamingPlanner::plan(const std::vector<StreamingSource>& sources,
                                     const CellLifecycle& lifecycle) const {
    std::unordered_set<CellKey, CellKeyHash> desired_set;
    for (const auto& source : sources) {
        if (source.radius_cells < 0 || source.prefetch_radius < 0) {
            continue;
        }
        const auto radius = static_cast<long long>(source.radius_cells)
            + static_cast<long long>(source.prefetch_radius);
        const auto bounded_radius = std::min(
            radius, static_cast<long long>(std::numeric_limits<int>::max()));
        const auto radius_value = static_cast<int>(bounded_radius);
        for (auto y = -radius_value; y <= radius_value; ++y) {
            for (auto x = -radius_value; x <= radius_value; ++x) {
                desired_set.insert({source.center.x + x, source.center.y + y,
                                    source.center.level, source.center.z});
            }
        }
    }

    StreamingPlan result;
    result.desired.assign(desired_set.begin(), desired_set.end());
    std::sort(result.desired.begin(), result.desired.end());

    const auto known = lifecycle.known_cells();
    std::unordered_set<CellKey, CellKeyHash> known_set(known.begin(), known.end());
    for (const auto cell : result.desired) {
        if (!known_set.contains(cell) || lifecycle.state(cell) == CellState::unloaded) {
            result.to_load.push_back(cell);
        }
    }
    for (const auto cell : known) {
        if (desired_set.contains(cell)) {
            continue;
        }
        const auto state = lifecycle.state(cell);
        if (state == CellState::loaded || state == CellState::active) {
            result.to_sleep.push_back(cell);
        }
    }
    return result;
}

} // namespace geoworld::spatial
