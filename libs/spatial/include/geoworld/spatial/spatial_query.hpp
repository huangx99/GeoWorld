#pragma once

#include "geoworld/spatial/cell_grid.hpp"

#include <boost/geometry.hpp>
#include <boost/geometry/index/rtree.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace geoworld::spatial {

struct Aabb {
    Enu minimum;
    Enu maximum;
};

class StaticSpatialIndex {
public:
    void insert(foundation::WorldId id, Aabb bounds);
    [[nodiscard]] bool erase(foundation::WorldId id);
    [[nodiscard]] std::vector<foundation::WorldId> query(Aabb bounds) const;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    using Point = boost::geometry::model::point<double, 2, boost::geometry::cs::cartesian>;
    using Box = boost::geometry::model::box<Point>;
    using Value = std::pair<Box, foundation::WorldId>;
    using RTree = boost::geometry::index::rtree<Value,
        boost::geometry::index::quadratic<16>>;

    static Box to_box(Aabb bounds);

    RTree tree_;
    std::unordered_map<foundation::WorldId, Box, foundation::WorldIdHash> bounds_;
};

class SpatialQuery {
public:
    explicit SpatialQuery(CellGrid grid, double floor_height_meters = 0.0);

    [[nodiscard]] DynamicCellIndex& dynamic() noexcept;
    [[nodiscard]] StaticSpatialIndex& statics() noexcept;
    [[nodiscard]] std::vector<foundation::WorldId> query(Aabb bounds) const;

private:
    DynamicCellIndex dynamic_;
    StaticSpatialIndex statics_;
};

} // namespace geoworld::spatial
