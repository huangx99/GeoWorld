#include "geoworld/spatial/coordinates.hpp"

#include <proj.h>

#include <cmath>

int main() {
    constexpr auto latitude = 31.2304;
    constexpr auto longitude = 121.4737;
    constexpr auto height = 10.0;
    const auto expected = geoworld::spatial::geodetic_to_ecef({latitude, longitude, height});

    auto* context = proj_context_create();
    auto* raw = proj_create_crs_to_crs(context, "EPSG:4979", "EPSG:4978", nullptr);
    auto* transform = proj_normalize_for_visualization(context, raw);
    if (context == nullptr || raw == nullptr || transform == nullptr) {
        if (transform != nullptr) proj_destroy(transform);
        if (raw != nullptr) proj_destroy(raw);
        if (context != nullptr) proj_context_destroy(context);
        return 1;
    }

    const auto result = proj_trans(transform, PJ_FWD, proj_coord(longitude, latitude, height, 0.0));
    const auto passed = std::abs(result.xyz.x - expected.x) < 1e-3
        && std::abs(result.xyz.y - expected.y) < 1e-3
        && std::abs(result.xyz.z - expected.z) < 1e-3;
    proj_destroy(transform);
    proj_destroy(raw);
    proj_context_destroy(context);
    return passed ? 0 : 1;
}
