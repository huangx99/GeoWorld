#include "geoworld/spatial/aoi.hpp"

#include <algorithm>
#include <utility>

namespace geoworld::spatial {

bool AoiManager::subscribe(foundation::WorldId observer, CellKey center, int radius_cells) {
    if (!observer.valid() || radius_cells < 0) {
        return false;
    }
    return subscriptions_.emplace(observer, Subscription{center, radius_cells, {}}).second;
}

bool AoiManager::unsubscribe(foundation::WorldId observer) {
    return subscriptions_.erase(observer) != 0;
}

AoiDelta AoiManager::update(foundation::WorldId observer,
                             const std::vector<foundation::WorldId>& candidates) {
    const auto iterator = subscriptions_.find(observer);
    if (iterator == subscriptions_.end()) {
        return {};
    }

    std::unordered_set<foundation::WorldId, foundation::WorldIdHash> next;
    for (const auto id : candidates) {
        if (id.valid()) {
            next.insert(id);
        }
    }

    AoiDelta delta;
    for (const auto id : next) {
        if (!iterator->second.visible.contains(id)) {
            delta.entered.push_back(id);
        }
    }
    for (const auto id : iterator->second.visible) {
        if (!next.contains(id)) {
            delta.left.push_back(id);
        }
    }
    std::sort(delta.entered.begin(), delta.entered.end());
    std::sort(delta.left.begin(), delta.left.end());
    iterator->second.visible = std::move(next);
    return delta;
}

AoiDelta AoiManager::update(foundation::WorldId observer, const SpatialQuery& query,
                            Aabb bounds) {
    return update(observer, query.query(bounds));
}

std::vector<foundation::WorldId> AoiManager::visible(foundation::WorldId observer) const {
    const auto iterator = subscriptions_.find(observer);
    if (iterator == subscriptions_.end()) {
        return {};
    }
    std::vector<foundation::WorldId> result{
        iterator->second.visible.begin(), iterator->second.visible.end()
    };
    std::sort(result.begin(), result.end());
    return result;
}

} // namespace geoworld::spatial
