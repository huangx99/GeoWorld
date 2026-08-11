#pragma once

#include "geoworld/foundation/ids.hpp"
#include "geoworld/spatial/cell_grid.hpp"
#include "geoworld/spatial/spatial_query.hpp"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace geoworld::spatial {

struct AoiDelta {
    std::vector<foundation::WorldId> entered;
    std::vector<foundation::WorldId> left;
};

class AoiManager {
public:
    [[nodiscard]] bool subscribe(foundation::WorldId observer, CellKey center,
                                 int radius_cells);
    [[nodiscard]] bool unsubscribe(foundation::WorldId observer);
    [[nodiscard]] AoiDelta update(foundation::WorldId observer,
                                   const std::vector<foundation::WorldId>& candidates);
    [[nodiscard]] AoiDelta update(foundation::WorldId observer,
                                   const SpatialQuery& query, Aabb bounds);
    [[nodiscard]] std::vector<foundation::WorldId> visible(foundation::WorldId observer) const;

private:
    struct Subscription {
        CellKey center;
        int radius_cells{};
        std::unordered_set<foundation::WorldId, foundation::WorldIdHash> visible;
    };

    std::unordered_map<foundation::WorldId, Subscription, foundation::WorldIdHash> subscriptions_;
};

} // namespace geoworld::spatial
