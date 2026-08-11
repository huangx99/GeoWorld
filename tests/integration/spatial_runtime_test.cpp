#include "geoworld/spatial/aoi.hpp"
#include "geoworld/spatial/cell_lifecycle.hpp"
#include "geoworld/spatial/spatial_query.hpp"
#include "geoworld/spatial/streaming.hpp"

int main() {
    using geoworld::foundation::WorldId;
    using geoworld::spatial::CellKey;

    geoworld::spatial::SpatialQuery query{geoworld::spatial::CellGrid{128.0}};
    static_cast<void>(query.dynamic().upsert(WorldId{1}, {10.0, 10.0, 0.0}));
    query.statics().insert(WorldId{2}, {{20.0, 20.0, 0.0}, {40.0, 40.0, 0.0}});
    const auto candidates = query.query({{0.0, 0.0, 0.0}, {50.0, 50.0, 0.0}});
    if (candidates != std::vector<WorldId>{WorldId{1}, WorldId{2}}) {
        return 1;
    }
    geoworld::spatial::SpatialQuery floors{geoworld::spatial::CellGrid{128.0}, 4.0};
    static_cast<void>(floors.dynamic().upsert(WorldId{10}, {10.0, 10.0, 4.0}));
    static_cast<void>(floors.dynamic().upsert(WorldId{11}, {10.0, 10.0, -4.0}));
    if (floors.query({{0.0, 0.0, -4.0}, {50.0, 50.0, 4.0}})
        != std::vector<WorldId>{WorldId{10}, WorldId{11}}) {
        return 1;
    }
    query.statics().insert(WorldId{2}, {{500.0, 500.0, 0.0}, {520.0, 520.0, 0.0}});
    if (query.query({{0.0, 0.0, 0.0}, {50.0, 50.0, 0.0}})
        != std::vector<WorldId>{WorldId{1}}) {
        return 1;
    }
    query.statics().insert(WorldId{3}, {{25.0, 25.0, 0.0}, {30.0, 30.0, 0.0}});
    if (!query.statics().erase(WorldId{3}) || query.statics().size() != 1) {
        return 1;
    }

    geoworld::spatial::CellLifecycle lifecycle;
    const CellKey cell{0, 0, 0};
    if (!lifecycle.request_load(cell, 0) || !lifecycle.mark_loaded(cell, 1)
        || !lifecycle.activate(cell, 2) || !lifecycle.sleep(cell, 3, 10)
        || lifecycle.unload_if_due(cell, 12) || !lifecycle.unload_if_due(cell, 13)
        || lifecycle.state(cell) != geoworld::spatial::CellState::unloaded) {
        return 1;
    }
    if (lifecycle.prefetch(cell, 1).size() != 9
        || lifecycle.prefetch({0, 0, 0, 3}, 0) != std::vector<CellKey>{{0, 0, 0, 3}}) {
        return 1;
    }

    const CellKey upper{1, 0, 0, 2};
    if (!lifecycle.request_load(upper, 0) || !lifecycle.mark_loaded(upper, 1)) {
        return 1;
    }
    geoworld::spatial::StreamingPlanner planner{geoworld::spatial::CellGrid{128.0}, lifecycle};
    const auto streaming = planner.plan({
        {{0, 0, 0, 0}, 0, 1},
        {{0, 0, 0, 0}, 1, 0},
    });
    if (streaming.desired.size() != 9 || streaming.to_load.size() != 9
        || streaming.to_sleep != std::vector<CellKey>{upper}) {
        return 1;
    }

    geoworld::spatial::AoiManager aoi;
    if (!aoi.subscribe(WorldId{99}, cell, 1)) {
        return 1;
    }
    const auto first = aoi.update(WorldId{99}, query,
                                  {{0.0, 0.0, 0.0}, {50.0, 50.0, 0.0}});
    const auto second = aoi.update(WorldId{99}, {WorldId{2}, WorldId{3}});
    return first.entered == std::vector<WorldId>{WorldId{1}}
        && second.entered == std::vector<WorldId>{WorldId{2}, WorldId{3}}
        && second.left == std::vector<WorldId>{WorldId{1}} ? 0 : 1;
}
