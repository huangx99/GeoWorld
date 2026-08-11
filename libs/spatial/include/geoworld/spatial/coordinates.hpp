#pragma once

#include <cmath>

namespace geoworld::spatial {

struct Geodetic {
    double latitude_degrees{};
    double longitude_degrees{};
    double height_meters{};
};

struct Ecef {
    double x{};
    double y{};
    double z{};
};

struct Enu {
    double east{};
    double north{};
    double up{};
};

struct EnuFloat {
    float east{};
    float north{};
    float up{};
};

[[nodiscard]] Ecef geodetic_to_ecef(Geodetic coordinate) noexcept;
[[nodiscard]] Geodetic ecef_to_geodetic(Ecef coordinate) noexcept;
[[nodiscard]] Enu ecef_to_enu(Ecef origin, Geodetic origin_geodetic, Ecef point) noexcept;
[[nodiscard]] Ecef enu_to_ecef(Ecef origin, Geodetic origin_geodetic, Enu point) noexcept;
[[nodiscard]] EnuFloat to_float(Enu point) noexcept;

} // namespace geoworld::spatial
