#include "geoworld/spatial/cell_grid.hpp"
#include "geoworld/spatial/coordinates.hpp"
#include "geoworld/spatial/s2_key.hpp"

#include <cmath>
#include <optional>
#include <vector>

int main() {
    using namespace geoworld::spatial;

    const Geodetic origin{31.2304, 121.4737, 10.0};
    const auto ecef = geodetic_to_ecef(origin);
    const auto recovered = ecef_to_geodetic(ecef);
    if (std::abs(recovered.latitude_degrees - origin.latitude_degrees) > 1e-7
        || std::abs(recovered.longitude_degrees - origin.longitude_degrees) > 1e-7
        || std::abs(recovered.height_meters - origin.height_meters) > 1e-3) {
        return 1;
    }

    const Enu local{12.0, -7.0, 3.0};
    const auto roundtrip = enu_to_ecef(ecef, origin, local);
    const auto recovered_local = ecef_to_enu(ecef, origin, roundtrip);
    if (std::abs(recovered_local.east - local.east) > 1e-8
        || std::abs(recovered_local.north - local.north) > 1e-8
        || std::abs(recovered_local.up - local.up) > 1e-8) {
        return 1;
    }

    const auto global_key = s2_key_for(origin, 10);
#if GW_HAS_S2
    if (!global_key.has_value() || global_key->level != 10 || global_key->value == 0) {
        return 1;
    }
#else
    if (global_key.has_value()) {
        return 1;
    }
#endif

    CellGrid grid{128.0, 4};
    if (grid.cell_for({0.0, 0.0, 0.0}) != CellKey{0, 0, 0}
        || grid.cell_for({127.999, -0.001, 0.0}) != CellKey{0, -1, 0}
        || grid.parent(CellKey{-1, -1, 0}) != CellKey{-1, -1, 1}
        || grid.neighbors({0, 0, 0}).size() != 8) {
        return 1;
    }

    if (grid.cell_for_2_5d({127.999, -0.001, 3.99}, 0, 4.0)
            != CellKey{0, -1, 0, 0}
        || grid.cell_for_2_5d({128.0, 0.0, 4.0}, 0, 4.0)
            != CellKey{1, 0, 0, 1}
        || grid.cell_for_2_5d({0.0, 0.0, -0.001}, 0, 4.0)
            != CellKey{0, 0, 0, -1}
        || grid.parent(CellKey{3, -2, 0, -4}) != CellKey{1, -1, 1, -4}
        || grid.neighbors(CellKey{0, 0, 0, 7}).front().z != 7) {
        return 1;
    }

    DynamicCellIndex index{grid};
    if (index.upsert({1}, {1.0, 1.0, 0.0}).has_value()) {
        return 1;
    }
    const auto migration = index.upsert({1}, {129.0, 1.0, 0.0});
    if (!migration.has_value() || migration->previous != CellKey{0, 0, 0}
        || migration->current != CellKey{1, 0, 0}
        || index.members({1, 0, 0}).size() != 1) {
        return 1;
    }
    if (!index.erase({1}) || index.size() != 0) {
        return 1;
    }

    DynamicCellIndex floors{grid, 0, 4.0};
    static_cast<void>(floors.upsert({2}, {1.0, 1.0, 4.0}));
    static_cast<void>(floors.upsert({3}, {1.0, 1.0, -4.0}));
    if (floors.cell_of({2}) != std::optional<CellKey>{CellKey{0, 0, 0, 1}}
        || floors.cell_of({3}) != std::optional<CellKey>{CellKey{0, 0, 0, -1}}
        || floors.query_candidates({0.0, 0.0, -4.0}, {2.0, 2.0, 4.0})
            != std::vector<geoworld::foundation::WorldId>{geoworld::foundation::WorldId{2},
                                                          geoworld::foundation::WorldId{3}}) {
        return 1;
    }
    return 0;
}
