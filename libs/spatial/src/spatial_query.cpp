#include "geoworld/spatial/spatial_query.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace geoworld::spatial {
namespace bgi = boost::geometry::index;

StaticSpatialIndex::Box StaticSpatialIndex::to_box(Aabb bounds) {
    const auto min_x = std::min(bounds.minimum.east, bounds.maximum.east);
    const auto min_y = std::min(bounds.minimum.north, bounds.maximum.north);
    const auto max_x = std::max(bounds.minimum.east, bounds.maximum.east);
    const auto max_y = std::max(bounds.minimum.north, bounds.maximum.north);
    return {Point{min_x, min_y}, Point{max_x, max_y}};
}

void StaticSpatialIndex::insert(foundation::WorldId id, Aabb bounds) {
    if (!id.valid()) {
        return;
    }
    const auto box = to_box(bounds);
    const auto existing = bounds_.find(id);
    if (existing != bounds_.end()) {
        tree_.remove({existing->second, id});
        existing->second = box;
    } else {
        bounds_.emplace(id, box);
    }
    tree_.insert({box, id});
}

bool StaticSpatialIndex::erase(foundation::WorldId id) {
    const auto iterator = bounds_.find(id);
    if (iterator == bounds_.end()) {
        return false;
    }
    tree_.remove({iterator->second, id});
    bounds_.erase(iterator);
    return true;
}

std::vector<foundation::WorldId> StaticSpatialIndex::query(Aabb bounds) const {
    std::vector<Value> values;
    tree_.query(bgi::intersects(to_box(bounds)), std::back_inserter(values));
    std::vector<foundation::WorldId> result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(value.second);
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::size_t StaticSpatialIndex::size() const noexcept { return bounds_.size(); }

SpatialQuery::SpatialQuery(CellGrid grid, double floor_height_meters)
    : dynamic_(std::move(grid), 0, floor_height_meters) {}

DynamicCellIndex& SpatialQuery::dynamic() noexcept { return dynamic_; }
StaticSpatialIndex& SpatialQuery::statics() noexcept { return statics_; }

std::vector<foundation::WorldId> SpatialQuery::query(Aabb bounds) const {
    auto result = statics_.query(bounds);
    auto dynamic = dynamic_.query_candidates(bounds.minimum, bounds.maximum);
    result.insert(result.end(), dynamic.begin(), dynamic.end());
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

} // namespace geoworld::spatial
