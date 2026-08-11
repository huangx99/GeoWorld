#include "geoworld/spatial/coordinates.hpp"

#include <algorithm>

namespace geoworld::spatial {
namespace {

constexpr double semi_major_axis = 6378137.0;
constexpr double eccentricity_squared = 6.6943799901413165e-3;
constexpr double degrees_to_radians = 0.017453292519943295;
constexpr double radians_to_degrees = 57.29577951308232;

} // namespace

Ecef geodetic_to_ecef(Geodetic coordinate) noexcept {
    const auto latitude = coordinate.latitude_degrees * degrees_to_radians;
    const auto longitude = coordinate.longitude_degrees * degrees_to_radians;
    const auto sin_latitude = std::sin(latitude);
    const auto cos_latitude = std::cos(latitude);
    const auto sin_longitude = std::sin(longitude);
    const auto cos_longitude = std::cos(longitude);
    const auto radius = semi_major_axis
        / std::sqrt(1.0 - eccentricity_squared * sin_latitude * sin_latitude);

    return {
        (radius + coordinate.height_meters) * cos_latitude * cos_longitude,
        (radius + coordinate.height_meters) * cos_latitude * sin_longitude,
        (radius * (1.0 - eccentricity_squared) + coordinate.height_meters) * sin_latitude
    };
}

Geodetic ecef_to_geodetic(Ecef coordinate) noexcept {
    const auto longitude = std::atan2(coordinate.y, coordinate.x);
    const auto horizontal = std::hypot(coordinate.x, coordinate.y);
    const auto semi_minor_axis = semi_major_axis * std::sqrt(1.0 - eccentricity_squared);
    const auto second_eccentricity_squared =
        (semi_major_axis * semi_major_axis - semi_minor_axis * semi_minor_axis)
        / (semi_minor_axis * semi_minor_axis);
    const auto theta = std::atan2(coordinate.z * semi_major_axis,
                                  horizontal * semi_minor_axis);
    const auto sin_theta = std::sin(theta);
    const auto cos_theta = std::cos(theta);
    const auto latitude = std::atan2(
        coordinate.z + second_eccentricity_squared * semi_minor_axis * sin_theta * sin_theta * sin_theta,
        horizontal - eccentricity_squared * semi_major_axis * cos_theta * cos_theta * cos_theta);
    const auto sin_latitude = std::sin(latitude);
    const auto radius = semi_major_axis
        / std::sqrt(1.0 - eccentricity_squared * sin_latitude * sin_latitude);

    return {
        latitude * radians_to_degrees,
        longitude * radians_to_degrees,
        horizontal / std::cos(latitude) - radius
    };
}

Enu ecef_to_enu(Ecef origin, Geodetic origin_geodetic, Ecef point) noexcept {
    const auto latitude = origin_geodetic.latitude_degrees * degrees_to_radians;
    const auto longitude = origin_geodetic.longitude_degrees * degrees_to_radians;
    const auto sin_latitude = std::sin(latitude);
    const auto cos_latitude = std::cos(latitude);
    const auto sin_longitude = std::sin(longitude);
    const auto cos_longitude = std::cos(longitude);
    const auto dx = point.x - origin.x;
    const auto dy = point.y - origin.y;
    const auto dz = point.z - origin.z;

    return {
        -sin_longitude * dx + cos_longitude * dy,
        -sin_latitude * cos_longitude * dx - sin_latitude * sin_longitude * dy
            + cos_latitude * dz,
        cos_latitude * cos_longitude * dx + cos_latitude * sin_longitude * dy
            + sin_latitude * dz
    };
}

Ecef enu_to_ecef(Ecef origin, Geodetic origin_geodetic, Enu point) noexcept {
    const auto latitude = origin_geodetic.latitude_degrees * degrees_to_radians;
    const auto longitude = origin_geodetic.longitude_degrees * degrees_to_radians;
    const auto sin_latitude = std::sin(latitude);
    const auto cos_latitude = std::cos(latitude);
    const auto sin_longitude = std::sin(longitude);
    const auto cos_longitude = std::cos(longitude);

    return {
        origin.x - sin_longitude * point.east
            - sin_latitude * cos_longitude * point.north
            + cos_latitude * cos_longitude * point.up,
        origin.y + cos_longitude * point.east
            - sin_latitude * sin_longitude * point.north
            + cos_latitude * sin_longitude * point.up,
        origin.z + cos_latitude * point.north + sin_latitude * point.up
    };
}

EnuFloat to_float(Enu point) noexcept {
    return {static_cast<float>(point.east), static_cast<float>(point.north), static_cast<float>(point.up)};
}

} // namespace geoworld::spatial
