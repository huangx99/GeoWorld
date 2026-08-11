#pragma once

#include "geoworld/spatial/cell_lifecycle.hpp"

#include <vector>

namespace geoworld::spatial {

struct StreamingSource {
    CellKey center;
    int radius_cells{};
    int prefetch_radius{};
};

struct StreamingPlan {
    std::vector<CellKey> desired;
    std::vector<CellKey> to_load;
    std::vector<CellKey> to_sleep;
};

class StreamingPlanner {
public:
    explicit StreamingPlanner(CellGrid grid) noexcept;
    StreamingPlanner(CellGrid grid, CellLifecycle& lifecycle) noexcept;

    [[nodiscard]] StreamingPlan plan(const std::vector<StreamingSource>& sources) const;
    [[nodiscard]] StreamingPlan plan(const std::vector<StreamingSource>& sources,
                                     const CellLifecycle& lifecycle) const;

private:
    CellGrid grid_;
    const CellLifecycle* lifecycle_{};
};

} // namespace geoworld::spatial
